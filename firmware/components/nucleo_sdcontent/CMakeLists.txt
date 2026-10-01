idf_component_register(
    SRCS "content_policy.c" "nucleo_sdcontent.c"
    INCLUDE_DIRS "include"
    # nucleo_board: the SD mount path (header only). nucleo_setup_ip / nucleo_i18n_lang resolve at final
    # link (the app pulls them in) to avoid a REQUIRES cycle, exactly like nucleo_app's weak externs.
    REQUIRES nucleo_board esp_http_client mbedtls nvs_flash esp_app_format esp_timer esp_system
)
