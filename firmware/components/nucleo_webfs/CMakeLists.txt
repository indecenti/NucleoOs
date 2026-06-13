idf_component_register(
    SRCS "nucleo_webfs.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board esp_http_server
)
