// nucleo_w5500 — minimal WIZnet W5500 driver in MACRAW mode over SPI.
//
// We deliberately do NOT use the chip's hardware TCP/IP stack and do NOT stand up an lwIP netif. The
// W5500 is opened with Socket 0 in MACRAW so it becomes a dumb L2 MAC over SPI: we hand it raw
// Ethernet frames to transmit and it hands us raw Ethernet frames it received (MAC filter OFF =
// promiscuous). All ARP/DHCP/TCP craft happens in eth_frames.c. This is what lets the wired-attack
// suite run on the no-PSRAM Cardputer — the chip's 16KB TX + 16KB RX buffers live ON the W5500, so
// the host only spends ~one MTU of heap for SPI scratch, instead of a whole second lwIP stack.
//
// Electrical reality on a bare Cardputer: the only practical attach is to SHARE the microSD SPI bus
// (SPI2_HOST: SCLK 40 / MOSI 14 / MISO 39) plus a chip-select on a free GPIO — soldered, not a Grove
// plug (the Grove port only breaks out G1/G2). The driver adds itself as a second device on that bus;
// per-device CS + the "one operation at a time" discipline keep it from colliding with SD access.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     spi_host;     // SPI host (default SPI2_HOST = the SD bus)
    int     pin_sclk;     // -1 = bus already initialised (by the SD mount) — just add our device
    int     pin_miso;
    int     pin_mosi;
    int     pin_cs;       // our dedicated chip-select GPIO
    int     pin_rst;      // hardware reset GPIO (-1 = none / tied high; soft reset still used)
    int     clock_hz;     // SPI clock (e.g. 16 MHz; conservative for a shared/soldered bus)
    uint8_t mac[6];       // source MAC programmed into SHAR (our wired identity)
} w5500_cfg_t;

// Initialise the SPI device (init the bus too if pin_sclk >= 0), hardware+soft reset, verify the chip
// (VERSIONR == 0x04), give Socket 0 all 16KB TX/RX, and open it in MACRAW promiscuous. Returns true
// only if a real W5500 answered; false (and w5500_present()==false) means "no module" — the app then
// shows its connect-module screen instead of pretending. Idempotent-ish: call w5500_end() first.
bool    w5500_begin(const w5500_cfg_t *cfg);
void    w5500_end(void);            // close the socket and release the SPI device
bool    w5500_present(void);        // last detection result
bool    w5500_link(void);           // PHY link up (cable connected + autoneg done)
uint8_t w5500_version(void);        // VERSIONR (0x04 for a genuine W5500; 0 if absent)

// Transmit one raw Ethernet frame (no FCS — the chip appends it). Returns len on success, 0 if the TX
// buffer is momentarily full (caller may retry/pace), <0 on SPI/socket error.
int     w5500_send(const uint8_t *frame, size_t len);

// Receive one raw Ethernet frame if present. Returns the frame length (FCS stripped) copied into buf,
// 0 if nothing waiting, <0 on error. Non-blocking — poll it.
int     w5500_recv(uint8_t *buf, size_t max);

// Program the on-chip L3 identity registers (SIPR/SUBR/GAR). MACRAW ignores them for TX/RX, but
// keeping them in sync documents the lease the bootstrap mini-DHCP-client obtained. Host order IPs.
void    w5500_set_l3(uint32_t ip, uint32_t mask, uint32_t gw);

#ifdef __cplusplus
}
#endif
