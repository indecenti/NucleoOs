// presence_alarm.c — see presence_alarm.h.
#include "presence_alarm.h"
#include <string.h>

const int PA_ARM_S[PA_N_ARM] = { 5, 10, 20, 30, 60, 120 };
const int PA_DUR_S[PA_N_DUR] = { 10, 20, 30, 60, 120, 300 };

static int clampi(int v, int n) { return v < 0 ? 0 : v >= n ? n - 1 : v; }

void pa_init(pa_t *a, int arm_i, int dur_i)
{
    memset(a, 0, sizeof *a);
    a->arm_i = clampi(arm_i, PA_N_ARM);
    a->dur_i = clampi(dur_i, PA_N_DUR);
}

void pa_arm(pa_t *a, uint32_t now)
{
    a->st = PA_ARMING;
    a->t0 = now;
    a->last_tick_s = -1;
}

void pa_disarm(pa_t *a, uint32_t now)
{
    if (a->st == PA_TRIGGERED) a->last_trig_len_ms = now - a->t0;
    a->st = PA_OFF;
    a->t0 = now;
}

pa_event_t pa_step(pa_t *a, uint32_t now, bool moving)
{
    uint32_t el = now - a->t0;
    switch (a->st) {
    case PA_ARMING: {
        uint32_t len = (uint32_t)PA_ARM_S[a->arm_i] * 1000;
        if (el >= len) { a->st = PA_ARMED; a->t0 = now; return PA_EV_ARMED; }
        int s = (int)(el / 1000);
        if (s != a->last_tick_s) { a->last_tick_s = s; return PA_EV_TICK; }
        return PA_EV_NONE;
    }
    case PA_ARMED:
        if (moving && el >= PA_GRACE_MS) {
            a->st = PA_TRIGGERED; a->t0 = now;
            a->trig_count++; a->last_trig_ms = now ? now : 1;
            return PA_EV_TRIGGER;
        }
        return PA_EV_NONE;
    case PA_TRIGGERED:
        if (el >= (uint32_t)PA_DUR_S[a->dur_i] * 1000) {
            a->last_trig_len_ms = el;
            a->st = PA_OFF; a->t0 = now;
            return PA_EV_AUTO_OFF;
        }
        return PA_EV_NONE;
    default:
        return PA_EV_NONE;
    }
}

int pa_left_s(const pa_t *a, uint32_t now)
{
    uint32_t el = now - a->t0, len;
    if (a->st == PA_ARMING) len = (uint32_t)PA_ARM_S[a->arm_i] * 1000;
    else if (a->st == PA_TRIGGERED) len = (uint32_t)PA_DUR_S[a->dur_i] * 1000;
    else return 0;
    return el >= len ? 0 : (int)((len - el + 999) / 1000);
}

bool pa_locked(const pa_t *a) { return a->st != PA_OFF; }
