// claude-stream-test.c: host gate for anima_claude.h, the pure half of ANIMA's Claude client: the effort
// each model accepts (and whether it thinks), the streamed /v1/messages body, and the SSE reader fed a
// realistic stream in odd-sized chunks (thinking + signature + text pieces + message_delta + stop), the
// caller's answer cap, and an error event. Run by tools/anima-host/claude-stream-check.mjs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "cJSON.h"
#define TAG "t"
#define ESP_LOGW(t, ...) (printf("W: " __VA_ARGS__), printf("\n"))
#define ESP_LOGI(t, ...) (printf("I: " __VA_ARGS__), printf("\n"))
typedef struct { const char *q, *a; } anima_turn_t;
#include "anima_claude.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static void eff(const char *m, const char *w, const char *exp, bool expthink)
{
    bool th; const char *e = anth_effort_for(m, w, &th);
    bool ok = (exp == NULL ? e == NULL : (e && !strcmp(e, exp))) && th == expthink;
    if (!ok) { printf("FAIL effort %s/%s -> %s think=%d\n", m, w ? w : "-", e ? e : "NULL", th); fails++; }
}
int main(void)
{
    eff("claude-sonnet-5-5", "medium", "medium", true);
    eff("claude-sonnet-5-5", NULL, "medium", true);
    eff("claude-sonnet-5-5", "bogus", "medium", true);
    eff("claude-opus-5-5", "max", "max", true);
    eff("claude-haiku-5-5", "low", "low", true);
    eff("claude-fable-5-1", "xhigh", "xhigh", true);
    eff("claude-opus-4-8", "xhigh", "xhigh", false);
    eff("claude-opus-4-6", "xhigh", "high", false);
    eff("claude-sonnet-4-6", "max", "max", false);
    eff("claude-opus-4-5-20251101", "max", "high", false);
    eff("claude-haiku-4-5", "low", NULL, false);
    eff("claude-sonnet-4-5", "low", NULL, false);
    eff("claude-3-5-haiku-20241022", "low", NULL, false);
    eff("llama-3.1-8b-instant", "low", NULL, false);

    char *b = anthropic_body("claude-sonnet-5-5", "sys", NULL, 0, "ciao", 3182, "medium");
    CHECK(b && strstr(b, "\"stream\":true") && strstr(b, "\"output_config\":{\"effort\":\"medium\"}") && strstr(b, "\"max_tokens\":3182"));
    printf("body: %s\n", b); free(b);
    b = anthropic_body("claude-haiku-4-5", NULL, NULL, 0, "ciao", 110, NULL);
    CHECK(b && !strstr(b, "output_config")); free(b);

    // A realistic stream: thinking block (empty), signature, a text block in pieces, message_delta, stop.
    const char *stream =
        "event: message_start\r\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"content\":[]}}\r\n\r\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
        "event: ping\ndata: {\"type\": \"ping\"}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"abc\"}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Alessandro \"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Manzoni, 1840 \\u00e8 \\\"definitiva\\\".\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":40}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"AFTER\"}}\n\n";
    anth_sse_t *s = calloc(1, sizeof *s); s->cap = 1000;
    size_t L = strlen(stream);
    for (size_t i = 0; i < L; i += 7) anth_sse_feed(s, stream + i, (int)(L - i < 7 ? L - i : 7));   // odd chunking
    printf("text: [%s] stop=%s done=%d\n", s->txt ? s->txt : "", s->stop, s->done);
    CHECK(s->done && !s->err && s->txt && !strcmp(s->txt, "Alessandro Manzoni, 1840 \xc3\xa8 \"definitiva\".") && !strcmp(s->stop, "end_turn"));
    free(s->txt); free(s);

    // cap: stops at the caller's budget
    s = calloc(1, sizeof *s); s->cap = 5;
    anth_sse_feed(s, stream, (int)L);
    CHECK(s->len == 5 && !strncmp(s->txt, "Aless", 5));
    free(s->txt); free(s);

    // error event
    const char *err = "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n";
    s = calloc(1, sizeof *s); s->cap = 100; anth_sse_feed(s, err, (int)strlen(err));
    CHECK(s->err && !s->txt); free(s);

    printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
