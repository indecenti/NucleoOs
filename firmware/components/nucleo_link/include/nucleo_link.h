// nucleo_link — NucleoOS "Vicino" device-to-device link (evolved, Nucleo<->Nucleo mode).
//
// A reliable file/command transfer protocol on top of a connectionless datagram radio
// (ESP-NOW on device; an in-memory lossy channel in the host gate). This is the EVOLVED
// path — the Bruce-compatible path lives in nucleo_link_bruce.h.
//
// vs Bruce's naive ESP-NOW share (reference/bruce/src/core/connect): Bruce sends 150-byte
// chunks every 100ms with NO ACK and NO retransmission (one dropped frame corrupts the file)
// and re-sends the filename+filepath in EVERY frame. This protocol adds:
//   • sliding window + cumulative/selective ACK + timeout retransmission (lossless),
//   • whole-file CRC32 integrity verification,
//   • resume of an interrupted transfer from the contiguous prefix already received,
//   • metadata (name/size/crc) sent ONCE in an OFFER, then pure-data frames (more payload),
//   • named peers via ping/pong (not raw MAC).
//
// Design: SANS-I/O state machine. The core never calls esp_now or the SD card directly — it
// drives everything through the nlink_io callbacks. That keeps it (a) ZERO-HEAP and RAM-O(window)
// regardless of file size, and (b) fully host-testable on a PC under simulated packet loss.
// See tools/anima-host/link-ctest.c (npm run link:test).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NLINK_MTU       250          // ESP-NOW max payload
#define NLINK_VER       1
#define NLINK_HDR       8            // common header: magic(2) ver(1) type(1) session(4)
#define NLINK_CHUNK     236          // data bytes per DATA frame (MTU - HDR - seq(4) - len(2))
#define NLINK_WINDOW    8            // in-flight frames (<=32; bounds RAM on BOTH ends)
#define NLINK_NAME_MAX  48

enum nlink_type {
    NL_PING = 1, NL_PONG, NL_OFFER, NL_OFFER_ACK, NL_DATA, NL_ACK, NL_DONE, NL_FIN, NL_ABORT,
};
enum nlink_role  { NL_ROLE_IDLE = 0, NL_ROLE_SEND, NL_ROLE_RECV };
enum nlink_state { NL_ST_IDLE = 0, NL_ST_OFFER, NL_ST_RUN, NL_ST_DRAIN, NL_ST_DONE, NL_ST_FAIL };
enum nlink_mode  { NL_MODE_FILE = 0, NL_MODE_CMD = 1 };
enum nlink_ev    { NL_EV_PEER = 1, NL_EV_OFFER, NL_EV_PROGRESS, NL_EV_COMPLETE, NL_EV_FAILED, NL_EV_PING };
enum nlink_reason{ NL_R_NONE = 0, NL_R_TIMEOUT, NL_R_DECLINED, NL_R_CRC, NL_R_IO, NL_R_ABORT, NL_R_PROTO };

typedef struct {
    uint32_t session;
    uint32_t total_size;
    uint32_t n_chunks;
    uint32_t crc32;
    uint16_t chunk_size;
    uint8_t  mode;                       // nlink_mode
    char     name[NLINK_NAME_MAX + 1];
} nlink_offer_t;

typedef struct {
    const uint8_t *peer;                 // 6-byte MAC (PEER/PING)
    const char    *name;                 // peer name (PEER) or offer name (OFFER)
    nlink_offer_t  offer;                // valid for NL_EV_OFFER
    uint32_t       done_bytes, total_bytes; // PROGRESS
    int            reason;               // nlink_reason (FAILED)
} nlink_evt_t;

// I/O seam the host implements (radio + storage). All return 0 on success unless noted.
typedef struct {
    // Send one frame to peer6 (NULL = broadcast). Return 0 on success.
    int  (*send_frame)(void *user, const uint8_t *peer6, const uint8_t *buf, int len);
    // SENDER: copy chunk #seq of the source into buf (capacity NLINK_CHUNK). Return byte count.
    int  (*read_chunk)(void *user, uint32_t seq, uint8_t *buf);
    // RECEIVER: write `len` bytes of chunk #seq at byte offset seq*chunk_size. Return 0 on success.
    int  (*write_chunk)(void *user, uint32_t seq, const uint8_t *buf, int len);
    // RECEIVER: compute the CRC32 of the fully-received file (for integrity verify).
    uint32_t (*verify)(void *user);
    // Notify the app (progress / offer / completion / peer discovery).
    void (*on_event)(void *user, int ev, const nlink_evt_t *e);
    void *user;
} nlink_io_t;

// Fully-public so apps can stack/static-allocate it (no heap on the PSRAM-less Cardputer).
typedef struct {
    const nlink_io_t *io;
    char     local_name[NLINK_NAME_MAX + 1];
    uint8_t  caps;
    int      role, state;
    uint8_t  peer[6];
    // transfer params (mirrors the OFFER)
    uint32_t session, total_size, n_chunks, crc32;
    uint16_t chunk_size;
    uint8_t  mode;
    char     name[NLINK_NAME_MAX + 1];
    // sender window
    uint32_t base, next;
    struct { uint32_t seq, sent_ms; uint8_t acked, used; uint16_t tries; } win[NLINK_WINDOW];
    // receiver
    uint32_t recv_base;                  // contiguous chunks done (also the resume high-water)
    uint32_t recv_mask;                  // out-of-order received in (recv_base, recv_base+32]
    bool     offer_pending;
    // timing
    uint32_t now_ms, last_rx_ms, last_ack_ms, last_prog_ms, last_done_ms;
} nlink_ctx_t;

// ---- lifecycle -------------------------------------------------------------
void nlink_init(nlink_ctx_t *c, const nlink_io_t *io, const char *local_name);

// ---- discovery -------------------------------------------------------------
void nlink_discover(nlink_ctx_t *c);     // broadcast a PING (peers reply PONG -> NL_EV_PEER)

// ---- sender ----------------------------------------------------------------
// Begin offering `name` (total_size bytes, precomputed crc32) to peer6. `session` should be a
// fresh nonce (esp_random on device, a counter in tests).
void nlink_send_begin(nlink_ctx_t *c, const uint8_t *peer6, uint32_t session,
                      const char *name, uint32_t total_size, uint8_t mode, uint32_t crc32);

// ---- receiver --------------------------------------------------------------
void nlink_recv_listen(nlink_ctx_t *c); // wait for an OFFER (fires NL_EV_OFFER)
// Answer a pending OFFER. accept=false declines. have_prefix = chunks already on disk (resume); 0 = fresh.
void nlink_recv_accept(nlink_ctx_t *c, bool accept, uint32_t have_prefix);

// ---- driving ---------------------------------------------------------------
void nlink_on_frame(nlink_ctx_t *c, const uint8_t *peer6, const uint8_t *buf, int len, uint32_t now_ms);
void nlink_tick(nlink_ctx_t *c, uint32_t now_ms);
void nlink_abort(nlink_ctx_t *c);

static inline bool nlink_busy(const nlink_ctx_t *c) {
    return c->state == NL_ST_OFFER || c->state == NL_ST_RUN || c->state == NL_ST_DRAIN;
}

// ---- util ------------------------------------------------------------------
uint32_t nlink_crc32(uint32_t crc, const uint8_t *p, size_t n);  // seed with 0

#ifdef __cplusplus
}
#endif
