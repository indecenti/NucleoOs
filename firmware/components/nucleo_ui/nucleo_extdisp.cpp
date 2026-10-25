// External display — the hardware half (see include/nucleo_extdisp.h and docs/external-display.md).
// Pure rules live in extdisp_probe.c, the mirror panel in extdisp_mirror.cpp; both are host-tested
// (npm run extdisp:test). This file only senses pins, talks SPI, wires the pieces into `d`, and reports.
//
// Bus: the panel and the touch controller join SPI2 = the SD card's bus (SCK 40 / MOSI 14 / MISO 39),
// each with its own chip-select on a free EXT pin. The SD driver initialised the bus; we only add devices.
//
// Module revisions: the red "2.8\" TFT SPI 240x320" board (V1.1, V1.2) ships with an ILI9341 OR an
// ST7789V under the same silkscreen. With SDO wired the controller is read back (ID4 / RDDID); without it
// the ILI9341 is assumed and settings.json "external_controller" picks the other one.
#include "nucleo_extdisp.h"
#include "extdisp_probe.h"
#include "extdisp_mirror.hpp"
#include "nucleo_ui.h"
#include "nucleo_board.h"
#include "nucleo_prefs.h"
#include "nucleo_storage.h"
#include <M5GFX.h>
#include "lgfx/v1/panel/Panel_ST7789.hpp"
#include <new>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "extdisp";
extern M5GFX d;

namespace {

// ILI9341 on the generic Panel_LCD (M5GFX ships no ILI9341 driver). Init values are the datasheet's
// power-on recommendations used by most ILI9341 modules; COLMOD comes from Panel_LCD. MADCTL does NOT:
// Panel_LCD's table is the ST7789 convention, and on an ILI9341 module its column order runs the other
// way (landscape text came out mirrored left-right on the red 2.8" board). Flipping MX (with MH, which
// Panel_LCD keeps paired with it) gives the usual ILI9341 table: MX, MV, MY, MX|MY|MV for rotations 0-3.
struct Panel_ILI9341 : public lgfx::Panel_LCD
{
    Panel_ILI9341(void)
    {
        _cfg.memory_width = _cfg.panel_width = 240;
        _cfg.memory_height = _cfg.panel_height = 320;
    }
protected:
    uint8_t getMadCtl(uint8_t r) const override { return Panel_LCD::getMadCtl(r) ^ (MAD_MX | MAD_MH); }
    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            0x01, 0 + CMD_INIT_DELAY, 120,                // SWRESET: a warm reboot finds the panel running
            0xCF, 3, 0x00, 0xC1, 0x30,                    // power control B
            0xED, 4, 0x64, 0x03, 0x12, 0x81,              // power-on sequence
            0xE8, 3, 0x85, 0x00, 0x78,                    // driver timing A
            0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,        // power control A
            0xF7, 1, 0x20,                                // pump ratio
            0xEA, 2, 0x00, 0x00,                          // driver timing B
            0xC0, 1, 0x23,                                // PWCTR1 4.60 V
            0xC1, 1, 0x10,                                // PWCTR2
            0xC5, 2, 0x3E, 0x28,                          // VMCTR1
            0xC7, 1, 0x86,                                // VMCTR2
            0xB1, 2, 0x00, 0x13,                          // frame rate
            0xB6, 3, 0x08, 0x82, 0x27,                    // display function control
            0xF2, 1, 0x00,                                // 3-gamma off
            0x26, 1, 0x01,                                // gamma curve 1
            0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
            0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
            CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
            CMD_DISPON, 0,
            0xFF, 0xFF,
        };
        return listno == 0 ? list0 : nullptr;
    }
};

// ---- state (everything heavier than these scalars is heap, allocated only when a display is found) ----
extdisp_cfg_t      s_cfg;
extdisp_decision_t s_dec = { false, false, "not probed" };
extdisp_probe_t    s_probe;
extdisp_ctrl_t     s_ctrl = EXTDISP_CTRL_AUTO;   // controller actually driven
bool               s_ctrl_read = false;          // ...and whether it was read back (vs assumed / configured)
uint8_t            s_id_d3[4], s_id_04[4];
int                s_t0 = -1, s_t1 = -1;
Panel_Mirror      *s_mirror = nullptr;
int                s_bl_pin = -1;
spi_device_handle_t s_touch = nullptr;           // kept for the diagnostics task
volatile int       s_presses = 0, s_last_x = -1, s_last_y = -1;

// A pin another module actively drives LOW defeats our weak pull-up (e.g. a LoRa cap's BUSY on G6).
bool driven_low_against_pullup(int pin)
{
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    for (int i = 0; i < 20; i++) {                  // ~2 ms: a module output that is low at any instant backs us off
        esp_rom_delay_us(100);
        if (gpio_get_level((gpio_num_t)pin) == 0) return true;
    }
    return false;
}
// ...and one driven HIGH defeats our weak pull-down (e.g. a GPS/UART TX idling high on G15).
bool driven_high_against_pulldown(int pin)
{
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(100);
    bool high = gpio_get_level((gpio_num_t)pin) == 1;
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
    return high;
}

// Add a device on the SD's bus; if no SD driver brought the bus up this boot, do it with the SD's own
// config so a later mount finds it compatible (it accepts INVALID_STATE as "shared").
esp_err_t add_device(int cs, int hz, spi_device_handle_t *h)
{
    spi_device_interface_config_t dc;
    memset(&dc, 0, sizeof dc);
    dc.clock_speed_hz = hz;
    dc.mode = 0;
    dc.spics_io_num = cs;
    dc.queue_size = 1;
    esp_err_t e = spi_bus_add_device(NUCLEO_SD_SPI_HOST, &dc, h);
    if (e == ESP_ERR_INVALID_STATE) {
        spi_bus_config_t bus;
        memset(&bus, 0, sizeof bus);
        bus.mosi_io_num = NUCLEO_SD_PIN_MOSI; bus.miso_io_num = NUCLEO_SD_PIN_MISO; bus.sclk_io_num = NUCLEO_SD_PIN_SCLK;
        bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = 4000;
        if (spi_bus_initialize(NUCLEO_SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) == ESP_OK)
            e = spi_bus_add_device(NUCLEO_SD_SPI_HOST, &dc, h);
    }
    return e;
}

int xpt_read(spi_device_handle_t h, uint8_t cmd)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 24;
    t.tx_data[0] = cmd;
    if (spi_device_polling_transmit(h, &t) != ESP_OK) return -1;
    return xpt_sample(t.rx_data);
}

// Touch fingerprint on T_CS at 1 MHz. Leaves T_CS driven HIGH if a chip answered (a deselected XPT2046
// releases MISO; a floating CS would let it fight the SD card), pulled up otherwise.
bool probe_touch(int t_cs)
{
    spi_device_handle_t h = nullptr;
    esp_err_t e = add_device(t_cs, 1000000, &h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "touch probe: add device failed (%s)", esp_err_to_name(e)); return false; }
    xpt_read(h, XPT_CMD_TEMP0);                      // reference + ADC power-up conversion
    esp_rom_delay_us(3000);
    xpt_read(h, XPT_CMD_TEMP0);
    s_t0 = xpt_read(h, XPT_CMD_TEMP0);
    xpt_read(h, XPT_CMD_TEMP1);
    s_t1 = xpt_read(h, XPT_CMD_TEMP1);
    xpt_read(h, XPT_CMD_IDLE);                       // back to power-down, PENIRQ armed
    spi_bus_remove_device(h);

    bool found = xpt_fingerprint(s_t0, s_t1);
    ESP_LOGW(TAG, "touch probe on G%d: temp0=%d temp1=%d -> %s", t_cs, s_t0, s_t1, found ? "XPT2046" : "nothing");
    gpio_reset_pin((gpio_num_t)t_cs);
    if (found) { gpio_set_level((gpio_num_t)t_cs, 1); gpio_set_direction((gpio_num_t)t_cs, GPIO_MODE_OUTPUT); }
    else       { gpio_set_direction((gpio_num_t)t_cs, GPIO_MODE_INPUT); gpio_set_pull_mode((gpio_num_t)t_cs, GPIO_PULLUP_ONLY); }
    return found;
}

// Read the controller's ID over SDO (if wired): command with DC low, then 4 bytes with DC high, CS held
// across both. Without SDO the line reads the SD's idle level (0xFF) and identify() says "unknown".
void read_lcd_ids(void)
{
    memset(s_id_d3, 0xFF, sizeof s_id_d3);
    memset(s_id_04, 0xFF, sizeof s_id_04);
    gpio_reset_pin((gpio_num_t)s_cfg.dc);
    gpio_set_direction((gpio_num_t)s_cfg.dc, GPIO_MODE_OUTPUT);
    if (s_cfg.rst >= 0) {                            // hardware reset: a known state for the read
        gpio_reset_pin((gpio_num_t)s_cfg.rst);
        gpio_set_level((gpio_num_t)s_cfg.rst, 0);
        gpio_set_direction((gpio_num_t)s_cfg.rst, GPIO_MODE_OUTPUT);
        esp_rom_delay_us(20000);
        gpio_set_level((gpio_num_t)s_cfg.rst, 1); vTaskDelay(pdMS_TO_TICKS(130));
    }
    spi_device_handle_t h = nullptr;
    if (add_device(s_cfg.cs, 2000000, &h) != ESP_OK) return;
    if (spi_device_acquire_bus(h, portMAX_DELAY) == ESP_OK) {
        const uint8_t cmds[2] = { 0xD3, 0x04 };
        uint8_t *outs[2] = { s_id_d3, s_id_04 };
        for (int i = 0; i < 2; i++) {
            spi_transaction_t t;
            memset(&t, 0, sizeof t);
            t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
            t.length = 8; t.tx_data[0] = cmds[i];
            gpio_set_level((gpio_num_t)s_cfg.dc, 0);
            spi_device_polling_transmit(h, &t);
            memset(&t, 0, sizeof t);
            t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
            t.length = 32; t.rxlength = 32;
            gpio_set_level((gpio_num_t)s_cfg.dc, 1);
            if (spi_device_polling_transmit(h, &t) == ESP_OK) memcpy(outs[i], t.rx_data, 4);
        }
        spi_device_release_bus(h);
    }
    spi_bus_remove_device(h);
    gpio_reset_pin((gpio_num_t)s_cfg.cs);           // CS high until the panel driver takes it over
    gpio_set_level((gpio_num_t)s_cfg.cs, 1);
    gpio_set_direction((gpio_num_t)s_cfg.cs, GPIO_MODE_OUTPUT);
    ESP_LOGW(TAG, "LCD id: D3=%02X %02X %02X %02X  04=%02X %02X %02X %02X", s_id_d3[0], s_id_d3[1], s_id_d3[2],
             s_id_d3[3], s_id_04[0], s_id_04[1], s_id_04[2], s_id_04[3]);
}

volatile bool s_screen_on = true;
void bl_follow(uint8_t brightness)
{
    s_screen_on = brightness != 0;
    if (s_bl_pin >= 0) gpio_set_level((gpio_num_t)s_bl_pin, brightness ? 1 : 0);
}

// The letterbox around the mirrored frame is never written by the mirror, so it carries a permanent test
// card: R/G/B/W swatches (wrong colours = flip external_rgb / external_invert) and the live config.
void draw_test_card(lgfx::LGFX_Device *dev, int oy, int fh)
{
    const int W = dev->width(), top = oy, bot = oy + fh;
    dev->fillScreen(TFT_BLACK);
    dev->setTextColor(TFT_WHITE, TFT_BLACK);
    dev->setFont(&lgfx::fonts::Font2);
    char line[64];
    snprintf(line, sizeof line, "NucleoOS EXT  %s%s  %dMHz", extdisp_ctrl_str(s_ctrl), s_ctrl_read ? "" : "?", s_cfg.mhz);
    dev->drawString(line, 6, 4);
    static const struct { uint16_t c; const char *l; } SW[4] = {
        { TFT_RED, "R" }, { TFT_GREEN, "G" }, { TFT_BLUE, "B" }, { TFT_WHITE, "W" } };
    for (int i = 0; i < 4; i++) {
        int x = 6 + i * 40;
        dev->fillRect(x, 24, 34, top - 30 > 8 ? top - 30 : 8, SW[i].c);
        dev->setTextColor(i == 3 ? TFT_BLACK : TFT_WHITE, SW[i].c);
        dev->drawString(SW[i].l, x + 13, 26);
    }
    dev->setTextColor(TFT_DARKGREY, TFT_BLACK);
    dev->drawString("colours wrong? see docs", 172, 30);
    dev->drawRect(dev->width() / 2 - 121, top - 1, 242, fh + 2, TFT_DARKGREY);
    dev->setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(line, sizeof line, "touch: %s   rot %d%s", s_dec.touch ? "XPT2046" : "none", s_cfg.rotation,
             s_cfg.mirror ? " mirror" : "");
    dev->drawString(line, 6, bot + 8);
    (void)W;
}

// Bring the panel up and swap the mirror in under `d`. Heap only on this path (~1 KB of driver objects),
// so a Cardputer with nothing on the header pays nothing.
bool attach(void)
{
    s_ctrl = s_cfg.controller;
    if (s_ctrl == EXTDISP_CTRL_AUTO) {
        read_lcd_ids();
        s_ctrl = extdisp_identify(s_id_d3, s_id_04);
        s_ctrl_read = s_ctrl != EXTDISP_CTRL_AUTO;
        if (!s_ctrl_read) s_ctrl = EXTDISP_CTRL_ILI9341;   // SDO not wired: the common chip
    }
    bool invert, rgb_order;
    extdisp_panel_flags(&s_cfg, s_ctrl, &invert, &rgb_order);

    auto *bus = new (std::nothrow) lgfx::Bus_SPI();
    lgfx::Panel_LCD *panel = s_ctrl == EXTDISP_CTRL_ST7789
                           ? (lgfx::Panel_LCD *)new (std::nothrow) lgfx::Panel_ST7789()
                           : (lgfx::Panel_LCD *)new (std::nothrow) Panel_ILI9341();
    auto *dev = new (std::nothrow) lgfx::LGFX_Device();
    auto *mirror = new (std::nothrow) Panel_Mirror();
    if (!bus || !panel || !dev || !mirror) {
        delete bus; delete panel; delete dev; delete mirror;
        s_dec.reason = "out of memory for the panel driver";
        return false;
    }
    {
        auto c = bus->config();
        c.spi_host = NUCLEO_SD_SPI_HOST;
        c.spi_mode = 0;
        c.spi_3wire = false;
        c.use_lock = true;
        c.freq_write = (uint32_t)s_cfg.mhz * 1000000u;
        c.freq_read = 8000000;
        c.pin_sclk = NUCLEO_SD_PIN_SCLK;
        c.pin_mosi = NUCLEO_SD_PIN_MOSI;
        c.pin_miso = -1;          // the panel is never read through the driver (the mirror reads the internal one)
        c.pin_dc = s_cfg.dc;
        c.dma_channel = 0;        // CPU FIFO writes: never touch the DMA channel the SD driver owns
        bus->config(c);
    }
    panel->setBus(bus);
    {
        auto c = panel->config();
        c.pin_cs = s_cfg.cs;
        c.pin_rst = s_cfg.rst;
        c.panel_width = c.memory_width = 240;
        c.panel_height = c.memory_height = 320;
        c.offset_x = c.offset_y = 0;
        c.readable = false;
        c.invert = invert;
        c.rgb_order = rgb_order;
        c.bus_shared = true;
        panel->config(c);
    }
    dev->setPanel(panel);
    // lgfx adds its own CS-less 3-wire device on SPI2 and, if that fails, only logs and goes on writing SPI2's
    // registers WITHOUT the bus lock while the SD driver may be mid-transfer. Try the same device first and
    // give up cleanly if the host has no room for it.
    {
        spi_device_interface_config_t dc;
        memset(&dc, 0, sizeof dc);
        dc.clock_speed_hz = 10000000; dc.spics_io_num = -1; dc.queue_size = 1;
        dc.flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX;
        spi_device_handle_t trial = nullptr;
        if (spi_bus_add_device(NUCLEO_SD_SPI_HOST, &dc, &trial) != ESP_OK) {
            s_dec.reason = "SPI2 has no free device slot for the panel driver";
            return false;
        }
        spi_bus_remove_device(trial);
    }
    // NB: lgfx's spi::init also writes SPI2's USER/CTRL registers once, unlocked. Safe ONLY because this runs
    // in the single-threaded boot (no other task touches the SD yet): keep nucleo_extdisp_boot() there.
    int64_t t0 = esp_timer_get_time();
    if (!dev->init()) {          // reset, init list, clear to black
        s_dec.reason = "panel init failed";
        return false;            // objects leak on purpose: Bus_SPI::release() would free the SD's bus
    }
    dev->setRotation(extdisp_lgfx_rotation(&s_cfg));

    if (s_cfg.bl >= 0) {
        s_bl_pin = s_cfg.bl;
        gpio_reset_pin((gpio_num_t)s_bl_pin);
        gpio_set_level((gpio_num_t)s_bl_pin, 1);
        gpio_set_direction((gpio_num_t)s_bl_pin, GPIO_MODE_OUTPUT);
    }
    mirror->set_brightness_cb(bl_follow);           // also tells the touch diagnostics when the screen is off

    // Swap the mirror in under `d`. Boot is single-threaded here (no UI task yet), so no frame is mid-flight.
    mirror->attach(d.getPanel(), panel);
    draw_test_card(dev, mirror->origin_y(), mirror->height());
    d.setPanel(mirror);
    s_mirror = mirror;
    ESP_LOGW(TAG, "%s%s %dx%d invert=%d bgr=%d: mirroring %dx%d at %d,%d (init %d ms)",
             extdisp_ctrl_str(s_ctrl), s_ctrl_read ? " (read back)" : " (assumed/configured)",
             panel->width(), panel->height(), invert, !rgb_order, mirror->width(), mirror->height(),
             mirror->origin_x(), mirror->origin_y(), (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

// ---- tester diagnostics: touch events in the log --------------------------------------------------------
// Polls the XPT2046 every 40 ms (only a Z read when untouched; skipped entirely while T_IRQ, if wired,
// says "no touch") and logs press / release with raw and approximate screen coordinates. Phase 3 replaces
// this with real input; until then it proves the touch path end to end without any UI change.
void touch_diag_task(void *)
{
    bool down = false;
    int64_t t_down = 0;
    int x0 = 0, y0 = 0, rx = 0, ry = 0, rz = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(40));
        if (!s_screen_on) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }   // screen off: stop polling
        if (s_cfg.t_irq >= 0 && gpio_get_level((gpio_num_t)s_cfg.t_irq) == 1 && !down) continue;
        int z = xpt_pressure(xpt_read(s_touch, XPT_CMD_Z1), xpt_read(s_touch, XPT_CMD_Z2));
        bool now = z > XPT_Z_TOUCH;
        if (now) {
            xpt_read(s_touch, XPT_CMD_X);
            int ax = xpt_read(s_touch, XPT_CMD_X), ay = xpt_read(s_touch, XPT_CMD_Y);
            ay = (ay + xpt_read(s_touch, XPT_CMD_Y)) / 2;
            if (ax >= 0 && ay >= 0) { rx = ax; ry = ay; rz = z; }
        }
        xpt_read(s_touch, XPT_CMD_IDLE);
        int sx, sy;
        extdisp_touch_map(rx, ry, extdisp_lgfx_rotation(&s_cfg), &sx, &sy);
        if (now && !down) {
            down = true; t_down = esp_timer_get_time(); x0 = sx; y0 = sy;
            s_presses = s_presses + 1; s_last_x = sx; s_last_y = sy;
            ESP_LOGW(TAG, "touch DOWN #%d raw x=%d y=%d z=%d -> ~%d,%d", s_presses, rx, ry, rz, sx, sy);
        } else if (!now && down) {
            down = false;
            ESP_LOGW(TAG, "touch UP after %d ms at raw x=%d y=%d -> ~%d,%d (moved %d,%d)",
                     (int)((esp_timer_get_time() - t_down) / 1000), rx, ry, sx, sy, sx - x0, sy - y0);
        } else if (now) {
            s_last_x = sx; s_last_y = sy;
        }
    }
}

void start_touch_diag(void)
{
    if (add_device(s_cfg.t_cs, 2000000, &s_touch) != ESP_OK) { ESP_LOGW(TAG, "touch diag: no device"); return; }
    if (s_cfg.t_irq >= 0) {
        gpio_reset_pin((gpio_num_t)s_cfg.t_irq);
        gpio_set_direction((gpio_num_t)s_cfg.t_irq, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)s_cfg.t_irq, GPIO_PULLUP_ONLY);   // PENIRQ is open-drain, low = touched
    }
    if (xTaskCreate(touch_diag_task, "extdisp_touch", 4096, nullptr, 2, nullptr) != pdPASS)
        ESP_LOGW(TAG, "touch diag: task not created (heap %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    else
        ESP_LOGW(TAG, "touch diag: logging touch DOWN/UP (T_IRQ %s)", s_cfg.t_irq >= 0 ? "wired" : "polled");
}

// Everything a tester needs to send back, on the SD. Built in RAM first and rewritten only when it differs
// from the file already on the card: an ADV that reports the same lines every boot costs one write, not
// one per boot. (Heap varies per boot, so it goes to the log, not the file.)
#define REPORT_PATH NUCLEO_SD_MOUNT "/extdisp-report.txt"
void write_report(size_t heap_before, size_t heap_after)
{
    ESP_LOGW(TAG, "heap free %u -> %u B, largest block %u B", (unsigned)heap_before, (unsigned)heap_after,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!nucleo_storage_info()->mounted) return;
    char *buf = nullptr;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return;
    const esp_app_desc_t *app = esp_app_get_description();
    fprintf(f, "NucleoOS external display report\n");
    fprintf(f, "firmware      %s (%s %s)\n", app->version, app->date, app->time);
    fprintf(f, "board         %s (M5GFX id %d)\n", nucleo_ui_is_adv() ? "Cardputer ADV" : "Cardputer (original)", (int)d.getBoard());
    fprintf(f, "mode          %s\n", extdisp_mode_str(s_cfg.mode));
    fprintf(f, "controller    setting=%s driven=%s %s\n", extdisp_ctrl_str(s_cfg.controller), extdisp_ctrl_str(s_ctrl),
            s_ctrl_read ? "(read back over SDO)" : s_cfg.controller == EXTDISP_CTRL_AUTO ? "(SDO not answering: assumed)" : "(configured)");
    fprintf(f, "pins          cs=%d dc=%d rst=%d t_cs=%d t_irq=%d bl=%d  (SCK 40 MOSI 14 MISO 39 shared with SD)\n",
            s_cfg.cs, s_cfg.dc, s_cfg.rst, s_cfg.t_cs, s_cfg.t_irq, s_cfg.bl);
    fprintf(f, "spi           %d MHz, rotation %d, mirror=%d, invert=%d rgb=%d (-1 = controller default)\n",
            s_cfg.mhz, s_cfg.rotation, s_cfg.mirror, s_cfg.invert, s_cfg.rgb);
    fprintf(f, "pin sense     t_cs_busy=%d dc_busy=%d\n", s_probe.t_cs_busy, s_probe.dc_busy);
    fprintf(f, "touch probe   temp0=%d temp1=%d -> %s\n", s_t0, s_t1, s_probe.touch_found ? "XPT2046 found" : "no answer");
    fprintf(f, "lcd id        D3: %02X %02X %02X %02X   04: %02X %02X %02X %02X\n", s_id_d3[0], s_id_d3[1], s_id_d3[2],
            s_id_d3[3], s_id_04[0], s_id_04[1], s_id_04[2], s_id_04[3]);
    fprintf(f, "decision      %s: %s\n", s_dec.attach ? "ATTACHED" : "not attached", s_dec.reason);
    if (s_mirror)
        fprintf(f, "mirror        %dx%d at %d,%d on 320x240\n", s_mirror->width(), s_mirror->height(),
                s_mirror->origin_x(), s_mirror->origin_y());
    fprintf(f, "live          GET /api/status -> display_ext ; heap + touch events in /api/logs (tag extdisp)\n");
    fclose(f);
    if (!buf) return;
    bool same = false;
    if (FILE *old = fopen(REPORT_PATH, "r")) {
        same = true;
        for (size_t i = 0; i < len && same; i++) same = fgetc(old) == (unsigned char)buf[i];
        if (same) same = fgetc(old) == EOF;
        fclose(old);
    }
    if (!same) {
        if (FILE *w = fopen(REPORT_PATH, "w")) { fwrite(buf, 1, len, w); fclose(w); }
        else ESP_LOGW(TAG, "report: cannot write " REPORT_PATH);
    }
    free(buf);
}

} // namespace

extern "C" void nucleo_extdisp_early(void)
{
    if (!nucleo_ui_is_adv()) return;              // original Cardputer: G5/G6 are keyboard matrix lines
    extdisp_cfg_t c;
    extdisp_cfg_default(&c);
    gpio_set_pull_mode((gpio_num_t)c.cs, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)c.t_cs, GPIO_PULLUP_ONLY);
}

extern "C" void nucleo_extdisp_boot(void)
{
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    extdisp_cfg_default(&s_cfg);
    if (!nucleo_ui_is_adv()) {                      // original Cardputer: no EXT header, not even a settings read
        s_cfg.mode = EXTDISP_OFF;
        s_dec.reason = "original Cardputer: no EXT header";
        return;
    }
    char mode[8] = "auto", ctrl[12] = "auto";
    int pins[6] = { s_cfg.cs, s_cfg.dc, s_cfg.rst, s_cfg.t_cs, s_cfg.t_irq, s_cfg.bl };
    nucleo_prefs_load_extdisp(mode, sizeof mode, ctrl, sizeof ctrl, &s_cfg.rotation, &s_cfg.mhz,
                              &s_cfg.mirror, &s_cfg.invert, &s_cfg.rgb, &s_cfg.diag, pins);
    s_cfg.mode = extdisp_mode_parse(mode);
    s_cfg.controller = extdisp_ctrl_parse(ctrl);
    s_cfg.cs = pins[0]; s_cfg.dc = pins[1]; s_cfg.rst = pins[2];
    s_cfg.t_cs = pins[3]; s_cfg.t_irq = pins[4]; s_cfg.bl = pins[5];
    // Tester shortcut: an empty file /sd/extdisp-on forces mode "on" (a module wired WITHOUT its touch lines,
    // so auto has nothing to fingerprint) without hand-editing settings.json. An explicit "off" still wins.
    if (s_cfg.mode == EXTDISP_AUTO && nucleo_storage_info()->mounted) {
        if (FILE *m = fopen(NUCLEO_SD_MOUNT "/extdisp-on", "r")) { fclose(m); s_cfg.mode = EXTDISP_ON; }
    }

    // Sense and probe only where the rules allow touching pins at all (ADV, not off, valid pins).
    s_probe = extdisp_probe_t{ nucleo_ui_is_adv(), false, false, false };
    if (s_cfg.mode != EXTDISP_OFF && s_probe.is_adv && !extdisp_cfg_check(&s_cfg)) {
        if (s_cfg.t_cs >= 0) {
            s_probe.t_cs_busy = driven_low_against_pullup(s_cfg.t_cs);
            if (!s_probe.t_cs_busy) s_probe.touch_found = probe_touch(s_cfg.t_cs);
        }
        if (s_probe.touch_found && s_cfg.mode == EXTDISP_AUTO) s_probe.dc_busy = driven_high_against_pulldown(s_cfg.dc);
    }
    s_dec = extdisp_decide(&s_cfg, &s_probe);
    if (s_dec.attach && !attach()) s_dec.attach = false;
    ESP_LOGW(TAG, "mode=%s -> %s: %s", extdisp_mode_str(s_cfg.mode), s_dec.attach ? "ON" : "off", s_dec.reason);
    if (s_dec.attach && s_dec.touch && s_cfg.diag) start_touch_diag();
    if (s_cfg.diag && s_cfg.mode != EXTDISP_OFF && s_probe.is_adv)
        write_report(heap_before, heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

extern "C" bool nucleo_extdisp_active(void)        { return s_mirror != nullptr; }
extern "C" bool nucleo_extdisp_touch(void)         { return s_dec.touch; }
extern "C" const char *nucleo_extdisp_mode(void)   { return extdisp_mode_str(s_cfg.mode); }
extern "C" const char *nucleo_extdisp_reason(void) { return s_dec.reason; }
extern "C" const char *nucleo_extdisp_controller(void) { return s_mirror ? extdisp_ctrl_str(s_ctrl) : "none"; }
extern "C" void nucleo_extdisp_stats(unsigned *ext_tx, int *presses, int *last_x, int *last_y)
{
    if (ext_tx) *ext_tx = s_mirror ? s_mirror->ext_transactions() : 0;
    if (presses) *presses = s_presses;
    if (last_x) *last_x = s_last_x;
    if (last_y) *last_y = s_last_y;
}
