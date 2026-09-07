#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef void *i2s_chan_handle_t;
#define ESP_OK 0
typedef int esp_err_t;
#ifdef __cplusplus
extern "C" {
#endif
void      nucleo_codec_mic(bool on);
esp_err_t nucleo_codec_mic_open(int rate, i2s_chan_handle_t *out);
esp_err_t nucleo_codec_mic_read(i2s_chan_handle_t h, void *buf, size_t sz, size_t *got, int to);
void      nucleo_codec_mic_close(i2s_chan_handle_t h);
#ifdef __cplusplus
}
#endif
