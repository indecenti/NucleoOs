// presence_detect — the Wi-Fi CSI motion / presence detector behind the Presence app (app_presence.cpp).
// Pure C, no ESP-IDF: the radio glue feeds it one sample per CSI frame and steps it on the UI clock, and
// tools/presence-host drives the very same code on the PC.
//
// Physics in one line: a body moving through the radio path between the router and the Cardputer changes
// the multipath fingerprint of every frame. Per frame we keep the mean subcarrier amplitude and the mean
// sin(phase); their variance over the last PD_WIN frames, each normalised against a slow-rising floor and a
// decaying peak, is blended 60/40 into a 0..1 motion score (the constants of the original
// Cardputer-CSI-Human-Detector). Presence is latched when motion crosses the threshold on two consecutive
// steps and coasts for PD_HOLD_MS after the last motion, so a person standing still stays "present".
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PD_WIN       50          // CSI frames in the variance window
#define PD_HIST      120         // history buckets kept for the graph
#define PD_BUCKET_MS 500         // one bucket = 0.5 s -> the graph spans 60 s
#define PD_HOLD_MS   10000       // presence coasts this long after the last motion
#define PD_CALIB_MS  6000        // baseline learning at start / on demand
#define PD_EVENTS    4           // last presence episodes remembered

typedef enum { PD_CALIB, PD_RUN } pd_phase_t;

typedef struct { uint32_t start_ms, end_ms; uint8_t peak; } pd_event_t;   // end_ms 0 = still going

typedef struct {
    // CSI ring (written by pd_feed)
    float    amp[PD_WIN], pha[PD_WIN];
    uint32_t frames;                // total frames fed
    int8_t   rssi;
    // normalisation state
    float    vmin, vmax, pmin, pmax;
    // output
    pd_phase_t phase;
    uint32_t calib_start_ms;
    float    motion;                // 0..1, this step
    float    shown;                 // motion as displayed: fades during the hold
    float    thr;                   // presence threshold, 0.05..0.95
    bool     present;
    bool     moving;                // motion above threshold right now
    int      over;                  // consecutive steps above threshold
    uint32_t last_motion_ms;        // when motion was last above threshold
    uint32_t clear_since_ms;        // when presence last ended (or calibration finished)
    float    held;                  // peak motion of the current episode
    // history: max of `shown` per PD_BUCKET_MS
    uint8_t  hist[PD_HIST];
    int      hist_head;             // next bucket to write
    uint32_t bucket_start_ms;
    uint8_t  bucket_max;
    pd_event_t ev[PD_EVENTS];       // ev[0] = most recent
    int      n_ev;
} pd_state_t;

void  pd_init(pd_state_t *s, float thr, uint32_t now_ms);   // starts in PD_CALIB
void  pd_calibrate(pd_state_t *s, uint32_t now_ms);         // forget the baseline, learn it again
// One CSI frame as esp_wifi delivers it: `buf` = int8 (imag, real) pairs, L-LTF first, `len` bytes.
void  pd_feed(pd_state_t *s, const int8_t *buf, int len, int8_t rssi, bool first_word_invalid);
void  pd_step(pd_state_t *s, uint32_t now_ms);              // ~10-15 Hz, from the UI task
float pd_calib_progress(const pd_state_t *s, uint32_t now_ms);   // 0..1 while calibrating
// Rough router distance from RSSI (log-distance, n=2.7, -40 dBm at 1 m), clamped to 0.5..30 m.
float pd_distance_m(int rssi);
// Signal quality 0..100 from RSSI and the CSI frame rate (frames/s).
int   pd_quality(int rssi, int rate);

#ifdef __cplusplus
}
#endif
