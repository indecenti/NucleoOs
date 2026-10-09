// brawler_sfx.cpp — SCORRIBANDA: sound effects.
//
// Every cue is a WAV of the game's arcade pack, rendered on the PC (tools/sfx-gen/games/brawler.py ->
// /sd/data/brawler/pack) and streamed off the SD by nucleo_audio: zero device CPU, no PCM buffer. Nothing is
// ever synthesized on the device (a burst of synthesis on the app task froze the open; on Tanks it tripped
// the 8 s Task WDT). Without the pack an important cue falls back to a short tone and the rest stay silent.
//
// Policy: no-op if g.audio is off; small cues (nav, whiff, jump...) are dropped while a clip is still playing
// so they never stack, the important ones (hits, KO, fanfares) stop the current clip and replace it.

#include "brawler.h"
#include <stdio.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_audio.h"
}

#define DIRR "/sd/data/brawler"

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
        case BSFX_HEAVY: return "heavy";
        case BSFX_BLOCK: return "block";
        case BSFX_GO:    return "go";
        default:         return "x";
    }
}
// Cues that must always be heard: they stop + replace whatever is playing.
static bool bsfx_important(int id) {
    return id == BSFX_HIT || id == BSFX_HEAVY || id == BSFX_KO || id == BSFX_HURT || id == BSFX_CLEAR || id == BSFX_OVER || id == BSFX_GO;
}
// A playable WAV = present AND longer than its 44-byte header (a copy cut short leaves a 0-byte file).
static bool wav_ok(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 44; }

void bsfx(int id) {
    if (!g.audio || id <= 0) return;
    bool imp = bsfx_important(id);
    if (!imp && nucleo_audio_is_playing()) return;                      // small cues never stack
    char p[48]; snprintf(p, sizeof p, DIRR "/pack/%s.wav", bsfx_name(id));
    if (!wav_ok(p)) { if (imp) nucleo_audio_tone(id == BSFX_CLEAR ? 880 : 180, 45, 55); return; }   // no pack: a tone
    if (imp) nucleo_audio_stop();
    nucleo_audio_play(p);
}
