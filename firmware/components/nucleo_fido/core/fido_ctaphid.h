// fido_ctaphid — the USB-HID transport for FIDO (CTAPHID). Reassembles 64-byte
// reports into messages, dispatches INIT/PING/MSG/CBOR/WINK/CANCEL, and fragments
// responses back out. The heavy response scratch is caller-provided so the core
// keeps no large statics (device RAM discipline).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIDO_HID_PKT        64
#define FIDO_HID_INIT_DATA  (FIDO_HID_PKT - 7)   // 57
#define FIDO_HID_CONT_DATA  (FIDO_HID_PKT - 5)   // 59
#ifndef FIDO_HID_MAXLEN
#define FIDO_HID_MAXLEN     2048                  // largest single transaction we accept
#endif
#define FIDO_HID_BROADCAST  0xFFFFFFFFu

// KEEPALIVE status bytes.
#define FIDO_HID_KA_PROCESSING 0x01
#define FIDO_HID_KA_UPNEEDED   0x02

typedef void     (*fido_hid_sink)(const uint8_t pkt[FIDO_HID_PKT], void *ctx);
typedef uint16_t (*fido_hid_msg_fn)(const uint8_t *req, uint16_t rl,
                                    uint8_t *resp, uint16_t cap, void *ctx);

typedef struct {
    // reassembler
    uint32_t cid; uint8_t cmd; uint16_t bcnt; uint16_t got; uint8_t seq; uint8_t active;
    uint8_t  buf[FIDO_HID_MAXLEN];
    // io
    fido_hid_sink   sink;   void *sink_ctx;
    fido_hid_msg_fn on_msg; void *msg_ctx;    // CTAPHID_MSG  (U2F)
    fido_hid_msg_fn on_cbor;void *cbor_ctx;   // CTAPHID_CBOR (FIDO2)
    uint8_t *rbuf; uint16_t rcap;             // response scratch (caller-owned)
    uint32_t next_cid;                        // next channel id to hand out
    uint32_t cur_cid;                         // channel of the in-flight transaction
} fido_ctaphid_ctx;

void fido_ctaphid_init(fido_ctaphid_ctx *c, fido_hid_sink sink, void *sink_ctx,
                       uint8_t *rbuf, uint16_t rcap);
void fido_ctaphid_set_msg(fido_ctaphid_ctx *c, fido_hid_msg_fn fn, void *ctx);
void fido_ctaphid_set_cbor(fido_ctaphid_ctx *c, fido_hid_msg_fn fn, void *ctx);

// Feed one 64-byte OUT report. Runs the whole request→response when a message
// completes. Returns the channel id the packet belonged to.
uint32_t fido_ctaphid_rx(fido_ctaphid_ctx *c, const uint8_t pkt[FIDO_HID_PKT]);

// Send a KEEPALIVE for the in-flight transaction (call from a blocking approval).
void fido_ctaphid_keepalive(fido_ctaphid_ctx *c, uint8_t status);

// Low-level framer, exposed for tests.
void fido_ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len,
                       fido_hid_sink sink, void *sctx);

#ifdef __cplusplus
}
#endif
