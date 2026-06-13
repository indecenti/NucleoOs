idf_component_register(
    SRCS "nucleo_gpio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_http_server json nucleo_auth
)
