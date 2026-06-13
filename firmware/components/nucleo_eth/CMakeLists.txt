idf_component_register(
    SRCS "eth_frames.c" "nucleo_w5500.c" "nucleo_eth.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer esp_hw_support nucleo_board
)

# Bounded snprintf into fixed-size IP/MAC/path buffers on purpose; truncation is safe here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

# nucleo_exclusive.h (NX_NET_APP reclaim) lives in nucleo_app. Include-dir only — a REQUIRES would
# cycle (nucleo_app REQUIRES nucleo_eth); the symbols resolve at final link since main pulls nucleo_app
# in. Same arrangement nucleo_wifiatk uses.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_app/include")
