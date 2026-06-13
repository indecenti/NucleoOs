// nucleo_w5500 — W5500 MACRAW driver implementation. See header for rationale.
// Privacy: no serial logging from the offensive stack (matches nucleo_wifiatk).
#define LOG_LOCAL_LEVEL ESP_LOG_NONE
#include "nucleo_w5500.h"
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"          // esp_rom_delay_us
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- W5500 register map -----------------------------------------------------
// SPI frame = 16-bit offset address + 8-bit control [BSB(5)|RWB(1)|OM(2)] + data. We pack the 24 bits
// (offset<<8 | control) into the SPI transaction address phase, so the data buffers stay pure payload.
#define BSB_COMMON   0x00
#define BSB_S0_REG   0x01
#define BSB_S0_TX    0x02
#define BSB_S0_RX    0x03
#define RWB_READ     0x00
#define RWB_WRITE    0x04          // bit2
// VDM (OM=00): CS stays asserted for the whole transaction (one spi_transaction).

// Common registers
#define W5_MR        0x0000        // mode (bit7 = RST) — W5_ prefix avoids clash with SoC 'MR' macro
#define GAR          0x0001        // gateway (4)
#define SUBR         0x0005        // subnet mask (4)
#define SHAR         0x0009        // source MAC (6)
#define SIPR         0x000F        // source IP (4)
#define VERSIONR     0x0039        // == 0x04
#define PHYCFGR      0x002E        // bit0 = link up

// Socket 0 registers
#define Sn_MR        0x0000
#define Sn_CR        0x0001
#define Sn_IR        0x0002
#define Sn_SR        0x0003
#define Sn_RXBUF_SZ  0x001E
#define Sn_TXBUF_SZ  0x001F
#define Sn_TX_FSR    0x0020        // free size (2)
#define Sn_TX_WR     0x0024        // write pointer (2)
#define Sn_RX_RSR    0x0026        // received size (2)
#define Sn_RX_RD     0x0028        // read pointer (2)

#define Sn_MR_MACRAW 0x04          // MAC filter bit (0x80) left OFF -> promiscuous
#define CR_OPEN      0x01
#define CR_CLOSE     0x10
#define CR_SEND      0x20
#define CR_RECV      0x40
#define SR_MACRAW    0x42
#define IR_SENDOK    0x10

#define W5_BUF       1600          // one MTU + W5500 2-byte length header, DMA scratch

static spi_device_handle_t s_dev;
static bool     s_present;
static int      s_rst = -1;
static uint8_t *s_tx, *s_rx;       // DMA-capable scratch (alloc at begin, free at end)

// ---- raw SPI transaction ----------------------------------------------------
static esp_err_t xfer(uint16_t offset, uint8_t bsb, bool write, const uint8_t *out, uint8_t *in, size_t len)
{
    if (!s_dev || len == 0) return ESP_FAIL;
    spi_transaction_t t = {0};
    t.flags  = 0;
    // ESP-IDF clocks command then address; we use address only (24 bits) = offset<<8 | control.
    t.addr   = ((uint32_t)offset << 8) | (uint8_t)((bsb << 3) | (write ? RWB_WRITE : RWB_READ));
    t.length = len * 8;            // bits on MOSI
    t.rxlength = write ? 0 : len * 8;
    t.tx_buffer = write ? out : NULL;
    t.rx_buffer = write ? NULL : in;
    return spi_device_polling_transmit(s_dev, &t);
}

static uint8_t r8(uint16_t off, uint8_t bsb)
{
    uint8_t v = 0; s_rx[0] = 0;
    if (xfer(off, bsb, false, NULL, s_rx, 1) == ESP_OK) v = s_rx[0];
    return v;
}
static void w8(uint16_t off, uint8_t bsb, uint8_t v) { s_tx[0] = v; xfer(off, bsb, true, s_tx, NULL, 1); }

// 16-bit volatile registers (RSR/pointers) can change mid-read; read until two reads agree.
static uint16_t r16(uint16_t off, uint8_t bsb)
{
    uint16_t a, b = 0; int tries = 0;
    do {
        s_rx[0] = s_rx[1] = 0;
        if (xfer(off, bsb, false, NULL, s_rx, 2) != ESP_OK) return 0;
        a = (uint16_t)((s_rx[0] << 8) | s_rx[1]);
        if (a == b) return a;
        b = a;
    } while (++tries < 4);
    return a;
}
static void w16(uint16_t off, uint8_t bsb, uint16_t v) { s_tx[0] = (uint8_t)(v >> 8); s_tx[1] = (uint8_t)v; xfer(off, bsb, true, s_tx, NULL, 2); }
// Buffer transfers always route through the DMA-capable scratch (s_tx/s_rx) — the SPI master needs
// DMA-able, word-aligned buffers, and the caller's frame buffers are neither.
static void wbuf(uint16_t off, uint8_t bsb, const uint8_t *p, size_t n)
{
    if (n > W5_BUF) n = W5_BUF;
    if (p != s_tx) memcpy(s_tx, p, n);
    xfer(off, bsb, true, s_tx, NULL, n);
}
static void rbuf(uint16_t off, uint8_t bsb, uint8_t *p, size_t n)
{
    if (n > W5_BUF) n = W5_BUF;
    xfer(off, bsb, false, NULL, s_rx, n);
    if (p != s_rx) memcpy(p, s_rx, n);
}

// ---- lifecycle --------------------------------------------------------------
bool w5500_begin(const w5500_cfg_t *cfg)
{
    s_present = false;
    s_rst = cfg->pin_rst;

    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }   // idempotent: re-detect won't double-add

    if (!s_tx) s_tx = heap_caps_malloc(W5_BUF, MALLOC_CAP_DMA);
    if (!s_rx) s_rx = heap_caps_malloc(W5_BUF, MALLOC_CAP_DMA);
    if (!s_tx || !s_rx) { w5500_end(); return false; }

    // Init the bus only if we own the pins; otherwise the SD mount already did it (shared bus).
    if (cfg->pin_sclk >= 0) {
        spi_bus_config_t bus = {0};
        bus.sclk_io_num = cfg->pin_sclk;
        bus.mosi_io_num = cfg->pin_mosi;
        bus.miso_io_num = cfg->pin_miso;
        bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
        bus.max_transfer_sz = W5_BUF;
        esp_err_t e = spi_bus_initialize((spi_host_device_t)cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { w5500_end(); return false; }
    }

    spi_device_interface_config_t dev = {0};
    dev.clock_speed_hz = cfg->clock_hz > 0 ? cfg->clock_hz : 16 * 1000 * 1000;
    dev.mode = 0;                  // CPOL=0 CPHA=0
    dev.spics_io_num = cfg->pin_cs;
    dev.queue_size = 2;
    dev.command_bits = 0;
    dev.address_bits = 24;         // offset(16) | control(8)
    if (spi_bus_add_device((spi_host_device_t)cfg->spi_host, &dev, &s_dev) != ESP_OK) { w5500_end(); return false; }

    // Optional hardware reset pulse (active low).
    if (s_rst >= 0) {
        gpio_set_direction((gpio_num_t)s_rst, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)s_rst, 0); esp_rom_delay_us(600);
        gpio_set_level((gpio_num_t)s_rst, 1); vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Soft reset, then wait for it to clear.
    w8(W5_MR, BSB_COMMON, 0x80);
    for (int i = 0; i < 50 && (r8(W5_MR, BSB_COMMON) & 0x80); i++) vTaskDelay(pdMS_TO_TICKS(1));

    if (w5500_version() != 0x04) { w5500_end(); return false; }   // no genuine W5500 answering

    // Program our source MAC.
    wbuf(SHAR, BSB_COMMON, cfg->mac, 6);

    // Give Socket 0 the entire 16KB TX + 16KB RX by zeroing sockets 1..7 then maxing socket 0.
    for (int n = 1; n < 8; n++) {
        uint8_t bsb = (uint8_t)((n * 4) + 1);     // socket n register block
        w8(Sn_RXBUF_SZ, bsb, 0);
        w8(Sn_TXBUF_SZ, bsb, 0);
    }
    w8(Sn_RXBUF_SZ, BSB_S0_REG, 16);
    w8(Sn_TXBUF_SZ, BSB_S0_REG, 16);

    // Open Socket 0 in MACRAW (MAC filter off -> promiscuous capture).
    w8(Sn_MR, BSB_S0_REG, Sn_MR_MACRAW);
    w8(Sn_CR, BSB_S0_REG, CR_OPEN);
    for (int i = 0; i < 50 && r8(Sn_SR, BSB_S0_REG) != SR_MACRAW; i++) vTaskDelay(pdMS_TO_TICKS(1));
    if (r8(Sn_SR, BSB_S0_REG) != SR_MACRAW) { w5500_end(); return false; }

    s_present = true;
    return true;
}

void w5500_end(void)
{
    if (s_dev) {
        w8(Sn_CR, BSB_S0_REG, CR_CLOSE);
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    if (s_tx) { heap_caps_free(s_tx); s_tx = NULL; }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
    s_present = false;
}

bool    w5500_present(void) { return s_present; }
uint8_t w5500_version(void) { return s_dev ? r8(VERSIONR, BSB_COMMON) : 0; }
bool    w5500_link(void)    { return s_dev ? (r8(PHYCFGR, BSB_COMMON) & 0x01) != 0 : false; }

void w5500_set_l3(uint32_t ip, uint32_t mask, uint32_t gw)
{
    if (!s_dev) return;
    uint8_t b[4];
    b[0]=(uint8_t)(ip>>24);  b[1]=(uint8_t)(ip>>16);  b[2]=(uint8_t)(ip>>8);  b[3]=(uint8_t)ip;  wbuf(SIPR, BSB_COMMON, b, 4);
    b[0]=(uint8_t)(mask>>24);b[1]=(uint8_t)(mask>>16);b[2]=(uint8_t)(mask>>8);b[3]=(uint8_t)mask;wbuf(SUBR, BSB_COMMON, b, 4);
    b[0]=(uint8_t)(gw>>24);  b[1]=(uint8_t)(gw>>16);  b[2]=(uint8_t)(gw>>8);  b[3]=(uint8_t)gw;  wbuf(GAR,  BSB_COMMON, b, 4);
}

// ---- TX / RX ----------------------------------------------------------------
int w5500_send(const uint8_t *frame, size_t len)
{
    if (!s_dev || !s_present || len == 0 || len > W5_BUF) return -1;
    if (r16(Sn_TX_FSR, BSB_S0_REG) < len) return 0;       // buffer momentarily full — caller paces
    uint16_t wr = r16(Sn_TX_WR, BSB_S0_REG);
    wbuf(wr, BSB_S0_TX, frame, len);                      // chip wraps within the 16KB socket buffer
    w16(Sn_TX_WR, BSB_S0_REG, (uint16_t)(wr + len));
    w8(Sn_CR, BSB_S0_REG, CR_SEND);
    // Wait for SEND completion (bounded) and clear the interrupt flag.
    for (int i = 0; i < 200; i++) {
        uint8_t ir = r8(Sn_IR, BSB_S0_REG);
        if (ir & IR_SENDOK) { w8(Sn_IR, BSB_S0_REG, IR_SENDOK); return (int)len; }
        esp_rom_delay_us(20);
    }
    return (int)len;   // timed out waiting for SENDOK, but the frame was queued
}

int w5500_recv(uint8_t *buf, size_t max)
{
    if (!s_dev || !s_present) return -1;
    uint16_t rsr = r16(Sn_RX_RSR, BSB_S0_REG);
    if (rsr < 2) return 0;                                // nothing (need at least the 2-byte header)
    uint16_t rd = r16(Sn_RX_RD, BSB_S0_REG);

    // MACRAW: each packet is prefixed by a 2-byte big-endian length INCLUDING the 2 header bytes.
    uint8_t hdr[2];
    rbuf(rd, BSB_S0_RX, hdr, 2);
    uint16_t pkt = (uint16_t)((hdr[0] << 8) | hdr[1]);
    if (pkt < 2 || pkt > rsr) {                           // desync — flush the whole RX buffer
        w16(Sn_RX_RD, BSB_S0_REG, (uint16_t)(rd + rsr));
        w8(Sn_CR, BSB_S0_REG, CR_RECV);
        return 0;
    }
    uint16_t flen = (uint16_t)(pkt - 2);
    if (flen > max) flen = (uint16_t)max;                 // truncate into caller buffer (still consume all)
    rbuf((uint16_t)(rd + 2), BSB_S0_RX, buf, flen);
    w16(Sn_RX_RD, BSB_S0_REG, (uint16_t)(rd + pkt));      // advance past the WHOLE packet
    w8(Sn_CR, BSB_S0_REG, CR_RECV);
    return (int)flen;
}
