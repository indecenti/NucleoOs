idf_component_register(
    SRCS "nucleo_wifiatk.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_timer nucleo_httpd
)

# Bounded snprintf into fixed-size SSID/BSSID buffers on purpose; truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

# nucleo_exclusive.h (NX_NET_APP reclaim) lives in nucleo_app. Include-dir only — a REQUIRES would
# cycle (nucleo_app REQUIRES nucleo_wifiatk); symbols resolve at final link since main pulls nucleo_app
# in (same arrangement this engine already uses for nucleo_discovery_*/nucleo_setup_*).
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_app/include")

# We provide a strong ieee80211_raw_frame_sanity_check() that shadows the one in the precompiled
# libnet80211.a so esp_wifi_80211_tx() accepts deauth/disassoc frames. Tell the linker to accept
# the duplicate (it keeps the first definition it scans — ours). This is the standard ESP-IDF
# raw-frame-injection arrangement.
idf_build_set_property(LINK_OPTIONS "-Wl,--allow-multiple-definition" APPEND)
