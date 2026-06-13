idf_component_register(
    SRCS "nucleo_ws.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server nucleo_eventbus nucleo_auth
)
