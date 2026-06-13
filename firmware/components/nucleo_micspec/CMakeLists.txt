idf_component_register(
    SRCS "nucleo_micspec.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s nucleo_board nucleo_recorder
)
