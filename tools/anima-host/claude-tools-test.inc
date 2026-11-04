// claude-tools-test.inc: the full event stream of anima_claude.h (anth_stream_t), included by
// claude-stream-test.c: text, a tool call whose input arrives in pieces, usage with cache reads, the stop
// reason, and an input larger than the caller's buffer (json == NULL).
static char T_txt[256], T_id[48], T_name[48], T_json[256], T_stop[24];
static anth_usage_t T_u; static int T_ntool, T_err;
static void t_text(void *op, const char *s, size_t n) { (void)op; strncat(T_txt, s, n); }
static void t_tool(void *op, const char *id, const char *name, const char *json, size_t n)
{ (void)op; T_ntool++; snprintf(T_id, sizeof T_id, "%s", id); snprintf(T_name, sizeof T_name, "%s", name); if (json) snprintf(T_json, sizeof T_json, "%.*s", (int)n, json); else snprintf(T_json, sizeof T_json, "NULL"); }
static void t_done(void *op, const char *st, const anth_usage_t *u) { (void)op; snprintf(T_stop, sizeof T_stop, "%s", st); T_u = *u; }
static void t_error(void *op, const char *m) { (void)op; (void)m; T_err++; }

static const char TOOL_STREAM[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"usage\":{\"input_tokens\":1200,\"cache_read_input_tokens\":900,\"cache_creation_input_tokens\":0,\"output_tokens\":1}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Creo il file.\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"str_replace_based_edit_tool\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"command\\\": \\\"cre\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"ate\\\", \\\"path\\\": \\\"a.js\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":2}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":57}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static void test_tool_stream(void)
{
    static char tb[256];
    anth_stream_t *as = calloc(1, sizeof *as);
    anth_cb_t cb = { t_text, t_tool, t_done, t_error, NULL };
    size_t L = strlen(TOOL_STREAM);
    anth_stream_init(as, &cb, NULL, tb, sizeof tb);
    for (size_t i = 0; i < L; i += 5) anth_stream_feed(as, TOOL_STREAM + i, (int)(L - i < 5 ? L - i : 5));
    printf("tool stream: text=[%s] tool=%s/%s json=%s stop=%s in=%u cache=%u out=%u\n", T_txt, T_id, T_name, T_json, T_stop,
           (unsigned)T_u.in, (unsigned)T_u.cache_read, (unsigned)T_u.out);
    CHECK(!strcmp(T_txt, "Creo il file.") && T_ntool == 1 && !strcmp(T_id, "toolu_01") && !strcmp(T_name, "str_replace_based_edit_tool"));
    CHECK(!strcmp(T_json, "{\"command\": \"create\", \"path\": \"a.js\"}") && !strcmp(T_stop, "tool_use"));
    CHECK(T_u.in == 1200 && T_u.cache_read == 900 && T_u.out == 57 && !T_err);
    anth_stream_init(as, &cb, NULL, tb, 8); T_ntool = 0;          // input larger than the buffer
    anth_stream_feed(as, TOOL_STREAM, (int)L);
    printf("overflow: ntool=%d json=%s\n", T_ntool, T_json);
    CHECK(T_ntool == 1 && !strcmp(T_json, "NULL"));
    free(as);
}
