// External display (ILI9341 + XPT2046 on the Cardputer ADV EXT header) — the PURE half: config, pin
// validation, the XPT2046 fingerprint and the attach decision. No ESP-IDF, no LovyanGFX: compiled as-is
// by the host gate (tools/extdisp-host) so every rule below is tested on the PC, never by flashing.
// The hardware half (GPIO sensing, SPI transfers, panel bring-up) lives in nucleo_extdisp.cpp.
// See docs/external-display.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { EXTDISP_OFF = 0, EXTDISP_AUTO = 1, EXTDISP_ON = 2 } extdisp_mode_t;

// The red "2.8\" TFT SPI 240x320" module (V1.1, V1.2, ...) ships with EITHER controller under the same
// silkscreen: the revision label does not tell them apart. Both are 240x320 portrait-native.
typedef enum { EXTDISP_CTRL_AUTO = 0, EXTDISP_CTRL_ILI9341 = 1, EXTDISP_CTRL_ST7789 = 2 } extdisp_ctrl_t;

typedef struct {
    extdisp_mode_t mode;
    extdisp_ctrl_t controller;  // AUTO = read the ID over SDO if wired, else assume ILI9341 (most common)
    int rotation;           // landscape: 1 or 3 (180 deg flip for a module mounted upside down)
    bool mirror;            // left-right flip for a panel whose source scan runs the other way (text reads mirrored)
    int mhz;                // SPI write clock; jumper wires rarely survive 40 MHz
    int invert;             // -1 = controller default, 0/1 = force (colours look like a photo negative -> flip)
    int rgb;                // -1 = controller default (BGR), 0/1 = force (red and blue swapped -> flip)
    bool diag;              // tester diagnostics: touch event log + per-boot report on the SD
    int cs, dc, rst;        // LCD: CS + DC required, RST optional (-1 = tied to EN / 3V3)
    int t_cs, t_irq;        // touch: T_CS optional (-1 = no auto-detect), T_IRQ optional (polled when -1)
    int bl;                 // backlight: -1 = LED wired to a supply rail (always on)
} extdisp_cfg_t;

extdisp_ctrl_t extdisp_ctrl_parse(const char *s);       // "ili9341" | "st7789" | anything else -> AUTO
const char *extdisp_ctrl_str(extdisp_ctrl_t c);

// Identify the controller from two reads done with SDO wired (both all-0xFF or all-0x00 when it is not):
//   rd_d3[4]  = the 4 bytes clocked after command 0xD3 (ILI9341 "Read ID4": dummy, 0x00, 0x93, 0x41)
//   rd_04[4]  = the 4 bytes clocked after command 0x04 (RDDID: 1 dummy BIT, then ID1 ID2 ID3;
//               ST7789V answers 0x85 0x85 0x52)
// Returns ILI9341, ST7789 or AUTO (= could not tell).
extdisp_ctrl_t extdisp_identify(const uint8_t rd_d3[4], const uint8_t rd_04[4]);

// The panel flags actually used: the controller's default unless the config forces one.
// ILI9341 modules: invert off. ST7789V modules: invert on (the IPS default; TN batches need invert=0).
// Both default to BGR order.
void extdisp_panel_flags(const extdisp_cfg_t *c, extdisp_ctrl_t ctrl, bool *invert, bool *rgb_order);

// The LovyanGFX rotation to set: the configured 1/3, plus 4 (lgfx's flip flag = a left-right mirror in
// landscape) when `mirror` is on.
int extdisp_lgfx_rotation(const extdisp_cfg_t *c);

// Approximate raw XPT2046 -> 320x240 landscape position for the diagnostics log only (phase 3 adds real
// calibration). raw_x/raw_y 0..4095; rotation = extdisp_lgfx_rotation() (bit 4 mirrors x like the image).
void extdisp_touch_map(int raw_x, int raw_y, int rotation, int *sx, int *sy);
#define XPT_CMD_X      0xD0   // differential, 12-bit, PENIRQ stays armed between conversions
#define XPT_CMD_Y      0x90
#define XPT_CMD_Z1     0xB0
#define XPT_CMD_Z2     0xC0
// Pressure from the two Z reads: 0 = no touch. Above XPT_Z_TOUCH counts as a press.
int xpt_pressure(int z1, int z2);
#define XPT_Z_TOUCH    300

// Defaults = the documented wiring (docs/external-display.md): LCD on the same EXT pins as the known-good
// community build (CS 5 / DC 15 / RST 13), touch CS on G6, IRQ on G4, LED on the 5VOUT rail.
void extdisp_cfg_default(extdisp_cfg_t *c);

// "off" | "auto" | "on" (case-sensitive, as settings.json writes them). Anything else -> AUTO.
extdisp_mode_t extdisp_mode_parse(const char *s);
const char *extdisp_mode_str(extdisp_mode_t m);

// Every pin must be one of the ADV EXT header's free GPIOs (3, 4, 5, 6, 13, 15) or -1 where optional, and
// no two roles may share a pin. The shared SPI lines (40/14/39), the SD CS (12), the I2C bus (8/9 = keyboard,
// codec, IMU) and everything else on the board are refused: a typo in settings.json must never be able to
// drive the SD card's chip-select or the keyboard bus. Returns NULL if valid, else a short reason.
const char *extdisp_cfg_check(const extdisp_cfg_t *c);

// XPT2046 / ADS7846 control bytes used by the probe (single-ended, 12-bit, internal reference + ADC on).
#define XPT_CMD_TEMP0  0x87
#define XPT_CMD_TEMP1  0xF7
#define XPT_CMD_IDLE   0x80   // power down between conversions, PENIRQ enabled (the chip's resting state)

// One 3-byte full-duplex exchange [cmd,0,0] -> rx[0..2]. Returns the 12-bit sample, or -1 when the frame
// is not one an XPT2046 can produce: the busy bit (rx[1] bit 7) is always 0 and the 3 bits after D0
// (rx[2] bits 2..0) are always 0. A floating or pulled-up MISO (no chip, or a deselected SD card) reads
// 0xFF and fails this.
int xpt_sample(const uint8_t rx[3]);

// The fingerprint: both temperature diodes read in range and TEMP1 (91x bias current) sits above TEMP0.
// A bus with nothing on it (all 0x00 or all 0xFF) can't fake that relation. Values are the raw 12-bit
// samples (or -1 from xpt_sample).
bool xpt_fingerprint(int temp0, int temp1);

typedef struct {
    bool is_adv;            // only the ADV has the EXT header: on the original these GPIOs scan the keyboard
    bool t_cs_busy;         // T_CS pin read LOW with the internal pull-up: something drives it (e.g. a LoRa BUSY)
    bool dc_busy;           // DC pin read HIGH with the internal pull-down: something drives it (e.g. a UART TX)
    bool touch_found;       // xpt_fingerprint() passed
} extdisp_probe_t;

typedef struct {
    bool attach;            // bring the LCD up and mirror onto it
    bool touch;             // an XPT2046 answered (phase 3 uses it; phase 1 only reports it)
    const char *reason;     // one line for the log / /api/status
} extdisp_decision_t;

// The single place that decides. OFF never touches a pin; the original Cardputer is refused even in ON
// mode; AUTO attaches only on a positive touch fingerprint and backs off if a pin looks owned by another
// module. ON (the user's explicit word) attaches without a fingerprint — the way to run a module with no
// touch wired — but still refuses invalid pins.
extdisp_decision_t extdisp_decide(const extdisp_cfg_t *c, const extdisp_probe_t *p);

// Where the 240x135 internal frame lands on the 320x240 panel: centred, 1:1 (no scaling at panel level).
void extdisp_mirror_origin(int panel_w, int panel_h, int src_w, int src_h, int *ox, int *oy);

#ifdef __cplusplus
}
#endif
