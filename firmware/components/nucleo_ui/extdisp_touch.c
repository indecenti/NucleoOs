// See extdisp_touch.h. Pure C: no ESP-IDF, no LovyanGFX (the host gate compiles this file unchanged).
#include "extdisp_touch.h"
#include "extdisp_probe.h"
#include <stddef.h>
#include <string.h>

static int iabs(int v) { return v < 0 ? -v : v; }
static int32_t fx(double v) { return (int32_t)(v * 65536.0 + (v < 0 ? -0.5 : 0.5)); }

// ---- calibration ------------------------------------------------------------------------------------------

void extdisp_cal_default(int rotation, extdisp_cal_t *cal)
{
    // Three raw points well inside the range, mapped by the approximate phase-1 rule (none of them clamps).
    const int raw[3][2] = { { 500, 500 }, { 3600, 500 }, { 500, 3600 } };
    int scr[3][2];
    for (int i = 0; i < 3; i++) extdisp_touch_map(raw[i][0], raw[i][1], rotation, &scr[i][0], &scr[i][1]);
    extdisp_cal_solve(raw, scr, cal);
}

bool extdisp_cal_solve(const int raw[3][2], const int scr[3][2], extdisp_cal_t *cal)
{
    const double x0 = raw[0][0], y0 = raw[0][1], x1 = raw[1][0], y1 = raw[1][1], x2 = raw[2][0], y2 = raw[2][1];
    const double det = x0 * (y1 - y2) - y0 * (x1 - x2) + (x1 * y2 - x2 * y1);
    if (det > -100000.0 && det < 100000.0) return false;           // collinear or too close together
    double k[2][3];
    for (int axis = 0; axis < 2; axis++) {                          // Cramer's rule, once per screen axis
        const double s0 = scr[0][axis], s1 = scr[1][axis], s2 = scr[2][axis];
        k[axis][0] = (s0 * (y1 - y2) - y0 * (s1 - s2) + (s1 * y2 - s2 * y1)) / det;
        k[axis][1] = (x0 * (s1 - s2) - s0 * (x1 - x2) + (x1 * s2 - x2 * s1)) / det;
        k[axis][2] = (x0 * (y1 * s2 - y2 * s1) - y0 * (x1 * s2 - x2 * s1) + s0 * (x1 * y2 - x2 * y1)) / det;
    }
    cal->a = fx(k[0][0]); cal->b = fx(k[0][1]); cal->c = fx(k[0][2]);
    cal->d = fx(k[1][0]); cal->e = fx(k[1][1]); cal->f = fx(k[1][2]);
    return true;
}

static void cal_map(const extdisp_cal_t *cal, int rx, int ry, int *sx, int *sy)
{
    *sx = (int)(((int64_t)cal->a * rx + (int64_t)cal->b * ry + cal->c + 32768) >> 16);
    *sy = (int)(((int64_t)cal->d * rx + (int64_t)cal->e * ry + cal->f + 32768) >> 16);
}

void extdisp_cal_apply(const extdisp_cal_t *cal, int rx, int ry, int w, int h, int *sx, int *sy)
{
    int x, y;
    cal_map(cal, rx, ry, &x, &y);
    *sx = x < 0 ? 0 : x >= w ? w - 1 : x;
    *sy = y < 0 ? 0 : y >= h ? h - 1 : y;
}

int extdisp_cal_error(const extdisp_cal_t *cal, const int raw[2], const int scr[2])
{
    int x, y;
    cal_map(cal, raw[0], raw[1], &x, &y);
    int dx = iabs(x - scr[0]), dy = iabs(y - scr[1]);
    return dx > dy ? dx : dy;
}

void extdisp_cal_pack(int rotation, const extdisp_cal_t *cal, int out[7])
{
    out[0] = rotation;
    out[1] = cal->a; out[2] = cal->b; out[3] = cal->c;
    out[4] = cal->d; out[5] = cal->e; out[6] = cal->f;
}

bool extdisp_cal_unpack(const int in[7], int rotation, extdisp_cal_t *cal)
{
    if (in[0] != rotation) return false;                            // taken in another orientation
    // Each screen axis must depend on the raw sample: a zeroed row means a hand-edited / corrupt entry.
    if ((in[1] == 0 && in[2] == 0) || (in[4] == 0 && in[5] == 0)) return false;
    cal->a = in[1]; cal->b = in[2]; cal->c = in[3];
    cal->d = in[4]; cal->e = in[5]; cal->f = in[6];
    return true;
}

// ---- soft keys ---------------------------------------------------------------------------------------------

static extdisp_rect_t rect(int x, int y, int w, int h)
{
    extdisp_rect_t r = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
    return r;
}

void extdisp_sk_layout(int pw, int ph, int ox, int oy, int fw, int fh, extdisp_rect_t r[SK_COUNT])
{
    memset(r, 0, sizeof(extdisp_rect_t) * SK_COUNT);
    (void)fw;
    if (oy >= 24) {
        const int side = pw / 4;
        r[SK_ESC] = rect(0, 0, side, oy);
        r[SK_STATUS] = rect(side, 0, pw - 2 * side, oy);
        r[SK_MENU] = rect(pw - side, 0, side, oy);
    }
    const int by = oy + fh, bh = ph - by;
    if (bh >= 24) {
        static const int order[5] = { SK_LEFT, SK_UP, SK_OK, SK_DOWN, SK_RIGHT };
        const int kw = pw / 5;
        for (int i = 0; i < 5; i++) r[order[i]] = rect(i * kw, by, i == 4 ? pw - 4 * kw : kw, bh);
    }
    r[SK_FRAME] = rect(0, oy, pw, fh);
    (void)ox;
}

int extdisp_sk_hit(const extdisp_rect_t r[SK_COUNT], int x, int y)
{
    for (int k = SK_ESC; k <= SK_STATUS; k++)
        if (r[k].w > 0 && x >= r[k].x && x < r[k].x + r[k].w && y >= r[k].y && y < r[k].y + r[k].h) return k;
    return SK_FRAME;                                                // frame, side strips, bands too thin for keys
}

// ---- gestures ----------------------------------------------------------------------------------------------

void extdisp_gesture_init(extdisp_gesture_t *g, const extdisp_rect_t r[SK_COUNT])
{
    memset(g, 0, sizeof *g);
    memcpy(g->r, r, sizeof g->r);
}

static bool is_arrow(int k) { return k == SK_LEFT || k == SK_UP || k == SK_DOWN || k == SK_RIGHT; }
static int key_event(int k)
{
    switch (k) {
        case SK_LEFT:  return EXT_EV_LEFT;
        case SK_UP:    return EXT_EV_UP;
        case SK_DOWN:  return EXT_EV_DOWN;
        case SK_RIGHT: return EXT_EV_RIGHT;
        case SK_OK:    return EXT_EV_ENTER;
        case SK_ESC:   return EXT_EV_BACK;
        case SK_MENU:  return EXT_EV_MENU;
        default:       return EXT_EV_NONE;
    }
}

int extdisp_gesture_feed(extdisp_gesture_t *g, uint32_t now, bool touch, int x, int y, int ev[EXT_MAX_EVENTS])
{
    int n = 0;
#define EMIT(e) do { if (n < EXT_MAX_EVENTS) ev[n++] = (e); } while (0)
    if (!touch) {
        if (!g->down) return 0;
        if (++g->up_n < EXT_RELEASE_N) return 0;                    // a flicker of the film, not a release
        if (g->key == SK_FRAME) {
            if (!g->moved && !g->long_fired) EMIT(EXT_EV_ENTER);
        } else if (!is_arrow(g->key) && g->key != SK_STATUS && g->pressed == g->key) {
            EMIT(key_event(g->key));
        }
        g->down = false;
        g->pressed = SK_NONE;
        return n;
    }
    g->up_n = 0;
    if (!g->down) {                                                 // contact starts
        g->down = true;
        g->key = extdisp_sk_hit(g->r, x, y);
        g->x0 = g->ax = x; g->y0 = g->ay = y;
        g->t0 = now;
        g->moved = g->long_fired = false;
        g->pressed = g->key == SK_FRAME ? SK_NONE : g->key;
        if (is_arrow(g->key)) { EMIT(key_event(g->key)); g->t_rep = now + EXT_REPEAT_DELAY; }
        return n;
    }
    // contact held
    if (g->key == SK_FRAME) {
        if (!g->moved && (iabs(x - g->x0) > EXT_TAP_SLOP || iabs(y - g->y0) > EXT_TAP_SLOP)) g->moved = true;
        if (g->moved) {
            for (;;) {
                int dx = x - g->ax, dy = y - g->ay;
                if (iabs(dy) >= iabs(dx) && iabs(dy) >= EXT_DRAG_STEP) {
                    EMIT(dy < 0 ? EXT_EV_DOWN : EXT_EV_UP);         // drag up = content up = next item
                    g->ay += dy < 0 ? -EXT_DRAG_STEP : EXT_DRAG_STEP;
                } else if (iabs(dx) > iabs(dy) && iabs(dx) >= EXT_DRAG_STEP) {
                    EMIT(dx < 0 ? EXT_EV_RIGHT : EXT_EV_LEFT);      // drag left = next page
                    g->ax += dx < 0 ? -EXT_DRAG_STEP : EXT_DRAG_STEP;
                } else break;
                if (n == EXT_MAX_EVENTS) { g->ax = x; g->ay = y; break; }   // a fling: drop the excess
            }
        } else if (!g->long_fired && now - g->t0 >= EXT_LONG_MS) {
            g->long_fired = true;
            EMIT(EXT_EV_BACK);
        }
        return n;
    }
    const bool on_key = extdisp_sk_hit(g->r, x, y) == g->key;
    if (!on_key) { g->pressed = SK_NONE; return n; }                // slid off: cancelled for good
    if (g->pressed != g->key) return n;
    if (is_arrow(g->key)) {
        if ((int32_t)(now - g->t_rep) >= 0) { EMIT(key_event(g->key)); g->t_rep = now + EXT_REPEAT_EVERY; }
    } else if (g->key == SK_STATUS && !g->long_fired && now - g->t0 >= EXT_CAL_HOLD_MS) {
        g->long_fired = true;
        EMIT(EXT_EV_CALIBRATE);
    }
    return n;
#undef EMIT
}

// ---- calibration flow --------------------------------------------------------------------------------------

void extdisp_cal_targets(int pw, int ph, int t[4][2])
{
    t[0][0] = EXT_CAL_INSET;           t[0][1] = EXT_CAL_INSET;
    t[1][0] = pw - 1 - EXT_CAL_INSET;  t[1][1] = EXT_CAL_INSET;
    t[2][0] = pw - 1 - EXT_CAL_INSET;  t[2][1] = ph - 1 - EXT_CAL_INSET;
    t[3][0] = EXT_CAL_INSET;           t[3][1] = ph - 1 - EXT_CAL_INSET;
}

// Starts by waiting for a lift: the finger that asked for the calibration (status hold) must not land on
// target 1. With no finger down that wait ends after EXT_RELEASE_N samples.
void extdisp_calib_start(extdisp_calib_t *c)
{
    memset(c, 0, sizeof *c);
    c->wait_release = true;
}

int extdisp_calib_feed(extdisp_calib_t *c, uint32_t now, bool touch, int rx, int ry)
{
    if (c->step >= 4) return CAL_BUSY;
    if (!touch) {
        if ((c->down || c->wait_release) && ++c->up_n >= EXT_RELEASE_N) {
            c->down = false;                                        // an early lift discards the partial sample
            c->wait_release = false;
        }
        return CAL_BUSY;
    }
    c->up_n = 0;
    if (c->wait_release) return CAL_BUSY;                           // the same press must not feed the next target
    if (!c->down) { c->down = true; c->t0 = now; c->n = 0; c->sx = c->sy = 0; }
    if (now - c->t0 < EXT_CAL_SETTLE_MS) return CAL_BUSY;
    c->sx += rx; c->sy += ry;
    if (++c->n < EXT_CAL_HOLD_N) return CAL_BUSY;
    c->raw[c->step][0] = (int)(c->sx / c->n);
    c->raw[c->step][1] = (int)(c->sy / c->n);
    c->step++;
    c->wait_release = true;
    return c->step == 4 ? CAL_DONE : CAL_NEXT;
}

bool extdisp_calib_finish(const extdisp_calib_t *c, int pw, int ph, extdisp_cal_t *cal, int *err_px)
{
    int t[4][2];
    extdisp_cal_targets(pw, ph, t);
    extdisp_cal_t k;
    if (c->step < 4 || !extdisp_cal_solve((const int (*)[2])c->raw, (const int (*)[2])t, &k)) {
        if (err_px) *err_px = -1;
        return false;
    }
    int err = extdisp_cal_error(&k, c->raw[3], t[3]);
    if (err_px) *err_px = err;
    if (err > EXT_CAL_MAX_ERR) return false;
    *cal = k;
    return true;
}
