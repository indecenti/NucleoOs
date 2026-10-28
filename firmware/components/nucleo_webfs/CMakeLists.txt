idf_component_register(
    SRCS "nucleo_webfs.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board esp_http_server
)

# Rescue console served from flash when the SD shell is missing (see static_get). Kept as a
# single source shared with the dev simulator (tools/serve-shell.mjs serves this same file).
# handoff.html: the "preparing web mode" page served while the device reboots into server Solo.
# Both are embedded gzipped (served with Content-Encoding: gzip): ~18 KB of flash saved. gzip with
# mtime 0, so the same page always gives the same bytes (reproducible image).
foreach(page rescue handoff)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${page}.html")
    set(gz "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz")
    add_custom_command(OUTPUT "${gz}"
        COMMAND ${PYTHON} -c "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))" "${src}" "${gz}"
        DEPENDS "${src}"
        VERBATIM)
    add_custom_target(nucleo_webfs_${page}_gz DEPENDS "${gz}")
    target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY DEPENDS nucleo_webfs_${page}_gz)
endforeach()
