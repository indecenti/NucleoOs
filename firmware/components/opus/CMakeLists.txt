# libopus 1.5.2 (BSD-3-Clause, opus/COPYING), fixed point: the codec of the XiaoZhi voice assistant (store only).
# Nothing in the firmware calls it, so the linker keeps none of it in the image; it is built here so that every source
# has its flags in compile_commands.json, which is what the store's native_build.py compiles a module with.
# Copied unmodified from the xiph release tag v1.5.2 (git ddbe4838): only the SILK fixed + CELT + Opus sources and
# their headers (opus/sources.cmake lists the sources).
# Temporary buffers go to a pseudostack the module provides (NONTHREADSAFE_PSEUDOSTACK: it sets global_stack to its
# own heap block, ~13 KB to encode a 60 ms frame, ~6 KB to decode one), so a codec call needs little task stack
# besides silk_encode_frame_FIX's own 9.5 KB frame.
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
idf_component_register(
    SRCS ${OPUS_SOURCES}
    INCLUDE_DIRS "opus/include"
    PRIV_INCLUDE_DIRS "opus/celt" "opus/silk" "opus/silk/fixed" "opus/src" "opus"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API NONTHREADSAFE_PSEUDOSTACK GLOBAL_STACK_SIZE=16384 HAVE_LRINT HAVE_LRINTF)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -w)
