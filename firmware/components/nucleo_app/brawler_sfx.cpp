// brawler_sfx.cpp — SCORRIBANDA: procedural sound effects.
//
// Same trick as Pong/Tanks: synthesise each cue ADDITIVELY to a mono WAV on SD exactly once
// (notify_synth), then play the cached file async (nucleo_audio). No PCM buffer, ~zero RAM — CPU
// paid once, never per frame. Cues are short and punchy to fit a belt-scroll brawler: nav blips,
// dry whiffs, low thuds for connecting blows, descending KO, dissonant hurt, fanfares.
//
// Policy mirrors Pong's sfx(): no-op if g.audio is off; non-critical cues are dropped while a clip
// is still playing (avoids stomping a fanfare with a footstep), critical cues stop+replace.

#include "brawler.h"
#include "notify_synth.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_audio.h"
}

// ---------------------------------------------------------------- cache layout
#define DIRR   "/sd/data/brawler"
#define BNSFX  10          // BSFX_NAV(1) .. BSFX_OVER(10)

static const char *bsfx_name(int id) {
    switch (id) {
        case BSFX_NAV:   return "nav";
        case BSFX_SEL:   return "sel";
        case BSFX_BACK:  return "back";
        case BSFX_WHIFF: return "whiff";
        case BSFX_HIT:   return "hit";
        case BSFX_KO:    return "ko";
        case BSFX_HURT:  return "hurt";
        case BSFX_JUMP:  return "jump";
        case BSFX_CLEAR: return "clear";
        case BSFX_OVER:  return "over";
        default:         return "x";
    }
}

// ---------------------------------------------------------------- voice recipes
// Returns the voice count for `id` filled into v[] (sized >= 8 by the caller).
static int build_voices(int id, notify_voice_t *v) {
    switch (id) {
        case BSFX_NAV:                                                   // short high blip
            notify__voice(&v[0], 760, 0, 0.04f); v[0].amp = 0.5f;
            return 1;
        case BSFX_SEL:                                                   // two-note up
            notify__voice(&v[0], 659.25f, 0,     0.06f);
            notify__voice(&v[1], 987.77f, 0.04f, 0.09f);
            return 2;
        case BSFX_BACK:                                                  // low blip
            notify__voice(&v[0], 320, 0,     0.06f);
            notify__voice(&v[1], 220, 0.05f, 0.08f); v[1].amp = 0.7f;
            return 2;
        case BSFX_WHIFF:                                                 // very short airy miss
            notify__voice(&v[0], 1200, 0, 0.03f); v[0].amp = 0.30f;
            return 1;
        case BSFX_HIT:                                                   // low thud + short transient
            notify__voice(&v[0], 90,  0, 0.10f); v[0].amp = 1.0f;
            notify__voice(&v[1], 180, 0, 0.04f); v[1].amp = 0.7f;
            return 2;
        case BSFX_KO:                                                    // descending 220 -> 90 Hz
            notify__voice(&v[0], 220, 0,     0.10f); v[0].amp = 0.9f;
            notify__voice(&v[1], 150, 0.08f, 0.12f); v[1].amp = 0.9f;
            notify__voice(&v[2], 90,  0.18f, 0.16f); v[2].amp = 1.0f;
            return 3;
        case BSFX_HURT:                                                  // dissonant pair (minor 2nd)
            notify__voice(&v[0], 300, 0, 0.07f); v[0].amp = 0.8f;
            notify__voice(&v[1], 318, 0, 0.07f); v[1].amp = 0.8f;
            return 2;
        case BSFX_JUMP:                                                  // quick up-chirp
            notify__voice(&v[0], 440, 0,     0.04f); v[0].amp = 0.6f;
            notify__voice(&v[1], 700, 0.03f, 0.05f); v[1].amp = 0.6f;
            return 2;
        case BSFX_CLEAR:                                                 // ascending fanfare
            notify__voice(&v[0], 523.25f, 0,     0.10f);
            notify__voice(&v[1], 659.25f, 0.08f, 0.10f);
            notify__voice(&v[2], 783.99f, 0.16f, 0.12f);
            notify__voice(&v[3], 1046.5f, 0.24f, 0.18f);
            return 4;
        case BSFX_OVER:                                                  // somber descending minor
            notify__voice(&v[0], 392, 0,     0.12f);
            notify__voice(&v[1], 311.13f, 0.11f, 0.14f);
            notify__voice(&v[2], 196, 0.24f, 0.24f);
            return 3;
    }
    return 0;
}

// Cues that must always be heard: they stop+replace whatever is playing.
static bool bsfx_important(int id) {
    return id == BSFX_HIT || id == BSFX_KO || id == BSFX_CLEAR || id == BSFX_OVER;
}

// ---------------------------------------------------------------- SD plumbing
static void ensure_dirs(void) {
    mkdir("/sd/data", 0777);
    mkdir(DIRR, 0777);
    mkdir(DIRR "/sfx", 0777);
}

static void presynth(void) {
    if (!g.audio) return;
    notify_voice_t v[8];
    for (int id = 1; id <= BNSFX; id++) {
        char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", bsfx_name(id));
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); continue; }                 // already cached
        int nv = build_voices(id, v);
        if (nv > 0) notify_synth_voices_wav(v, nv, p, 12000);
    }
}

// ---------------------------------------------------------------- public API
void bsfx_presynth(void) { ensure_dirs(); presynth(); }

void bsfx(int id) {
    if (!g.audio || id <= 0) return;
    if (!bsfx_important(id) && nucleo_audio_is_playing()) return;       // drop non-critical when busy
    char p[80]; snprintf(p, sizeof p, DIRR "/sfx/%s.wav", bsfx_name(id));
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    else {                                                              // synth-on-miss (first play of the session)
        notify_voice_t v[8];
        int nv = build_voices(id, v);
        if (nv <= 0 || notify_synth_voices_wav(v, nv, p, 12000) != 0) return;
    }
    if (bsfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
