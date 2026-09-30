# Guest mode under M5Launcher (runtime-detected; stand-alone behaviour untouched). docs/m5launcher.md
idf_component_register(
    SRCS "nucleo_guest.c" "guest_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition app_update esp_hw_support nvs_flash
)
