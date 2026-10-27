// See extdisp_probe.h. Pure C: no ESP-IDF, no LovyanGFX (the host gate compiles this file unchanged).
#include "extdisp_probe.h"
#include <stddef.h>
#include <string.h>

void extdisp_cfg_default(extdisp_cfg_t *c)
{
    c->mode = EXTDISP_AUTO;
    c->controller = EXTDISP_CTRL_AUTO;
    c->rotation = 1;
    c->mirror = false;
    c->mhz = 27;
    c->invert = -1;
    c->rgb = -1;
    c->diag = true;
    c->cs = 5; c->dc = 15; c->rst = 13;
    c->t_cs = 6; c->t_irq = 4;
    c->bl = -1;
}

extdisp_mode_t extdisp_mode_parse(const char *s)
{
    if (s && !strcmp(s, "off")) return EXTDISP_OFF;
    if (s && !strcmp(s, "on"))  return EXTDISP_ON;
    return EXTDISP_AUTO;
}

const char *extdisp_mode_str(extdisp_mode_t m)
{
    return m == EXTDISP_OFF ? "off" : m == EXTDISP_ON ? "on" : "auto";
}

// The Cardputer ADV EXT 2x7 header's GPIOs that are not already a system bus: RESET/G3, INT/G4, CS/G5,
// BUSY/G6, UART_TX/G13, UART_RX/G15. (SCK 40 / MOSI 14 / MISO 39 are the SD bus the display joins;
// SDA 8 / SCL 9 are the keyboard + codec + IMU bus.)
static bool ext_pin(int p) { return p == 3 || p == 4 || p == 5 || p == 6 || p == 13 || p == 15; }

const char *extdisp_cfg_check(const extdisp_cfg_t *c)
{
    if (c->rotation != 1 && c->rotation != 3) return "rotation must be 1 or 3";
    if (c->mhz < 1 || c->mhz > 80) return "mhz out of range (1..80)";
    if (!ext_pin(c->cs)) return "cs is not a free EXT pin";
    if (!ext_pin(c->dc)) return "dc is not a free EXT pin";
    const int opt[4] = { c->rst, c->t_cs, c->t_irq, c->bl };
    for (int i = 0; i < 4; i++)
        if (opt[i] != -1 && !ext_pin(opt[i])) return "optional pin is not a free EXT pin (or -1)";
    const int all[6] = { c->cs, c->dc, c->rst, c->t_cs, c->t_irq, c->bl };
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (all[i] != -1 && all[i] == all[j]) return "two roles share one pin";
    return NULL;
}

int xpt_sample(const uint8_t rx[3])
{
    if (rx[1] & 0x80) return -1;      // the busy/null bit is always 0
    if (rx[2] & 0x07) return -1;      // trailing bits after D0 are always 0
    return (int)((((unsigned)rx[1] << 8) | rx[2]) >> 3);
}

// TEMP0 is a diode at ~0.6 V: ~980 LSB on the 2.5 V internal reference, ~750 if the module ties VREF to
// 3.3 V. TEMP1 is the same diode at 91x the current, ~140 mV higher: +170..+230 LSB. The windows are
// generous (clones, temperature) but a dead bus (0 / 4095 / identical values) never fits them.
bool xpt_fingerprint(int temp0, int temp1)
{
    if (temp0 < 200 || temp0 > 3500) return false;
    if (temp1 < 0 || temp1 > 4000) return false;
    int d = temp1 - temp0;
    return d >= 20 && d <= 1000;
}

extdisp_decision_t extdisp_decide(const extdisp_cfg_t *c, const extdisp_probe_t *p)
{
    extdisp_decision_t r = { false, false, "" };
    if (c->mode == EXTDISP_OFF) { r.reason = "disabled in settings"; return r; }
    if (!p->is_adv) { r.reason = "original Cardputer: no EXT header (those GPIOs scan the keyboard)"; return r; }
    const char *bad = extdisp_cfg_check(c);
    if (bad) { r.reason = bad; return r; }
    r.touch = p->touch_found;
    if (c->mode == EXTDISP_ON) {
        r.attach = true;
        r.reason = p->touch_found ? "forced on (touch answered)" : "forced on (no touch fingerprint)";
        return r;
    }
    if (c->t_cs < 0) { r.reason = "auto needs t_cs to detect the module (set mode on)"; return r; }
    if (p->t_cs_busy) { r.reason = "t_cs is driven by another module: not probing"; return r; }
    if (!p->touch_found) { r.reason = "no display found"; return r; }
    if (p->dc_busy) { r.reason = "touch found but dc is driven by another module"; r.touch = false; return r; }
    r.attach = true;
    r.reason = "detected (XPT2046 fingerprint)";
    return r;
}

extdisp_ctrl_t extdisp_ctrl_parse(const char *s)
{
    if (s && !strcmp(s, "ili9341")) return EXTDISP_CTRL_ILI9341;
    if (s && !strcmp(s, "st7789"))  return EXTDISP_CTRL_ST7789;
    return EXTDISP_CTRL_AUTO;
}

const char *extdisp_ctrl_str(extdisp_ctrl_t c)
{
    return c == EXTDISP_CTRL_ILI9341 ? "ili9341" : c == EXTDISP_CTRL_ST7789 ? "st7789" : "auto";
}

extdisp_ctrl_t extdisp_identify(const uint8_t rd_d3[4], const uint8_t rd_04[4])
{
    if (rd_d3[2] == 0x93 && rd_d3[3] == 0x41) return EXTDISP_CTRL_ILI9341;
    uint32_t v = ((uint32_t)rd_04[0] << 24) | ((uint32_t)rd_04[1] << 16) | ((uint32_t)rd_04[2] << 8) | rd_04[3];
    uint32_t id = (v >> 7) & 0xFFFFFFu;               // drop the leading dummy bit
    if ((id >> 16) == 0x85 && (id & 0xFF) == 0x52) return EXTDISP_CTRL_ST7789;
    return EXTDISP_CTRL_AUTO;
}

void extdisp_panel_flags(const extdisp_cfg_t *c, extdisp_ctrl_t ctrl, bool *invert, bool *rgb_order)
{
    *invert = c->invert >= 0 ? c->invert != 0 : ctrl == EXTDISP_CTRL_ST7789;
    *rgb_order = c->rgb >= 0 ? c->rgb != 0 : false;
}

int extdisp_lgfx_rotation(const extdisp_cfg_t *c)
{
    return c->rotation | (c->mirror ? 4 : 0);
}

// Typical red-module touch panels report ~200..3900 on both axes; X runs along the 240-px side and Y
// along the 320-px side of the portrait glass. Landscape rotation 1 puts raw Y on screen X; 3 mirrors it.
void extdisp_touch_map(int raw_x, int raw_y, int rotation, int *sx, int *sy)
{
    const int lo = 200, hi = 3900;
    int u = (raw_y - lo) * 320 / (hi - lo), v = (raw_x - lo) * 240 / (hi - lo);
    if ((rotation & 3) == 3) { u = 319 - u; v = 239 - v; }
    if (rotation & 4) u = 319 - u;
    *sx = u < 0 ? 0 : u > 319 ? 319 : u;
    *sy = v < 0 ? 0 : v > 239 ? 239 : v;
}

int xpt_pressure(int z1, int z2)
{
    if (z1 <= 0 || z2 < 0) return 0;
    int z = z1 + 4095 - z2;
    return z < 0 ? 0 : z;
}

void extdisp_mirror_origin(int panel_w, int panel_h, int src_w, int src_h, int *ox, int *oy)
{
    int x = (panel_w - src_w) / 2, y = (panel_h - src_h) / 2;
    *ox = x < 0 ? 0 : x;
    *oy = y < 0 ? 0 : y;
}
