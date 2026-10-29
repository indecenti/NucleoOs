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
#include "nucleo_surface.h"
#include "nucleo_touch.h"
#include "nucleo_theme.h"
#include "extdisp_probe.h"
#include "extdisp_mirror.hpp"
#include "extdisp_touch.h"
#include "extdisp_chrome.hpp"
#include "nucleo_kbd.h"
#include "nucleo_i18n.h"
#include "nucleo_ui.h"
#include "nucleo_board.h"
#include "nucleo_prefs.h"
#include "nucleo_storage.h"
#include <M5GFX.h>
#include "lgfx/v1/panel/Panel_ST7789.hpp"
#include "lgfx/v1/platforms/esp32/Light_PWM.hpp"
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
#include "freertos/semphr.h"

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
lgfx::LGFX_Device *s_dev = nullptr;             // the external panel (touch layer draws the letterbox with it)
int                s_bl_pin = -1;
volatile int       s_hud_h = 0;                  // the system bar over a store app on the large surface (0 = none)
char               s_hud_title[28];              // ...what it says (nucleo_extdisp_set_hud)
uint16_t           s_hud_col = 0xFFFF;
lgfx::Light_PWM   *s_bl_pwm = nullptr;           // the backlight dimmed on its own LEDC channel (bl on a GPIO)
int                s_bl_pct = 100;               // settings "external_brightness": the panel's own level, 10..100
spi_device_handle_t s_touch = nullptr;           // the XPT2046, kept for the touch task
volatile int       s_presses = 0, s_last_x = -1, s_last_y = -1;
volatile int       s_tap_x = -1, s_tap_y = -1;       // where the last target tap landed (nucleo_touch_last)
bool               s_primary = true;             // settings "external_primary": an attached panel hosts large screens
bool               s_fit = true;                 // settings "external_fit": compact screens scaled 4/3 (320x180)
volatile int       s_layout = 0;                 // settings "external_layout": 0 duplicate, 1 Cardputer only, 2 second only
nucleo_extdisp_conf_t s_saved;                   // what settings.json holds now (the page shows "after restart")
volatile int       s_layout_gen = 0;             // bumped when the frame geometry changes (touch re-lays its keys)
volatile bool      s_cal_req = false;            // Settings asked for a recalibration
volatile bool      s_solo = false;               // the panel is `d`, natively (the launcher on 320x240)
volatile bool      s_calibrating = false;        // the calibration screen owns the panel (the mirror is dark)

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
void draw_chrome(void);
void dark_sync(void);
void draw_hud(void);
void lock_opt(void);
void unlock_opt(void);
// Screen off (idle auto-off, the Control Center, the web): the backlight goes with the internal one when it is
// on a GPIO. With the LED on the 5 V rail it cannot be switched, so the panel is painted black instead (a TN
// panel in sleep/display-off shows WHITE). Coming back, the OS repaints the frame and we redraw the letterbox.
void bl_follow(uint8_t brightness)
{
    const bool on = brightness != 0, was = s_screen_on;
    s_screen_on = on;
    if (s_bl_pwm) s_bl_pwm->setBrightness(on ? (uint8_t)(s_bl_pct * 255 / 100) : 0);
    else if (s_bl_pin >= 0) gpio_set_level((gpio_num_t)s_bl_pin, on ? 1 : 0);
    dark_sync();                                    // dark: nothing reaches it until on (or calibration ends)
    if (on == was || !s_dev || s_layout == 1) return;   // Cardputer only: the panel is black already
    if (!on && s_bl_pin < 0) { lock_opt(); s_dev->fillScreen(TFT_BLACK); unlock_opt(); }
    if (on) draw_chrome();
    if (on && s_solo && s_hud_h) { lock_opt(); draw_hud(); unlock_opt(); }   // the OS repaints the rest
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
    if (top > 1) dev->drawRect(0, top - 1, W, fh + 2, TFT_DARKGREY);
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
        // Dimmable: LEDC channel 5 (timer 2) — the Cardputer's own backlight holds channel 7 (timer 3).
        // No PWM (out of RAM) leaves the plain on/off GPIO above.
        if (auto *pwm = new (std::nothrow) lgfx::Light_PWM()) {
            auto bc = pwm->config();
            bc.pin_bl = (int16_t)s_bl_pin; bc.pwm_channel = 5; bc.freq = 12000; bc.offset = 0; bc.invert = false;
            pwm->config(bc);
            if (pwm->init((uint8_t)(s_bl_pct * 255 / 100))) s_bl_pwm = pwm; else delete pwm;
        }
    }
    mirror->set_brightness_cb(bl_follow);           // also tells the touch diagnostics when the screen is off

    // Swap the mirror in under `d`. Boot is single-threaded here (no UI task yet), so no frame is mid-flight.
    mirror->attach(d.getPanel(), panel);
    // Frame hold: one external transaction per frame (8 ms budget), handed back before every SD transfer.
    mirror->set_hold([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000); },
                     []() -> void * { return (void *)xTaskGetCurrentTaskHandle(); }, 8);
    nucleo_storage_set_sd_pre([]() { if (s_mirror) s_mirror->release_hold(); });
    mirror->set_fit(s_fit);
    mirror->set_ext_off(s_layout == 1);
    mirror->set_pri_off(s_layout == 2);
    draw_test_card(dev, mirror->origin_y(), mirror->frame_h());
    d.setPanel(mirror);
    s_mirror = mirror;
    s_dev = dev;
    ESP_LOGW(TAG, "%s%s %dx%d invert=%d bgr=%d: mirroring %dx%d at %d,%d (init %d ms)",
             extdisp_ctrl_str(s_ctrl), s_ctrl_read ? " (read back)" : " (assumed/configured)",
             panel->width(), panel->height(), invert, !rgb_order, mirror->frame_w(), mirror->frame_h(),
             mirror->origin_x(), mirror->origin_y(), (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

// ---- touch (phase 3): XPT2046 -> calibrated panel pixels -> soft keys / gestures -> injected nav keys ------
// The task polls the XPT2046 every 40 ms (20 ms while a finger is down; nothing at all while T_IRQ, if wired,
// says "no touch"), maps raw samples through the stored calibration and feeds the pure gesture machine
// (extdisp_touch.c). Its events become ordinary key presses via nucleo_kbd_inject(), so every app, game and
// the launcher take touch with no change. The soft keys live in the letterbox, which the mirror never writes;
// drawing there happens under s_ext_mux, the same lock the mirror takes around each external primitive.
// Nothing here exists without a display: the state below is heap, allocated by attach(), and the task and
// the mutex are created only when a panel came up and its touch answered (zero RAM and CPU otherwise).
struct TouchCtx {
    lgfx::LGFX_Device *dev;                      // the external panel's own device (letterbox drawing only)
    SemaphoreHandle_t mux;
    extdisp_cal_t cal;
    bool cal_stored;                             // a calibration taken in this rotation is in use
    int cal_err;                                 // check-target error of the last calibration (px)
    extdisp_rect_t keys[SK_COUNT];
    extdisp_rect_t keys_solo[SK_COUNT];          // solo: no letterbox, the whole panel is the gesture area
    extdisp_frame_t frame;
    char status1[32];
    // Touch targets (nucleo_touch.h): the set being built by the UI task, and the published one.
    struct Target { int16_t x, y, w, h; int16_t key; char ch; };
    Target tgt_build[NUCLEO_TOUCH_MAX], tgt[NUCLEO_TOUCH_MAX];
    uint8_t ntgt_build, ntgt;
    // Scroll areas (nucleo_touch_add_scroll), built and published with the targets.
    struct Scroll { int16_t x, y, w, h, step, kb, kf; char cb, cf; bool pad; };   // pad: nucleo_touch_add_pad
    Scroll scr_build[NUCLEO_TOUCH_SCROLL_MAX], scr[NUCLEO_TOUCH_SCROLL_MAX];
    uint8_t nscr_build, nscr;
};
TouchCtx *s_tc = nullptr;
bool      s_touch_input = true;                  // settings "external_touch": false = log touches only
int       s_cal_raw[7];                          // settings "external_touch_cal", as loaded
bool      s_has_cal_raw = false;

// Recursive: the task drawing a frame holds it across primitives (frame hold) and may take it again inside
// (touch targets, a brightness change mid-frame).
void ext_lock(void)   { xSemaphoreTakeRecursive(s_tc->mux, portMAX_DELAY); }
void ext_unlock(void) { xSemaphoreGiveRecursive(s_tc->mux); }

bool chrome_on(void) { return s_touch_input && s_tc && s_tc->mux && !s_solo && s_layout != 1; }
// Calibration can run on any surface (the mirrored frame or the large one): it needs only touch input and a
// panel that is shown.
bool cal_on(void) { return s_touch_input && s_tc && s_tc->mux && s_layout != 1; }
volatile bool s_repaint_req = false;
void draw_hud(void)
{
    if (!s_hud_h || !s_dev) return;
    const int W = s_dev->width(), h = s_hud_h, bh = h - 8, cy = h / 2;
    s_dev->startWrite();
    s_dev->fillRect(0, 0, W, h, THEME_BG);
    s_dev->fillRoundRect(4, 4, 40, bh, bh / 2, THEME_LINE);              // the Back button (lg::back's look)
    for (int k = 0; k < 3; k++) { s_dev->drawLine(28 + k, cy - 6, 22 + k, cy, THEME_FG); s_dev->drawLine(22 + k, cy, 28 + k, cy + 6, THEME_FG); }
    s_dev->setFont(&fonts::Font2); s_dev->setTextSize(1);
    s_dev->setTextColor(s_hud_col, THEME_BG);
    s_dev->setTextDatum(textdatum_t::middle_left);
    s_dev->drawString(s_hud_title, 54, cy + 1);
    s_dev->setTextDatum(textdatum_t::top_left);
    s_dev->drawFastHLine(0, h - 1, W, THEME_LINE);
    s_dev->endWrite();
}           // the panel was painted over (calibration): the OS repaints it all
// The mirror stays dark while the screen is off with an unswitchable LED, and while calibration owns the panel.
void dark_sync(void)
{
    if (s_mirror) s_mirror->set_dark(s_calibrating || (!s_screen_on && s_bl_pin < 0 && s_layout != 1));
}
// Before this task drives the external panel itself (s_dev), it gives back any frame hold of its own — the
// same bus device must not be acquired twice.
void lock_opt(void)   { if (s_mirror) s_mirror->release_hold(); if (s_tc && s_tc->mux) ext_lock(); }
void unlock_opt(void) { if (s_tc && s_tc->mux) ext_unlock(); }
const char *status2(void) { return TR5("tieni: calibra", "hold: calibrate", "manten: calibrar", "maintenir: calibrer", "halten: kalibrieren"); }

void draw_chrome(void)
{
    if (!chrome_on()) return;
    if (s_mirror) s_mirror->release_hold();
    ext_lock();
    extdisp_chrome_draw(s_tc->dev, s_tc->keys, s_tc->frame, s_tc->status1, status2(), s_cfg.diag);
    ext_unlock();
}

void draw_key(int key, bool pressed)
{
    if (!chrome_on() || key == SK_NONE) return;
    ext_lock();
    extdisp_chrome_key(s_tc->dev, s_tc->keys, key, pressed, s_tc->status1, status2(), s_cfg.diag);
    ext_unlock();
}

// The calibration screen over the whole panel (step 4 = done). The mirror goes dark first, then the lock is
// taken: a frame already writing finishes before the paint, and nothing of the OS reaches the panel after it.
void draw_calib(int step, bool retry)
{
    const bool entering = !s_calibrating;
    s_calibrating = true;
    dark_sync();
    char l2[32];
    snprintf(l2, sizeof l2, TR5("Passo %d di 4", "Step %d of 4", "Paso %d de 4", "Etape %d sur 4", "Schritt %d von 4"), step + 1);
    const char *title = step >= 4 ? TR5("Touch calibrato", "Touch calibrated", "Tactil calibrado", "Tactile calibre", "Touch kalibriert")
                                  : TR5("Calibra il touch", "Calibrate touch", "Calibrar tactil", "Calibrer le tactile", "Touch kalibrieren");
    const char *instr = step >= 4 ? TR5("Pronto", "Ready", "Listo", "Pret", "Fertig")
                      : retry ? TR5("Non preciso: ricominciamo dal primo", "Not precise: again from the first",
                                    "Impreciso: otra vez desde el primero", "Imprecis : on recommence", "Ungenau: nochmal von vorn")
                              : TR5("Tocca il centro del bersaglio rosso", "Tap the centre of the red target",
                                    "Toca el centro del objetivo rojo", "Touchez le centre de la cible rouge", "Mitte des roten Ziels antippen");
    for (int pass = 0; pass < (entering ? 2 : 1); pass++) {
        // Entering: a write that tested "not dark" just before may still land after the first paint; the
        // second one, a frame later, covers it.
        if (pass) vTaskDelay(pdMS_TO_TICKS(80));
        lock_opt();
        extdisp_chrome_calib(s_tc->dev, step, retry, title, l2, instr);
        unlock_opt();
    }
}

// Calibration over: the mirror reaches the panel again, the letterbox keys come back, the OS repaints its frame.
void end_calib(void)
{
    s_calibrating = false;
    dark_sync();
    lock_opt();
    s_tc->dev->fillScreen(TFT_BLACK);
    unlock_opt();
    draw_chrome();
    s_repaint_req = true;
}

int med3(int a, int b, int c) { return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b)); }

// One sample: true = a finger is down and (rx, ry) is a median-filtered position. A lift during the reads
// (pressure gone by the second check) or a malformed frame counts as no contact.
bool touch_sample(int *rx, int *ry)
{
    bool on = xpt_pressure(xpt_read(s_touch, XPT_CMD_Z1), xpt_read(s_touch, XPT_CMD_Z2)) > XPT_Z_TOUCH;
    if (on) {
        int v[3], w[3];
        xpt_read(s_touch, XPT_CMD_X);                // the first conversion after switching drivers settles
        for (int i = 0; i < 3; i++) v[i] = xpt_read(s_touch, XPT_CMD_X);
        xpt_read(s_touch, XPT_CMD_Y);
        for (int i = 0; i < 3; i++) w[i] = xpt_read(s_touch, XPT_CMD_Y);
        *rx = med3(v[0], v[1], v[2]);
        *ry = med3(w[0], w[1], w[2]);
        on = *rx >= 0 && *ry >= 0 &&
             xpt_pressure(xpt_read(s_touch, XPT_CMD_Z1), xpt_read(s_touch, XPT_CMD_Z2)) > XPT_Z_TOUCH;
    }
    xpt_read(s_touch, XPT_CMD_IDLE);
    return on;
}

void inject(int ev)
{
    nucleo_key_t k = { NK_NONE, 0 };
    switch (ev) {
        case EXT_EV_UP:    k = { NK_UP, ';' }; break;
        case EXT_EV_DOWN:  k = { NK_DOWN, '.' }; break;
        case EXT_EV_LEFT:  k = { NK_LEFT, ',' }; break;
        case EXT_EV_RIGHT: k = { NK_RIGHT, '/' }; break;
        case EXT_EV_ENTER: k = { NK_ENTER, 0 }; break;
        case EXT_EV_BACK:  k = { NK_BACK, 0 }; break;
        case EXT_EV_MENU:  k = { NK_TAB, 0 }; break;          // Tab = the Control Center
        default: return;
    }
    nucleo_kbd_inject(k);
}

const char *ev_name(int ev)
{
    static const char *const N[] = { "-", "up", "down", "left", "right", "enter", "back", "menu", "calibrate" };
    return ev >= 0 && ev <= EXT_EV_CALIBRATE ? N[ev] : "?";
}

bool t_irq_idle(void) { return s_cfg.t_irq >= 0 && gpio_get_level((gpio_num_t)s_cfg.t_irq) == 1; }

// A panel point in the foreground screen's own coordinates: the panel's on the large surface, the compact
// 240x135 inside the mirrored frame. False outside the frame.
bool to_screen(bool solo, int px, int py, int *x, int *y)
{
    if (solo) { *x = px; *y = py - s_hud_h; return *y >= 0; }
    const extdisp_frame_t &f = s_tc->frame;
    if (f.fw <= 0 || f.fh <= 0) return false;
    const int fx = px - f.ox, fy = py - f.oy;
    if (fx < 0 || fy < 0 || fx >= f.fw || fy >= f.fh) return false;
    *x = fx * 240 / f.fw; *y = fy * 135 / f.fh;
    return true;
}

// Kinetic scrolling: the finger's travel in screen pixels becomes keys, one per area step; after a flick the
// same accumulator keeps being fed by a decaying velocity. Velocity in screen px per ms, x1000 (fixed point).
struct ScrollTrack {
    bool on;                       // a drag that began in a scroll area owns this touch
    TouchCtx::Scroll a;            // that area (copied: the app may republish mid-drag)
    int  last, acc;                // last finger y (screen px), travel not yet turned into keys
    int  lastx, accx;              // pad: the same along x
    int  hist_y[4]; uint32_t hist_t[4]; int hn;   // recent samples, for the release velocity
    int  vel, frac;                // fling velocity (px/ms x1000), 0 = none; sub-pixel carry
    uint32_t t_fling;
};
void scroll_emit(ScrollTrack &st, int dy)
{
    st.acc += dy;
    while (st.acc <= -st.a.step) { st.acc += st.a.step; if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ st.a.kf, st.a.cf }); }
    while (st.acc >=  st.a.step) { st.acc -= st.a.step; if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ st.a.kb, st.a.cb }); }
}

// A drag pad: the finger's travel on each axis becomes arrows in ITS direction (finger right = Right), one per step.
void pad_emit(ScrollTrack &st, int dx, int dy)
{
    st.accx += dx; st.acc += dy;
    while (st.accx >=  st.a.step) { st.accx -= st.a.step; if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ NK_RIGHT, '/' }); }
    while (st.accx <= -st.a.step) { st.accx += st.a.step; if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ NK_LEFT, ',' }); }
    while (st.acc  >=  st.a.step) { st.acc  -= st.a.step; if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ NK_DOWN, '.' }); }
    while (st.acc  <= -st.a.step) { st.acc  += st.a.step; if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ NK_UP, ';' }); }
}

void touch_task(void *)
{
    const int W = s_tc->dev->width(), H = s_tc->dev->height();
    extdisp_gesture_t g;
    extdisp_gesture_init(&g, s_tc->keys);
    extdisp_calib_t cal;
    bool calibrating = cal_on() && !s_tc->cal_stored;      // first use: calibrate before trusting the keys
    if (calibrating) { extdisp_calib_start(&cal); draw_calib(0, false); }
    int shown = SK_NONE, x0 = 0, y0 = 0;
    bool was_down = false, wake_hold = false, solo_seen = false;
    ScrollTrack sc = {};
    bool was_scroll = false;
    int64_t t_down = 0;
    for (;;) {
        if (s_cal_req) {                           // Settings / Control Center: "Calibrate touch"
            s_cal_req = false;
            if (cal_on() && !calibrating) {
                calibrating = true;
                extdisp_calib_start(&cal);
                draw_calib(0, false);
                shown = SK_NONE; was_down = false;
                continue;
            }
        }
        static int layout_seen = 0;
        if (s_solo != solo_seen || s_layout_gen != layout_seen) {   // surface or frame changed: re-lay the keys
            solo_seen = s_solo;
            layout_seen = s_layout_gen;
            extdisp_gesture_init(&g, solo_seen ? s_tc->keys_solo : s_tc->keys);
            shown = SK_NONE;
            was_down = false;
        }
        if (s_layout == 1 && !calibrating) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }   // Cardputer only
        const bool busy = g.down || wake_hold || sc.vel != 0 || (calibrating && (cal.down || cal.wait_release));
        vTaskDelay(pdMS_TO_TICKS(busy ? 20 : 40));
        int rx = 0, ry = 0;
        if (!s_screen_on || wake_hold) {
            // Screen off: a tap only wakes it (an injected Back, which the wake path consumes), and the same
            // finger is ignored until it lifts so it cannot also press whatever appears under it.
            if (!s_screen_on) vTaskDelay(pdMS_TO_TICKS(200));
            bool on = !t_irq_idle() && touch_sample(&rx, &ry);
            if (on && !wake_hold && !s_screen_on && s_touch_input) {
                nucleo_kbd_inject(nucleo_key_t{ NK_BACK, 0 });
                ESP_LOGW(TAG, "touch: wake");
            }
            wake_hold = on;
            if (!on) extdisp_gesture_init(&g, solo_seen ? s_tc->keys_solo : s_tc->keys);
            continue;
        }
        if (sc.vel != 0 && (t_irq_idle() || !touch_sample(&rx, &ry))) {   // a flick coasting, no finger on the glass
            const uint32_t nowf = (uint32_t)(esp_timer_get_time() / 1000);
            const int dt = (int)(nowf - sc.t_fling); sc.t_fling = nowf;
            sc.frac += sc.vel * dt;                  // x1000
            scroll_emit(sc, sc.frac / 1000); sc.frac %= 1000;
            sc.vel = sc.vel * 92 / 100;              // ~0.92 per 20 ms: a phone-like glide, ~0.5 s
            if (sc.vel > -40 && sc.vel < 40) sc.vel = 0;
            continue;
        }
        if (!busy && t_irq_idle()) continue;
        bool on = touch_sample(&rx, &ry);
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        if (calibrating) {
            int r = extdisp_calib_feed(&cal, now, on, rx, ry);
            if (r == CAL_NEXT) draw_calib(cal.step, false);
            else if (r == CAL_DONE) {
                extdisp_cal_t k;
                int err = -1;
                if (extdisp_calib_finish(&cal, W, H, &k, &err)) {
                    s_tc->cal = k;
                    s_tc->cal_stored = true;
                    s_tc->cal_err = err;
                    calibrating = false;
                    int packed[7];
                    extdisp_cal_pack(extdisp_lgfx_rotation(&s_cfg), &k, packed);
                    bool saved = nucleo_prefs_save_extdisp_cal(packed);
                    ESP_LOGW(TAG, "touch calibrated: check target off by %d px%s", err, saved ? "" : " (NOT saved)");
                    extdisp_gesture_init(&g, solo_seen ? s_tc->keys_solo : s_tc->keys);
                    shown = SK_NONE;
                    draw_calib(4, false);                // "done", for a moment
                    vTaskDelay(pdMS_TO_TICKS(900));
                    end_calib();
                } else {
                    ESP_LOGW(TAG, "touch calibration rejected (check target off by %d px): again", err);
                    extdisp_calib_start(&cal);
                    draw_calib(0, true);
                }
            }
            continue;
        }

        int sx = 0, sy = 0;
        if (on) extdisp_cal_apply(&s_tc->cal, rx, ry, W, H, &sx, &sy);
        int ev[EXT_MAX_EVENTS];
        const int n = extdisp_gesture_feed(&g, now, on, sx, sy, ev);
        if (g.down && !was_down) {
            t_down = esp_timer_get_time(); x0 = sx; y0 = sy;
            s_presses = s_presses + 1;
            // Does this touch start in a scroll area? Then the drag is the app's scroll, not the arrows.
            sc.vel = 0; sc.on = false;
            int ax, ay;
            if (to_screen(solo_seen, sx, sy, &ax, &ay)) {
                ext_lock();
                for (int t = 0; t < s_tc->nscr; t++) {
                    const TouchCtx::Scroll &a = s_tc->scr[t];
                    if (ax >= a.x && ax < a.x + a.w && ay >= a.y && ay < a.y + a.h) { sc.a = a; sc.on = true; break; }
                }
                ext_unlock();
                if (sc.on) { sc.last = ay; sc.acc = 0; sc.hn = 0; sc.lastx = ax; sc.accx = 0; }
            }
            if (s_cfg.diag) ESP_LOGW(TAG, "touch DOWN #%d raw x=%d y=%d -> %d,%d", s_presses, rx, ry, sx, sy);
        } else if (!g.down && was_down && s_cfg.diag) {
            ESP_LOGW(TAG, "touch UP after %d ms (moved %d,%d)", (int)((esp_timer_get_time() - t_down) / 1000),
                     s_last_x - x0, s_last_y - y0);
        }
        // Only a real contact sample moves the "where the finger is": the release is debounced over a few
        // EMPTY samples (sx, sy = 0 there), and a tap's meaning is read from this position after it lifts.
        if (g.down && on) { s_last_x = sx; s_last_y = sy; }
        if (sc.on && g.down && on) {                 // the finger moves the content, 1:1 (past the tap slop)
            const int ay = solo_seen ? sy - s_hud_h : (s_tc->frame.fh > 0 ? (sy - s_tc->frame.oy) * 135 / s_tc->frame.fh : sy);
            const int ax = solo_seen ? sx : (s_tc->frame.fw > 0 ? (sx - s_tc->frame.ox) * 240 / s_tc->frame.fw : sx);
            if (sc.a.pad) { if (g.moved) pad_emit(sc, ax - sc.lastx, ay - sc.last); }
            else if (g.moved) scroll_emit(sc, ay - sc.last);
            sc.last = ay; sc.lastx = ax;
            if (sc.hn == 4) { memmove(sc.hist_y, sc.hist_y + 1, sizeof(int) * 3); memmove(sc.hist_t, sc.hist_t + 1, sizeof(uint32_t) * 3); sc.hn = 3; }
            sc.hist_y[sc.hn] = ay; sc.hist_t[sc.hn] = now; sc.hn++;
        }
        if (sc.on && !g.down && was_down) {          // lift: a flick keeps coasting
            sc.on = false;
            if (!sc.a.pad && g.moved && sc.hn >= 2 && now - sc.hist_t[sc.hn - 1] < 80) {
                const int dt = (int)(sc.hist_t[sc.hn - 1] - sc.hist_t[0]);
                if (dt > 0) {
                    int v = (sc.hist_y[sc.hn - 1] - sc.hist_y[0]) * 1000 / dt;
                    if (v > 3000) v = 3000;
                    if (v < -3000) v = -3000;
                    if (v > 250 || v < -250) { sc.vel = v; sc.frac = 0; sc.t_fling = now; }
                }
            }
            sc.hn = 0;
        }
        was_down = g.down;
        if (g.pressed != shown) { draw_key(shown, false); draw_key(g.pressed, true); shown = g.pressed; }
        const bool scroll_touch = sc.on || (sc.vel != 0) || (!g.down && was_scroll);
        was_scroll = sc.on;
        for (int i = 0; i < n; i++) {
            if (s_cfg.diag) ESP_LOGW(TAG, "touch -> %s", ev_name(ev[i]));
            // A hold on a large screen's row or tile (even inside a scroll area): that screen's menu for it.
            if (solo_seen && ev[i] == EXT_EV_BACK && !g.moved && g.down) {
                const int ty = s_last_y - s_hud_h;
                bool row = false;
                ext_lock();
                for (int t = 0; t < s_tc->ntgt; t++) {
                    const TouchCtx::Target &r = s_tc->tgt[t];
                    if (s_last_x >= r.x && s_last_x < r.x + r.w && ty >= r.y && ty < r.y + r.h) {
                        row = r.key == NK_CHAR && (uint8_t)r.ch >= NUCLEO_TOUCH_ROW && (uint8_t)r.ch < NUCLEO_TOUCH_ROW + 16;
                        break;
                    }
                }
                ext_unlock();
                if (row) {
                    s_tap_x = s_last_x; s_tap_y = ty;
                    if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ NK_CHAR, (char)NUCLEO_TOUCH_HOLD });
                    continue;
                }
            }
            if (scroll_touch && ev[i] != EXT_EV_ENTER) continue;    // a scroll's drag / hold is not an arrow or Back
            if (scroll_touch && ev[i] == EXT_EV_ENTER && g.moved) continue;
            if (ev[i] == EXT_EV_CALIBRATE) {
                if (!cal_on()) continue;
                calibrating = true;
                extdisp_calib_start(&cal);        // waits for this finger to lift before target 1
                draw_calib(0, false);
                extdisp_gesture_init(&g, s_tc->keys);
                shown = SK_NONE;
                was_down = false;
                break;
            }
            // Large surface (the launcher home, 320x240): a tap means what is under the finger — the status bar
            // goes back up, the side icons step the carousel toward them, the centre opens. Drags/holds as usual.
            if (solo_seen && ev[i] == EXT_EV_ENTER && !g.moved) {
                const int hh = s_hud_h, ty = s_last_y - hh;   // the screen's own y: below the system bar
                if (hh && ty < 0) {                           // the system bar: Back
                    if (s_touch_input) nucleo_kbd_inject(nucleo_key_t{ NK_BACK, 0 });
                    continue;
                }
                nucleo_key_t hit = { NK_NONE, 0 };            // a large screen's own targets, in its pixels
                ext_lock();
                for (int t = 0; t < s_tc->ntgt; t++) {
                    const TouchCtx::Target &r = s_tc->tgt[t];
                    if (s_last_x >= r.x && s_last_x < r.x + r.w && ty >= r.y && ty < r.y + r.h) { hit = { (int)r.key, r.ch }; break; }
                }
                ext_unlock();
                if (hit.key != NK_NONE) {
                    if (s_cfg.diag) ESP_LOGW(TAG, "touch -> target key %d at %d,%d", hit.key, s_last_x, ty);
                    s_tap_x = s_last_x; s_tap_y = ty;
                    if (s_touch_input) nucleo_kbd_inject(hit);
                    continue;
                }
                const int pw = s_dev->width();
                if (s_last_y < 24) ev[i] = EXT_EV_BACK;
                else if (s_last_x < pw * 3 / 10) ev[i] = EXT_EV_LEFT;
                else if (s_last_x > pw * 7 / 10) ev[i] = EXT_EV_RIGHT;
            }
            // A compact screen's own targets: a tap on one delivers its key instead of Enter.
            if (!solo_seen && ev[i] == EXT_EV_ENTER && !g.moved && s_tc->frame.fw > 0 && s_tc->frame.fh > 0) {
                const int fx = s_last_x - s_tc->frame.ox, fy = s_last_y - s_tc->frame.oy;
                if (fx >= 0 && fy >= 0 && fx < s_tc->frame.fw && fy < s_tc->frame.fh) {
                    const int cx = fx * 240 / s_tc->frame.fw, cy = fy * 135 / s_tc->frame.fh;
                    nucleo_key_t hit = { NK_NONE, 0 };
                    ext_lock();
                    for (int t = 0; t < s_tc->ntgt; t++) {
                        const TouchCtx::Target &r = s_tc->tgt[t];
                        if (cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h) { hit = { (int)r.key, r.ch }; break; }
                    }
                    ext_unlock();
                    if (hit.key != NK_NONE) {
                        if (s_cfg.diag) ESP_LOGW(TAG, "touch -> target key %d '%c' at %d,%d", hit.key, hit.ch ? hit.ch : ' ', cx, cy);
                        s_tap_x = cx; s_tap_y = cy;
                        if (s_touch_input) nucleo_kbd_inject(hit);
                        continue;
                    }
                }
            }
            if (s_touch_input) inject(ev[i]);
        }
    }
}

// Called by attach() with the panel up and the mirror geometry known, BEFORE the mirror is installed:
// allocates the touch state and the external-panel lock. False = no memory (the mirror still works).
bool touch_prepare(lgfx::LGFX_Device *dev, Panel_Mirror *mirror)
{
    s_tc = new (std::nothrow) TouchCtx();
    if (!s_tc) return false;
    s_tc->dev = dev;
    s_tc->mux = xSemaphoreCreateRecursiveMutex();
    if (!s_tc->mux) { delete s_tc; s_tc = nullptr; return false; }
    s_tc->frame = { mirror->origin_x(), mirror->origin_y(), mirror->frame_w(), mirror->frame_h() };
    extdisp_sk_layout(dev->width(), dev->height(), s_tc->frame.ox, s_tc->frame.oy, s_tc->frame.fw, s_tc->frame.fh,
                      s_tc->keys);
    extdisp_sk_layout(dev->width(), dev->height(), 0, 0, dev->width(), dev->height(), s_tc->keys_solo);
    const int rot = extdisp_lgfx_rotation(&s_cfg);
    s_tc->cal_stored = s_has_cal_raw && extdisp_cal_unpack(s_cal_raw, rot, &s_tc->cal);
    if (!s_tc->cal_stored) extdisp_cal_default(rot, &s_tc->cal);
    s_tc->cal_err = -1;
    if (s_cfg.diag) snprintf(s_tc->status1, sizeof s_tc->status1, "%s%s %dMHz", extdisp_ctrl_str(s_ctrl),
                             s_ctrl_read ? "" : "?", s_cfg.mhz);
    else snprintf(s_tc->status1, sizeof s_tc->status1, "NucleoOS");
    mirror->set_ext_lock(ext_lock, ext_unlock);
    return true;
}

void start_touch(void)
{
    if (!s_tc) return;
    if (add_device(s_cfg.t_cs, 2000000, &s_touch) != ESP_OK) { ESP_LOGW(TAG, "touch: no SPI device"); return; }
    if (s_cfg.t_irq >= 0) {
        gpio_reset_pin((gpio_num_t)s_cfg.t_irq);
        gpio_set_direction((gpio_num_t)s_cfg.t_irq, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)s_cfg.t_irq, GPIO_PULLUP_ONLY);   // PENIRQ is open-drain, low = touched
    }
    // 6 KB: the calibration save runs cJSON + FATFS on this stack.
    if (xTaskCreate(touch_task, "extdisp_touch", 6144, nullptr, 2, nullptr) != pdPASS)
        ESP_LOGW(TAG, "touch: task not created (heap %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    else
        ESP_LOGW(TAG, "touch: %s, calibration %s (T_IRQ %s)", s_touch_input ? "input" : "log only",
                 s_tc->cal_stored ? "stored" : "pending", s_cfg.t_irq >= 0 ? "wired" : "polled");
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
        fprintf(f, "mirror        %dx%d at %d,%d on 320x240%s\n", s_mirror->frame_w(), s_mirror->frame_h(),
                s_mirror->origin_x(), s_mirror->origin_y(), s_mirror->fit() ? " (fit 4/3)" : " (1:1)");
    fprintf(f, "primary       %s\n", s_primary ? "external hosts the launcher (320x240)" : "mirror only");
    fprintf(f, "touch         %s, calibration %s\n", s_touch_input ? "input" : "log only",
            !s_tc ? "-" : s_tc->cal_stored ? "stored" : "pending (first use asks for it)");
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
    char mode[8] = "auto", ctrl[12] = "auto", layout[12] = "duplicate";
    int pins[6] = { s_cfg.cs, s_cfg.dc, s_cfg.rst, s_cfg.t_cs, s_cfg.t_irq, s_cfg.bl };
    nucleo_prefs_load_extdisp(mode, sizeof mode, ctrl, sizeof ctrl, &s_cfg.rotation, &s_cfg.mhz,
                              &s_cfg.mirror, &s_cfg.invert, &s_cfg.rgb, &s_cfg.diag, pins,
                              &s_touch_input, s_cal_raw, &s_has_cal_raw, &s_primary, &s_fit, layout, sizeof layout);
    s_layout = !strcmp(layout, "cardputer") ? 1 : !strcmp(layout, "external") ? 2 : 0;
    if (s_cfg.mhz > 40) s_cfg.mhz = 40;            // 80 MHz is gone (flicker / lost frames on real wiring): 40 tops
    nucleo_prefs_load_display_int("external_brightness", &s_bl_pct);
    s_bl_pct = s_bl_pct < 10 ? 10 : s_bl_pct > 100 ? 100 : s_bl_pct;
    s_cfg.mode = extdisp_mode_parse(mode);
    s_cfg.controller = extdisp_ctrl_parse(ctrl);
    s_saved = nucleo_extdisp_conf_t{ s_cfg.mode == EXTDISP_ON ? 1 : s_cfg.mode == EXTDISP_OFF ? 2 : 0, s_cfg.mhz,
                                     s_cfg.rotation, s_primary, s_fit, s_touch_input, s_layout };
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
    // Touch only with a panel up AND a touch controller that answered: otherwise no task, no lock, no state.
    if (s_dec.attach && s_dec.touch && (s_touch_input || s_cfg.diag) && touch_prepare(s_dev, s_mirror)) {
        draw_chrome();                              // soft keys replace the test card (input on)
        start_touch();
    }
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

// ---- primary surface (include/nucleo_surface.h) ------------------------------------------------------------
namespace {

} // namespace

extern "C" bool nucleo_surface_large_ok(void)
{
    return s_mirror && s_primary && s_layout != 1 && (!s_tc || (s_tc->cal_stored && !s_calibrating));
}

extern "C" bool nucleo_surface_large(void) { return s_solo; }

extern "C" bool nucleo_surface_set_large(bool on)
{
    if (on && !nucleo_surface_large_ok()) on = false;
    if (!s_mirror || on == s_solo) return false;
    lock_opt();                                   // the touch task never draws mid-switch
    if (!on && s_hud_h) { s_mirror->set_inset(0); s_hud_h = 0; }   // the system bar belongs to the large surface
    bool ok = s_mirror->set_solo(on);
    if (ok && !on) s_dev->fillScreen(TFT_BLACK);  // wipe the large screen; the caller repaints the centred frame
    unlock_opt();
    if (!ok) return false;
    s_solo = on;
    d.clearClipRect();                            // LGFXBase caches the clip box of the old size
    d.clearScrollRect();
    if (!on) draw_chrome();                       // large: the launcher paints its companion on the internal panel
    ESP_LOGI(TAG, "surface: %s", on ? "large (external 320x240)" : "compact (mirror)");
    return true;
}

// ---- second-screen settings (include/nucleo_extdisp.h) --------------------------------------------------------
extern "C" bool nucleo_extdisp_conf(nucleo_extdisp_conf_t *live, nucleo_extdisp_conf_t *saved)
{
    if (!nucleo_ui_is_adv()) return false;
    if (live) *live = nucleo_extdisp_conf_t{ s_cfg.mode == EXTDISP_ON ? 1 : s_cfg.mode == EXTDISP_OFF ? 2 : 0, s_cfg.mhz,
                                             s_cfg.rotation, s_primary, s_mirror ? s_mirror->fit() : s_fit, s_touch_input,
                                             s_layout };
    if (saved) *saved = s_saved;
    return true;
}

extern "C" bool nucleo_extdisp_set_layout(int layout)
{
    if (layout < 0 || layout > 2) return false;
    s_saved.layout = layout;
    bool ok = nucleo_prefs_save_display_str("external_layout", layout == 1 ? "cardputer" : layout == 2 ? "external" : "duplicate");
    if (!s_mirror || layout == s_layout) { s_layout = layout; return ok; }
    nucleo_surface_set_large(false);              // every mode starts compact; the launcher re-asks for large
    lock_opt();
    s_mirror->set_ext_off(layout == 1);
    s_mirror->set_pri_off(layout == 2);
    s_layout = layout;
    s_dev->fillScreen(TFT_BLACK);                 // Cardputer only: stays black; otherwise the caller repaints
    unlock_opt();
    d.clearClipRect();
    draw_chrome();
    return ok;
}

extern "C" bool nucleo_extdisp_set_primary(bool on)
{
    s_primary = on; s_saved.primary = on;
    return nucleo_prefs_save_display_bool("external_primary", on);   // the run loop follows on its next frame
}

extern "C" bool nucleo_extdisp_set_fit(bool on)
{
    s_fit = on; s_saved.fit = on;
    bool ok = nucleo_prefs_save_display_bool("external_fit", on);
    if (s_mirror && !s_solo && s_mirror->fit() != on) {
        lock_opt();
        if (s_mirror->set_fit(on)) {
            s_dev->fillScreen(TFT_BLACK);                            // the frame moves: wipe, the caller repaints it
            if (s_tc) {
                s_tc->frame = { s_mirror->origin_x(), s_mirror->origin_y(), s_mirror->frame_w(), s_mirror->frame_h() };
                extdisp_sk_layout(s_dev->width(), s_dev->height(), s_tc->frame.ox, s_tc->frame.oy, s_tc->frame.fw,
                                  s_tc->frame.fh, s_tc->keys);
            }
            s_layout_gen = s_layout_gen + 1;
        }
        unlock_opt();
        d.clearClipRect();
        draw_chrome();
    }
    return ok;
}

extern "C" bool nucleo_extdisp_set_mode(int mode)
{
    s_saved.mode = mode;
    return nucleo_prefs_save_display_str("external", mode == 1 ? "on" : mode == 2 ? "off" : "auto");
}
extern "C" bool nucleo_extdisp_set_mhz(int mhz)     { s_saved.mhz = mhz; return nucleo_prefs_save_display_int("external_mhz", mhz); }
// The panel's own backlight level (10..100 %), separate from the Cardputer's: live, saved when persist.
extern "C" bool nucleo_extdisp_brightness_ok(void) { return s_bl_pwm != nullptr; }
extern "C" int  nucleo_extdisp_brightness(void)    { return s_bl_pct; }
extern "C" bool nucleo_extdisp_set_brightness(int pct, bool persist)
{
    if (!s_bl_pwm) return false;
    pct = pct < 10 ? 10 : pct > 100 ? 100 : pct;
    s_bl_pct = pct;
    if (s_screen_on) s_bl_pwm->setBrightness((uint8_t)(pct * 255 / 100));
    return persist ? nucleo_prefs_save_display_int("external_brightness", pct) : true;
}
extern "C" bool nucleo_extdisp_set_rotation(int r)  { s_saved.rotation = r; return nucleo_prefs_save_display_int("external_rotation", r); }
extern "C" bool nucleo_extdisp_set_touch(bool on)   { s_saved.touch = on; return nucleo_prefs_save_display_bool("external_touch", on); }

extern "C" bool nucleo_extdisp_recalibrate(void)
{
    if (!s_tc || !s_tc->mux || !s_touch_input || s_layout == 1) return false;
    s_cal_req = true;
    return true;
}

extern "C" bool nucleo_extdisp_set_hud(const char *title, unsigned short color)
{
    if (!s_mirror || !s_solo) return false;
    const int h = title ? 30 : 0;
    if (h == s_hud_h && (!title || !strcmp(title, s_hud_title))) return false;
    lock_opt();
    bool ok = s_mirror->set_inset(h);
    if (ok) {
        s_hud_h = h;
        if (title) { snprintf(s_hud_title, sizeof s_hud_title, "%s", title); s_hud_col = color ? color : THEME_FG; draw_hud(); }
    }
    unlock_opt();
    if (!ok) return false;
    d.clearClipRect();                            // LGFXBase caches the clip box of the old size
    d.clearScrollRect();
    return true;
}

extern "C" bool nucleo_extdisp_take_repaint(void)
{
    if (!s_repaint_req) return false;
    s_repaint_req = false;
    return true;
}

// ---- screenshots ------------------------------------------------------------------------------------------------
extern "C" bool nucleo_extdisp_screen_size(int which, int *w, int *h)
{
    if (!s_mirror) return false;
    lgfx::IPanel *p = which ? (lgfx::IPanel *)s_dev->getPanel() : s_mirror->primary();
    if (which && s_solo) return false;
    *w = p->width(); *h = p->height();
    return true;
}

extern "C" bool nucleo_extdisp_screen_row(int which, int y, int w, uint16_t *out)
{
    if (!s_mirror) return false;
    if (!which) { if (w != s_mirror->primary()->width()) return false; s_mirror->primary()->startWrite();
                  s_mirror->pri_row(y, out); s_mirror->primary()->endWrite(); return true; }
    static uint16_t tmp[240];                     // one internal row (httpd task only)
    if (w != s_dev->width() || s_mirror->primary()->width() > 240) return false;
    s_mirror->primary()->startWrite();
    bool ok = s_mirror->ext_row(y, out, tmp);
    s_mirror->primary()->endWrite();
    return ok;
}

extern "C" bool nucleo_extdisp_draw_internal(void (*fn)(void *gfx))
{
    if (!s_mirror || !s_solo || s_layout == 2 || !fn) return false;
    lgfx::LGFX_Device g;
    g.setPanel(static_cast<lgfx::Panel_Device *>(s_mirror->primary()));   // the internal ST7789 (a Panel_Device)
    g.clearClipRect();                            // a fresh LGFXBase starts with an EMPTY clip box: nothing would draw
    g.clearScrollRect();
    fn(&g);
    return true;
}

// ---- touch targets (include/nucleo_touch.h) ----------------------------------------------------------------
extern "C" bool nucleo_touch_last(int *x, int *y)
{
    if (s_tap_x < 0) return false;
    if (x) *x = s_tap_x;
    if (y) *y = s_tap_y;
    return true;
}
extern "C" void nucleo_touch_clear(void) { if (s_tc) { s_tc->ntgt_build = 0; s_tc->nscr_build = 0; } }

extern "C" void nucleo_touch_add_scroll(int x, int y, int w, int h, int step, int key_back, char ch_back, int key_fwd, char ch_fwd)
{
    if (!s_tc || s_tc->nscr_build >= NUCLEO_TOUCH_SCROLL_MAX || w <= 0 || h <= 0 || step <= 0) return;
    s_tc->scr_build[s_tc->nscr_build++] = TouchCtx::Scroll{ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)step,
                                                            (int16_t)key_back, (int16_t)key_fwd, ch_back, ch_fwd, false };
}

extern "C" void nucleo_touch_add_pad(int x, int y, int w, int h, int step)
{
    if (!s_tc || s_tc->nscr_build >= NUCLEO_TOUCH_SCROLL_MAX || w <= 0 || h <= 0 || step <= 0) return;
    s_tc->scr_build[s_tc->nscr_build++] = TouchCtx::Scroll{ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)step,
                                                            0, 0, 0, 0, true };
}

extern "C" void nucleo_touch_add(int x, int y, int w, int h, int key, char ch)
{
    if (!s_tc || s_tc->ntgt_build >= NUCLEO_TOUCH_MAX || w <= 0 || h <= 0) return;
    s_tc->tgt_build[s_tc->ntgt_build++] = TouchCtx::Target{ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)key, ch };
}

extern "C" void nucleo_touch_commit(void)
{
    if (!s_tc || !s_tc->mux) return;
    ext_lock();                                   // the touch task never sees a half-copied set
    memcpy(s_tc->tgt, s_tc->tgt_build, sizeof s_tc->tgt);
    s_tc->ntgt = s_tc->ntgt_build;
    memcpy(s_tc->scr, s_tc->scr_build, sizeof s_tc->scr);
    s_tc->nscr = s_tc->nscr_build;
    ext_unlock();
}

extern "C" void nucleo_extdisp_frame_hold(bool on) { if (s_mirror) s_mirror->hold_enable(on); }

extern "C" void *nucleo_extdisp_internal_gfx(void)
{
    static lgfx::LGFX_Device *s_igfx = nullptr;     // ~200 B, created on first use (only with a panel attached)
    if (!s_mirror || !s_solo || s_layout == 2) return nullptr;
    if (!s_igfx) {
        s_igfx = new (std::nothrow) lgfx::LGFX_Device();
        if (!s_igfx) return nullptr;
        s_igfx->setPanel(static_cast<lgfx::Panel_Device *>(s_mirror->primary()));
    }
    s_igfx->clearClipRect();
    s_igfx->clearScrollRect();
    return s_igfx;
}
