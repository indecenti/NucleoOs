// External display touch (phase 3) — the PURE half: calibration maths, the soft-key layout in the letterbox,
// the gesture machine that turns touch samples into nav events, and the 4-target calibration flow.
// No ESP-IDF, no LovyanGFX: compiled as-is by the host gate (tools/extdisp-host). The hardware half
// (XPT2046 sampling, key injection, drawing, persistence) lives in nucleo_extdisp.cpp.
// See docs/external-display.md, "Touch".
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- calibration: raw XPT2046 (0..4095) -> panel pixel, an affine map in 16.16 fixed point ----------
//   sx = (a*rx + b*ry + c) >> 16,  sy = (d*rx + e*ry + f) >> 16
// Affine (not just scale + offset) so it absorbs axis swap, mirroring and a slightly rotated glass.
typedef struct { int32_t a, b, c, d, e, f; } extdisp_cal_t;

// The uncalibrated map for an lgfx rotation (1/3, +4 = mirrored), equal to extdisp_touch_map().
void extdisp_cal_default(int rotation, extdisp_cal_t *cal);
// Exact solve from three raw -> screen pairs. False when the raw points are (nearly) collinear.
bool extdisp_cal_solve(const int raw[3][2], const int scr[3][2], extdisp_cal_t *cal);
// Map a raw sample, clamped to the w x h panel.
void extdisp_cal_apply(const extdisp_cal_t *cal, int rx, int ry, int w, int h, int *sx, int *sy);
// Distance (max of |dx|, |dy|, in pixels, unclamped) between where `raw` maps and where it should land.
int extdisp_cal_error(const extdisp_cal_t *cal, const int raw[2], const int scr[2]);
// Persisted form (settings.json "display.external_touch_cal" = [rotation, a, b, c, d, e, f]): a stored
// calibration only applies to the rotation it was taken in.
void extdisp_cal_pack(int rotation, const extdisp_cal_t *cal, int out[7]);
bool extdisp_cal_unpack(const int in[7], int rotation, extdisp_cal_t *cal);

// ---- soft keys in the letterbox around the mirrored frame ---------------------------------------------
// Top band:    [ ESC ] [   status (hold 3 s = recalibrate)   ] [ MENU ]
// Bottom band: [ < ] [ ^ ] [ OK ] [ v ] [ > ]
// Everything between the bands (the frame and its side strips) is the gesture area.
enum { SK_NONE = 0, SK_ESC, SK_MENU, SK_LEFT, SK_UP, SK_OK, SK_DOWN, SK_RIGHT, SK_STATUS, SK_FRAME, SK_COUNT };
typedef struct { int16_t x, y, w, h; } extdisp_rect_t;

// pw x ph panel, the mirrored frame at ox,oy (fw x fh). Bands thinner than 24 px get no keys (SK_FRAME).
void extdisp_sk_layout(int pw, int ph, int ox, int oy, int fw, int fh, extdisp_rect_t r[SK_COUNT]);
int extdisp_sk_hit(const extdisp_rect_t r[SK_COUNT], int x, int y);

// ---- gestures -> nav events -----------------------------------------------------------------------------
// Soft keys: arrows fire on press and auto-repeat while held; OK / ESC / MENU fire on release, only if the
// finger is still on the key (slide off = cancel). On the frame: tap = Enter, drag = arrows (natural
// scrolling: drag up = Down, drag left = Right, one step per EXT_DRAG_STEP px), long press = Back. Holding
// the status area for EXT_CAL_HOLD_MS asks for a recalibration.
enum { EXT_EV_NONE = 0, EXT_EV_UP, EXT_EV_DOWN, EXT_EV_LEFT, EXT_EV_RIGHT, EXT_EV_ENTER, EXT_EV_BACK, EXT_EV_MENU,
       EXT_EV_CALIBRATE };
#define EXT_TAP_SLOP      12     // px a tap may wander before it becomes a drag
#define EXT_DRAG_STEP     28     // px of drag per arrow event
#define EXT_LONG_MS       600    // frame hold -> Back
#define EXT_CAL_HOLD_MS   3000   // status hold -> recalibrate
#define EXT_REPEAT_DELAY  450    // arrow key: first auto-repeat
#define EXT_REPEAT_EVERY  120    // ...then every
#define EXT_RELEASE_N     2      // consecutive no-contact samples that end a touch (resistive panels flicker)
#define EXT_MAX_EVENTS    4

typedef struct {
    extdisp_rect_t r[SK_COUNT];
    bool down;                  // debounced contact
    int up_n;                   // no-contact samples seen while down
    int key;                    // SK_* under the first contact
    int pressed;                // SK_* to draw highlighted right now (SK_NONE when none)
    int x0, y0, ax, ay;         // first contact, drag anchor
    uint32_t t0, t_rep;         // contact start, next auto-repeat
    bool moved, long_fired;
} extdisp_gesture_t;

void extdisp_gesture_init(extdisp_gesture_t *g, const extdisp_rect_t r[SK_COUNT]);
// One sample (`touch` = contact, x/y in panel pixels, ignored without contact). Writes up to
// EXT_MAX_EVENTS EXT_EV_* codes to ev[] and returns how many.
int extdisp_gesture_feed(extdisp_gesture_t *g, uint32_t now_ms, bool touch, int x, int y, int ev[EXT_MAX_EVENTS]);

// ---- the calibration flow: four targets near the corners, all inside the letterbox --------------------
#define EXT_CAL_INSET     20     // target distance from the panel edges
#define EXT_CAL_SETTLE_MS 60     // ignore the first samples of a press (the film is still settling)
#define EXT_CAL_HOLD_N    6      // samples averaged per target
#define EXT_CAL_MAX_ERR   20     // px: the 4th target checks the solve made from the first three
enum { CAL_BUSY = 0, CAL_NEXT, CAL_DONE };

typedef struct {
    int step;                   // target being collected, 0..3 (4 = all collected)
    int n, up_n;
    long sx, sy;
    uint32_t t0;
    bool down, wait_release;
    int raw[4][2];
} extdisp_calib_t;

void extdisp_cal_targets(int pw, int ph, int t[4][2]);
void extdisp_calib_start(extdisp_calib_t *c);
// Feed raw samples. CAL_NEXT = a target was captured (draw the next one), CAL_DONE = all four captured.
int extdisp_calib_feed(extdisp_calib_t *c, uint32_t now_ms, bool touch, int rx, int ry);
// After CAL_DONE: solve from targets 0..2, verify with target 3. False = retry.
bool extdisp_calib_finish(const extdisp_calib_t *c, int pw, int ph, extdisp_cal_t *cal, int *err_px);

#ifdef __cplusplus
}
#endif
