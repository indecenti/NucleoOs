idf_component_register(
    SRCS "nucleo_anima.c" "anima_solve.c" "anima_text.c" "nucleo_anima_bench.c" "nucleo_anima_l1.c" "nucleo_anima_online.c" "nucleo_anima_learn.c" "nucleo_anima_profile.c" "nucleo_anima_translate.c" "nucleo_anima_hdc.c" "nucleo_anima_facet.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nucleo_board esp_timer
    # online tier (docs/anima-online.md): HTTPS fetch + JSON parse + the live Wi-Fi IP.
    # nucleo_arb: the heavy-work arbiter that serializes the outbound TLS window (no concurrent-OOM).
    PRIV_REQUIRES esp_http_client mbedtls json nucleo_setup nucleo_arb
)

# Bounded snprintf into fixed buffers on purpose (entity/slug/url/reply); truncation is safe.
# misleading-indentation: this file packs two statements per line in many spots; it's a style choice,
# not a bug, so don't let it -Werror the whole image (downgraded to a warning, behaviour unchanged).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-error=misleading-indentation)
