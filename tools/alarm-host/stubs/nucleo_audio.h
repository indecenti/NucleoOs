#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int  nucleo_audio_volume(void);
void nucleo_audio_set_volume(int v);
void nucleo_audio_siren(int ms);
void nucleo_audio_siren_stop(void);
#ifdef __cplusplus
}
#endif
