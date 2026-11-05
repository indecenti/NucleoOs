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
    uint32_t timeout_ms;                                // wall clock for the whole run (0 = 10 s, CODE_NO_LIMIT = none)
    // The screen (docs/nucleo-code.md 4.5): scr_w = 0 means none, and gfx calls throw. The canvas exists only once a
    // script draws; every flush hands the changed rows to `blit` (NULL = drawn but shown nowhere, e.g. a host test).
    int scr_w, scr_h;
    void (*blit)(void *op, int x, int y, int w, int h, const uint16_t *rgb565);
    const char *(*key)(void *op);                       // the next key pressed ("up", "enter", "a"...), NULL = none
    bool (*key_down)(void *op, const char *name);       // is it held right now
    bool (*touch)(void *op, int *x, int *y, bool *down);// the next touch sample on the panel, false = none
    const char *shot;                                   // a script that drew: its last frame as a PNG here at the end
} code_env_t;
#define CODE_NO_LIMIT 0xFFFFFFFFu

typedef struct {
    bool     ok;                // ran to the end without an exception
    bool     timeout, cancelled, oom;
    char     error[192];        // "TypeError: x is undefined" (empty when ok)
    char     where[64];         // "main.js:31" when the engine knows it
    uint32_t ms;                // wall clock of the run
    uint32_t live_max;          // the most memory live after a GC (bytes): what this script really needs
    uint32_t used_end;          // in use at the end, before the final GC (bytes)
    uint32_t arena;             // the arena it was given (bytes)
    bool     drew;              // it used the screen
    bool     shot_ok;           // env->shot was written
    uint32_t frames;            // gfx.flush() calls
    int      scr_w, scr_h;      // the canvas it drew on
} code_result_t;

// Run `src` (UTF-8, `len` bytes, named `filename` in errors) in `arena` (`arena_size` bytes, 8-aligned).
// Returns res->ok.
bool code_run(void *arena, size_t arena_size, const char *src, size_t len, const char *filename,
              const code_env_t *env, code_result_t *res);

// Resolve a script path against env->root, refusing anything that escapes it ("..", a drive, a leading
// separator, backslashes). Returns false when refused. Exposed for the agent's tools and the host gate.
bool code_path(const char *root, const char *rel, char *out, size_t cap);

// The model's markdown made readable on the device's fonts (240x135, ASCII): **bold**, __bold__ and `code` lose
// their marks, ``` fences become two-space indented lines, "# Title" loses its hashes, [text](url) keeps the
// text. The model is asked for plain text; this is the guarantee. Returns the length written to `out`.
size_t code_md_plain(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
