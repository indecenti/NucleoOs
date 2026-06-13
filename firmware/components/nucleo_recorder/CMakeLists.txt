idf_component_register(
    SRCS "nucleo_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json esp_driver_i2s nucleo_board nucleo_eventbus nucleo_auth
)

# Bounded snprintf into fixed buffers on purpose; truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
