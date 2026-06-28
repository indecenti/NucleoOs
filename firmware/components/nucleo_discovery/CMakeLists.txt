idf_component_register(
    SRCS "nucleo_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__mdns esp_app_format
)
