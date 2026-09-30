idf_component_register(
    SRCS "nucleo_webfs.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board esp_http_server
    # Rescue console served from flash when the SD shell is missing (see static_get). Kept as a
    # single source shared with the dev simulator (tools/serve-shell.mjs serves this same file).
    # handoff.html: the "preparing web mode" page served while the device reboots into server Solo.
    EMBED_TXTFILES "rescue.html" "handoff.html"
)
