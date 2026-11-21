// Host test of xz_core.c (the XiaoZhi client) against tools/xiaozhi-host/fake_server.py.
// The firmware services it uses are replaced here: nucleo_tcp over Winsock (plain TCP; TLS is refused), the mic
// plays a synthetic voice, the speaker writes a WAV, secrets live in memory, the heap numbers are fixed.
// Drives a whole session: activation (code, 202, 200), a 3 s push-to-talk, the exchange, the spoken answer.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xz_core.h"
#include "nucleo_tcp.h"
#include "nucleo_mic.h"
#include "nucleo_secret.h"
#include "nucleo_audio.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

// ---- platform ----------------------------------------------------------------------------------------------------
int64_t esp_timer_get_time(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (int64_t)(c.QuadPart * 1000000.0 / f.QuadPart); }
uint32_t esp_random(void) { return (uint32_t)rand() << 16 ^ (uint32_t)rand(); }
esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 70000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 31000; }

// ---- secrets -----------------------------------------------------------------------------------------------------
static char sk[8][16], sv[8][64];
bool nucleo_secret_set(const char *name, const char *value)
{
    for (int i = 0; i < 8; i++) if (!strcmp(sk[i], name) || !sk[i][0]) {
        snprintf(sk[i], 16, "%s", name); snprintf(sv[i], 64, "%s", value ? value : ""); return true;
    }
    return false;
}
bool nucleo_secret_get(const char *name, char *out, int cap)
{
    out[0] = 0;
    for (int i = 0; i < 8; i++) if (!strcmp(sk[i], name) && sv[i][0]) { snprintf(out, cap, "%s", sv[i]); return true; }
    return false;
}

// ---- TCP ---------------------------------------------------------------------------------------------------------
static SOCKET s_fd[NUCLEO_TCP_MAX + 1];
bool nucleo_tcp_network_up(void) { return true; }
int nucleo_tcp_connect(const char *host, int port, int timeout_ms)
{
    (void)timeout_ms;
    for (int h = 1; h <= NUCLEO_TCP_MAX; h++) if (!s_fd[h]) {
        struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        inet_pton(AF_INET, !strcmp(host, "localhost") ? "127.0.0.1" : host, &a.sin_addr);
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return NUCLEO_TCP_ERROR; }
        s_fd[h] = s; return h;
    }
    return NUCLEO_TCP_FULL;
}
int nucleo_tcp_connect_tls(const char *host, int port, int timeout_ms) { (void)host; (void)port; (void)timeout_ms; return NUCLEO_TCP_TLS; }
bool nucleo_tcp_readable(int h, int timeout_ms)
{
    fd_set r; FD_ZERO(&r); FD_SET(s_fd[h], &r);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    return select(0, &r, NULL, NULL, &tv) > 0;
}
int nucleo_tcp_recv(int h, void *buf, int cap, int timeout_ms)
{
    if (!nucleo_tcp_readable(h, timeout_ms)) return NUCLEO_TCP_TIMEOUT;
    const int n = recv(s_fd[h], buf, cap, 0);
    return n > 0 ? n : n == 0 ? NUCLEO_TCP_CLOSED : NUCLEO_TCP_ERROR;
}
int nucleo_tcp_send(int h, const void *buf, int len, int timeout_ms) { (void)timeout_ms; return send(s_fd[h], buf, len, 0) == len ? len : NUCLEO_TCP_ERROR; }
void nucleo_tcp_close(int h) { if (h >= 1 && h <= NUCLEO_TCP_MAX && s_fd[h]) { closesocket(s_fd[h]); s_fd[h] = 0; } }
void nucleo_tcp_close_all(void) { for (int h = 1; h <= NUCLEO_TCP_MAX; h++) nucleo_tcp_close(h); }

// ---- mic: a voice-like signal ------------------------------------------------------------------------------------
static int s_mic_open, s_mic_level; static long s_mic_t;
int nucleo_mic_open(int rate) { if (rate != NUCLEO_MIC_RATE) return NUCLEO_MIC_DENIED; if (s_mic_open) return NUCLEO_MIC_BUSY; s_mic_open = 1; return 0; }
int nucleo_mic_read(int16_t *pcm, int max, int timeout_ms)
{
    (void)timeout_ms;
    if (!s_mic_open) return NUCLEO_MIC_NOTOPEN;
    int peak = 0;
    for (int i = 0; i < max; i++, s_mic_t++) {
        const double t = s_mic_t / 16000.0;
        pcm[i] = (int16_t)(7000 * sin(2 * 3.14159265 * 150 * t) * (0.5 + 0.5 * sin(2 * 3.14159265 * 2.5 * t)) + 2000 * sin(2 * 3.14159265 * 900 * t));
        if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
    }
    s_mic_level = peak * 100 / 32768;
    return max;
}
int nucleo_mic_level(void) { return s_mic_level; }
void nucleo_mic_close(void) { s_mic_open = 0; }
void nucleo_mic_close_all(void) { s_mic_open = 0; }

// ---- speaker: a WAV --------------------------------------------------------------------------------------------
static FILE *s_wav; static long s_wav_bytes; static double s_wav_energy;
esp_err_t nucleo_audio_pcm_open_ex(int rate, int ch, int q)
{
    (void)q; if (s_mic_open) return ESP_ERR_INVALID_STATE;
    s_wav = fopen("build/xiaozhi-host/reply.wav", "wb");
    const uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, s_wav); s_wav_bytes = 0; (void)rate; (void)ch;
    return ESP_OK;
}
esp_err_t nucleo_audio_pcm_write(const int16_t *pcm, size_t bytes)
{
    fwrite(pcm, 1, bytes, s_wav); s_wav_bytes += (long)bytes;
    for (size_t i = 0; i < bytes / 2; i++) s_wav_energy += (double)pcm[i] * pcm[i];
    return ESP_OK;
}
void nucleo_audio_pcm_close(void)
{
    if (!s_wav) return;
    uint8_t h[44]; const uint32_t rate = 16000, br = 32000, ds = (uint32_t)s_wav_bytes, rs = ds + 36;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &rs, 4); memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t f16 = 16; const uint16_t pcm1 = 1, ch = 1, ba = 2, bits = 16;
    memcpy(h + 16, &f16, 4); memcpy(h + 20, &pcm1, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &ds, 4);
    fseek(s_wav, 0, SEEK_SET); fwrite(h, 1, 44, s_wav); fclose(s_wav); s_wav = NULL;
}

// ---- the session ---------------------------------------------------------------------------------------------------
static const char *NAMES[] = { "SETUP", "OTA", "ACTIVATE", "IDLE", "RECORDING", "ENCODING", "EXCHANGE", "WAITING", "PLAYING", "ERROR" };

static int run_until(int want, int max_ms)
{
    const int64_t until = esp_timer_get_time() / 1000 + max_ms;
    int last = -1;
    while (esp_timer_get_time() / 1000 < until) {
        xz_poll();
        const xz_view_t *v = xz_view();
        if ((int)v->state != last) { printf("  %-9s %s\n", NAMES[v->state], v->status); last = v->state; }
        if ((int)v->state == want) return 1;
        if (v->state == XZ_ERROR) return 0;
        Sleep(20);
    }
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    const int port = argc > 1 ? atoi(argv[1]) : 18790;
    CreateDirectoryA("build", NULL); CreateDirectoryA("build/xiaozhi-host", NULL);
    DeleteFileA("build/xiaozhi-host/xz/config.json");
    int fails = 0;
#define CHECK(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)
    xz_init("build/xiaozhi-host/xz", "en-US");
    CHECK(xz_view()->state == XZ_SETUP, "no configuration: setup");
    char url[96]; snprintf(url, sizeof url, "http://127.0.0.1:%d/xiaozhi/ota/", port);
    CHECK(xz_set_server(url), "server accepted");
    CHECK(run_until(XZ_ACTIVATE, 5000), "activation code offered");
    CHECK(!strcmp(xz_view()->code, "424242"), "the code is shown");
    CHECK(run_until(XZ_IDLE, 20000), "bound after 202 then 200, ready");
    xz_talk(true);
    CHECK(xz_view()->state == XZ_RECORDING, "push-to-talk records");
    while (xz_view()->rec_ms < 3000) { xz_poll(); }
    xz_talk(false);
    CHECK(run_until(XZ_PLAYING, 20000), "encoded, sent, answer received");
    CHECK(!strcmp(xz_view()->user, "hello nucleo"), "stt shown");
    CHECK(strstr(xz_view()->reply, "fake XiaoZhi") != NULL, "tts sentence shown");
    CHECK(!strcmp(xz_view()->emotion, "happy"), "emotion");
    CHECK(run_until(XZ_IDLE, 20000), "answer played, ready again");
    printf("  reply: %ld bytes of PCM (%.1f s), rms %.0f\n", s_wav_bytes, s_wav_bytes / 32000.0,
           s_wav_bytes ? sqrt(s_wav_energy / (s_wav_bytes / 2)) : 0);
    CHECK(s_wav_bytes > 32000 * 2, "the answer is longer than 2 s");
    CHECK(s_wav_bytes ? sqrt(s_wav_energy / (s_wav_bytes / 2)) > 1000 : 0, "the answer is not silence");
    // a reload keeps the binding: no activation, straight to the service
    xz_shutdown();
    xz_init("build/xiaozhi-host/xz", "en-US");
    CHECK(run_until(XZ_IDLE, 5000), "restart: settings kept, no new activation");
    xz_shutdown();
    printf(fails ? "xiaozhi-host: %d FAILED\n" : "xiaozhi-host: all passed\n", fails);
    return fails ? 1 : 0;
}
