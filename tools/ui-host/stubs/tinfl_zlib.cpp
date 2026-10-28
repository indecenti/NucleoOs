// tinfl_decompress for the host gallery: raw (or zlib-wrapped) inflate into a non-wrapping buffer, the way the
// firmware's pictures call it: in one go, or fed a chunk at a time (TINFL_FLAG_HAS_MORE_INPUT). The zlib stream
// lives in the decompressor's own bytes between calls. A bad stream fails, as a ROM call would.
#include "miniz.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>
extern "C" tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *in, size_t *in_size, mz_uint8 *,
                                         mz_uint8 *out_next, size_t *out_size, const mz_uint32 flags)
{
    z_stream *z = nullptr;
    if (r->m_state == 0) {
        z = (z_stream *)calloc(1, sizeof *z);
        if (!z || inflateInit2(z, (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) ? 15 : -15) != Z_OK) { free(z); return TINFL_STATUS_FAILED; }
        memcpy(r->m_pad, &z, sizeof z);
        r->m_state = 1;
    } else memcpy(&z, r->m_pad, sizeof z);
    z->next_in = (Bytef *)in; z->avail_in = (uInt)*in_size;
    z->next_out = out_next;   z->avail_out = (uInt)*out_size;
    const int rc = inflate(z, Z_NO_FLUSH);
    *in_size -= z->avail_in; *out_size -= z->avail_out;
    tinfl_status st;
    if (rc == Z_STREAM_END) st = TINFL_STATUS_DONE;
    else if (rc != Z_OK && rc != Z_BUF_ERROR) st = TINFL_STATUS_FAILED;
    else if (z->avail_out == 0) st = TINFL_STATUS_HAS_MORE_OUTPUT;
    else st = (flags & TINFL_FLAG_HAS_MORE_INPUT) ? TINFL_STATUS_NEEDS_MORE_INPUT : TINFL_STATUS_FAILED;
    if (st == TINFL_STATUS_DONE || st == TINFL_STATUS_FAILED) { inflateEnd(z); free(z); r->m_state = 2; }
    return st;
}
