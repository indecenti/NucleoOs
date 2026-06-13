// Audio engine: transport state, I2S TX lifecycle, and the WAV path. The MP3 path lives
// in nucleo_audio_mp3.c (Helix). See nucleo_audio.h for the HW sharing note (GPIO43).
#include "nucleo_audio.h"
#include "nucleo_audio_priv.h"
#include "nucleo_board.h"
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // single-owner playback lock: serialize start/stop across tasks
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_task_wdt.h"   // pet the watchdog while waiting for the player task to unwind
#include "esp_heap_caps.h"  // largest-free-block: diagnose a failed player-task stack alloc

// The recorder owns the shared mic/speaker pins; declared here to enforce record-XOR-play.
extern bool nucleo_recorder_is_recording(void);
extern bool nucleo_recorder_is_busy(void);   // true until the mic RX is FULLY released (lingers ~200ms after stop)

static const char *TAG = "audio";

// SINGLE-OWNER playback lock (one resource at a time). The I2S channel + the player task are a single
// shared resource, but start_play/stop had NO serialization: two tasks (e.g. the ANIMA confirmation
// VOICE on the UI task and a calendar/notification CHIME on the cal-svc task) could install/tear down
// the I2S and spawn the player task CONCURRENTLY -> driver state corruption that HUNG the whole device
// (no reboot; IDLE keeps feeding the WDT). This recursive mutex makes start_play_window and
// nucleo_audio_stop mutually exclusive across tasks; start_play calls stop internally, hence recursive.
// The player task itself NEVER takes it (it only sets atomics + s_task=NULL + self-deletes), so a holder
// waiting for the player to unwind can never deadlock against it. Hold time is bounded (stop's ~4.5 s cap).
// Created in a constructor (runs single-threaded before app_main) so the handle is race-free on first use.
static SemaphoreHandle_t s_audio_mtx;
__attribute__((constructor)) static void audio_mtx_init(void) { s_audio_mtx = xSemaphoreCreateRecursiveMutex(); }
static inline void audio_lock(void)   { if (s_audio_mtx) xSemaphoreTakeRecursive(s_audio_mtx, portMAX_DELAY); }
static inline void audio_unlock(void) { if (s_audio_mtx) xSemaphoreGiveRecursive(s_audio_mtx); }

// Shared MP3 decode scratch (see nucleo_audio_priv.h) — one copy for the file and radio
// decoders, which never run concurrently (single player task). ~6.6 KB saved vs a static
// buffer set per decoder.
uint8_t nucleo_audio_in[NUCLEO_AUDIO_IN_SZ];
int16_t nucleo_audio_out[NUCLEO_AUDIO_OUT_SZ];

static i2s_chan_handle_t s_tx = NULL;
static int s_rate = 0, s_chans = 0;             // current I2S config
static atomic_bool s_playing = false, s_paused = false, s_stop = false;
static TaskHandle_t s_task = NULL;
static char s_path[200];
static atomic_uint_least32_t s_total = 0, s_done = 0;     // file bytes (progress %)
static atomic_uint_least64_t s_frames = 0;                // output frames (elapsed)
static int s_elapsed_rate = 16000;

// Soft volume ramp for crossfade: -1 = inactive
static volatile int  s_ramp_from = -1, s_ramp_to = 0;
static volatile int  s_ramp_dur_ms = 0;
static volatile uint32_t s_ramp_start_ms = 0;

// Seek state. s_base_ms offsets the elapsed clock after a seek (so A/V stays in sync); the
// rest map a target time to a file byte offset for the decode loop to jump to.
static atomic_uint_least32_t s_base_ms = 0, s_duration_ms = 0, s_seek_ms = 0, s_start_ms = 0;
static atomic_bool s_seek_req = false;

// Embedded-audio window: when an MP3 lives INSIDE another file (the NFV v3 container appends it
// as the last section), s_audio_base is the byte offset of its first frame and s_audio_len its
// length. Decode starts there and the byte<->time seek math is taken relative to it. Both 0 for a
// normal standalone file (the whole file is the audio). Progress bytes (s_done) are kept relative
// to the window so the % and duration estimate stay correct.
static atomic_uint_least32_t s_audio_base = 0, s_audio_len = 0;

// ---- I2S sink (shared with the MP3 decoder via nucleo_audio_priv.h) ----
static esp_err_t i2s_open(int rate, int chans)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.auto_clear = true; // MUST be true to prevent micro-looping on underflow/pause
    esp_err_t err = i2s_new_channel(&cc, &s_tx, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err)); s_tx = NULL; return err; }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                       chans == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = NUCLEO_SPK_PIN_BCLK,
            .ws = NUCLEO_SPK_PIN_WS, .dout = NUCLEO_SPK_PIN_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err != ESP_OK) { ESP_LOGE(TAG, "init_std: %s", esp_err_to_name(err)); i2s_del_channel(s_tx); s_tx = NULL; return err; }
    i2s_channel_enable(s_tx);
    s_rate = rate; s_chans = chans;
    ESP_LOGI(TAG, "i2s TX up: %d Hz, %d ch (bclk=%d ws=%d dout=%d)", rate, chans,
             NUCLEO_SPK_PIN_BCLK, NUCLEO_SPK_PIN_WS, NUCLEO_SPK_PIN_DOUT);
    return ESP_OK;
}

static void i2s_close(void)
{
    if (!s_tx) return;
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = NULL; s_rate = 0; s_chans = 0;
}

esp_err_t nucleo_audio_i2s_rate(int rate, int chans)
{
    if (rate <= 0) return ESP_ERR_INVALID_ARG;
    if (chans < 1) chans = 1;
    if (chans > 2) chans = 2;
    if (!s_tx) return i2s_open(rate, chans);
    if (rate == s_rate && chans == s_chans) return ESP_OK;
    // Slot count changed -> reopen; otherwise just retune the clock.
    if (chans != s_chans) { i2s_close(); return i2s_open(rate, chans); }
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
    if (err == ESP_OK) s_rate = rate;
    return err;
}

// Software output volume, 0..100 (%). Applied centrally here so it covers both the WAV
// and MP3 paths. The little NS4168 speaker distorts near the top, so we default below max
// and scale with a mild perceptual curve (squared) for usable low-end steps.
static atomic_int s_volume = 80;
void nucleo_audio_set_volume(int v) { if (v < 0) v = 0; if (v > 100) v = 100; atomic_store(&s_volume, v); }
int  nucleo_audio_volume(void)      { return atomic_load(&s_volume); }

// Hard mute: silences the output while LEAVING s_volume untouched, so the UI keeps showing the
// real level and unmuting restores it instantly. Independent of pause; persists across seek and
// track changes. Not auto-cleared on stop — the owner (the video player) resets it on exit.
static atomic_bool s_muted = false;
void nucleo_audio_set_mute(bool m) { atomic_store(&s_muted, m); }
bool nucleo_audio_is_muted(void)   { return atomic_load(&s_muted); }

// Pop-free fade-in: ramp the EFFECTIVE gain 0 -> current volume over dur_ms using the existing
// crossfade ramp (s_volume stays at the real target the whole time, so meters read correctly).
// Call right after starting playback to soften the click the little speaker makes on a hard start.
void nucleo_audio_fade_in(int dur_ms)
{
    s_ramp_to       = atomic_load(&s_volume);
    s_ramp_from     = 0;
    s_ramp_start_ms = nucleo_audio_elapsed_ms();
    s_ramp_dur_ms   = dur_ms > 0 ? dur_ms : 150;
}

esp_err_t nucleo_audio_i2s_write(const int16_t *pcm, size_t bytes)
{
    if (!s_tx || !bytes) return ESP_OK;
    size_t wrote = 0;
    
    // Determine effective volume (apply ramp if active)
    int vol = atomic_load(&s_volume);
    if (s_ramp_from >= 0) {
        uint32_t el = nucleo_audio_elapsed_ms();
        uint32_t dt = (el > s_ramp_start_ms) ? (el - s_ramp_start_ms) : 0;
        if (dt >= (uint32_t)s_ramp_dur_ms) {
            vol = s_ramp_to;
            atomic_store(&s_volume, vol);
            s_ramp_from = -1; // ramp done
        } else {
            vol = s_ramp_from + (int)((int64_t)(s_ramp_to - s_ramp_from) * dt / s_ramp_dur_ms);
        }
    }
    if (atomic_load(&s_muted)) vol = 0;     // hard mute overrides volume + ramp -> silence path below

    if (vol >= 100) return i2s_channel_write(s_tx, pcm, bytes, &wrote, pdMS_TO_TICKS(300));
    if (vol == 0) {
        // Write silence — do NOT drop: DMA must stay fed to prevent stale frame repeat.
        // const → lives in .rodata (flash), not 1 KB of always-resident .bss (it's never written).
        static const int16_t z[512] = {0};
        size_t nsamp = bytes / sizeof(int16_t), off = 0;
        while (off < nsamp) {
            size_t n = nsamp - off; if (n > 512) n = 512;
            i2s_channel_write(s_tx, z, n * sizeof(int16_t), &wrote, pdMS_TO_TICKS(300));
            off += n;
        }
        return ESP_OK;
    }
    int32_t mul = (int32_t)((int64_t)vol * vol * 65536 / 10000);
    int16_t tmp[512];
    size_t nsamp = bytes / sizeof(int16_t), off = 0;
    while (off < nsamp) {
        size_t n = nsamp - off; if (n > 512) n = 512;
        for (size_t i = 0; i < n; i++) tmp[i] = (int16_t)((pcm[off + i] * mul) >> 16);
        i2s_channel_write(s_tx, tmp, n * sizeof(int16_t), &wrote, pdMS_TO_TICKS(300));
        off += n;
    }
    return ESP_OK;
}

// ---- transport helpers ----
bool nucleo_audio_keep_running(void)
{
    // While paused: feed silence to DMA so the last PCM frame does NOT repeat.
    // Using a 256-sample (512 byte) zero-block keeps the I2S DMA always fed.
    static const int16_t silence[256] = {0};
    while (atomic_load(&s_paused) && !atomic_load(&s_stop)) {
        size_t wrote;
        if (s_tx) i2s_channel_write(s_tx, silence, sizeof(silence), &wrote, pdMS_TO_TICKS(50));
        else      vTaskDelay(pdMS_TO_TICKS(40));
    }
    return !atomic_load(&s_stop);
}
void nucleo_audio_set_total_bytes(uint32_t b) { atomic_store(&s_total, b); }
void nucleo_audio_add_file_bytes(uint32_t b)  { atomic_fetch_add(&s_done, b); }
void nucleo_audio_add_samples(uint32_t frames, int rate) { atomic_fetch_add(&s_frames, frames); if (rate > 0) s_elapsed_rate = rate; }

// Best-known clip length in ms: exact when set at play_at, otherwise estimated from the
// observed bytes-per-ms (filesize * elapsed / consumed). Lets seek work on a plain MP3 whose
// duration we never parsed — good enough for ±10 s steps once playback has warmed up.
static uint32_t est_duration_ms(void)
{
    uint32_t setdur = atomic_load(&s_duration_ms);
    if (setdur) return setdur;
    uint32_t total = atomic_load(&s_total), done = atomic_load(&s_done);
    uint64_t fr = atomic_load(&s_frames);
    int rate = s_elapsed_rate ? s_elapsed_rate : 22050;
    uint32_t el = atomic_load(&s_base_ms) + (uint32_t)(fr * 1000ULL / rate);
    if (total && done && el) return (uint32_t)((uint64_t)total * el / done);
    return 0;
}
uint32_t nucleo_audio_duration_ms(void) { return est_duration_ms(); }

// Decode-loop hook: apply a pending seek. Maps the target ms to a byte offset (time-linear,
// i.e. CBR-exact) and re-bases the elapsed clock so callers see the new position at once.
bool nucleo_audio_poll_seek(long *byte_off)
{
    if (!atomic_exchange(&s_seek_req, false)) return false;
    uint32_t ms = atomic_load(&s_seek_ms), dur = est_duration_ms(), total = atomic_load(&s_total);
    uint32_t base = atomic_load(&s_audio_base);            // 0 for a standalone file
    long rel = 0;                                          // byte offset WITHIN the audio window
    if (dur && total) {
        rel = (long)((uint64_t)total * ms / dur);
        if (rel < 0) rel = 0;
        if (rel > (long)total - 1) rel = (long)total - 1;
    }
    atomic_store(&s_frames, 0);            // decoded-since-seek resets...
    atomic_store(&s_base_ms, ms);          // ...and the clock continues from the seek target
    atomic_store(&s_done, (uint32_t)rel);  // progress bytes are window-relative
    *byte_off = (long)base + rel;          // ...but the fseek target is absolute in the file
    return true;
}

void nucleo_audio_seek(uint32_t ms)
{
    if (!atomic_load(&s_playing)) return;
    atomic_store(&s_seek_ms, ms);
    atomic_store(&s_seek_req, true);
}

// ---- WAV path: parse the 44-byte header, stream the PCM body ----
static void play_wav(FILE *f)
{
    uint8_t h[44];
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
    int rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    int chans = h[22] | (h[23] << 8);
    int bits = h[34] | (h[35] << 8);
    uint32_t data_len = h[40] | (h[41] << 8) | (h[42] << 16) | (h[43] << 24);
    if (bits != 16 || rate <= 0) return;        // only 16-bit PCM
    if (nucleo_audio_i2s_rate(rate, chans) != ESP_OK) return;
    nucleo_audio_set_total_bytes(data_len ? data_len : 0);

    static int16_t buf[1024];
    size_t got;
    while (nucleo_audio_keep_running() && (got = fread(buf, 1, sizeof(buf), f)) > 0) {
        nucleo_audio_i2s_write(buf, got);
        nucleo_audio_add_file_bytes((uint32_t)got);
        nucleo_audio_add_samples((uint32_t)(got / 2 / (chans < 1 ? 1 : chans)), rate);
    }
}

// ---- player task ----
static void player_task(void *arg)
{
    if (strncmp(s_path, "http", 4) == 0) {                  // live HTTP radio stream
        nucleo_audio_stream_url(s_path);
        i2s_close();
        atomic_store(&s_playing, false);
        atomic_store(&s_paused, false);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    FILE *f = fopen(s_path, "rb");
    if (f) {
        const char *dot = strrchr(s_path, '.');
        bool mp3 = dot && !strcasecmp(dot, ".mp3");
        if (mp3) {
            // Total bytes for the time<->byte seek map. For an embedded window (NFV v3) it's the
            // window length and the decoder starts at its base; for a standalone file it's the
            // whole size and we start at byte 0. Either way the decoder reads to EOF — the v3
            // container appends the MP3 LAST, so EOF coincides with the window's end.
            uint32_t alen = atomic_load(&s_audio_len), abase = atomic_load(&s_audio_base);
            if (alen) {
                atomic_store(&s_total, alen);
            } else {
                struct stat st;
                if (stat(s_path, &st) == 0 && st.st_size > 0) atomic_store(&s_total, (uint32_t)st.st_size);
            }
            if (fseek(f, (long)abase, SEEK_SET) != 0) abase = 0;   // start of the window (or top)
            uint32_t start = atomic_load(&s_start_ms);
            if (start > 0 && atomic_load(&s_duration_ms)) {        // resume part-way -> arm a seek
                atomic_store(&s_seek_ms, start); atomic_store(&s_seek_req, true);
            }
            nucleo_audio_play_mp3(f);
        } else {
            play_wav(f);
        }
        fclose(f);
    } else {
        ESP_LOGE(TAG, "open %s failed", s_path);
    }
    i2s_close();
    atomic_store(&s_playing, false);
    atomic_store(&s_paused, false);
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---- public API ----
static esp_err_t start_play_window(const char *path, uint32_t start_ms, uint32_t duration_ms,
                                   uint32_t file_off, uint32_t file_len)
{
    if (nucleo_recorder_is_busy()) return ESP_ERR_INVALID_STATE;        // shared GPIO43 (mic⊕speaker): wait for the mic RX to FULLY release, not just is_recording()
    audio_lock();                                                       // single owner: no concurrent I2S/task setup
    nucleo_audio_stop();                                                // stop anything current (re-takes the recursive lock)
    snprintf(s_path, sizeof(s_path), "%s", path);
    atomic_store(&s_stop, false); atomic_store(&s_paused, false);
    atomic_store(&s_done, 0); atomic_store(&s_total, 0); atomic_store(&s_frames, 0);
    atomic_store(&s_seek_req, false);
    atomic_store(&s_audio_base, file_off); atomic_store(&s_audio_len, file_len);
    atomic_store(&s_duration_ms, duration_ms);
    atomic_store(&s_start_ms, start_ms);
    atomic_store(&s_base_ms, start_ms);    // optimistic: elapsed reads start_ms before the
                                           // decode task applies the byte-offset seek
    atomic_store(&s_playing, true);
    // Pin to core 1 (APP_CPU): the Wi-Fi/LWIP stack lives on core 0, so keeping the
    // read->decode->I2S loop off that core removes the contention that starved the DMA
    // and caused stutter on the live stream.
    // RAM/frammentazione: il WAV (la voce TTS — MOLTE clip corte) scorre su buffer STATICI (play_wav usa
    // buf[1024] static; i2s_write tmp[512]) -> stack SHALLOW (~3KB). Gli 8KB sono tarati sul decoder MP3
    // (Helix, profondo). Dare 8KB anche al WAV churnava un blocco grosso a OGNI play -> il largest free block
    // scendeva (misurato 21.5KB->13KB in 40 play) finche', dopo il ciclo exit/re-enter ANIMA (worker 30KB),
    // l'alloc del task audio falliva e la VOCE restava muta. 5KB al WAV: meno churn, libera RAM durante la
    // voce, fitta anche con largest piccolo. MP3/radio restano 8KB.
    const char *_ext = strrchr(path, '.');
    uint32_t stack = (_ext && !strcasecmp(_ext, ".wav")) ? 5120 : 8192;
    // Un player task auto-cancellante libera lo stack via IDLE (DIFFERITO, dopo vTaskDelete). Un stop->play
    // piu' stretto del reclaim serve vecchio+nuovo stack insieme e, sotto frammentazione, xTaskCreate fallisce
    // -> play scartato in SILENZIO. Dai un tick all'IDLE per recuperare e RIPROVA una volta; il doppio
    // fallimento (vero OOM) ora e' LOGGATO.
    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "audio", stack, NULL, 5, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "audio task create retry (heap tight: largest %u B)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        vTaskDelay(pdMS_TO_TICKS(20));      // let IDLE reclaim deferred-freed task stacks
        ok = xTaskCreatePinnedToCore(player_task, "audio", stack, NULL, 5, &s_task, 1);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio task create FAILED (free %u, largest %u) -> play dropped",
                 (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        atomic_store(&s_playing, false); audio_unlock(); return ESP_FAIL;
    }
    audio_unlock();
    return ESP_OK;
}
// Standalone file (or http URL): the whole file is the audio (no window).
static esp_err_t start_play(const char *path, uint32_t start_ms, uint32_t duration_ms)
{
    return start_play_window(path, start_ms, duration_ms, 0, 0);
}
esp_err_t nucleo_audio_play(const char *path) { return start_play(path, 0, 0); }
esp_err_t nucleo_audio_play_at(const char *path, uint32_t start_ms, uint32_t duration_ms)
{
    return start_play(path, start_ms, duration_ms);
}
esp_err_t nucleo_audio_play_window(const char *path, uint32_t start_ms, uint32_t duration_ms,
                                   uint32_t file_off, uint32_t file_len)
{
    return start_play_window(path, start_ms, duration_ms, file_off, file_len);
}
esp_err_t nucleo_audio_play_url(const char *url) { return start_play(url, 0, 0); }   // player_task detects http://

void nucleo_audio_stop(void)
{
    audio_lock();                              // single owner: serialize against a concurrent start/stop
    if (!atomic_load(&s_playing)) { audio_unlock(); return; }
    atomic_store(&s_stop, true);
    atomic_store(&s_paused, false);
    // Wait for the player task to FULLY unwind (it sets s_task=NULL only after i2s_close()). The old
    // 500 ms cap let stop return while a radio task was still blocked in a stream read, so it closed
    // the shared I2S out from under the NEXT app's audio. Normal exit is ~150 ms (the read returns
    // with data); a stalled-stream stop takes up to the read timeout (~3 s). Return as soon as gone.
    // Pet the task watchdog while we wait: the caller (e.g. the UI task that owns video/music
    // playback) is watchdog-watched, and a stalled SD/stream read here can take seconds — without
    // this the wait alone can trip TASK_WDT and reset the chip on exit. Only reset if THIS task is
    // actually subscribed (avoids an error log when stop() is called from an unwatched task).
    bool watched = (esp_task_wdt_status(NULL) == ESP_OK);
    for (int i = 0; i < 450 && s_task; i++) { vTaskDelay(pdMS_TO_TICKS(10)); if (watched) esp_task_wdt_reset(); }
    audio_unlock();
}

// Aspetta che la riproduzione in corso FINISCA DA SOLA (non la taglia, a differenza di stop). Serve a
// SERIALIZZARE: chi sta per fare un'operazione PESANTE in RAM (es. ANIMA che scrive il calendario — albero
// cJSON fino a ~32KB) la chiama PRIMA, cosi' il task audio (stack 8KB) e' gia' liberato e i due picchi NON
// si sommano (niente "concomitanze che caricano la RAM"). No-op se niente suona; cap a max_ms (poi procede
// comunque, fail-safe). Pet del WDT come stop() — la chiama una UI/worker task watchdog-watched.
void nucleo_audio_wait_idle(uint32_t max_ms)
{
    if (!atomic_load(&s_playing)) return;
    bool watched = (esp_task_wdt_status(NULL) == ESP_OK);
    for (uint32_t w = 0; atomic_load(&s_playing) && w < max_ms; w += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (watched) esp_task_wdt_reset();
    }
}

void nucleo_audio_toggle_pause(void) { if (atomic_load(&s_playing)) atomic_store(&s_paused, !atomic_load(&s_paused)); }

bool nucleo_audio_is_playing(void) { return atomic_load(&s_playing); }
bool nucleo_audio_is_paused(void)  { return atomic_load(&s_paused); }
const char *nucleo_audio_path(void) { return s_path; }
uint32_t nucleo_audio_elapsed(void) { return nucleo_audio_elapsed_ms() / 1000; }

// Sample-accurate elapsed in milliseconds — the master clock for A/V sync. s_frames counts
// PCM frames pushed to the I2S DMA since the last (re)start/seek; s_base_ms carries the seek
// origin so the clock is continuous across a seek. The DMA runs ~one buffer (~65 ms) ahead
// of the sound actually leaving the speaker; callers that care about lip-sync subtract that.
uint32_t nucleo_audio_elapsed_ms(void)
{
    uint64_t fr = atomic_load(&s_frames);
    int rate = s_elapsed_rate ? s_elapsed_rate : 22050;
    return atomic_load(&s_base_ms) + (uint32_t)(fr * 1000ULL / rate);
}
int nucleo_audio_progress(void)
{
    uint32_t total = atomic_load(&s_total), done = atomic_load(&s_done);
    if (!total) return 0;
    int p = (int)((uint64_t)done * 100 / total);
    return p > 100 ? 100 : p;
}

