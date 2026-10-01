idf_component_register(
    SRCS "nucleo_registry.c" "registry_scan.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board json
)
