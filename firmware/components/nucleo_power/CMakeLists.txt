idf_component_register(
    SRCS "nucleo_power.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_pm esp_adc esp_timer
)
