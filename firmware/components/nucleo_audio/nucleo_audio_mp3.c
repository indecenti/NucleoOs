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
    // Helix allocates ~20 KB across 8 small blocks. On a fragmented heap the last one can fail. Don't
    // give up silently (the old behaviour = "clip plays with no sound"): free a contiguous block (unload
    // the idle L1 index, etc.) and retry once before conceding.
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGW(TAG, "MP3InitDecoder OOM — reclaiming + retrying");
        nucleo_audio_do_reclaim();
        dec = MP3InitDecoder();
    }
    if (!dec) { ESP_LOGE(TAG, "MP3InitDecoder failed (out of RAM?) — clip will be SILENT"); nucleo_audio_dbg_set_init(2); return; }
    nucleo_audio_dbg_set_init(1);

    uint8_t *in = nucleo_audio_in;        // shared scratch — file & radio decoders never run at once
    int16_t *out = nucleo_audio_out;
    uint8_t *rp = in;
    int left = 0;
    bool logged = false;                                     // diagnostics: report the first frame
    uint32_t frames = 0;
    bool eof = false;                                        // the file has no more bytes to give

    while (nucleo_audio_keep_running()) {
        long seek_off;
        if (nucleo_audio_poll_seek(&seek_off)) {             // resume-start or live seek
            nucleo_audio_seek_far(f, seek_off);              // bounded, WDT-safe: a far live seek on a big MP3 is a multi-second FATFS cluster walk -> watchdog reboot
            left = 0; rp = in; eof = false;                  // drop the buffer; resync below
        }
        if (left < 1441) {                                   // refill: keep at least one frame
            memmove(in, rp, left); rp = in;
            int n = (int)fread(in + left, 1, IN_SZ - left, f);
            if (n > 0) { left += n; nucleo_audio_add_file_bytes((uint32_t)n); }
            else { eof = true; if (left == 0) break; }       // EOF; drain whatever is still buffered
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
                // The I2S result was previously computed only to be printed. If the sink never came up
                // the writes below are silently dropped and the file decodes to EOF at full speed with
                // no sound and no error — the hardest silence to diagnose. Stop and record it instead.
                if (ie != ESP_OK) {
                    ESP_LOGE(TAG, "I2S sink unavailable (%s) — aborting playback", esp_err_to_name(ie));
                    nucleo_audio_dbg_set_err(-1);            // negative = sink failure, not a decode error
                    break;
                }
                nucleo_audio_i2s_write(out, (size_t)fi.outputSamps * sizeof(int16_t));
                nucleo_audio_add_samples((uint32_t)(fi.outputSamps / (fi.nChans < 1 ? 1 : fi.nChans)), fi.samprate);
                nucleo_audio_dbg_frame(fi.samprate);
                frames++;
            }
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            // Both are recoverable "need more context" returns — MAINDATA_UNDERFLOW in particular is a
            // NORMAL Helix result whenever the bit reservoir is not primed (the opening frames, after
            // every seek, routinely on VBR). Dropping `left` here discarded up to 2 KB of already-read
            // valid audio on each occurrence and forced a resync, thinning the stream. Keep what Helix
            // left in the buffer; the refill at the top of the loop tops it back up via the memmove.
            // Two ways this could spin forever now that `left` is preserved: a full buffer the decoder
            // refuses to advance past, and a tail at EOF that can never be topped up. Guard both.
            if (eof) break;
            if (left >= 1441) { rp++; left--; }              // full buffer and still stuck: nudge past the bad frame
        } else {
            nucleo_audio_dbg_set_err(err);                    // first hard decode error (diagnostics)
            if (left > 0) { rp++; left--; }                  // skip a byte past the bad frame
        }
    }
    ESP_LOGI(TAG, "decode finished: %u frames", (unsigned)frames);
    MP3FreeDecoder(dec);
}
