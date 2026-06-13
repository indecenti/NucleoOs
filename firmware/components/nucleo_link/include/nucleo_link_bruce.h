// nucleo_link_bruce.h — Bruce-compatible ESP-NOW share codec ("Bruce mode" of the Vicino app).
//
// Wire-identical to Bruce's EspConnection::Message (reference/bruce/src/core/connect/esp_connection.h)
// so a NucleoOS device can send/receive files with a real Bruce device. Bruce declares the size fields
// as C++ `size_t`, which is 4 bytes on the ESP32 (xtensa, 32-bit); we use uint32_t so the layout is
// IDENTICAL on both the device and the x86-64 host gate. The _Static_asserts in the .c lock the 248-byte
// layout against compiler drift.
//
// Bruce's protocol is intentionally naive (no ACK, no retransmission, filename re-sent every frame,
// 150-byte payload, ~100ms pacing). In Bruce mode we MUST match it; the evolved reliability lives in
// nucleo_link.h (Nucleo<->Nucleo mode).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRUCE_FILENAME_SIZE 30
#define BRUCE_FILEPATH_SIZE 50
#define BRUCE_DATA_SIZE     150
#define BRUCE_MSG_SIZE      248          // sizeof(Message) on ESP32 — what Bruce puts on the wire

#pragma pack(push, 4)
typedef struct {
    char     filename[BRUCE_FILENAME_SIZE];
    char     filepath[BRUCE_FILEPATH_SIZE];
    char     data[BRUCE_DATA_SIZE];
    uint32_t dataSize;                   // Bruce: size_t (== uint32_t on ESP32)
    uint32_t totalBytes;
    uint32_t bytesSent;
    uint8_t  isFile;                     // Bruce: bool
    uint8_t  done;
    uint8_t  ping;
    uint8_t  pong;
} bruce_msg_t;
#pragma pack(pop)

// ---- builders (mirror EspConnection::createXxxMessage + the FileSharing::sendFile loop) ----
void bruce_build_ping(bruce_msg_t *m);
void bruce_build_pong(bruce_msg_t *m);
// Prime a file transfer: filename (basename) + filepath (dir, no trailing '/') + total size.
void bruce_file_init(bruce_msg_t *m, const char *filename, const char *filepath, uint32_t total_bytes);
// Fill one data frame; updates bytesSent/done. `chunk` is up to BRUCE_DATA_SIZE bytes.
void bruce_file_chunk(bruce_msg_t *m, const uint8_t *chunk, uint32_t chunk_len);

// ---- classify an inbound frame ----
static inline bool bruce_is_msg(const void *buf, int len) { return buf && len == BRUCE_MSG_SIZE; }

#ifdef __cplusplus
}
#endif
