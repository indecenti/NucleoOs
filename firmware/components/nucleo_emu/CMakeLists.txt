idf_component_register(
    SRCS "nucleo_gb.c" "vendor/minigb_apu.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board nucleo_audio
)
# Peanut-GB and minigb_apu are vendored verbatim. They are mature, widely-used cores written to a
# different warning budget than this project's: silencing these here keeps the build strict for OUR
# code while leaving the upstream files byte-identical, so updating them stays a clean diff.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-type-limits)
# The APU refuses to compile without an explicit sample format. 16-bit signed is what the I2S sink
# takes, so anything else would only add a conversion pass between the APU and the speaker.
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)
