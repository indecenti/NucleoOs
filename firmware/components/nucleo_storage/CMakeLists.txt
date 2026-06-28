idf_component_register(
    SRCS "nucleo_storage.c" "nucleo_log.c" "nucleo_i18n.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board fatfs sdmmc esp_driver_spi esp_driver_sdspi json esp_app_format
)
