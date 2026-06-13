idf_component_register(
    SRCS "nucleo_fsapi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json nucleo_board nucleo_eventbus nucleo_storage nucleo_auth nucleo_registry
)

# We use bounded snprintf into fixed buffers on purpose; truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
