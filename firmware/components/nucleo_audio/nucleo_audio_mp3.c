// MP3 decode loop, isolated so the Helix dependency touches only this file. Streams a
// frame at a time from the open file, decodes to PCM, and pushes it to the I2S sink.
// Helix is fixed-point and frame-local — no PSRAM needed, ~30 KB working set.
#include "nucleo_audio_priv.h"
#include <string.h>
#include "esp_log.h"
#include "mp3dec.h"          // chmorgan/esp-libhelix-mp3 (managed component)

static const char *TAG = "audio.mp3";

#define IN_SZ   NUCLEO_AUDIO_IN_SZ    // shared decode scratch sizes (nucleo_audio_priv.h)
#define OUT_SZ  NUCLEO_AUDIO_OUT_SZ

void nucleo_audio_play_mp3(FILE *f)
{
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { ESP_LOGE(TAG, "MP3InitDecoder failed (out of RAM?)"); return; }

    uint8_t *in = nucleo_audio_in;        // shared scratch — file & radio decoders never run at once
    int16_t *out = nucleo_audio_out;
    uint8_t *rp = in;
    int left = 0;
    bool logged = false;                                     // diagnostics: report the first frame
    uint32_t frames = 0;

    while (nucleo_audio_keep_running()) {
        long seek_off;
        if (nucleo_audio_poll_seek(&seek_off)) {             // resume-start or live seek
            fseek(f, seek_off, SEEK_SET);
            left = 0; rp = in;                               // drop the buffer; resync below
        }
        if (left < 1441) {                                   // refill: keep at least one frame
            memmove(in, rp, left); rp = in;
            int n = (int)fread(in + left, 1, IN_SZ - left, f);
            if (n > 0) { left += n; nucleo_audio_add_file_bytes((uint32_t)n); }
            else if (left == 0) break;                       // EOF, nothing buffered
        }
        int off = MP3FindSyncWord(rp, left);
        if (off < 0) { left = 0; continue; }                 // no frame in buffer -> refill
        rp += off; left -= off;

        int err = MP3Decode(dec, &rp, &left, out, 0);
        if (err == ERR_MP3_NONE) {
            MP3FrameInfo fi; MP3GetLastFrameInfo(dec, &fi);
            if (fi.outputSamps > 0) {
                esp_err_t ie = nucleo_audio_i2s_rate(fi.samprate, fi.nChans);
                if (!logged) {                               // one-shot: prove what the decoder produced
                    logged = true;
                    ESP_LOGI(TAG, "first frame: %d Hz, %d ch, %d kbps, i2s=%s",
                             fi.samprate, fi.nChans, fi.bitrate / 1000, esp_err_to_name(ie));
                }
                nucleo_audio_i2s_write(out, (size_t)fi.outputSamps * sizeof(int16_t));
                nucleo_audio_add_samples((uint32_t)(fi.outputSamps / (fi.nChans < 1 ? 1 : fi.nChans)), fi.samprate);
                frames++;
            }
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            left = 0;                                         // need more bytes -> refill
        } else {
            if (left > 0) { rp++; left--; }                  // skip a byte past the bad frame
        }
    }
    ESP_LOGI(TAG, "decode finished: %u frames", (unsigned)frames);
    MP3FreeDecoder(dec);
}
