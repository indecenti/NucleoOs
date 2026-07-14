# Bluetooth is OFF by default on this PSRAM-less chip: the BLE controller (libbtdm_app) keeps ~18 KB of
# non-releasable .text in DRAM even idle, starving the contiguous heap ANIMA needs at boot. When BT is
# disabled we compile a no-op stub (nucleo_ble_stub.c) so the OS + BLE app still build and run (the app
# reports the radio absent). Flip CONFIG_BT_ENABLED on to link the real NimBLE stack.
# IDF expands a component's REQUIRES/PRIV_REQUIRES BEFORE sdkconfig is loaded, so they must NOT be gated on
# CONFIG_*: a conditional `REQUIRES` silently takes the else/off branch while `SRCS` (re-evaluated with config
# in the build pass) takes the on branch — the real BLE sources then compile WITHOUT esp_timer/nimble on the
# include path (fatal: esp_timer.h / nimble/ble.h not found). So keep requirements UNCONDITIONAL and vary only
# SRCS. When CONFIG_BT_ENABLED is off, the `bt` component registers empty, so the stub keeps its RAM benefit.
if(CONFIG_BT_ENABLED)
    set(ble_srcs "nucleo_ble.c" "nucleo_ble_adv.c" "nucleo_ble_hid.c")
else()
    set(ble_srcs "nucleo_ble_stub.c")
endif()
idf_component_register(
    SRCS ${ble_srcs}
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES bt esp_timer esp_hw_support
)
