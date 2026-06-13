idf_component_register(
    SRCS "nucleo_httpd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_http_client mbedtls json nucleo_storage nucleo_registry nucleo_webfs nucleo_ws nucleo_fsapi nucleo_recorder nucleo_ir nucleo_gpio nucleo_setup nucleo_auth nucleo_anima nucleo_eventbus nucleo_arb nucleo_power esp_timer esp_system app_update nucleo_voice nucleo_tts
)
