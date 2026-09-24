# The Game Gear core's real cost on the ESP32-S3 CPU, measured under QEMU -icount (see ../run.ps1).
# bench.c includes firmware/components/nucleo_emu/nucleo_gg.c itself, with the SD card replaced by
# ROM images embedded in flash (fmemopen), and the same compile flags the firmware component uses.
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(EMU  "${REPO}/firmware/components/nucleo_emu")
idf_component_register(SRCS "bench.c"
    INCLUDE_DIRS "." "${EMU}" "${EMU}/include" "${REPO}/firmware/components/nucleo_audio/include"
    REQUIRES esp_timer
    EMBED_FILES "../roms/sonic.gg" "../roms/sonic2.gg" "../roms/chaos.gg" "../roms/mm2.gg" "../roms/ewj.gg" "../roms/sor2.gg")
target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_SKIP=$ENV{BENCH_SKIP} NUCLEO_GG_PROFILE)
target_compile_options(${COMPONENT_LIB} PRIVATE $ENV{BENCH_OPT} -fjump-tables -ftree-switch-conversion -Wno-unused-function)
