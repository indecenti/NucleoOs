#include "nucleo_imu_motion.h"
#include <math.h>

// Tunables, all fractions of gravity (unit-free -> range-independent).
#define G_ALPHA   0.10f    // gravity EMA: slow, so sustained motion stays in "energy", not absorbed
#define E_ALPHA   0.25f    // energy EMA: short memory -> reacts in a few samples without flapping
#define STILL_HI  0.035f   // energy below this => STILL
#define HAND_HI   0.22f    // STILL_HI..HAND_HI => HAND; above => MOVE
#define HYST      0.010f   // dead-band on the way back down, so a value on an edge doesn't chatter

static float vlen(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }

void nk_motion_reset(nk_motion_state *m)
{
    if (!m) return;
    m->gx = m->gy = m->gz = 0.0f;
    m->energy = 0.0f;
    m->seeded = 0;
    m->state = NK_MOTION_UNKNOWN;
}

nk_motion_t nk_motion_push(nk_motion_state *m, float ax, float ay, float az)
{
    if (!m) return NK_MOTION_UNKNOWN;
    if (!m->seeded) {
        m->gx = ax; m->gy = ay; m->gz = az;
        m->seeded = 1; m->energy = 0.0f;
        m->state = NK_MOTION_STILL;          // first sample: assume settled until proven otherwise
        return m->state;
    }
    // dynamic component = live sample minus the slow gravity estimate, measured BEFORE updating it
    float rx = ax - m->gx, ry = ay - m->gy, rz = az - m->gz;
    m->gx += rx * G_ALPHA; m->gy += ry * G_ALPHA; m->gz += rz * G_ALPHA;
    float g = vlen(m->gx, m->gy, m->gz);
    float frac = g > 1.0f ? vlen(rx, ry, rz) / g : 0.0f;   // |residual| as a fraction of gravity
    if (frac > 4.0f) frac = 4.0f;                          // clamp a wild / corrupt read
    m->energy += (frac - m->energy) * E_ALPHA;

    // hysteretic thresholding: cross UP at the high edge, fall back DOWN only past the dead-band
    float e = m->energy;
    nk_motion_t s = m->state;
    switch (s) {
        case NK_MOTION_STILL:
            if (e > HAND_HI)       s = NK_MOTION_MOVE;
            else if (e > STILL_HI) s = NK_MOTION_HAND;
            break;
        case NK_MOTION_HAND:
            if (e > HAND_HI)            s = NK_MOTION_MOVE;
            else if (e < STILL_HI - HYST) s = NK_MOTION_STILL;
            break;
        case NK_MOTION_MOVE:
            if (e < STILL_HI - HYST)   s = NK_MOTION_STILL;
            else if (e < HAND_HI - HYST) s = NK_MOTION_HAND;
            break;
        default:
            s = e > HAND_HI ? NK_MOTION_MOVE : e > STILL_HI ? NK_MOTION_HAND : NK_MOTION_STILL;
            break;
    }
    m->state = s;
    return s;
}

const char *nk_motion_name(nk_motion_t s)
{
    switch (s) {
        case NK_MOTION_STILL: return "still";
        case NK_MOTION_HAND:  return "hand";
        case NK_MOTION_MOVE:  return "move";
        default:              return "?";
    }
}

// Orientation bands on the screen-normal component nz = gz/|g| (cosine of the screen-up angle).
// |nz| > FLAT_T => lying flat; in-plane fraction > UP_T => upright; the gap between is TILTED.
#define FLAT_T  0.85f   // |nz| above this => flat (~32 deg of horizontal)
#define UP_T    0.85f   // sqrt(gx^2+gy^2)/|g| above this => upright (|nz| below ~0.53)

nk_orient_t nk_orient_classify(float gx, float gy, float gz)
{
    float g = vlen(gx, gy, gz);
    if (g < 1.0f) return NK_ORIENT_UNKNOWN;       // implausible / unseeded
    float nz = gz / g;                            // screen-normal component, -1..+1
    float plane = vlen(gx, gy, 0.0f) / g;         // fraction of gravity in the screen plane
    if (nz >  FLAT_T) return NK_ORIENT_FLAT_UP;
    if (nz < -FLAT_T) return NK_ORIENT_FLAT_DOWN;
    if (plane > UP_T) return NK_ORIENT_UPRIGHT;
    return NK_ORIENT_TILTED;
}

const char *nk_orient_name(nk_orient_t o)
{
    switch (o) {
        case NK_ORIENT_FLAT_UP:   return "flat-up";
        case NK_ORIENT_FLAT_DOWN: return "flat-down";
        case NK_ORIENT_UPRIGHT:   return "upright";
        case NK_ORIENT_TILTED:    return "tilted";
        default:                  return "?";
    }
}

int nk_level(float gx, float gy, float gz, float *lx, float *ly, float *deg)
{
    if (lx) *lx = 0;
    if (ly) *ly = 0;
    if (deg) *deg = 0;
    float g = vlen(gx, gy, gz);
    if (g < 1.0f) return 0;                       // implausible / unseeded
    float x = gx / g, y = gy / g, nz = gz / g;
    if (x >  1.0f) x =  1.0f; else if (x < -1.0f) x = -1.0f;
    if (y >  1.0f) y =  1.0f; else if (y < -1.0f) y = -1.0f;
    if (nz < 0.0f) nz = -nz;                       // |cos| of screen-normal vs gravity
    if (nz > 1.0f) nz = 1.0f;
    if (lx)  *lx  = x;
    if (ly)  *ly  = y;
    if (deg) *deg = acosf(nz) * (180.0f / 3.14159265358979f);   // 0 = flat, 90 = on edge
    return 1;
}

// Pedometer thresholds in raw LSB (the IMU runs +/-2g, so 1g = 16384 LSB). A walking stride gives the
// device ~0.15-0.5g of dynamic accel; HI catches a moderate step, LO must be crossed to re-arm.
#define STEP_HI    2200.0f    // ~0.13 g dynamic peak => count a step
#define STEP_LO     900.0f    // ~0.055 g => fell back, ready for the next peak
#define STEP_REFR    11       // min ~0.22 s between steps at ~50 Hz (caps cadence ~270/min, kills double-counts)

void nk_step_reset(nk_step_state *s)
{
    if (!s) return;
    s->base = 0.0f; s->dyn = 0.0f; s->refr = 0; s->armed = 1; s->steps = 0;
}

unsigned nk_step_push(nk_step_state *s, float ax, float ay, float az)
{
    if (!s) return 0;
    float mag = vlen(ax, ay, az);
    if (s->base < 1.0f) s->base = mag;                 // seed on the first sample
    s->base += (mag - s->base) * 0.05f;                // slow baseline (gravity + posture drift)
    float dyn = mag - s->base;                         // SIGNED dynamic accel (one positive peak per stride)
    s->dyn += (dyn - s->dyn) * 0.30f;                  // light smoothing
    if (s->refr > 0) s->refr--;
    if (s->dyn < STEP_LO) s->armed = 1;                // fell below the low gate -> re-armed for the next peak
    if (s->armed && s->dyn > STEP_HI && s->refr == 0) {
        s->steps++; s->armed = 0; s->refr = STEP_REFR;
    }
    return s->steps;
}
