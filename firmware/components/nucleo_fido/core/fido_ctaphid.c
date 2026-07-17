#include "fido_ctaphid.h"
#include <string.h>

enum { W_PING=0x81, W_MSG=0x83, W_INIT=0x86, W_WINK=0x88, W_CBOR=0x90,
       W_CANCEL=0x91, W_KEEPALIVE=0xBB, W_ERROR=0xBF };
enum { ERR_INVALID_CMD=0x01, ERR_INVALID_LEN=0x03, ERR_INVALID_SEQ=0x04,
       ERR_CHANNEL_BUSY=0x06, ERR_OTHER=0x7F };

static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static void     wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }

void fido_ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len,
                       fido_hid_sink sink, void *sctx) {
    uint8_t pkt[FIDO_HID_PKT];
    memset(pkt, 0, sizeof pkt);
    wr32(pkt, cid);
    pkt[4] = cmd; pkt[5] = (uint8_t)(len >> 8); pkt[6] = (uint8_t)len;
    uint16_t n = len < FIDO_HID_INIT_DATA ? len : FIDO_HID_INIT_DATA;
    if (n) memcpy(pkt + 7, data, n);
    sink(pkt, sctx);
    uint16_t off = n;
    uint8_t seq = 0;
    while (off < len) {
        memset(pkt, 0, sizeof pkt);
        wr32(pkt, cid);
        pkt[4] = seq++;
        uint16_t remain = len - off;
        uint16_t c = remain < FIDO_HID_CONT_DATA ? remain : FIDO_HID_CONT_DATA;
        memcpy(pkt + 5, data + off, c);
        sink(pkt, sctx);
        off += c;
    }
}

void fido_ctaphid_init(fido_ctaphid_ctx *c, fido_hid_sink sink, void *sink_ctx,
                       uint8_t *rbuf, uint16_t rcap) {
    memset(c, 0, sizeof *c);
    c->sink = sink; c->sink_ctx = sink_ctx;
    c->rbuf = rbuf; c->rcap = rcap;
    c->next_cid = 1;
    c->cur_cid = FIDO_HID_BROADCAST;
}
void fido_ctaphid_set_msg(fido_ctaphid_ctx *c, fido_hid_msg_fn fn, void *ctx)  { c->on_msg = fn;  c->msg_ctx = ctx; }
void fido_ctaphid_set_cbor(fido_ctaphid_ctx *c, fido_hid_msg_fn fn, void *ctx) { c->on_cbor = fn; c->cbor_ctx = ctx; }

static void send_err(fido_ctaphid_ctx *c, uint32_t cid, uint8_t code) {
    fido_ctaphid_send(cid, W_ERROR, &code, 1, c->sink, c->sink_ctx);
}

void fido_ctaphid_keepalive(fido_ctaphid_ctx *c, uint8_t status) {
    if (c->cur_cid != FIDO_HID_BROADCAST)
        fido_ctaphid_send(c->cur_cid, W_KEEPALIVE, &status, 1, c->sink, c->sink_ctx);
}

// Reassemble one report. Returns 1 when a full message is ready, 0 when more
// packets are needed, -1 on a framing error (already reset).
static int feed(fido_ctaphid_ctx *a, const uint8_t pkt[FIDO_HID_PKT]) {
    uint32_t cid = rd32(pkt);
    if (pkt[4] & 0x80) {                         // init packet
        a->cid = cid;
        a->cmd = pkt[4];
        a->bcnt = (uint16_t)pkt[5] << 8 | pkt[6];
        if (a->bcnt > FIDO_HID_MAXLEN) { a->active = 0; return -1; }
        uint16_t n = a->bcnt < FIDO_HID_INIT_DATA ? a->bcnt : FIDO_HID_INIT_DATA;
        memcpy(a->buf, pkt + 7, n);
        a->got = n; a->seq = 0; a->active = 1;
        return a->got >= a->bcnt ? 1 : 0;
    }
    if (!a->active || cid != a->cid) return -1;  // stray continuation
    if (pkt[4] != a->seq) { a->active = 0; return -1; }
    a->seq++;
    uint16_t remain = a->bcnt - a->got;
    uint16_t n = remain < FIDO_HID_CONT_DATA ? remain : FIDO_HID_CONT_DATA;
    memcpy(a->buf + a->got, pkt + 5, n);
    a->got += n;
    return a->got >= a->bcnt ? 1 : 0;
}

uint32_t fido_ctaphid_rx(fido_ctaphid_ctx *c, const uint8_t pkt[FIDO_HID_PKT]) {
    int r = feed(c, pkt);
    if (r == 0) return c->cid;                   // need more packets
    if (r < 0) { send_err(c, c->cid, ERR_INVALID_LEN); return c->cid; }

    uint32_t cid = c->cid;
    uint8_t  cmd = c->cmd;
    c->cur_cid = cid;
    switch (cmd) {
        case W_INIT: {
            // INIT on the broadcast channel allocates a new channel id; INIT on an
            // existing channel just resynchronises it (no new allocation).
            uint32_t ncid;
            if (cid == FIDO_HID_BROADCAST) {
                ncid = c->next_cid++;
                if (c->next_cid == 0 || c->next_cid == FIDO_HID_BROADCAST) c->next_cid = 1;
            } else {
                ncid = cid;
            }
            uint8_t resp[17];
            memcpy(resp, c->buf, 8);             // echo the 8-byte nonce
            wr32(resp + 8, ncid);
            resp[12] = 2;                         // CTAPHID protocol version
            resp[13] = 0; resp[14] = 6; resp[15] = 3;   // device version major.minor.build
            resp[16] = 0x05;                     // capabilities: CBOR | WINK
            fido_ctaphid_send(cid, W_INIT, resp, sizeof resp, c->sink, c->sink_ctx);
            break;
        }
        case W_PING:
            fido_ctaphid_send(cid, W_PING, c->buf, c->bcnt, c->sink, c->sink_ctx);
            break;
        case W_MSG: {
            if (!c->on_msg) { send_err(c, cid, ERR_INVALID_CMD); break; }
            uint16_t n = c->on_msg(c->buf, c->bcnt, c->rbuf, c->rcap, c->msg_ctx);
            fido_ctaphid_send(cid, W_MSG, c->rbuf, n, c->sink, c->sink_ctx);
            break;
        }
        case W_CBOR: {
            if (!c->on_cbor) { send_err(c, cid, ERR_INVALID_CMD); break; }
            uint16_t n = c->on_cbor(c->buf, c->bcnt, c->rbuf, c->rcap, c->cbor_ctx);
            fido_ctaphid_send(cid, W_CBOR, c->rbuf, n, c->sink, c->sink_ctx);
            break;
        }
        case W_WINK:
            fido_ctaphid_send(cid, W_WINK, NULL, 0, c->sink, c->sink_ctx);
            break;
        case W_CANCEL:
            break;                               // synchronous dispatch: nothing pending
        default:
            send_err(c, cid, ERR_INVALID_CMD);
            break;
    }
    c->active = 0;
    c->cur_cid = FIDO_HID_BROADCAST;
    return cid;
}
