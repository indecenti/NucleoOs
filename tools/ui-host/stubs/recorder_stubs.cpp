// Host stub for app_recorder.cpp: the recorder's world with real numbers instead of return-0 stubs.
//  - A few FICTIONAL takes on the scene's card (written before main() runs: the gallery runs each scene in its own
//    box directory, the cwd), sized like real 16 kHz mono WAVs so the list shows true durations, with fixed
//    dates, one renamed take, and AI sidecars (summary / transcript / actions) on two of them.
//  - We are already in the dedicated Recorder Solo boot (no "preparing RAM" splash), a cloud engine is set up.
//  - The mic records once started (47 s, a moving level), a take plays once started (0:05 of 0:14).
// Nothing here touches hardware, the network or files outside the box.
#include "../host_state.h"
extern "C" {
#include "nucleo_recorder.h"           // a C header without its own extern "C" (the app wraps it the same way)
}
#include "nucleo_storage.h"
#include "esp_err.h"
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include <sys/utime.h>

static bool s_rec;
static nucleo_storage_info_t s_sd = { true, "FAT32", 31914983424ULL, 12482736128ULL, ESP_OK };

extern "C" {

bool nucleo_anima_solo_active(void) { return true; }
bool nucleo_anima_online_available(void) { return true; }
bool nucleo_anima_teacher_info(char *provider, int pcap, char *model, int mcap)
{
    if (provider && pcap > 0) snprintf(provider, pcap, "anthropic");
    if (model && mcap > 0) snprintf(model, mcap, "claude-sonnet-4-6");
    return true;
}
const nucleo_storage_info_t *nucleo_storage_info(void) { return &s_sd; }

esp_err_t nucleo_recorder_start(void) { s_rec = true; return ESP_OK; }
void      nucleo_recorder_stop(void) { s_rec = false; }
bool      nucleo_recorder_is_recording(void) { return s_rec; }
bool      nucleo_recorder_is_busy(void) { return s_rec; }
nucleo_mic_owner_t nucleo_recorder_owner(void) { return s_rec ? NUCLEO_MIC_REC : NUCLEO_MIC_IDLE; }
uint32_t  nucleo_recorder_seconds(void) { return s_rec ? 47 : 0; }
int       nucleo_recorder_level(void)
{
    static const int L[12] = { 12, 35, 58, 44, 71, 63, 28, 49, 82, 55, 30, 18 };
    return s_rec ? L[(g_host.now_us / 200000) % 12] : 0;
}

esp_err_t nucleo_audio_play(const char *) { g_host.playing = true; return ESP_OK; }
void      nucleo_audio_stop(void) { g_host.playing = false; }
uint32_t  nucleo_audio_elapsed_ms(void) { return g_host.playing ? 5000 : 0; }
uint32_t  nucleo_audio_elapsed(void) { return g_host.playing ? 5 : 0; }
uint32_t  nucleo_audio_duration_ms(void) { return g_host.playing ? 14000 : 0; }
int       nucleo_audio_progress(void) { return g_host.playing ? 36 : 0; }

}

// ---- the card ------------------------------------------------------------------------------------------------
static void put(const char *name, long bytes, time_t when)
{
    char p[200]; snprintf(p, sizeof p, "sd/data/Recordings/%s", name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    static const unsigned char hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' };
    fwrite(hdr, 1, sizeof hdr, f);
    if (bytes > 44) { fseek(f, bytes - 1, SEEK_SET); fputc(0, f); }
    fclose(f);
    struct _utimbuf t = { when, when }; _utime(p, &t);
}
static void text(const char *name, const char *body)
{
    char p[200]; snprintf(p, sizeof p, "sd/data/Recordings/%s", name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(body, f); fclose(f); }
}

namespace {
struct RecorderCard {
    RecorderCard()
    {
        _mkdir("sd"); _mkdir("sd/data"); _mkdir("sd/data/Recordings");
        const time_t day = 1790678520 - 9 * 3600;                         // 2026-09-29 00:22 UTC
        put("rec-20260929-0815.wav", 44 + 32000L * 14, day + 7 * 3600 + 53 * 60);
        put("Idee per il progetto.wav", 44 + 32000L * 23, day - 86400 + 17 * 3600);
        put("rec-20260927-1130.wav", 44 + 32000L * 6, day - 2 * 86400 + 11 * 3600 + 8 * 60);
        put("rec-20260925-1902.wav", 44 + 32000L * 9, day - 4 * 86400 + 18 * 3600 + 40 * 60);
        text("rec-20260929-0815.sum.txt",
             "Chiamata con Marco sul preventivo della cucina: misure confermate (320 x 260 cm), "
             "manca il colore delle ante. Montaggio possibile la settimana del 12 ottobre.");
        text("rec-20260929-0815.txt",
             "Allora, ho sentito Marco per la cucina. Le misure sono quelle, trecentoventi per duecentosessanta. "
             "Il colore delle ante lo decidiamo sabato. Lui dice che il montaggio si puo' fare la settimana del dodici.");
        text("rec-20260929-0815.act.txt", "- Scegliere il colore delle ante entro sabato\n- Confermare a Marco la settimana del 12 ottobre\n- Chiedere il preventivo per gli elettrodomestici");
        text("Idee per il progetto.sum.txt", "Un secondo schermo per ogni app: lista a sinistra, dettagli a destra, mai uno specchio.");
    }
} s_card;
}
