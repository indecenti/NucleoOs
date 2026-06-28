idf_component_register(
    SRCS "nucleo_ble.c" "nucleo_ble_adv.c" "nucleo_ble_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_timer esp_hw_support nvs_flash
)
