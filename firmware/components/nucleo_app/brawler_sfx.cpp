// brawler_sfx.cpp — SCORRIBANDA: sound effects.
//
// Each cue is a WAV in the deployed pack (/sd/data/brawler/pack/<name>.wav), baked on the PC from the
// recipes below (tools/sfx-gen/bake-recipe-packs.mjs) and played async (nucleo_audio) — no PCM buffer,
// ~zero RAM, zero CPU on the device. Cues are short and punchy to fit a belt-scroll brawler: nav blips,
// dry whiffs, low thuds for connecting blows, descending KO, dissonant hurt, fanfares.
//
// Policy mirrors the shared game_sfx engine: no-op if g.audio is off; non-critical cues are dropped while
// a clip is still playing (avoids stomping a fanfare with a footstep), critical cues stop+replace. Nothing
// is synthesized here: without a WAV a critical cue degrades to a short tone, the rest stay silent.

#include "brawler.h"
#include "game_sfx.h"          // pack lookup + tone fallback (and notify_voice_t for the recipes)
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- pack layout
#define DIRR   "/sd/data/brawler"

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

// ---------------------------------------------------------------- public API
void bsfx(int id) {
    if (!g.audio || id <= 0) return;
    if (!bsfx_important(id) && nucleo_audio_is_playing()) return;       // drop non-critical when busy
    char p[80];
    if (!game_sfx_find(DIRR, bsfx_name(id), p, sizeof p)) {            // pack -> legacy cache; never synth here
        if (bsfx_important(id)) { nucleo_audio_stop(); game_sfx_tone(build_voices, id); }
        return;
    }
    if (bsfx_important(id)) nucleo_audio_stop();
    nucleo_audio_play(p);
}
