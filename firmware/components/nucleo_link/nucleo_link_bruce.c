// nucleo_link_bruce.c — Bruce-compatible Message codec. See nucleo_link_bruce.h.
#include "nucleo_link_bruce.h"
#include <string.h>

// Lock the wire layout to Bruce-on-ESP32. If any of these fire, NucleoOS would no longer interoperate
// with a real Bruce device — the fix is the struct, never the assert.
_Static_assert(sizeof(bruce_msg_t) == BRUCE_MSG_SIZE, "Bruce Message must be 248 bytes");
_Static_assert(offsetof(bruce_msg_t, filename)   == 0,   "filename @0");
_Static_assert(offsetof(bruce_msg_t, filepath)   == 30,  "filepath @30");
_Static_assert(offsetof(bruce_msg_t, data)       == 80,  "data @80");
_Static_assert(offsetof(bruce_msg_t, dataSize)   == 232, "dataSize @232 (2B pad after data[150])");
_Static_assert(offsetof(bruce_msg_t, totalBytes) == 236, "totalBytes @236");
_Static_assert(offsetof(bruce_msg_t, bytesSent)  == 240, "bytesSent @240");
_Static_assert(offsetof(bruce_msg_t, isFile)     == 244, "isFile @244");
_Static_assert(offsetof(bruce_msg_t, pong)       == 247, "pong @247");

void bruce_build_ping(bruce_msg_t *m) { memset(m, 0, sizeof(*m)); m->ping = 1; }
void bruce_build_pong(bruce_msg_t *m) { memset(m, 0, sizeof(*m)); m->pong = 1; }

void bruce_file_init(bruce_msg_t *m, const char *filename, const char *filepath, uint32_t total_bytes) {
    memset(m, 0, sizeof(*m));
    m->isFile = 1;
    m->totalBytes = total_bytes;
    strncpy(m->filename, filename ? filename : "", BRUCE_FILENAME_SIZE - 1);
    strncpy(m->filepath, filepath ? filepath : "", BRUCE_FILEPATH_SIZE - 1);
}

void bruce_file_chunk(bruce_msg_t *m, const uint8_t *chunk, uint32_t chunk_len) {
    if (chunk_len > BRUCE_DATA_SIZE) chunk_len = BRUCE_DATA_SIZE;
    memcpy(m->data, chunk, chunk_len);
    m->dataSize = chunk_len;
    m->bytesSent += chunk_len;
    if (m->bytesSent > m->totalBytes) m->bytesSent = m->totalBytes;
    m->done = (m->bytesSent == m->totalBytes);
}
