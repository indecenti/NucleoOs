idf_component_register(
    SRCS
        "core/fido_cbor.c" "core/fido_cose.c" "core/fido_authdata.c" "core/fido_keywrap.c"
        "core/fido_cred_store_ram.c" "core/fido_u2f.c" "core/fido_ctap2.c" "core/fido_ctaphid.c"
        "core/fido_pin.c"
        "tinycbor/cborencoder.c" "tinycbor/cborparser.c"
        "port/fido_crypto_mbedtls.c" "port/fido_efuse_key.c" "port/fido_store_nvs.c"
        "port/fido_bootmode.c" "port/fido_service.c" "port/fido_api.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "core" "tinycbor" "port"
    PRIV_REQUIRES nucleo_usbhid mbedtls nvs_flash esp_hw_support esp_system esp_http_server json
)

# The web API guards each route with NUCLEO_AUTH_GUARD (nucleo_auth.h). Include-dir
# only — a REQUIRES on nucleo_auth would cycle (nucleo_auth -> httpd -> nucleo_fido);
# the auth symbols resolve at final link since httpd already pulls nucleo_auth in.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_auth/include")
