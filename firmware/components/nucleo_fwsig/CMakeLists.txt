idf_component_register(
    SRCS "fwsig.c" "fwsig_keys.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls
)
