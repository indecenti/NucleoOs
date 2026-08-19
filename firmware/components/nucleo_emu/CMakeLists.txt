idf_component_register(
    SRCS "nucleo_gb.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_board
)
# Peanut-GB is vendored verbatim (vendor/peanut_gb.h). It is a mature, widely-used core written to a
# different warning budget than this project's: silencing these here keeps the build strict for OUR
# code while leaving the upstream file byte-identical, so updating it stays a clean diff.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-type-limits)
