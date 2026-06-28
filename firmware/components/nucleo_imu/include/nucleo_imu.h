// BMI270 6-axis IMU on the Cardputer ADV, exposed as a simple TILT controller (accelerometer only;
// the gyro stays off to save power). Pure ESP-IDF i2c_master — mirrors the ES8311 codec: it attaches
// to the keyboard-owned system I2C bus and is a hard no-op on the original Cardputer.
#pragma once
#include "nucleo_imu_motion.h"   // nk_motion_t (coarse motion state)
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>

// Attach the BMI270 to the ADV's shared system I2C bus (pass nucleo_kbd_i2c_bus()). Returns false
// and stays a no-op on the original Cardputer (bus == NULL) or if the chip / config blob is absent.
bool nucleo_imu_init(void *i2c_bus);

// True once the BMI270 is up and streaming (implies Cardputer ADV). Gate any tilt UI on this.
bool nucleo_imu_present(void);

// Last init diagnostic (system I2C scan + probe result), for an on-device "no sensor" screen.
// Empty string when the IMU came up fine. Lets you debug a non-streaming BMI270 without serial.
const char *nucleo_imu_debug(void);

// Latest tilt as two signed, neutral-relative, low-passed axes in roughly [-1..+1] (the fraction of
// gravity along each screen axis, ~sin of the tilt angle). tx > 0 = tipped right, ty > 0 = nose down
// by the default mapping. Returns false (tx/ty set to 0) when the IMU is absent or a read fails.
bool nucleo_imu_tilt(float *tx, float *ty);

// Capture the CURRENT orientation as neutral/centre — "hold it however you like, then play".
void nucleo_imu_recenter(void);

// --- coarse motion sense (OS-wide, neutral-independent) -------------------------------------------
// Take one accelerometer reading to refresh the motion state, WITHOUT touching the tilt neutral.
// For pollers that don't drive nucleo_imu_tilt() themselves (e.g. /api/status). No-op + false when
// the IMU is absent (original Cardputer). Apps that poll tilt every frame already refresh motion.
bool nucleo_imu_sample(void);

// Current coarse motion class (still / hand / move). NK_MOTION_UNKNOWN when the IMU is absent.
nk_motion_t nucleo_imu_motion(void);

// Same as a stable short string for an API field / UI: "still" | "hand" | "move" | "?".
const char *nucleo_imu_motion_str(void);

// Current coarse orientation (flat-up / flat-down / upright / tilted), from the gravity estimate.
// NK_ORIENT_UNKNOWN when the IMU is absent or not yet seeded.
nk_orient_t nucleo_imu_orient(void);
const char *nucleo_imu_orient_str(void);

// Live "shake intensity": EMA of the dynamic accel as a fraction of gravity (~0 at rest, >0.5 = a real
// shake, can exceed 1 when shaken hard). Refresh it by calling nucleo_imu_sample() (or tilt) each frame.
// 0 when the IMU is absent. Use it for a deliberate shake-to-act gesture (threshold high to ignore noise).
float nucleo_imu_energy(void);

// Pedometer step total since the last reset. Updated on every accel read (call nucleo_imu_sample() at
// ~50 Hz to feed it). 0 when the IMU is absent. nucleo_imu_steps_reset() zeroes the running count.
unsigned nucleo_imu_steps(void);
void     nucleo_imu_steps_reset(void);

// Bubble-level readout for the native level app: *lx,*ly (screen-plane gravity, -1..1; 0,0 = level)
// and *deg (tilt from flat). Returns false when the IMU is absent. See nk_level().
bool nucleo_imu_level(float *lx, float *ly, float *deg);

#ifdef __cplusplus
}
#endif
