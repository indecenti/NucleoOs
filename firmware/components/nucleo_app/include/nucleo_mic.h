// nucleo_mic — the microphone an app may have (ABI level 10): live 16-bit mono PCM from the built-in mic.
//
// One mic, one owner: the Voice Recorder's takes, the web dictation stream and an app all claim the same mic, so
// nucleo_mic_open fails with NUCLEO_MIC_BUSY while another holds it. While an app holds it the speaker is off
// (the two share an I2S line): nucleo_audio_pcm_open / _play refuse, so a voice app records, closes, then plays
// (push-to-talk: nucleo_app_set_ptt_handler). A store app gets it only with the "mic" permission in its signed
// record (the Store shows it before installing); built-in apps always may. The framework closes the mic when the
// app closes. Every call is bounded in time and feeds the task watchdog. docs/native-apps.md ("Microphone").
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_MIC_RATE 16000   // the one rate every board delivers (the ADV decimates its 48 kHz ADC to it)

enum {
    NUCLEO_MIC_BUSY    = -1,   // a recording, a dictation stream, the spectrum, a sound or another app holds it
    NUCLEO_MIC_DENIED  = -2,   // this app has no microphone permission, or a rate other than NUCLEO_MIC_RATE
    NUCLEO_MIC_ERROR   = -3,   // the hardware did not start
    NUCLEO_MIC_NOTOPEN = -4,   // read before open (or after close)
};

int  nucleo_mic_open(int rate_hz);                                  // 0, or an error above
int  nucleo_mic_read(int16_t *pcm, int max_samples, int timeout_ms); // samples (>= 0, 0 = none yet), or < 0
int  nucleo_mic_level(void);                                        // peak of the last read, 0..100 (a meter)
void nucleo_mic_close(void);
void nucleo_mic_close_all(void);                                    // the framework, when an app closes

#ifdef __cplusplus
}
#endif
