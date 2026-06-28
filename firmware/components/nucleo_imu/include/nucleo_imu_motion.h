// Pure, host-testable motion classifier on top of the BMI270 accelerometer. No ESP-IDF dependency:
// it takes raw accel samples (int16 LSB as float) and returns a coarse motion state. Split from
// nucleo_imu.c (all I2C glue) the same way nucleo_voice_dsp.c is split from the I2S capture, so the
// MATH can be compiled and checked on the PC (tools/imu-host).
//
// The signal is range- and rate-independent: it tracks a slow gravity estimate and reports the EMA
// of how far the live sample sits from it, as a FRACTION OF GRAVITY. So it works in +/-2g or +/-4g,
// and whether sampled at 100 Hz by a game or every ~2 s by /api/status.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_MOTION_UNKNOWN = 0,   // no sample yet (or IMU absent)
    NK_MOTION_STILL,         // resting on a surface
    NK_MOTION_HAND,          // held in hand (small constant tremor)
    NK_MOTION_MOVE,          // being moved / shaken / walked with
} nk_motion_t;

typedef struct {
    float gx, gy, gz;   // slow gravity estimate (raw LSB units)
    float energy;       // EMA of |sample - gravity| / |gravity|, ~0 at rest
    int   seeded;       // gravity estimate primed
    nk_motion_t state;  // last classification (hysteretic)
} nk_motion_state;

// Coarse static orientation, derived from the gravity vector (independent of the motion state).
typedef enum {
    NK_ORIENT_UNKNOWN = 0,   // gravity too small to trust (or IMU absent)
    NK_ORIENT_FLAT_UP,       // lying screen-up (e.g. on a desk)
    NK_ORIENT_FLAT_DOWN,     // lying screen-down
    NK_ORIENT_UPRIGHT,       // screen roughly vertical (held up / propped)
    NK_ORIENT_TILTED,        // in between flat and upright
} nk_orient_t;

// Forget all history (call when the IMU (re)starts).
void nk_motion_reset(nk_motion_state *m);

// Feed ONE accelerometer sample (raw int16 LSB as float, any range). Returns the updated state.
nk_motion_t nk_motion_push(nk_motion_state *m, float ax, float ay, float az);

// Stable short label for an API field / UI: "still" | "hand" | "move" | "?".
const char *nk_motion_name(nk_motion_t s);

// Classify orientation from a gravity vector (raw accel LSB). Assumes accel Z (3rd arg) is the
// screen-normal axis with +Z = screen-up — flip gz's sign at the call site if a unit reads inverted,
// mirroring the IMU_SX/SY tilt convention. flat-vs-upright is sign-robust; only up-vs-down depends on
// that convention. Returns NK_ORIENT_UNKNOWN when the vector is too small to trust.
nk_orient_t nk_orient_classify(float gx, float gy, float gz);

// Stable short label: "flat-up" | "flat-down" | "upright" | "tilted" | "?".
const char *nk_orient_name(nk_orient_t o);

// Bubble-level readout from a gravity vector. *lx,*ly = gravity projected on the screen plane,
// normalized to gravity (each -1..1; 0,0 = perfectly level; points toward the LOW side, so a UI
// places the bubble at the OPPOSITE point). *deg = tilt of the screen from horizontal (0 = flat,
// 90 = on its edge). Returns 0 when the vector is too small to trust, 1 otherwise.
int nk_level(float gx, float gy, float gz, float *lx, float *ly, float *deg);

// Pedometer: detect walking steps from the raw accel stream (peak detection on the dynamic accel with
// a low-gate re-arm and a refractory window, so one stride = one count). Feed samples at ~50 Hz.
typedef struct {
    float base;        // slow baseline of |accel| (tracks gravity + posture)
    float dyn;         // lightly smoothed dynamic-accel magnitude
    int   refr;        // refractory countdown (samples) — caps the max cadence
    int   armed;       // fell below the low gate -> ready to count the next peak
    unsigned steps;
} nk_step_state;
void     nk_step_reset(nk_step_state *s);
unsigned nk_step_push(nk_step_state *s, float ax, float ay, float az);   // returns the running step total

#ifdef __cplusplus
}
#endif
