idf_component_register(
    SRCS "nucleo_setup.c" "wifi_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board nucleo_ui nucleo_storage json esp_wifi esp_netif esp_event nucleo_app nucleo_kbd lwip nvs_flash
)

# Bounded snprintf into fixed buffers on purpose; truncation is safe here.
# -Wno-missing-field-initializers: the two built-in nucleo_app_def_t here use positional initializers
# that omit the optional trailing exclusive_flags (defaults to 0) — same benign convention as nucleo_app.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-missing-field-initializers)
