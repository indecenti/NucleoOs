// ES8311 audio codec — present only on the Cardputer ADV (I2C 0x18 on the shared system
// bus, SDA8/SCL9, owned by the TCA8418 keyboard). The original Cardputer uses a dumb
// NS4168 I2S amp + PDM mic that need no codec, so every call here is a no-op there.
//
// The I2S pins are the SAME on both boards (speaker BCLK41/WS43/DOUT42, mic DIN46). The
// ADV difference is that the ES8311 must be powered/configured over I2C before the DAC
// (speaker) or ADC (mic) passes any audio. Register sets ported from the Bruce
// m5stack-cardputer reference (proven on this hardware).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// Forward-declared so includers don't need the i2s driver headers (identical to the IDF
// typedef in driver/i2s_common.h; a redundant identical typedef is legal in C11/C++).
typedef struct i2s_channel_obj_t *i2s_chan_handle_t;

// Attach the ES8311 to the already-created system I2C bus. Pass nucleo_kbd_i2c_bus()
// (it owns the 8/9 master on the ADV); NULL on the original Cardputer -> codec stays
// absent and every call below is a no-op. Call once at boot, after the keyboard init.
void nucleo_codec_init(void *i2c_bus);

// True only when an ES8311 was attached (i.e. running on an ADV).
bool nucleo_codec_present(void);

// Power the DAC path up (speaker). enable=true: ~11 quick register writes (power up + un-mute).
// enable=false: DAC mute (one write, click-free) — leaves the idle codec SILENT instead of hissing.
// nucleo_audio calls disable when it tears down the I2S TX, so an idle ADV speaker is dead quiet.
void nucleo_codec_speaker(bool enable);

// Power the ADC path up/down (microphone — standard-I2S capture on the ADV).
void nucleo_codec_mic(bool enable);

// Open a mono 16-bit I2S RX channel for the BUILT-IN mic, board-aware: PDM (original
// SPM1423, pins CLK43/DIN46) or the ES8311 ADC over standard I2S (ADV, BCLK41/WS43/DIN46
// — it also powers the codec ADC). On success *out holds the enabled channel; on failure
// *out is NULL and nothing leaks. Single shared mic HAL for the recorder, voice and
// spectrum apps (all mutually exclusive on the shared mic pins).
esp_err_t nucleo_codec_mic_open(int rate, i2s_chan_handle_t *out);

// Read mono 16-bit PCM at the rate passed to nucleo_codec_mic_open. On the original Cardputer this
// is a pass-through to i2s_channel_read; on the ADV the ES8311 must be captured at its native 48 kHz
// (the only rate its MCLK-less clocking gets right), so this reads native samples and decimates them
// to the requested rate in software. ALWAYS read the mic through this, never i2s_channel_read
// directly, or ADV captures come out ~3x too slow. *got is OUTPUT bytes; partial/timeout reads
// return what they have (same contract as i2s_channel_read), so existing read loops work unchanged.
esp_err_t nucleo_codec_mic_read(i2s_chan_handle_t rx, void *out, size_t out_cap, size_t *got, uint32_t ticks);

// Tear down a channel from nucleo_codec_mic_open (and power down the ADV codec ADC).
void nucleo_codec_mic_close(i2s_chan_handle_t rx);

#ifdef __cplusplus
}
#endif
