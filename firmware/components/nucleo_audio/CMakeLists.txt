idf_component_register(
    SRCS "nucleo_audio.c" "nucleo_audio_mp3.c" "nucleo_audio_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s nucleo_board nucleo_codec esp_http_client mbedtls nucleo_eventbus
    PRIV_REQUIRES esp_timer   # monotonic clock for the fade-in ramp (the media clock is seek-rebased)
)
# Bounded snprintf into fixed buffers on purpose; truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
