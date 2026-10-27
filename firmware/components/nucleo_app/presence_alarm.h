// presence_alarm — the Presence app's intrusion alarm, as a pure state machine (no radio, no audio, no UI).
// app_presence.cpp drives it from the detector and turns its events into sounds; tools/presence-host
// tests it on a simulated clock.
//
//   OFF --arm--> ARMING (exit delay, the user leaves) --> ARMED --motion--> TRIGGERED (siren)
//   TRIGGERED --auto after dur_s--> OFF        any state --correct PIN--> OFF
// Only MOVING presence fires it (a person crossing a zone), never the 10 s "still present" coast, and
// not during a short grace after arming, while the detector settles from the user walking away.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PA_OFF, PA_ARMING, PA_ARMED, PA_TRIGGERED } pa_state_t;
typedef enum { PA_EV_NONE, PA_EV_TICK, PA_EV_ARMED, PA_EV_TRIGGER, PA_EV_AUTO_OFF } pa_event_t;

#define PA_GRACE_MS 1500

// Choices offered in the settings (seconds), indexed by the saved option.
#define PA_N_ARM 6
#define PA_N_DUR 6
extern const int PA_ARM_S[PA_N_ARM];   // exit delay: 5 10 20 30 60 120
extern const int PA_DUR_S[PA_N_DUR];   // siren, then auto-disarm: 10 20 30 60 120 300

typedef struct {
    pa_state_t st;
    uint32_t t0;            // when the current state began
    int      arm_i, dur_i;  // selected options
    int      last_tick_s;   // last whole second announced while arming
    uint32_t trig_count;
    uint32_t last_trig_ms;  // 0 = never
    uint32_t last_trig_len_ms;
} pa_t;

void pa_init(pa_t *a, int arm_i, int dur_i);
void pa_arm(pa_t *a, uint32_t now);
void pa_disarm(pa_t *a, uint32_t now);
// Advance the clock; `moving` = someone moving in a zone right now. Returns what just happened:
// TICK once per second of the exit delay (a beep), ARMED, TRIGGER, AUTO_OFF.
pa_event_t pa_step(pa_t *a, uint32_t now, bool moving);
int  pa_left_s(const pa_t *a, uint32_t now);   // seconds to the next transition (arming / triggered), else 0
bool pa_locked(const pa_t *a);                 // armed in any form: the PIN is the only way out

#ifdef __cplusplus
}
#endif
