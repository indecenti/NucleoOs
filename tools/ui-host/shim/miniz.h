// Host stand-in for the ESP32 ROM's miniz.h: just the tinfl surface the firmware uses. The implementation is
// stubs/tinfl_zlib.cpp (zlib's raw inflate), linked with "@lib z" by the scenes that need it.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned char mz_uint8;
typedef uint32_t mz_uint32;
typedef struct { int m_state; unsigned char m_pad[11000]; } tinfl_decompressor;
typedef enum { TINFL_STATUS_FAILED = -1, TINFL_STATUS_DONE = 0, TINFL_STATUS_NEEDS_MORE_INPUT = 1, TINFL_STATUS_HAS_MORE_OUTPUT = 2 } tinfl_status;
enum { TINFL_FLAG_PARSE_ZLIB_HEADER = 1, TINFL_FLAG_HAS_MORE_INPUT = 2, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4 };
#define tinfl_init(r) do { (r)->m_state = 0; } while (0)
#ifdef __cplusplus
extern "C" {
#endif
tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *in, size_t *in_size, mz_uint8 *out_start,
                              mz_uint8 *out_next, size_t *out_size, const mz_uint32 flags);
#ifdef __cplusplus
}
#endif
