// code_agent.h: Nucleo Code's agent (docs/nucleo-code.md 4.2, 6): the Claude Code loop on the device.
// The conversation lives on the SD card (<root>/.code/session.jsonl) and each request is written to a file
// (<root>/.code/request.json) that the transport sends, so RAM holds one message at a time however long the
// session grows. A STATE MACHINE the UI drives (the device's app polls the firmware's LLM service each tick; the
// PC bench calls agent_turn with a blocking transport): the model is called, its tools act on the workspace
// (the Anthropic text editor, run_js in MicroQuickJS, search, todo), and a write can wait for the user's yes.
// Platform-neutral; host gates: tools/code/agent-host.c and tools/code/agent-bench.mjs.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*agent_bytes_fn)(void *op, const char *b, int n);

typedef struct {
    // blocking transport for agent_turn (the PC bench): POST the request FILE to /v1/messages (the transport adds the
    // key and the headers), stream the raw response bytes to `bytes`, return the HTTP status (< 0 = no connection).
    int  (*post_file)(void *op, const char *path, agent_bytes_fn bytes, void *bop, char *err, size_t errcap);
    void *post_op;
    // UI (all optional)
    void (*on_text)(void *op, const char *s, size_t n);                        // the model's words, as they stream
    void (*on_tool)(void *op, const char *name, const char *summary);          // a tool about to run
    void (*on_tool_done)(void *op, const char *name, bool ok, const char *summary);
    bool (*approve)(void *op, const char *name, const char *summary);          // agent_turn only; NULL = yes
    void (*on_todo)(void *op, const char *todo);                               // the plan changed ("[x] a\n[>] b\n")
    void *ui_op;
    int64_t (*now_ms)(void);
    void (*sleep_ms)(uint32_t ms);
    bool (*cancel)(void *op);                                                  // Esc: stop the turn now
} agent_io_t;

typedef struct {
    const char *root;           // the workspace (absolute); every tool path resolves under it
    const char *model;          // "claude-sonnet-5-5"
    const char *effort;         // "low".."max" (NULL = medium)
    const char *lang;           // the OS language the answers are written in ("Italian"...; NULL = the user's)
    int         max_calls;      // model calls per user turn (0 = 30)
    int         max_tokens;     // per model call (0 = 16000)
    void       *arena;          // run_js engine arena (only used while a script runs)
    size_t      arena_size;
    char       *toolbuf;        // one tool call's input as it streams (a whole file for `create`)
    size_t      toolcap;
    bool        ask_writes;     // writes and runs wait for agent_answer (mode Ask); false = Auto
} agent_cfg_t;

typedef struct {
    uint32_t calls, tools, tool_errors;
    uint64_t in, out, cache_read, cache_write;
    uint32_t last_context;      // tokens the last request carried (context use)
    double   usd;               // at the model's list price
    char     stop[24];          // the last stop reason
    char     error[200];        // why the turn stopped early ("" = it finished)
} agent_stats_t;

typedef enum { AG_IDLE = 0, AG_SEND, AG_RECV, AG_TOOLS, AG_ASK, AG_DONE, AG_ERROR } agent_state_t;

typedef struct agent agent_t;

agent_t *agent_new(const agent_cfg_t *cfg, const agent_io_t *io);
void agent_free(agent_t *a);

// -- driven by the UI --
bool agent_submit(agent_t *a, const char *user_text);   // the user's message -> AG_SEND (request file written)
const char *agent_request_path(const agent_t *a);       // AG_SEND: the file to POST
void agent_begin_response(agent_t *a);                  // AG_SEND -> AG_RECV
void agent_feed(agent_t *a, const char *b, int n);      // AG_RECV: response bytes as they arrive
void agent_end_response(agent_t *a, int status, const char *err);   // -> AG_TOOLS / AG_DONE / AG_ERROR
agent_state_t agent_step(agent_t *a);                   // AG_TOOLS: runs ONE tool (redraw between) -> AG_ASK / AG_SEND
const char *agent_question(const agent_t *a);           // AG_ASK: "create pong.js"
void agent_answer(agent_t *a, bool yes);                // AG_ASK -> AG_TOOLS
void agent_cancel(agent_t *a, const char *why);         // any state -> AG_ERROR, history kept valid
agent_state_t agent_state(const agent_t *a);
void agent_reset(agent_t *a);                           // a new session (the history file is removed)

// -- blocking, for the bench --
bool agent_turn(agent_t *a, const char *user_text);

const agent_stats_t *agent_stats(const agent_t *a);
const char *agent_todo(const agent_t *a);

#ifdef __cplusplus
}
#endif
