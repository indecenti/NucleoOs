idf_component_register(
    SRCS "nucleo_gb.c" "nucleo_gg.c" "vendor/minigb_apu.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board nucleo_audio esp_timer
)
# Peanut-GB (upstream + marked NUCLEO PATCHes, see vendor/README.md) and minigb_apu are mature,
# widely-used cores written to a different warning budget than this project's: silencing these here
# keeps the build strict for OUR code while keeping the vendored files close to upstream.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-type-limits)
# SPEED, not size, for this component alone. The project builds at -Os because flash is the scarce
# resource across ~60 components (see docs/firmware-size), but the emulator is the one place where a
# real-time deadline exists: 70,224 Game Boy clocks must be retired inside 16.7 ms, and -Os costs
# roughly a quarter of that budget in the CPU interpreter's dispatch loop. Scoping -O2 here buys the
# frame rate back for a few KB of flash instead of the ~250 KB a global -O2 would cost.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
# JUMP TABLES back on, for this component alone. ESP-IDF adds -fno-jump-tables -fno-tree-switch-conversion
# to EVERY file (a table in flash would break code that must run from IRAM with the cache off), and
# that turns the CPU's 256-way opcode switch — plus the CB-prefix switch and the memory-map switch in
# every read and write — into a binary tree of ~8 compare-and-branch steps per decode. An interpreter
# decodes millions of times a second, so this is the single most expensive thing the global default
# does to it. Nothing here runs with the flash cache disabled (the IRAM_ATTR callbacks are called from
# the emulator task, never from an ISR), so the reason for the global flag does not apply.
target_compile_options(${COMPONENT_LIB} PRIVATE -fjump-tables -ftree-switch-conversion)
# The APU refuses to compile without an explicit sample format. 16-bit signed is what the I2S sink
# takes, so anything else would only add a conversion pass between the APU and the speaker.
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)
