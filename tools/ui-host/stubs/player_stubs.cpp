// Host stub for app_player.cpp: a track that "plays" once the app starts one, so Now Playing and the library's
// playing card render with real numbers (1:23 of 4:01, a spectrum), instead of a return-0 engine that never
// starts. host_stubs.cpp already owns volume / mute / is_playing (g_host.playing); this sets that flag.
// The compact UI's Now Playing is a blocking loop that only ends on a key: on the compact variant a play also
// scripts one Back for it (g_host.kbd), so the loop draws once and returns instead of spinning forever.
// Nothing here touches hardware or files.
#include "../host_state.h"
#include "nucleo_kbd.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

extern "C" {

static char s_track[200];
esp_err_t nucleo_audio_play(const char *path)
{
    snprintf(s_track, sizeof s_track, "%s", path ? path : "");
    g_host.playing = true; g_host.track = s_track;
    if (!g_host.two_screens) {
        static const nucleo_key_t back[1] = { { NK_BACK, 0 } };
        g_host.kbd = back; g_host.kbd_n = 1; g_host.kbd_i = 0;
    }
    return ESP_OK;
}
void     nucleo_audio_stop(void) { g_host.playing = false; }
void     nucleo_audio_toggle_pause(void) {}
void     nucleo_audio_seek(uint32_t) {}
uint32_t nucleo_audio_elapsed(void) { return g_host.playing ? 83 : 0; }
uint32_t nucleo_audio_elapsed_ms(void) { return g_host.playing ? 83000 : 0; }
uint32_t nucleo_audio_duration_ms(void) { return g_host.playing ? 241000 : 0; }
int      nucleo_audio_progress(void) { return g_host.playing ? 34 : 0; }
int      nucleo_audio_dbg_init(void) { return 1; }
int      nucleo_audio_dbg_frames(void) { return 100; }
bool     nucleo_audio_why_silent(char *, size_t) { return false; }
int      nucleo_audio_bands(uint8_t *out, int n)
{
    static const uint8_t b[6] = { 210, 170, 140, 120, 90, 60 };
    for (int i = 0; i < n && i < 6; i++) out[i] = g_host.playing ? b[i] : 0;
    return n < 6 ? n : 6;
}

}
