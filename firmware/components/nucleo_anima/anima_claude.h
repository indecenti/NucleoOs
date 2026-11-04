// anima_claude.h: the pure half of ANIMA's Claude client (no network, no ESP-IDF): which effort a model
// accepts, the /v1/messages request body, and the SSE stream reader. Included ONCE by
// nucleo_anima_online.c (the network half lives there) and by the host gate
// tools/anima-host/claude-stream-test.c, so what the gate checks is the code the device runs.
// Needs before inclusion: <string.h> <stdlib.h> <ctype.h> <stdbool.h>, cJSON.h, ESP_LOGW + TAG, anima_turn_t.
#pragma once

static const char *const ANTH_EFFORTS[] = { "low", "medium", "high", "xhigh", "max" };
static const int ANTH_THINK_TOK[]       = { 1024, 3072, 8192, 16000, 32000 };     // thinking allowance on top of the answer
static const int ANTH_TURN_MS[]         = { 20000, 30000, 60000, 90000, 120000 }; // whole-turn wall clock
#define ANTH_PLAIN_TURN_MS 20000    // a model without effort/thinking

static int anth_effort_index(const char *e)
{
    if (e && e[0]) for (int i = 0; i < 5; i++) if (!strcmp(e, ANTH_EFFORTS[i])) return i;
    return 1;                                                   // medium
}
// Family + version of a Claude model id ("claude-sonnet-5-5", "claude-opus-4-5-20251101", "claude-fable-5-1").
// Returns false for anything that isn't one. A dated suffix is not a minor version.
static bool anth_model_ver(const char *m, char *fam, int fcap, int *maj, int *min)
{
    if (!m || strncmp(m, "claude-", 7)) return false;
    const char *p = m + 7, *d = strchr(p, '-');
    if (!d || d == p || d - p >= fcap || !isalpha((unsigned char)*p)) return false;   // claude-3-5-haiku: old naming, no effort
    memcpy(fam, p, (size_t)(d - p)); fam[d - p] = 0;
    if (!isdigit((unsigned char)d[1])) return false;           // claude-3-5-sonnet-... (old naming): no effort
    char *e; long a = strtol(d + 1, &e, 10); long b = 0;
    if (*e == '-' && isdigit((unsigned char)e[1])) { char *f; long v = strtol(e + 1, &f, 10); if (v < 100) b = v; }
    *maj = (int)a; *min = (int)b;
    return true;
}
// The effort actually sent for `model` (NULL = the model takes none), clamped to the levels it accepts:
// Fable/Mythos and every 5.x take all five; Opus/Sonnet 4.6 have no xhigh; Opus 4.5 only low..high;
// Haiku 4.5, Sonnet 4.5 and older reject the field.
static const char *anth_effort_for(const char *model, const char *want, bool *thinks)
{
    char fam[12]; int maj = 0, min = 0; *thinks = false;
    if (!anth_model_ver(model, fam, sizeof fam, &maj, &min)) return NULL;
    int ei = anth_effort_index(want);
    bool top = !strcmp(fam, "fable") || !strcmp(fam, "mythos");
    if (top || maj >= 5) { *thinks = true; return ANTH_EFFORTS[ei]; }   // adaptive thinking on by default
    if (maj != 4) return NULL;
    if (!strcmp(fam, "opus") && min >= 7) return ANTH_EFFORTS[ei];
    if ((!strcmp(fam, "opus") || !strcmp(fam, "sonnet")) && min == 6) return ANTH_EFFORTS[ei == 3 ? 2 : ei];
    if (!strcmp(fam, "opus") && min == 5) return ANTH_EFFORTS[ei > 2 ? 2 : ei];
    return NULL;
}

// Build an Anthropic /v1/messages request body: {model,max_tokens,stream,output_config?,system?,messages[]}.
// `system` is a TOP-LEVEL field (not a message). Prior `turns` (oldest to newest) become real
// user/assistant messages; only complete turns (both q and a) are emitted so the alternation stays valid.
// The final user message is `user`. Returns a malloc'd JSON string (caller frees), or NULL.
static char *anthropic_body(const char *model, const char *sys, const anima_turn_t *turns,
                            int nturns, const char *user, int max_tokens, const char *effort)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddNumberToObject(req, "max_tokens", max_tokens);
    cJSON_AddBoolToObject(req, "stream", true);
    if (effort) { cJSON *oc = cJSON_AddObjectToObject(req, "output_config"); cJSON_AddStringToObject(oc, "effort", effort); }
    if (sys && sys[0]) cJSON_AddStringToObject(req, "system", sys);
    cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
    for (int i = 0; i < nturns && turns; i++) {
        if (!turns[i].q || !turns[i].q[0] || !turns[i].a || !turns[i].a[0]) continue;  // keep strict alternation
        cJSON *mu = cJSON_CreateObject(); cJSON_AddStringToObject(mu, "role", "user");      cJSON_AddStringToObject(mu, "content", turns[i].q); cJSON_AddItemToArray(msgs, mu);
        cJSON *ma = cJSON_CreateObject(); cJSON_AddStringToObject(ma, "role", "assistant"); cJSON_AddStringToObject(ma, "content", turns[i].a); cJSON_AddItemToArray(msgs, ma);
    }
    cJSON *m2 = cJSON_CreateObject(); cJSON_AddStringToObject(m2, "role", "user"); cJSON_AddStringToObject(m2, "content", user); cJSON_AddItemToArray(msgs, m2);
    char *body = cJSON_PrintUnformatted(req); cJSON_Delete(req);
    return body;
}

// SSE accumulator: one line at a time; only "data:" lines carrying a text_delta (or the final
// message_delta) are parsed, a few hundred bytes of cJSON each; message_stop ends it, an error event fails it.
typedef struct {
    char  *txt; size_t len, cap;    // the answer so far (malloc'd, grows); cap = chars the caller wants at most
    size_t ll; bool skip;           // current line length / line too long (skipped whole)
    bool   done, err;
    char   stop[24];                // stop_reason from message_delta ("end_turn", "max_tokens", "refusal"...)
    char   line[1536];
} anth_sse_t;

static void anth_sse_line(anth_sse_t *s)
{
    s->line[s->ll] = 0;
    if (strncmp(s->line, "data:", 5)) return;
    const char *d = s->line + 5; while (*d == ' ') d++;
    if (strstr(d, "\"message_stop\"")) { s->done = true; return; }
    if (strstr(d, "\"type\":\"error\"")) { ESP_LOGW(TAG, "Claude stream error: %.200s", d); s->err = true; return; }
    bool text = strstr(d, "\"text_delta\"") != NULL, mdelta = !text && strstr(d, "\"message_delta\"") != NULL;
    if (!text && !mdelta) return;                                // pings, thinking/signature deltas, block starts
    cJSON *o = cJSON_Parse(d); if (!o) return;
    cJSON *dl = cJSON_GetObjectItem(o, "delta");
    if (text) {
        cJSON *t = dl ? cJSON_GetObjectItem(dl, "text") : NULL;
        if (cJSON_IsString(t) && t->valuestring[0] && s->len < s->cap) {
            size_t l = strlen(t->valuestring);
            if (s->len + l > s->cap) l = s->cap - s->len;
            char *n = realloc(s->txt, s->len + l + 1);
            if (n) { s->txt = n; memcpy(s->txt + s->len, t->valuestring, l); s->len += l; s->txt[s->len] = 0; }
        }
    } else {
        cJSON *sr = dl ? cJSON_GetObjectItem(dl, "stop_reason") : NULL;
        if (cJSON_IsString(sr)) snprintf(s->stop, sizeof s->stop, "%s", sr->valuestring);
    }
    cJSON_Delete(o);
}
static void anth_sse_feed(anth_sse_t *s, const char *b, int n)
{
    for (int i = 0; i < n && !s->done && !s->err; i++) {
        char ch = b[i];
        if (ch == '\n') { if (!s->skip) anth_sse_line(s); s->ll = 0; s->skip = false; }
        else if (ch == '\r') { }
        else if (s->ll < sizeof s->line - 1) s->line[s->ll++] = ch;
        else s->skip = true;
    }
}


// ===========================================================================
// The full event stream (Nucleo Code's agent, the LLM service of ABI 9): text, TOOL CALLS, usage and the stop
// reason, delivered through callbacks as they complete. A tool call's input arrives as input_json_delta
// pieces; they are joined in a buffer the caller gives (bounded: an input that does not fit is reported with
// json == NULL so the agent can answer "too large, write it in parts"). Server tools (web search) run on
// Anthropic's side: their blocks are skipped here.
// ===========================================================================
typedef struct { uint32_t in, out, cache_read, cache_write; } anth_usage_t;
typedef struct {
    void (*text)(void *op, const char *s, size_t n);
    void (*tool)(void *op, const char *id, const char *name, const char *json, size_t n);   // json NULL = too large
    void (*done)(void *op, const char *stop_reason, const anth_usage_t *u);
    void (*error)(void *op, const char *msg);
    void (*thinking)(void *op, const char *signature);        // a finished thinking block (text omitted): pass it back
} anth_cb_t;
typedef struct {
    anth_cb_t cb; void *op;
    char  *tj; size_t tjlen, tjcap; bool tjover;   // the current tool call's input (caller's buffer)
    int    tool_index;                              // content block index of the open tool call, -1 = none
    int    think_index;                             // ... of the open thinking block, -1 = none
    size_t siglen; bool sigover;
    char   sig[2048];                               // its signature (the reasoning stays on Anthropic's side)
    char   tid[48], tname[48];
    char   stop[24];
    anth_usage_t usage;
    bool   done, err, skip;
    size_t ll;
    char   line[4096];
} anth_stream_t;

static void anth_stream_init(anth_stream_t *s, const anth_cb_t *cb, void *op, char *tool_buf, size_t tool_cap)
{
    memset(s, 0, sizeof *s);
    if (cb) s->cb = *cb;
    s->op = op; s->tj = tool_buf; s->tjcap = tool_cap; s->tool_index = -1; s->think_index = -1;
}
static uint32_t anth_u32(cJSON *o, const char *k) { cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL; return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0; }

static void anth_stream_line(anth_stream_t *s)
{
    s->line[s->ll] = 0;
    if (strncmp(s->line, "data:", 5)) return;
    const char *d = s->line + 5; while (*d == ' ') d++;
    cJSON *o = cJSON_Parse(d); if (!o) return;
    cJSON *ty = cJSON_GetObjectItem(o, "type");
    const char *t = cJSON_IsString(ty) ? ty->valuestring : "";
    if (!strcmp(t, "content_block_delta")) {
        cJSON *dl = cJSON_GetObjectItem(o, "delta"), *dt = dl ? cJSON_GetObjectItem(dl, "type") : NULL;
        const char *k = cJSON_IsString(dt) ? dt->valuestring : "";
        if (!strcmp(k, "text_delta")) {
            cJSON *x = cJSON_GetObjectItem(dl, "text");
            if (cJSON_IsString(x) && s->cb.text) s->cb.text(s->op, x->valuestring, strlen(x->valuestring));
        } else if (!strcmp(k, "signature_delta") && s->think_index >= 0) {
            cJSON *x = cJSON_GetObjectItem(dl, "signature");
            if (cJSON_IsString(x)) {
                size_t n = strlen(x->valuestring);
                if (s->siglen + n < sizeof s->sig) { memcpy(s->sig + s->siglen, x->valuestring, n); s->siglen += n; s->sig[s->siglen] = 0; }
                else s->sigover = true;
            }
        } else if (!strcmp(k, "input_json_delta") && s->tool_index >= 0) {
            cJSON *x = cJSON_GetObjectItem(dl, "partial_json");
            if (cJSON_IsString(x)) {
                size_t n = strlen(x->valuestring);
                if (s->tj && s->tjlen + n < s->tjcap) { memcpy(s->tj + s->tjlen, x->valuestring, n); s->tjlen += n; s->tj[s->tjlen] = 0; }
                else s->tjover = true;
            }
        }
    } else if (!strcmp(t, "content_block_start")) {
        cJSON *b = cJSON_GetObjectItem(o, "content_block"), *bt = b ? cJSON_GetObjectItem(b, "type") : NULL;
        if (cJSON_IsString(bt) && !strcmp(bt->valuestring, "thinking")) {
            s->think_index = (int)anth_u32(o, "index"); s->siglen = 0; s->sig[0] = 0; s->sigover = false;
        } else if (cJSON_IsString(bt) && !strcmp(bt->valuestring, "tool_use")) {
            cJSON *id = cJSON_GetObjectItem(b, "id"), *nm = cJSON_GetObjectItem(b, "name");
            s->tool_index = (int)anth_u32(o, "index");
            snprintf(s->tid, sizeof s->tid, "%s", cJSON_IsString(id) ? id->valuestring : "");
            snprintf(s->tname, sizeof s->tname, "%s", cJSON_IsString(nm) ? nm->valuestring : "");
            s->tjlen = 0; s->tjover = false; if (s->tj && s->tjcap) s->tj[0] = 0;
        }
    } else if (!strcmp(t, "content_block_stop")) {
        if (s->think_index >= 0 && (int)anth_u32(o, "index") == s->think_index) {
            if (s->cb.thinking && s->siglen && !s->sigover) s->cb.thinking(s->op, s->sig);
            s->think_index = -1;
        }
        if (s->tool_index >= 0 && (int)anth_u32(o, "index") == s->tool_index) {
            if (s->cb.tool) {
                if (s->tjover) s->cb.tool(s->op, s->tid, s->tname, NULL, 0);
                else s->cb.tool(s->op, s->tid, s->tname, s->tjlen ? s->tj : "{}", s->tjlen ? s->tjlen : 2);
            }
            s->tool_index = -1;
        }
    } else if (!strcmp(t, "message_start")) {
        cJSON *m = cJSON_GetObjectItem(o, "message"), *u = m ? cJSON_GetObjectItem(m, "usage") : NULL;
        s->usage.in = anth_u32(u, "input_tokens"); s->usage.cache_read = anth_u32(u, "cache_read_input_tokens");
        s->usage.cache_write = anth_u32(u, "cache_creation_input_tokens");
    } else if (!strcmp(t, "message_delta")) {
        cJSON *dl = cJSON_GetObjectItem(o, "delta"), *sr = dl ? cJSON_GetObjectItem(dl, "stop_reason") : NULL;
        if (cJSON_IsString(sr)) snprintf(s->stop, sizeof s->stop, "%s", sr->valuestring);
        cJSON *u = cJSON_GetObjectItem(o, "usage");
        if (u) { uint32_t out = anth_u32(u, "output_tokens"); if (out) s->usage.out = out; }
    } else if (!strcmp(t, "message_stop")) {
        s->done = true;
        if (s->cb.done) s->cb.done(s->op, s->stop[0] ? s->stop : "end_turn", &s->usage);
    } else if (!strcmp(t, "error")) {
        cJSON *e = cJSON_GetObjectItem(o, "error"), *msg = e ? cJSON_GetObjectItem(e, "message") : NULL;
        s->err = true;
        if (s->cb.error) s->cb.error(s->op, cJSON_IsString(msg) ? msg->valuestring : "stream error");
    }
    cJSON_Delete(o);
}
static void anth_stream_feed(anth_stream_t *s, const char *b, int n)
{
    for (int i = 0; i < n && !s->done && !s->err; i++) {
        char ch = b[i];
        if (ch == '\n') { if (!s->skip) anth_stream_line(s); s->ll = 0; s->skip = false; }
        else if (ch == '\r') { }
        else if (s->ll < sizeof s->line - 1) s->line[s->ll++] = ch;
        else s->skip = true;
    }
}
