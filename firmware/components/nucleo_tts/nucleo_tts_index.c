// nucleo_tts_index — vedi nucleo_tts_index.h. Puro stdio (host-compilabile).
#include "nucleo_tts_index.h"
#include <string.h>

static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

bool tts_index_open(tts_index_t *ix, const char *index_path)
{
    if (!ix) return false;
    ix->f = NULL; ix->count = 0; ix->rate = 0;
    FILE *f = fopen(index_path, "rb");
    if (!f) return false;
    uint8_t hdr[TTS_IDX_HDR];
    if (fread(hdr, 1, TTS_IDX_HDR, f) != TTS_IDX_HDR || memcmp(hdr, "NTI1", 4) != 0) { fclose(f); return false; }
    ix->rate  = rd_u32(hdr + 4);
    ix->count = rd_u32(hdr + 8);
    if (ix->rate < 8000 || ix->rate > 48000 || ix->count == 0) { fclose(f); return false; }
    ix->f = f;
    return true;
}

bool tts_index_find(tts_index_t *ix, const char *slug, uint32_t *off, uint32_t *len)
{
    if (!ix || !ix->f || !slug || !slug[0]) return false;
    long lo = 0, hi = (long)ix->count - 1;
    uint8_t rec[TTS_IDX_REC];
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (fseek(ix->f, (long)TTS_IDX_HDR + mid * TTS_IDX_REC, SEEK_SET) != 0) return false;
        if (fread(rec, 1, TTS_IDX_REC, ix->f) != TTS_IDX_REC) return false;
        rec[TTS_IDX_SLUG - 1] = 0;                      // garantisci terminazione
        int c = strcmp(slug, (const char *)rec);
        if (c == 0) {
            if (off) *off = rd_u32(rec + TTS_IDX_SLUG);
            if (len) *len = rd_u32(rec + TTS_IDX_SLUG + 4);
            return true;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return false;
}

void tts_index_close(tts_index_t *ix)
{
    if (ix && ix->f) { fclose(ix->f); ix->f = NULL; }
}
