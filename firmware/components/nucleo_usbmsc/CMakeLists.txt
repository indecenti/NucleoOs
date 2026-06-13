idf_component_register(
    SRCS "nucleo_usbmsc.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_storage nucleo_ui nucleo_kbd sdmmc esp_tinyusb
)
