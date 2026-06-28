idf_component_register(
    SRCS "nucleo_mesh.c" "nucleo_chorus.c" "nucleo_swarm_sec.c" "nucleo_swarm.c" "nucleo_swarm_espnow.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls esp_wifi esp_timer esp_system freertos nucleo_board
)
# nucleo_swarm_espnow.c is the device engine (ESP-NOW + mbedtls HMAC/PBKDF2). It calls
# nucleo_setup_device_name() via an extern decl (not REQUIRES) to avoid a dependency cycle, exactly
# as nucleo_link_espnow.c does.
# nucleo_mesh.c     = PURE SANS-I/O gossip seam (npm run mesh:test).
# nucleo_chorus.c   = PURE SANS-I/O capability directory riding chorus.* over MESH (npm run chorus:test).
# nucleo_swarm_sec.c= PURE SANS-I/O security gate: demux + HMAC seal/open (hash fn injected) + MAC
#                     trust-pin (npm run swarm:test). The injected hash fn is mbedtls HMAC-SHA256 on
#                     device; REQUIRES mbedtls for that shim.
# All three are no-esp_* and host-tested. The device binding (esp_now radio + PMK/LMK + nucleo_eventbus
# add_sink/inject + the organ's nucleo_anima_l1_query delegation) is the thin build-gated shim (P3b).
