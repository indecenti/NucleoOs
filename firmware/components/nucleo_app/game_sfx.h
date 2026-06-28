// game_sfx.h — ONE shared, RAM-safe sound-effect engine for the native games.
//
// Every native game used to copy ~80 lines of identical boilerplate (DIRR, g_audio, sfx_name,
// build_voices, ensure_dirs, sfx, sfx_cache_check, presynth). That copy-paste drifted (some games
// versioned the cache, some didn't; some synthesised inline, some only played from a pre-built cache)
// and every silent failure mode had to be re-debugged per game. This module is the single source of
// truth: a game declares its cue table once and calls game_sfx_ensure()/game_sfx_play().
//
// Storage model (resolved in this order, first hit wins):
//   1) <dir>/pack/<name>.wav   — an optional deployed arcade pack (real chiptune, zero CPU)
//   2) <dir>/sfx/<name>.wav    — the on-device synth cache (built once by game_sfx_ensure)
//   3) synth-now into the cache — first play of a cue that isn't cached yet
// The cache is versioned: bump game_sfx_t.ver to wipe + rebuild after changing a recipe.
//
// SOLIDITY: a cue NEVER goes silently mute. If the SD write fails OR nucleo_audio drops the play
// (RAM fragmentation / mic gate), game_sfx_play degrades to a short procedural tone derived from the
// cue's first voice — the player still gets feedback. Pair this with NX_NET_APP (exclusive mode) so
// the WAV player task reliably gets its contiguous stack.
//
// Header-only (static inline) — no .c, no extra CMake entry; include it from each app .cpp.
#pragma once
#include "nucleo_audio.h"      // play / stop / is_playing / tone
#include "notify_synth.h"      // notify_voice_t + notify_synth_voices_wav (+ notify__voice)
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fill v[] with the voices for cue `id`, return the voice count (0 = no such cue). v is sized
// GAME_SFX_MAXV, generous enough for the busiest recipe (Poker's two-part Ode to Joy = 28 voices).
#define GAME_SFX_MAXV 40
typedef int (*game_sfx_recipe_fn)(int id, notify_voice_t *v);

typedef struct {
    const char         *dir;        // e.g. "/sd/data/yahtzee" (no trailing slash)
    const char       *(*name)(int id);   // id -> short cue name (filename stem)
    game_sfx_recipe_fn  recipe;     // id -> voices
    int                 count;      // number of cues (ids 1..count)
    int                 ver;        // cache version — bump to force a rebuild
    int                 rate;       // synth/play sample rate in Hz (0 -> 12000)
    bool              (*important)(int id);  // cue interrupts current playback (NULL -> none important)
    const int          *enabled;    // optional pointer to the game's audio-on flag (NULL -> always on)
} game_sfx_t;

static inline int  game_sfx__rate(const game_sfx_t *g) { return g->rate > 0 ? g->rate : 12000; }
static inline bool game_sfx__off(const game_sfx_t *g)  { return g->enabled && !*g->enabled; }

// Build the SD cache once: create dirs, honour the version gate, then synth any missing cue WAV.
// Cheap on later launches (everything already cached). Call from the app's on_enter.
static inline void game_sfx_ensure(const game_sfx_t *g)
{
    if (!g || !g->dir || game_sfx__off(g)) return;
    mkdir("/sd", 0777); mkdir("/sd/data", 0777); mkdir(g->dir, 0777);
    char sub[112]; snprintf(sub, sizeof sub, "%s/sfx", g->dir); mkdir(sub, 0777);   // NB: 'd' is a display macro (app_gfx.h) — don't use it as a local

    // version gate — wipe the cache when a recipe changed (ver bumped)
    char vp[120]; snprintf(vp, sizeof vp, "%s/sfx/ver.bin", g->dir);
    int ver = 0; FILE *f = fopen(vp, "rb");
    if (f) { if (fread(&ver, sizeof ver, 1, f) != 1) ver = 0; fclose(f); }
    if (ver != g->ver) {
        for (int id = 1; id <= g->count; id++) { char p[120]; snprintf(p, sizeof p, "%s/sfx/%s.wav", g->dir, g->name(id)); remove(p); }
        f = fopen(vp, "wb"); if (f) { int vv = g->ver; fwrite(&vv, sizeof vv, 1, f); fclose(f); }
    }

    notify_voice_t v[GAME_SFX_MAXV];
    int rate = game_sfx__rate(g);
    for (int id = 1; id <= g->count; id++) {
        char p[120]; snprintf(p, sizeof p, "%s/sfx/%s.wav", g->dir, g->name(id));
        FILE *e = fopen(p, "rb"); if (e) { fclose(e); continue; }
        int nv = g->recipe(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, rate);
    }
}

// Play cue `id`. Resolves pack -> cache -> synth-now, then plays async; degrades to a procedural
// tone if no WAV can be produced or the play is dropped. Small (non-important) cues are skipped while
// something is already playing so rapid events (laser/move) don't thrash the single audio channel.
static inline void game_sfx_play(const game_sfx_t *g, int id)
{
    if (!g || id <= 0 || game_sfx__off(g)) return;
    bool imp = g->important && g->important(id);
    if (!imp && nucleo_audio_is_playing()) return;

    int rate = game_sfx__rate(g);
    const char *nm = g->name(id);
    char p[120];

    // 1) deployed pack?
    snprintf(p, sizeof p, "%s/pack/%s.wav", g->dir, nm);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); }
    else {
        // 2) synth cache?
        snprintf(p, sizeof p, "%s/sfx/%s.wav", g->dir, nm);
        f = fopen(p, "rb");
        if (f) { fclose(f); }
        else {
            // 3) synth-now into the cache; on SD failure degrade to a tone
            notify_voice_t v[GAME_SFX_MAXV]; int nv = g->recipe(id, v);
            if (nv <= 0) return;
            if (notify_synth_voices_wav(v, nv, p, rate) != 0) {
                if (imp) nucleo_audio_stop();
                nucleo_audio_tone((int)v[0].hz, 45, 55);
                return;
            }
        }
    }

    if (imp) nucleo_audio_stop();
    if (nucleo_audio_play(p) != ESP_OK) {          // dropped by RAM/mic gate -> still give feedback
        notify_voice_t v[GAME_SFX_MAXV]; int nv = g->recipe(id, v);
        if (nv > 0) nucleo_audio_tone((int)v[0].hz, 45, 55);
    }
}

#ifdef __cplusplus
}
#endif
