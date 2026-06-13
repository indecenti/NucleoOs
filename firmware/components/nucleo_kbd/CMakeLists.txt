idf_component_register(
    SRCS "nucleo_kbd.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_rom esp_driver_i2c esp_timer
)
