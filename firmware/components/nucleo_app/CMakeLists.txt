idf_component_register(
    SRCS "nucleo_app.cpp" "launcher_menu.cpp" "launcher_render.cpp" "app_info.cpp" "app_remote.cpp" "app_clock.cpp" "app_sysmon.cpp" "app_calc.cpp" "calc_eval.c" "app_recorder.cpp" "app_files.cpp" "app_photos.cpp" "app_player.cpp" "app_player_db.cpp" "app_video.cpp" "app_notepad.cpp" "app_calendar.cpp" "calendar_svc.cpp" "nucleo_notify.cpp" "app_notify.cpp" "app_ui.cpp" "app_usb.cpp" "app_radio.cpp" "app_anima.cpp" "app_torch.cpp" "app_usbkbd.cpp" "app_evilportal.cpp" "app_wifiatk.cpp" "app_beacon.cpp" "app_ethernet.cpp" "app_voice.cpp" "app_voicelab.cpp" "nucleo_exclusive.c" "app_ssh.cpp" "app_wifi.cpp" "app_ir.cpp" "app_link.cpp" "app_micspec.cpp" "app_qr.cpp"
    INCLUDE_DIRS "include"
    REQUIRES m5stack__m5gfx nucleo_storage nucleo_registry esp_timer nucleo_kbd nucleo_recorder nucleo_board nucleo_audio json esp_http_client esp_http_server mbedtls nucleo_ui nucleo_power nucleo_usbmsc nucleo_usbhid nucleo_evilportal nucleo_wifiatk nucleo_eth nucleo_voice nucleo_tts libssh2 lwip nucleo_link nucleo_micspec
)

# Bounded snprintf into fixed-size UI buffers on purpose; truncation is safe here.
# -Wno-missing-field-initializers: every app registers a nucleo_app_def_t with a POSITIONAL initializer
# that intentionally omits the trailing optional exclusive_flags (defaults to 0). Same benign pattern as
# the esp_timer/httpd_uri structs here. Suppress so the zero-default convention stays warning-clean.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-missing-field-initializers)

# ANIMA shell (app_anima.cpp) needs the assistant's public header for anima_result_t, but a
# REQUIRES on nucleo_anima would cycle (nucleo_anima -> nucleo_setup -> nucleo_app). Pull in only
# the include dir; the symbols resolve at final link since main already pulls nucleo_anima in —
# the same arrangement nucleo_app.cpp already uses for nucleo_anima_l1_unload().
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_anima/include")

# /api/display handler needs the auth guard macro (nucleo_auth.h). Same escape hatch: include
# dir only (a REQUIRES could cycle), symbols resolve at final link since httpd pulls nucleo_auth in.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_auth/include")

# Native IR app (app_ir.cpp) calls nucleo_ir_* and reads the SD preset catalog. Include-dir only
# (a REQUIRES would cycle); symbols resolve at final link since httpd already pulls nucleo_ir in.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../nucleo_ir/include")
