idf_component_register(
    SRCS "store_engine.c" "store_verify.c" "store_keys.c" "nucleo_store.c"
    INCLUDE_DIRS "include"
    # nucleo_sdcontent: the HTTPS transport, SHA-256 and registry merge/drop (host-tested together:
    # npm run store:test). nucleo_i18n_lang resolves at final link, like nucleo_sdcontent's.
    REQUIRES nucleo_sdcontent nucleo_storage nucleo_board mbedtls esp_app_format freertos
)
