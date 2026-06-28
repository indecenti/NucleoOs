#include "nucleo_eventbus.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "eventbus";

#define RING 16   // replay window for reconnecting subscribers; 16 events is ample on a
                  // single-user device. 32->16 frees ~4.3 KB .bss (event_t = 272 B) for the
                  // PSRAM-less heap — see the ANIMA L1 centroid contiguity note.
#define TOPIC_MAX   NUCLEO_EVENT_TOPIC_MAX     // 48; defined in the public header so publishers can size to it
#define PAYLOAD_MAX NUCLEO_EVENT_PAYLOAD_MAX   // 208
#define JOURNAL     NUCLEO_SD_MOUNT "/journal/events.ndjson"
#define JOURNAL_CAP (128 * 1024)               // rotate past this → ~2x on disk with the .0 backup

typedef struct {
    uint32_t seq;
    int64_t ts;
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX];
} event_t;

static event_t s_ring[RING];
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;     // protegge ring + seq (preso brevemente, MAI durante I/O SD)
static SemaphoreHandle_t s_jlock;    // serializza i SOLI scrittori del journal (I/O SD fuori da s_lock)

#define MAX_SINKS 2                  // slot 0 = WebSocket broadcaster, slot 1 = mesh gossiper.
static nucleo_event_sink_t s_sinks[MAX_SINKS];

// Fan one event out to every registered sink. Runs OUTSIDE s_lock on the publisher's stack copy
// (a sink may block on the WS/radio layer). Zero allocation — the static-pool lesson holds.
static void fan_sinks(uint32_t seq, const char *topic, const char *payload, const char *src)
{
    for (int i = 0; i < MAX_SINKS; i++)
        if (s_sinks[i]) s_sinks[i](seq, topic, payload, src);
}

esp_err_t nucleo_event_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_jlock = xSemaphoreCreateMutex();
    s_seq = 0;
    memset(s_ring, 0, sizeof(s_ring));
    return (s_lock && s_jlock) ? ESP_OK : ESP_ERR_NO_MEM;
}

void nucleo_event_set_sink(nucleo_event_sink_t sink) { s_sinks[0] = sink; }
void nucleo_event_add_sink(nucleo_event_sink_t sink)
{
    for (int i = 0; i < MAX_SINKS; i++) if (!s_sinks[i]) { s_sinks[i] = sink; return; }
}
uint32_t nucleo_event_current_seq(void) { return s_seq; }

// High-frequency / transient topics are LIVE-ONLY: never persisted to the journal. Journaling them
// put an SD fopen/fprintf/fclose on the realtime voice/recorder loops (rec.level ~75/min, voice/state
// per utterance) — under the bus lock — and grew events.ndjson without bound. The live tail still
// shows them (the WS sink runs regardless, see publish below); only the on-disk HISTORY skips them.
static bool journal_skip(const char *topic)
{
    return !strcmp(topic, "rec.level")  || !strcmp(topic, "voice/state") ||
           !strcmp(topic, "voice/match") || !strcmp(topic, "system.busy");
}

static void journal_append(const event_t *e)
{
    if (journal_skip(e->topic)) return;
    // O(1) size rotation (mirrors notify_journal.h): when the file already exceeds the cap, rename it
    // to ".0" (overwriting the previous backup) and start fresh — a rename, no read/parse/buffer, so
    // history stays bounded at ~2*cap on disk for ~zero RAM. The reader (log-viewer) parses on demand.
    struct stat st;
    if (stat(JOURNAL, &st) == 0 && (long)st.st_size > JOURNAL_CAP) {
        remove(JOURNAL ".0");
        rename(JOURNAL, JOURNAL ".0");
    }
    FILE *f = fopen(JOURNAL, "a");
    if (!f) return;
    fprintf(f, "{\"t\":\"%s\",\"seq\":%u,\"ts\":%lld,\"d\":%s}\n",
            e->topic, (unsigned)e->seq, (long long)e->ts, e->payload);
    fclose(f);
}

uint32_t nucleo_event_publish(const char *topic, const char *payload_json)
{
    if (!payload_json || !payload_json[0]) payload_json = "{}";
    // The sink runs OUTSIDE the lock (it may block on the WS layer), so it must get a stack COPY:
    // pointers into the ring slot go stale as soon as later publishers lap the ring (RING=16) and
    // the WS frame then carries another event's payload. ~256 B transient on the publisher's stack.
    char t_copy[TOPIC_MAX], p_copy[PAYLOAD_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t seq = ++s_seq;
    event_t *e = &s_ring[seq % RING];
    e->seq = seq;
    e->ts = time(NULL);
    strncpy(e->topic, topic, TOPIC_MAX - 1); e->topic[TOPIC_MAX - 1] = '\0';
    strncpy(e->payload, payload_json, PAYLOAD_MAX - 1); e->payload[PAYLOAD_MAX - 1] = '\0';
    memcpy(t_copy, e->topic, TOPIC_MAX);
    memcpy(p_copy, e->payload, PAYLOAD_MAX);
    int64_t ts_copy = e->ts;
    xSemaphoreGive(s_lock);

    // Journal FUORI da s_lock: journal_append fa I/O su SD (stat/fopen/fprintf/fclose). Farlo SOTTO s_lock
    // significava che una scrittura SD lenta/contesa (es. subito dopo la scrittura del calendario di ANIMA
    // sulla stessa task) teneva il lock del ring per TUTTA l'I/O -> ogni altra task che pubblica o legge un
    // evento si bloccava su s_lock (portMAX_DELAY) = FREEZE TOTALE del device, senza reboot (l'IDLE pasce il
    // WDT). Un s_jlock dedicato serializza solo i journaler; ring/lettori/httpd restano liberi durante l'I/O.
    // Attesa LIMITATA, MAI portMAX_DELAY. Se un journaler e' appeso nell'I/O SD (card lenta/assente, oppure
    // heap troppo frammentato per i buffer FATFS), gli ALTRI publisher non devono restare bloccati per sempre
    // su s_jlock -> e' esattamente il "FREEZE TOTALE senza reboot" descritto sopra, solo spostato dal ring al
    // journal. Scaduto il timeout saltiamo SOLO la riga su disco: il blocco si riduce alla sola task davvero
    // ferma sull'SD, tutte le altre proseguono. Persistenza best-effort (gia' a RAM zero): l'evento esce
    // comunque dal tail live (s_sink, sotto). NIENTE task/coda dedicata -> nessuno stack residente in ostaggio.
    if (s_jlock && xSemaphoreTake(s_jlock, pdMS_TO_TICKS(500)) == pdTRUE) {
        event_t je; je.seq = seq; je.ts = ts_copy;
        memcpy(je.topic, t_copy, sizeof je.topic);
        memcpy(je.payload, p_copy, sizeof je.payload);
        journal_append(&je);
        xSemaphoreGive(s_jlock);
    }

    fan_sinks(seq, t_copy, p_copy, NULL);
    ESP_LOGD(TAG, "event #%u %s", (unsigned)seq, topic);
    return seq;
}

uint32_t nucleo_event_inject(const char *src, const char *topic, const char *payload_json)
{
    if (!payload_json || !payload_json[0]) payload_json = "{}";
    // Same discipline as publish(): assign a local seq under s_lock, take a stack copy, then fan
    // OUTSIDE the lock. The difference is `src` is non-NULL, so a mesh sink will NOT re-forward it.
    char t_copy[TOPIC_MAX], p_copy[PAYLOAD_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t seq = ++s_seq;
    event_t *e = &s_ring[seq % RING];
    e->seq = seq;
    e->ts = time(NULL);
    strncpy(e->topic, topic, TOPIC_MAX - 1); e->topic[TOPIC_MAX - 1] = '\0';
    strncpy(e->payload, payload_json, PAYLOAD_MAX - 1); e->payload[PAYLOAD_MAX - 1] = '\0';
    memcpy(t_copy, e->topic, TOPIC_MAX);
    memcpy(p_copy, e->payload, PAYLOAD_MAX);
    int64_t ts_copy = e->ts;
    xSemaphoreGive(s_lock);

    if (s_jlock && xSemaphoreTake(s_jlock, pdMS_TO_TICKS(500)) == pdTRUE) {
        event_t je; je.seq = seq; je.ts = ts_copy;
        memcpy(je.topic, t_copy, sizeof je.topic);
        memcpy(je.payload, p_copy, sizeof je.payload);
        journal_append(&je);
        xSemaphoreGive(s_jlock);
    }

    fan_sinks(seq, t_copy, p_copy, src);
    ESP_LOGD(TAG, "inject #%u %s (src=%s)", (unsigned)seq, topic, src ? src : "?");
    return seq;
}

int nucleo_event_copy_since(uint32_t since, char *out, size_t out_sz)
{
    if (!out || out_sz < 3) { if (out && out_sz) out[0] = '\0'; return -1; }   // need room for "[]" + NUL
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // If the gap is larger than the ring, the client must resync.
    if (s_seq > since && s_seq - since > RING) { xSemaphoreGive(s_lock); out[0] = '\0'; return -1; }
    size_t n = 0; int count = 0;
    n += snprintf(out + n, out_sz - n, "[");
    for (uint32_t seq = since + 1; seq <= s_seq; seq++) {
        event_t *e = &s_ring[seq % RING];
        if (e->seq != seq) continue;  // overwritten
        // snprintf returns the would-be length. Commit the event ONLY if it AND the closing ']' fit:
        // this keeps n strictly < out_sz (no size_t underflow on out_sz-n) and never emits a partial
        // event. If it doesn't fit we discard the truncated attempt and stop — the client resyncs.
        int w = snprintf(out + n, out_sz - n, "%s{\"t\":\"%s\",\"seq\":%u,\"ts\":%lld,\"d\":%s}",
                         count ? "," : "", e->topic, (unsigned)e->seq, (long long)e->ts, e->payload);
        if (w < 0 || (size_t)w + 1 >= out_sz - n) { out[n] = '\0'; break; }   // +1 reserves the ']'
        n += (size_t)w;
        count++;
    }
    snprintf(out + n, out_sz - n, "]");   // always fits: the loop leaves >=1 byte for it (and the NUL)
    xSemaphoreGive(s_lock);
    return count;
}
