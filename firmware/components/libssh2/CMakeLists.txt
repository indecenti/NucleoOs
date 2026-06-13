# NucleoOS — libssh2 (1.11.1) vendored as an ESP-IDF component, mbedTLS crypto backend.
# Upstream source lives in upstream/ (git clone). We compile the portable src/*.c minus the
# non-mbedTLS crypto backends and the Windows/OS400-only files. Config comes from
# ../libssh2_config.h (this dir) via HAVE_CONFIG_H.

file(GLOB LIBSSH2_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/src/*.c")
# keep mbedtls.c; drop the other crypto backends and platform-specific files
list(FILTER LIBSSH2_SRCS EXCLUDE REGEX ".*/(openssl|libgcrypt|wincng|os400qc3|agent_win)\\.c$")

idf_component_register(
    SRCS ${LIBSSH2_SRCS}
    INCLUDE_DIRS "upstream/include"
    PRIV_INCLUDE_DIRS "upstream/src" "."
    REQUIRES mbedtls lwip
)

# HAVE_CONFIG_H -> libssh2_setup.h includes our libssh2_config.h; LIBSSH2_MBEDTLS selects the backend;
# MBEDTLS_ALLOW_PRIVATE_ACCESS lets libssh2's mbedtls.c reach mbedTLS-3 struct fields it still uses.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H LIBSSH2_MBEDTLS MBEDTLS_ALLOW_PRIVATE_ACCESS)
# libssh2 is third-party C with many -Wall findings; don't let them fail our -Werror-ish build.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
