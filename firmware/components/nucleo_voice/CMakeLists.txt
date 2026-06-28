idf_component_register(
    SRCS "nucleo_voice.c" "nucleo_voice_dsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_timer nucleo_board nucleo_codec nucleo_eventbus nucleo_kbd fatfs
    PRIV_REQUIRES nucleo_anima nucleo_ui nucleo_tts json
)
# sqrtf is in libm (already linked by ESP-IDF, but make intent explicit)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
