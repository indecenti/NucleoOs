# The Game Boy core's real cost on the ESP32-S3 CPU, measured under QEMU -icount (see ../run.ps1).
# Variant knobs come from the environment at configure time: BENCH_CORE (new|old), BENCH_HILCD (0|1),
# BENCH_JT (0|1 = jump tables), BENCH_OPT (-O2|-Os).
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(EMU  "${REPO}/firmware/components/nucleo_emu/vendor")
idf_component_register(SRCS "bench.c" "${EMU}/minigb_apu.c"
    INCLUDE_DIRS "." "${EMU}"
    REQUIRES esp_timer
    EMBED_FILES "../roms/tetris.gb" "../roms/kirby.gb" "../roms/zelda.gb" "../roms/sf2.gb" "../roms/tetris2.gb" "../roms/mm5.gb")
target_compile_options(${COMPONENT_LIB} PRIVATE $ENV{BENCH_OPT} -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-type-limits -Wno-unused-function)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS HILCD=$ENV{BENCH_HILCD})
if("$ENV{BENCH_CORE}" STREQUAL "old")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE "CORE_H=\"peanut_old.h\"")
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE "CORE_H=\"${EMU}/peanut_gb.h\"")
endif()
if("$ENV{BENCH_JT}" STREQUAL "1")
  target_compile_options(${COMPONENT_LIB} PRIVATE -fjump-tables -ftree-switch-conversion)
endif()
