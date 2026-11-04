# Nucleo Code (docs/nucleo-code.md): the script runtime of the Code store module. Every source here is
# store-only (guarded by NAP_MODULE): in the firmware build they compile to nothing; they are listed so
# the module build (tools/native_build.py) finds their flags in compile_commands.json.
idf_component_register(
    SRCS "code_rt.c" "code_agent.c" "code_mquickjs.c" "code_dtoa.c" "code_libm.c" "code_cutils.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "gen" "vendor/mquickjs" "../nucleo_anima"
    PRIV_REQUIRES json
)
# MicroQuickJS is written to its own warning budget; keep ours strict and the vendored code close to upstream.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
    -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-format -Wno-dangling-pointer
    -fno-math-errno -fno-trapping-math)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG _GNU_SOURCE)
