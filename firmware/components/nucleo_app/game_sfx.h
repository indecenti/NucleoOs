// game_sfx.h — ONE shared, RAM-safe sound-effect engine for the native games.
//
// Every native game used to copy ~80 lines of identical boilerplate (DIRR, g_audio, sfx_name,
// build_voices, ensure_dirs, sfx, sfx_cache_check, presynth). That copy-paste drifted (some games
// versioned the cache, some didn't; some synthesised inline, some only played from a pre-built cache)
// and every silent failure mode had to be re-debugged per game. This module is the single source of
// truth: a game declares its cue table once and calls game_sfx_ensure()/game_sfx_play().
//
// Storage model (resolved in this order, first hit wins):
//   1) <dir>/pack/<name>.wav   — the deployed arcade pack (rendered on the PC by tools/sfx-gen, zero CPU)
//   2) <dir>/sfx/<name>.wav    — the on-device synth cache, filled by game_sfx_ensure()
//   3) a short procedural tone — when neither exists yet (or the play is dropped)
// The cache is versioned: bump game_sfx_t.ver to wipe + rebuild after changing a recipe.
//
// NEVER BLOCKS THE GAME: synthesis runs only in game_sfx_ensure() (on_enter), under a time budget and
// feeding the Task WDT between cues; what does not fit is built on the next launches. game_sfx_play()
// never synthesizes — a cue not cached yet plays a tone. (Bulk synthesis at launch rebooted Tanks on a
// card without its cache: ~200 ms per cue x 40 cues on the 8 s-WDT app task.) With a deployed pack the
// cache is not built at all.
//
// SOLIDITY: a cue NEVER goes silently mute. If the SD write fails OR nucleo_audio drops the play
// (RAM fragmentation / mic gate), the cue degrades to a short procedural tone derived from its first
// voice — the player still gets feedback. Pair this with NX_NET_APP (exclusive mode) so the WAV player
// task reliably gets its contiguous stack. The voice scratch is heap, only while it is used (the games
// run on the 8 KB main task: 40 voices on the stack were 640 B of it).
//
// Header-only (static inline) — no .c, no extra CMake entry; include it from each app .cpp.
#pragma once
#include "nucleo_audio.h"      // play / stop / is_playing / tone
#include "notify_synth.h"      // notify_voice_t + notify_synth_voices_wav (+ notify__voice)
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fill v[] with the voices for cue `id`, return the voice count (0 = no such cue). v is sized
// GAME_SFX_MAXV, generous enough for the busiest recipe (Poker's two-part Ode to Joy = 28 voices).
#define GAME_SFX_MAXV 40
#define GAME_SFX_ENSURE_MS 1200            // synthesis budget per launch; the rest waits for the next one
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
// A playable WAV: present and longer than its 44-byte header (a reset mid-write leaves a 0-byte file).
static inline bool game_sfx__wav(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 44; }

// The tone a cue degrades to: its first voice's pitch.
static inline void game_sfx__tone(const game_sfx_t *g, int id)
{
    notify_voice_t *v = (notify_voice_t *)malloc(sizeof(notify_voice_t) * GAME_SFX_MAXV);
    if (!v) return;
    int nv = g->recipe(id, v);
    if (nv > 0) nucleo_audio_tone((int)v[0].hz, 45, 55);
    free(v);
}

// Prepare the sounds: create dirs, honour the version gate, then (no pack deployed) synthesize missing
// cues into the cache for at most GAME_SFX_ENSURE_MS. Cheap once everything is cached. Call in on_enter.
static inline void game_sfx_ensure(const game_sfx_t *g)
{
    if (!g || !g->dir || game_sfx__off(g)) return;
    char p[120];
    snprintf(p, sizeof p, "%s/pack/%s.wav", g->dir, g->name(1));
    if (game_sfx__wav(p)) return;                                  // a deployed pack covers every cue
    mkdir("/sd", 0777); mkdir("/sd/data", 0777); mkdir(g->dir, 0777);
    snprintf(p, sizeof p, "%s/sfx", g->dir); mkdir(p, 0777);   // NB: 'd' is a display macro (app_gfx.h) — don't use it as a local

    // version gate — wipe the cache when a recipe changed (ver bumped)
    snprintf(p, sizeof p, "%s/sfx/ver.bin", g->dir);
    int ver = 0; FILE *f = fopen(p, "rb");
    if (f) { if (fread(&ver, sizeof ver, 1, f) != 1) ver = 0; fclose(f); }
    if (ver != g->ver) {
        for (int id = 1; id <= g->count; id++) { char q[120]; snprintf(q, sizeof q, "%s/sfx/%s.wav", g->dir, g->name(id)); remove(q); }
        f = fopen(p, "wb"); if (f) { int vv = g->ver; fwrite(&vv, sizeof vv, 1, f); fclose(f); }
    }

    notify_voice_t *v = NULL;
    int64_t until = esp_timer_get_time() + (int64_t)GAME_SFX_ENSURE_MS * 1000;
    int rate = game_sfx__rate(g);
    for (int id = 1; id <= g->count && esp_timer_get_time() < until; id++) {
        snprintf(p, sizeof p, "%s/sfx/%s.wav", g->dir, g->name(id));
        if (game_sfx__wav(p)) continue;
        if (!v && !(v = (notify_voice_t *)malloc(sizeof(notify_voice_t) * GAME_SFX_MAXV))) break;
        int nv = g->recipe(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, rate);
        esp_task_wdt_reset();                                      // one cue can take a few hundred ms
    }
    free(v);
}

// Play cue `id`: pack, else cache, else a tone — never synthesis here (it would freeze the game). Small
// (non-important) cues are skipped while something is already playing so rapid events (laser/move) don't
// thrash the single audio channel.
static inline void game_sfx_play(const game_sfx_t *g, int id)
{
    if (!g || id <= 0 || game_sfx__off(g)) return;
    bool imp = g->important && g->important(id);
    if (!imp && nucleo_audio_is_playing()) return;
    const char *nm = g->name(id);
    char p[120];
    snprintf(p, sizeof p, "%s/pack/%s.wav", g->dir, nm);
    if (!game_sfx__wav(p)) snprintf(p, sizeof p, "%s/sfx/%s.wav", g->dir, nm);
    if (imp) nucleo_audio_stop();
    if (!game_sfx__wav(p) || nucleo_audio_play(p) != ESP_OK) game_sfx__tone(g, id);
}

#ifdef __cplusplus
}
#endif
