idf_component_register(
    SRCS "nucleo_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json nucleo_board esp_timer
)

# Bounded snprintf into fixed buffers (PIN, cookie, JSON) — truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
