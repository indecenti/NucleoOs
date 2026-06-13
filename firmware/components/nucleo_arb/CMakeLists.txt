idf_component_register(
    SRCS "nucleo_arb.c" "nucleo_arb_plat_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nucleo_eventbus
    PRIV_REQUIRES esp_timer esp_common freertos heap
)
