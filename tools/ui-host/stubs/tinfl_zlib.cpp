// tinfl_decompress for the host gallery: one-shot raw (or zlib-wrapped) inflate into a non-wrapping buffer —
// the only way the firmware's pictures call it. Anything else fails, as a ROM call with a bad stream would.
#include "miniz.h"
#include <zlib.h>
extern "C" tinfl_status tinfl_decompress(tinfl_decompressor *, const mz_uint8 *in, size_t *in_size, mz_uint8 *,
                                         mz_uint8 *out_next, size_t *out_size, const mz_uint32 flags)
{
    z_stream z = {};
    if (inflateInit2(&z, (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) ? 15 : -15) != Z_OK) return TINFL_STATUS_FAILED;
    z.next_in = (Bytef *)in; z.avail_in = (uInt)*in_size;
    z.next_out = out_next;   z.avail_out = (uInt)*out_size;
    const int rc = inflate(&z, Z_FINISH);
    *in_size -= z.avail_in; *out_size -= z.avail_out;
    inflateEnd(&z);
    return rc == Z_STREAM_END ? TINFL_STATUS_DONE : TINFL_STATUS_FAILED;
}
