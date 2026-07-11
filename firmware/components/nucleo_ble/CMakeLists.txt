# Bluetooth is OFF by default on this PSRAM-less chip: the BLE controller (libbtdm_app) keeps ~18 KB of
# non-releasable .text in DRAM even idle, starving the contiguous heap ANIMA needs at boot. When BT is
# disabled we compile a no-op stub (nucleo_ble_stub.c) so the OS + BLE app still build and run (the app
# reports the radio absent). Flip CONFIG_BT_ENABLED on to link the real NimBLE stack.
if(CONFIG_BT_ENABLED)
    idf_component_register(
        SRCS "nucleo_ble.c" "nucleo_ble_adv.c" "nucleo_ble_hid.c"
        INCLUDE_DIRS "include"
        REQUIRES bt esp_timer esp_hw_support nvs_flash
    )
else()
    idf_component_register(
        SRCS "nucleo_ble_stub.c"
        INCLUDE_DIRS "include"
        REQUIRES nvs_flash
    )
endif()
