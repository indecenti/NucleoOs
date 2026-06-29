idf_component_register(
    SRCS "nucleo_smtp.c" "nucleo_mailcfg.c" "smtp_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-tls mbedtls nvs_flash nucleo_arb esp_timer freertos esp_common log
)
