idf_component_register(
    SRCS "nucleo_evilportal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_http_server esp_timer lwip nucleo_httpd nucleo_board
)

# Bounded snprintf into fixed-size SSID/path buffers on purpose; truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

# nucleo_exclusive.h (NX_NET_APP reclaim) lives in nucleo_app. Include-dir only — a REQUIRES would
# cycle (nucleo_app REQUIRES nucleo_evilportal); the symbols resolve at final link since main pulls
# nucleo_app in (same arrangement this engine already uses for nucleo_discovery_*/nucleo_setup_*).
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_app/include")
