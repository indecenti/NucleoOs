// Host test for nucleo_event_copy_since() — the WebSocket delta serializer (BUG 2: a size_t
// underflow in `out_sz - n` could write the closing ']' PAST the caller's buffer when a max-size
// event landed near the end). Compiles the REAL firmware source (nucleo_eventbus.c) against tiny
// FreeRTOS/esp shims and hammers copy_since across every out_sz with a canary just past the buffer.
// A regression (OOB write, missing NUL, or invalid JSON) trips a CHECK. Build: eventbus-build.ps1.
#include "nucleo_eventbus.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); g_fail++; } } while (0)

// Minimal structural validity: non-empty, starts '[' ends ']'. (The payloads we publish are valid
// JSON objects, so a complete array is real JSON; a truncated/partial event would break this.)
static int valid_array(const char *s)
{
    size_t L = strlen(s);
    return L >= 2 && s[0] == '[' && s[L - 1] == ']';
}

int main(void)
{
    if (nucleo_event_init() != ESP_OK) { printf("init failed\n"); return 2; }

    // Worst-case events: topic = TOPIC_MAX-1 (47), payload = PAYLOAD_MAX-1 (207) valid JSON object.
    char topic[NUCLEO_EVENT_TOPIC_MAX];
    memset(topic, 'T', sizeof(topic) - 1); topic[sizeof(topic) - 1] = '\0';   // 47 'T'
    char payload[NUCLEO_EVENT_PAYLOAD_MAX];
    int pi = 0;
    pi += snprintf(payload + pi, sizeof(payload) - pi, "{\"d\":\"");
    while (pi < (int)sizeof(payload) - 3) payload[pi++] = 'x';
    payload[pi++] = '"'; payload[pi++] = '}'; payload[pi] = '\0';             // exactly 207 chars

    // Fill the whole 32-slot ring with max-size events.
    for (int i = 0; i < 40; i++) nucleo_event_publish(topic, payload);
    uint32_t cur = nucleo_event_current_seq();
    uint32_t since = (cur > 32) ? cur - 32 : 0;          // ask for the full ring window

    printf("eventbus-check: published %u events (topic=%u payload=%u)\n",
           (unsigned)cur, (unsigned)strlen(topic), (unsigned)strlen(payload));

    // Sweep EVERY out_sz from tiny to comfortably-larger-than-ws(1024). Guard 8 canary bytes that
    // copy_since must never touch (it is told the buffer is exactly `cap`).
    const unsigned char CAN = 0xAA;
    int swept = 0;
    for (size_t cap = 1; cap <= 1300; cap++) {
        size_t total = cap + 8;
        unsigned char *buf = (unsigned char *)malloc(total);
        memset(buf, CAN, total);
        int n = nucleo_event_copy_since(since, (char *)buf, cap);

        int canok = 1; for (int k = 0; k < 8; k++) if (buf[cap + k] != CAN) canok = 0;
        CHECK(canok, "no OOB write past out_sz");

        int nulok = 0; for (size_t k = 0; k < cap; k++) if (buf[k] == 0) { nulok = 1; break; }
        CHECK(nulok, "result NUL-terminated within out_sz");

        if (nulok) CHECK(strlen((char *)buf) < cap, "strlen < out_sz");
        if (n >= 0 && cap >= 3) CHECK(valid_array((char *)buf), "valid JSON array");

        free(buf);
        swept++;
    }
    printf("  swept %d out_sz values [1..1300]\n", swept);

    // Real-world size (ws out[1024]): must carry several events as valid JSON.
    char big[1024];
    int n = nucleo_event_copy_since(since, big, sizeof(big));
    CHECK(n > 0, "ws-size buffer carries at least one event");
    CHECK(valid_array(big), "ws-size buffer is valid JSON array");
    CHECK(strlen(big) < sizeof(big), "ws-size result within buffer");
    printf("  out[1024]: %d events, %u bytes\n", n, (unsigned)strlen(big));

    // resync path: since older than the ring -> -1, and out is a valid empty string.
    char small[64];
    int r = nucleo_event_copy_since(0, small, sizeof(small));   // 0 is far behind cur -> resync
    CHECK(r == -1, "stale since -> resync (-1)");
    CHECK(small[0] == '\0', "resync leaves out NUL-terminated");

    if (g_fail == 0) { printf("eventbus-check: ALL PASS\n"); return 0; }
    printf("eventbus-check: %d FAIL\n", g_fail);
    return 1;
}
