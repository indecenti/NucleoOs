// code_agent.h: Nucleo Code's agent (docs/nucleo-code.md 4.2, 6): the Claude Code loop on the device. The
// model is called through a transport the host gives (the firmware's LLM service on the device, curl on the
// PC), the tools act on the workspace (the Anthropic text editor, run_js in MicroQuickJS, search, todo), and
// every write can be put to the user first. Platform-neutral; the host gate tools/code/agent-host.c drives it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*agent_bytes_fn)(void *op, const char *b, int n);

typedef struct {
    // POST `body` to /v1/messages (the transport adds the key and the headers), stream the raw response
    // bytes to `bytes`, return the HTTP status (< 0 = no connection). `err` gets an error body's head.
    int  (*post)(void *op, const char *body, size_t len, agent_bytes_fn bytes, void *bop, char *err, size_t errcap);
    void *post_op;
    // UI (all optional)
    void (*on_text)(void *op, const char *s, size_t n);                        // the model's words, as they stream
    void (*on_tool)(void *op, const char *name, const char *summary);          // a tool about to run
    void (*on_tool_done)(void *op, const char *name, bool ok, const char *summary);
    bool (*approve)(void *op, const char *name, const char *summary);          // a write/run; NULL = allowed
    void (*on_todo)(void *op, const char *todo_json);                          // the plan changed
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
} agent_cfg_t;

typedef struct {
    uint32_t calls, tools, tool_errors;
    uint64_t in, out, cache_read, cache_write;
    double   usd;               // at the model's list price
    char     stop[24];          // the last stop reason
    char     error[160];        // why the last turn stopped early ("" = it finished)
} agent_stats_t;

typedef struct agent agent_t;

agent_t *agent_new(const agent_cfg_t *cfg, const agent_io_t *io);
// One user turn: runs model calls and tools until the model ends its turn, a limit is reached or the user
// cancels. Returns true when the model ended normally.
bool agent_turn(agent_t *a, const char *user_text);
const agent_stats_t *agent_stats(const agent_t *a);
void agent_free(agent_t *a);

#ifdef __cplusplus
}
#endif
