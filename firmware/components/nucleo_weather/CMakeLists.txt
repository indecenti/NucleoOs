idf_component_register(
    SRCS "weather_wmo.c" "nucleo_weather.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls json nucleo_arb
)
