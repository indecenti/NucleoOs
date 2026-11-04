// code_rt.h: Nucleo Code's script runtime (docs/nucleo-code.md 4.3). One MicroQuickJS context in a
// caller-given arena, run once and dropped: nothing of the engine stays alive between runs, so the TLS
// session of the next model request has the heap back. Platform-neutral (no ESP-IDF here): the host
// gate tools/code/rt-check.mjs compiles this same file.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *root;                                   // workspace root: every fs path resolves under it
    void (*out)(void *op, const char *s, size_t n);     // where print / console.log / errors go
    void *op;
    int64_t (*now_ms)(void);                            // monotonic clock
    void (*sleep_ms)(uint32_t ms);                      // sys.sleep (the host may poll its keys there)
    bool (*cancel)(void *op);                           // true = stop now (Esc); may be NULL
    uint32_t timeout_ms;                                // wall clock for the whole run (0 = 10 s)
} code_env_t;

typedef struct {
    bool     ok;                // ran to the end without an exception
    bool     timeout, cancelled, oom;
    char     error[192];        // "TypeError: x is undefined" (empty when ok)
    char     where[64];         // "main.js:31" when the engine knows it
    uint32_t ms;                // wall clock of the run
    uint32_t live_max;          // the most memory live after a GC (bytes): what this script really needs
    uint32_t used_end;          // in use at the end, before the final GC (bytes)
    uint32_t arena;             // the arena it was given (bytes)
} code_result_t;

// Run `src` (UTF-8, `len` bytes, named `filename` in errors) in `arena` (`arena_size` bytes, 8-aligned).
// Returns res->ok.
bool code_run(void *arena, size_t arena_size, const char *src, size_t len, const char *filename,
              const code_env_t *env, code_result_t *res);

// Resolve a script path against env->root, refusing anything that escapes it ("..", a drive, a leading
// separator, backslashes). Returns false when refused. Exposed for the agent's tools and the host gate.
bool code_path(const char *root, const char *rel, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
