// game_sfx.h — ONE shared, RAM-safe sound-effect engine for the native games.
//
// Every native game used to copy ~80 lines of identical boilerplate (DIRR, g_audio, sfx_name,
// build_voices, ensure_dirs, sfx, sfx_cache_check, presynth). That copy-paste drifted (some games
// versioned the cache, some didn't; some synthesised inline, some only played from a pre-built cache)
// and every silent failure mode had to be re-debugged per game. This module is the single source of
// truth: a game declares its cue table once and calls game_sfx_ensure()/game_sfx_play(). Games that keep
// their own sfx() (custom overrides, fanfare guards) resolve and degrade through the same helpers below.
//
// Storage model (resolved in this order, first hit wins):
//   1) <dir>/pack/<name>.wav   — the deployed WAV pack (baked on the PC: tools/sfx-gen), zero CPU
//   2) <dir>/sfx/<name>.wav    — the legacy synth cache older firmware wrote on the device
//   3) neither                 — a short square tone from the cue's first voice (important cues only)
// NOTHING is synthesized on the device any more. Bulk-synthesizing a cue table in on_enter (two float
// passes + an SD write per cue) ran past the 8 s Task WDT the app task is subscribed to and rebooted the
// device on any SD without the cache, leaving a truncated 0-byte WAV behind; synth-on-miss froze the game
// mid-frame the same way. The recipes stay as the source the PC bakes the pack from, and for the tone.
//
// SOLIDITY: an important cue never goes silently mute. With no WAV, or when nucleo_audio drops the play
// (RAM fragmentation / mic gate), it degrades to that short tone — the player still gets feedback. Small
// cues stay silent on a miss: the tone BLOCKS for its length, and a beep on every rapid cue (laser, move)
// would stall the frame. Pair this with NX_NET_APP (exclusive mode) so the WAV player task reliably gets
// its contiguous stack.
//
// Header-only (static inline) — no .c, no extra CMake entry; include it from each app .cpp.
#pragma once
#include "nucleo_audio.h"      // play / stop / is_playing / tone
#include "notify_synth.h"      // notify_voice_t (+ notify__voice) for the recipes
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
    game_sfx_recipe_fn  recipe;     // id -> voices (the bake source; on the device only the tone uses it)
    int                 count;      // number of cues (ids 1..count)
    int                 ver;        // legacy-cache version — bump to wipe a stale on-device cache
    int                 rate;       // unused on the device (packs carry their rate); kept for the table shape
    bool              (*important)(int id);  // cue interrupts current playback (NULL -> none important)
    const int          *enabled;    // optional pointer to the game's audio-on flag (NULL -> always on)
} game_sfx_t;

static inline bool game_sfx__off(const game_sfx_t *g)  { return g->enabled && !*g->enabled; }

// A playable WAV = present AND longer than its 44-byte header. A cache file cut short by a reset mid-write
// (0 bytes) must not count as "there".
static inline bool game_sfx_wav_ok(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 44;
}

// Resolve cue `nm` under `dir` into out: the pack first, then the legacy cache. false = no playable WAV.
static inline bool game_sfx_find(const char *dir, const char *nm, char *out, size_t cap)
{
    snprintf(out, cap, "%s/pack/%s.wav", dir, nm);
    if (game_sfx_wav_ok(out)) return true;
    snprintf(out, cap, "%s/sfx/%s.wav", dir, nm);
    return game_sfx_wav_ok(out);
}

// The no-WAV fallback: a short square tone at the pitch of the cue's first voice. Blocks ~45 ms — call it
// for important cues only. No-op while a track plays.
static inline void game_sfx_tone(game_sfx_recipe_fn recipe, int id)
{
    notify_voice_t v[GAME_SFX_MAXV];
    if (recipe && recipe(id, v) > 0) nucleo_audio_tone((int)v[0].hz, 45, 55);
}

// Prepare the game's SD folder and drop a stale legacy cache (ver bumped). Cheap: no synthesis, at most
// `count` removes once per version. Call from the app's on_enter.
static inline void game_sfx_ensure(const game_sfx_t *g)
{
    if (!g || !g->dir) return;
    mkdir("/sd", 0777); mkdir("/sd/data", 0777); mkdir(g->dir, 0777);

    // version gate — a cache synthesized from an older recipe must not shadow the silence/tone fallback
    char vp[120]; snprintf(vp, sizeof vp, "%s/sfx/ver.bin", g->dir);   // NB: 'd' is a display macro (app_gfx.h) — don't use it as a local
    int ver = 0; FILE *f = fopen(vp, "rb");
    if (f) { if (fread(&ver, sizeof ver, 1, f) != 1) ver = 0; fclose(f); }
    if (ver != g->ver) {
        for (int id = 1; id <= g->count; id++) { char p[120]; snprintf(p, sizeof p, "%s/sfx/%s.wav", g->dir, g->name(id)); remove(p); }
        char sub[112]; snprintf(sub, sizeof sub, "%s/sfx", g->dir); mkdir(sub, 0777);
        f = fopen(vp, "wb"); if (f) { int vv = g->ver; fwrite(&vv, sizeof vv, 1, f); fclose(f); }
    }
}

// Play cue `id` from the pack (or the legacy cache), async. With no WAV an important cue degrades to the
// tone; a small one stays silent. Small (non-important) cues are also skipped while something is already
// playing so rapid events (laser/move) don't thrash the single audio channel.
static inline void game_sfx_play(const game_sfx_t *g, int id)
{
    if (!g || id <= 0 || game_sfx__off(g)) return;
    bool imp = g->important && g->important(id);
    if (!imp && nucleo_audio_is_playing()) return;

    char p[120];
    if (!game_sfx_find(g->dir, g->name(id), p, sizeof p)) {
        if (imp) { nucleo_audio_stop(); game_sfx_tone(g->recipe, id); }
        return;
    }
    if (imp) nucleo_audio_stop();
    if (nucleo_audio_play(p) != ESP_OK) game_sfx_tone(g->recipe, id);   // dropped by RAM/mic gate -> still give feedback
}

#ifdef __cplusplus
}
#endif
