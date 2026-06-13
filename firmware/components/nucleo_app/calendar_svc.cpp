// Background calendar reminder service — event-light by design.
//
// A single low-priority task wakes every 15 s and keeps only TODAY's events in RAM
// (reloaded from the SD card on a day-change or at most every 5 min — never per wake, so it
// stays off ANIMA's SD path). When an event's HH:MM arrives it ALWAYS publishes
// "calendar.reminder" on the event bus → the WebSocket sink broadcasts it → any connected
// web client renders a toast. When NO web client is driving the device (the launcher
// suspends its UI while remote, handing CPU/RAM to the server), it ALSO chimes the speaker
// and raises the on-screen banner. So: standalone → device alerts; connected → the client
// owns the surface. The chime WAV is synthesized once so no binary asset ships.
//
// Resident cost: today's events (~24 × ~100 B) + a tiny task stack — well under 1% of the
// ANIMA footprint, and the two never share a hot loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "cJSON.h"
extern "C" {
#include "nucleo_board.h"
#include "nucleo_notify.h"
}

#define CAL_PATH NUCLEO_SD_MOUNT "/system/config/calendar.json"

// ---- today's events cache --------------------------------------------------
// Sized to the device, not to a worst case: 16 same-day reminders is ample for a personal
// Cardputer, and 64 chars covers a notification title on a 240 px screen (snprintf truncates a
// longer one cleanly). 24×104 B → 16×72 B reclaims ~1.3 KB of always-resident .bss.
#define MAX_EV 16
struct Ev { char t[6]; char x[64]; bool fired; };
static Ev s_ev[MAX_EV];
static int s_evn = 0;
static char s_day[12] = "";        // cached day key (empty -> force first load)
static int64_t s_loaded_us = 0;


// ---- load today's events (rare: day-change or >5 min stale) ----------------
static void load_today(const char *key)
{
    s_evn = 0; snprintf(s_day, sizeof s_day, "%s", key);
    FILE *f = fopen(CAL_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 256 * 1024) {
        char *buf = (char *)malloc(n + 1);
        if (buf) {
            size_t got = fread(buf, 1, n, f); buf[got] = 0;
            cJSON *root = cJSON_Parse(buf); free(buf);
            if (root) {
                cJSON *evs = cJSON_GetObjectItem(root, "events");
                cJSON *arr = cJSON_IsObject(evs) ? cJSON_GetObjectItem(evs, key) : nullptr;
                if (cJSON_IsArray(arr)) {
                    cJSON *e;
                    cJSON_ArrayForEach(e, arr) {
                        if (s_evn >= MAX_EV) break;
                        cJSON *t = cJSON_GetObjectItem(e, "time");
                        if (!cJSON_IsString(t) || !t->valuestring[0]) continue;
                        cJSON *x = cJSON_GetObjectItem(e, "text");
                        Ev *r = &s_ev[s_evn++];
                        snprintf(r->t, sizeof r->t, "%s", t->valuestring);
                        snprintf(r->x, sizeof r->x, "%s", cJSON_IsString(x) ? x->valuestring : "(event)");
                        r->fired = false;
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    fclose(f);
}

static void svc_task(void *)
{
    for (;;) {
        time_t now = time(NULL);
        if (now >= 1672531200) {                  // only once NTP has landed
            struct tm tm; localtime_r(&now, &tm);
            char key[12]; strftime(key, sizeof key, "%Y-%m-%d", &tm);
            int64_t us = esp_timer_get_time();
            if (strcmp(key, s_day) != 0 || us - s_loaded_us > 300LL * 1000000) {
                load_today(key); s_loaded_us = us;
            }
            char hhmm[6]; snprintf(hhmm, sizeof hhmm, "%02d:%02d", tm.tm_hour, tm.tm_min);
            for (int i = 0; i < s_evn; i++) {
                if (s_ev[i].fired || strcmp(s_ev[i].t, hhmm) != 0) continue;
                s_ev[i].fired = true;

                // Hand off to the unified backbone: it broadcasts notify.post and, standalone,
                // chimes + banners. Title = event text, body = time, click opens the Calendar.
                char id[24]; snprintf(id, sizeof id, "cal-%s", s_ev[i].t);
                nucleo_notify_emit("calendar", NOTIFY_INFO, id, s_ev[i].x, s_ev[i].t, "app:calendar");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

extern "C" void nucleo_calendar_svc_start(void)
{
    s_day[0] = 0; s_loaded_us = -300LL * 1000000;     // force a load on the first wake
    xTaskCreate(svc_task, "cal-svc", 4096, nullptr, 2, nullptr);   // low priority: never preempts UI/ANIMA/audio
}
