idf_component_register(
    SRCS "nucleo_link_proto.c" "nucleo_link_bruce.c" "nucleo_link_espnow.c" "nucleo_link_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_timer esp_hw_support nucleo_board freertos esp_http_server json nucleo_auth
)
# nucleo_link_proto.c + nucleo_link_bruce.c are pure C (host-tested via npm run link:test).
# nucleo_link_espnow.c is the device engine (ESP-NOW + SD). It calls nucleo_setup_device_name() and
# nucleo_anima_app_ask() via extern decls (not REQUIRES) to avoid a dependency cycle with nucleo_app.

# Bounded snprintf into fixed-size SD path buffers on purpose; truncation is safe here (same
# convention as nucleo_recorder / nucleo_app). Without this the -Os build trips -Werror=format-truncation.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
