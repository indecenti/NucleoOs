idf_component_register(
    SRCS "nucleo_pnet.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_timer freertos
)
# nucleo_pnet.c is the device engine (best-effort ESP-NOW datagrams for real-time native games).
# It calls nucleo_setup_device_name() via an extern decl (not REQUIRES) to avoid a dependency cycle,
# exactly as nucleo_link_espnow.c / nucleo_swarm_espnow.c do. esp_now.h ships in the esp_wifi component.
