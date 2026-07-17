idf_component_register(
    SRCS "core/sentinel_ble.c" "core/sentinel_track.c" "core/sentinel_wifi.c"
         "port/sentinel_radio.c" "port/sentinel_airspace.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_ble nucleo_wifiatk esp_timer
)
