// Host stubs for app_video.cpp: the browser renders for real; playback never runs in a scene (no decoder, no I2S).
#include <stdlib.h>
#include "host_state.h"
#include "esp_jpeg_dec.h"
extern "C" {
#include "nucleo_audio.h"
#include "nucleo_power.h"
}
#include "nucleo_app.h"
#include "nucleo_exclusive.h"
#include "nucleo_extdisp.h"

void *jpeg_calloc_align(size_t size, int) { return calloc(1, size); }
void jpeg_free_align(void *p) { free(p); }
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *, jpeg_dec_handle_t *) { return JPEG_ERR_FAIL; }
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t, jpeg_dec_io_t *, jpeg_dec_header_info_t *) { return JPEG_ERR_FAIL; }
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t, int *) { return JPEG_ERR_FAIL; }
jpeg_error_t jpeg_dec_get_process_count(jpeg_dec_handle_t, int *) { return JPEG_ERR_FAIL; }
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t, jpeg_dec_io_t *) { return JPEG_ERR_FAIL; }
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t) { return JPEG_ERR_OK; }

extern "C" esp_err_t nucleo_audio_pcm_open(int, int) { return ESP_FAIL; }
extern "C" esp_err_t nucleo_audio_pcm_open_ex(int, int, int) { return ESP_FAIL; }
extern "C" int nucleo_audio_pcm_queue_frames(void) { return 0; }
extern "C" esp_err_t nucleo_audio_pcm_write(const int16_t *, size_t) { return ESP_OK; }
extern "C" void nucleo_audio_pcm_close(void) {}
extern "C" void nucleo_audio_fade_in(int) {}
extern "C" void nucleo_power_perf_begin(void) {}
extern "C" void nucleo_power_perf_end(void) {}

void nucleo_app_display_hold(bool) {}
const char *nucleo_app_take_open_file(void) { return nullptr; }
bool nucleo_app_two_screens(void) { return g_host.large || g_host.two_screens; }
bool nucleo_exclusive_active(void) { return false; }
extern "C" void *nucleo_extdisp_internal_gfx(void) { return nullptr; }
