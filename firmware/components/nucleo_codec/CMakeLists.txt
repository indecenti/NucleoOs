idf_component_register(
    SRCS "nucleo_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_i2s nucleo_board
)
