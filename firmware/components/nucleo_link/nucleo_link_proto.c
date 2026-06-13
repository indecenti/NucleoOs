// nucleo_link_proto.c — the evolved Nucleo<->Nucleo link state machine. See nucleo_link.h.
// Pure C, no esp_now / no SD: everything goes through nlink_io. Host-tested in link-ctest.c.
#include "nucleo_link.h"
#include <string.h>

// Virtual-millisecond timing (real on device, simulated in the host gate).
#define NL_RTO_MS    120     // retransmit a frame unacked this long
#define NL_MAX_RETRY 40      // PER-FRAME retransmits before we give up
#define NL_IDLE_MS   8000    // no progress for this long -> TIMEOUT
#define NL_ACK_MS    40      // min spacing between ACKs (throttle reverse traffic)
#define NL_PROG_MS   100     // progress-event spacing
#define NL_DONE_MS   120     // DONE/FIN nudge spacing while draining

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ---- little-endian wire helpers -------------------------------------------
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

uint32_t nlink_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// ---- frame emit ------------------------------------------------------------
static int hdr(uint8_t *b, uint8_t type, uint32_t session) {
    b[0] = 'N'; b[1] = 'L'; b[2] = NLINK_VER; b[3] = type;
    put32(b + 4, session);
    return NLINK_HDR;
}
static void emit(nlink_ctx_t *c, const uint8_t *peer, const uint8_t *b, int n) {
    if (c->io && c->io->send_frame) c->io->send_frame(c->io->user, peer, b, n);
}
static void event(nlink_ctx_t *c, int ev, const nlink_evt_t *e) {
    if (c->io && c->io->on_event) c->io->on_event(c->io->user, ev, e);
}
static void fail(nlink_ctx_t *c, int reason) {
    if (c->state == NL_ST_DONE || c->state == NL_ST_FAIL) return;
    c->state = NL_ST_FAIL;
    nlink_evt_t e = {0}; e.reason = reason;
    event(c, NL_EV_FAILED, &e);
}
static void complete(nlink_ctx_t *c) {
    if (c->state == NL_ST_DONE) return;
    c->state = NL_ST_DONE;
    nlink_evt_t e = {0}; e.done_bytes = c->total_size; e.total_bytes = c->total_size;
    event(c, NL_EV_COMPLETE, &e);
}

static void send_ping(nlink_ctx_t *c) {
    uint8_t b[NLINK_HDR + 2 + NLINK_NAME_MAX]; int n = hdr(b, NL_PING, 0);
    int nl = (int)strlen(c->local_name); if (nl > NLINK_NAME_MAX) nl = NLINK_NAME_MAX;
    b[n++] = (uint8_t)nl; memcpy(b + n, c->local_name, nl); n += nl;
    emit(c, BCAST, b, n);
}
static void send_pong(nlink_ctx_t *c, const uint8_t *to) {
    uint8_t b[NLINK_HDR + 2 + NLINK_NAME_MAX]; int n = hdr(b, NL_PONG, 0);
    b[n++] = c->caps;
    int nl = (int)strlen(c->local_name); if (nl > NLINK_NAME_MAX) nl = NLINK_NAME_MAX;
    b[n++] = (uint8_t)nl; memcpy(b + n, c->local_name, nl); n += nl;
    emit(c, to, b, n);
}
static void send_offer(nlink_ctx_t *c) {
    uint8_t b[NLINK_HDR + 16 + NLINK_NAME_MAX + 1]; int n = hdr(b, NL_OFFER, c->session);
    put32(b + n, c->total_size);            n += 4;
    put16(b + n, c->chunk_size);            n += 2;
    put32(b + n, c->n_chunks);              n += 4;
    put32(b + n, c->crc32);                 n += 4;
    b[n++] = c->mode;
    int nl = (int)strlen(c->name); if (nl > NLINK_NAME_MAX) nl = NLINK_NAME_MAX;
    b[n++] = (uint8_t)nl; memcpy(b + n, c->name, nl); n += nl;
    emit(c, c->peer, b, n);
}
static void send_offer_ack(nlink_ctx_t *c, bool accept, uint32_t have_prefix) {
    uint8_t b[NLINK_HDR + 6]; int n = hdr(b, NL_OFFER_ACK, c->session);
    b[n++] = accept ? 1 : 0; b[n++] = 0; put32(b + n, have_prefix); n += 4;
    emit(c, c->peer, b, n);
}
static void send_data(nlink_ctx_t *c, uint32_t seq) {
    uint8_t b[NLINK_MTU]; int n = hdr(b, NL_DATA, c->session);
    put32(b + n, seq); n += 4;
    uint8_t payload[NLINK_CHUNK];
    int len = c->io->read_chunk ? c->io->read_chunk(c->io->user, seq, payload) : 0;
    if (len < 0) { fail(c, NL_R_IO); return; }
    if (len > NLINK_CHUNK) len = NLINK_CHUNK;
    put16(b + n, (uint16_t)len); n += 2;
    memcpy(b + n, payload, len); n += len;
    emit(c, c->peer, b, n);
}
static void send_ack(nlink_ctx_t *c) {
    uint8_t b[NLINK_HDR + 8]; int n = hdr(b, NL_ACK, c->session);
    put32(b + n, c->recv_base); n += 4;
    put32(b + n, c->recv_mask); n += 4;
    emit(c, c->peer, b, n);
    c->last_ack_ms = c->now_ms;
}
static void send_done(nlink_ctx_t *c) {
    uint8_t b[NLINK_HDR]; hdr(b, NL_DONE, c->session); emit(c, c->peer, b, NLINK_HDR);
    c->last_done_ms = c->now_ms;
}
static void send_fin(nlink_ctx_t *c, uint8_t ok) {
    uint8_t b[NLINK_HDR + 1]; int n = hdr(b, NL_FIN, c->session); b[n++] = ok; emit(c, c->peer, b, n);
}

// ---- sender helpers --------------------------------------------------------
static void pump_window(nlink_ctx_t *c) {
    while (c->next < c->n_chunks && (c->next - c->base) < NLINK_WINDOW) {
        uint32_t i = c->next % NLINK_WINDOW;
        c->win[i].seq = c->next; c->win[i].sent_ms = c->now_ms;
        c->win[i].acked = 0; c->win[i].used = 1; c->win[i].tries = 0;
        send_data(c, c->next);
        c->next++;
    }
    if (c->base == c->n_chunks && c->state == NL_ST_RUN) { c->state = NL_ST_DRAIN; send_done(c); }
}

static void progress(nlink_ctx_t *c, uint32_t done_chunks) {
    if (c->now_ms - c->last_prog_ms < NL_PROG_MS) return;
    c->last_prog_ms = c->now_ms;
    nlink_evt_t e = {0};
    uint64_t db = (uint64_t)done_chunks * c->chunk_size;
    e.done_bytes = db > c->total_size ? c->total_size : (uint32_t)db;
    e.total_bytes = c->total_size;
    event(c, NL_EV_PROGRESS, &e);
}

// ---- public API ------------------------------------------------------------
void nlink_init(nlink_ctx_t *c, const nlink_io_t *io, const char *local_name) {
    memset(c, 0, sizeof(*c));
    c->io = io;
    c->caps = NLINK_VER;
    if (local_name) { strncpy(c->local_name, local_name, NLINK_NAME_MAX); c->local_name[NLINK_NAME_MAX] = 0; }
}

void nlink_discover(nlink_ctx_t *c) { send_ping(c); }

void nlink_send_begin(nlink_ctx_t *c, const uint8_t *peer6, uint32_t session,
                      const char *name, uint32_t total_size, uint8_t mode, uint32_t crc32) {
    c->role = NL_ROLE_SEND; c->state = NL_ST_OFFER;
    memcpy(c->peer, peer6, 6);
    c->session = session; c->total_size = total_size; c->mode = mode; c->crc32 = crc32;
    c->chunk_size = NLINK_CHUNK;
    c->n_chunks = (total_size + NLINK_CHUNK - 1) / NLINK_CHUNK;   // 0 bytes -> 0 chunks
    strncpy(c->name, name ? name : "", NLINK_NAME_MAX); c->name[NLINK_NAME_MAX] = 0;
    c->base = c->next = 0;
    memset(c->win, 0, sizeof(c->win));
    c->last_rx_ms = c->last_prog_ms = c->last_done_ms = c->now_ms;
    send_offer(c);
}

void nlink_recv_listen(nlink_ctx_t *c) {
    c->role = NL_ROLE_RECV; c->state = NL_ST_IDLE; c->offer_pending = false;
}

void nlink_recv_accept(nlink_ctx_t *c, bool accept, uint32_t have_prefix) {
    if (!c->offer_pending) return;
    c->offer_pending = false;
    if (!accept) { send_offer_ack(c, false, 0); fail(c, NL_R_DECLINED); return; }
    if (have_prefix > c->n_chunks) have_prefix = c->n_chunks;
    c->recv_base = have_prefix; c->recv_mask = 0;
    c->state = NL_ST_RUN;
    c->last_rx_ms = c->last_ack_ms = c->last_prog_ms = c->now_ms;
    send_offer_ack(c, true, have_prefix);
    if (c->recv_base == c->n_chunks) {                  // resume that was already complete / 0-byte
        uint32_t got = c->io->verify ? c->io->verify(c->io->user) : c->crc32;
        send_fin(c, got == c->crc32); (got == c->crc32) ? complete(c) : fail(c, NL_R_CRC);
    }
}

void nlink_abort(nlink_ctx_t *c) {
    uint8_t b[NLINK_HDR]; hdr(b, NL_ABORT, c->session); emit(c, c->peer, b, NLINK_HDR);
    fail(c, NL_R_ABORT);
}

// ---- inbound frames --------------------------------------------------------
static void on_data(nlink_ctx_t *c, const uint8_t *p, int len) {
    if (len < NLINK_HDR + 6) return;
    uint32_t seq = get32(p + NLINK_HDR);
    uint16_t dl  = get16(p + NLINK_HDR + 4);
    const uint8_t *payload = p + NLINK_HDR + 6;
    if ((int)dl > len - (NLINK_HDR + 6)) return;
    if (c->state != NL_ST_RUN && c->state != NL_ST_DONE) return;

    // recv_mask bit i <-> seq (recv_base + i); bit0 is recv_base itself (always 0 after the absorb
    // loop below, since recv_base is by definition the first chunk NOT yet received).
    uint32_t before = c->recv_base;
    if (seq < c->recv_base) {
        /* duplicate of an already-stored chunk — re-ACK below */
    } else if (seq < c->recv_base + 32) {
        uint32_t i = seq - c->recv_base;
        if (i == 0 || !(c->recv_mask & (1u << i))) {
            if (c->io->write_chunk && c->io->write_chunk(c->io->user, seq, payload, dl) != 0) { fail(c, NL_R_IO); return; }
            c->recv_mask |= (1u << i);
        }
        while (c->recv_mask & 1u) { c->recv_mask >>= 1; c->recv_base++; }   // absorb the contiguous prefix
    } else {
        return;  // beyond the 32-chunk window (sender is bounded by NLINK_WINDOW) — ignore
    }
    c->last_rx_ms = c->now_ms;
    progress(c, c->recv_base);

    if (c->recv_base == c->n_chunks) {
        send_ack(c);
        uint32_t got = c->io->verify ? c->io->verify(c->io->user) : c->crc32;
        send_fin(c, got == c->crc32);
        (got == c->crc32) ? complete(c) : fail(c, NL_R_CRC);
        return;
    }
    // ACK at once when the contiguous window advanced; otherwise throttle the reverse traffic.
    if (c->recv_base != before || c->now_ms - c->last_ack_ms >= NL_ACK_MS) send_ack(c);
}

static void on_ack(nlink_ctx_t *c, const uint8_t *p, int len) {
    if (len < NLINK_HDR + 8 || c->role != NL_ROLE_SEND) return;
    uint32_t ack_base = get32(p + NLINK_HDR);
    uint32_t mask     = get32(p + NLINK_HDR + 4);
    c->last_rx_ms = c->now_ms;
    if (ack_base > c->base) {
        for (uint32_t s = c->base; s < ack_base && s < c->next; s++) c->win[s % NLINK_WINDOW].used = 0;
        c->base = ack_base;
    }
    for (int i = 1; i < 32; i++) {                       // mask bit i <-> seq (ack_base + i); suppress
        if (mask & (1u << i)) {                           // retransmit of those already-received ahead chunks
            uint32_t s = ack_base + (uint32_t)i;
            if (s >= c->base && s < c->next) c->win[s % NLINK_WINDOW].acked = 1;
        }
    }
    progress(c, c->base);
    pump_window(c);
}

static void on_fin(nlink_ctx_t *c, const uint8_t *p, int len) {
    if (len < NLINK_HDR + 1 || c->role != NL_ROLE_SEND) return;
    p[NLINK_HDR] ? complete(c) : fail(c, NL_R_CRC);
}

void nlink_on_frame(nlink_ctx_t *c, const uint8_t *peer6, const uint8_t *buf, int len, uint32_t now_ms) {
    c->now_ms = now_ms;
    if (len < NLINK_HDR || buf[0] != 'N' || buf[1] != 'L' || buf[2] != NLINK_VER) return;
    uint8_t type = buf[3];
    uint32_t session = get32(buf + 4);

    // Discovery is session-agnostic.
    if (type == NL_PING) {
        int nl = buf[NLINK_HDR]; if (nl > NLINK_NAME_MAX) nl = NLINK_NAME_MAX;
        char nm[NLINK_NAME_MAX + 1]; memcpy(nm, buf + NLINK_HDR + 1, nl); nm[nl] = 0;
        nlink_evt_t e = {0}; e.peer = peer6; e.name = nm; event(c, NL_EV_PING, &e);
        send_pong(c, peer6);
        return;
    }
    if (type == NL_PONG) {
        int nl = buf[NLINK_HDR + 1]; if (nl > NLINK_NAME_MAX) nl = NLINK_NAME_MAX;
        char nm[NLINK_NAME_MAX + 1]; memcpy(nm, buf + NLINK_HDR + 2, nl); nm[nl] = 0;
        nlink_evt_t e = {0}; e.peer = peer6; e.name = nm; event(c, NL_EV_PEER, &e);
        return;
    }

    if (type == NL_OFFER) {
        if (c->role != NL_ROLE_RECV) return;
        if (len < NLINK_HDR + 15) return;
        // Retransmitted OFFER for the transfer we ALREADY accepted (our OFFER_ACK was lost): re-ACK with
        // our current progress and do NOT re-prompt the user / reset state.
        if (session == c->session && (c->state == NL_ST_RUN || c->state == NL_ST_DONE)) {
            c->last_rx_ms = now_ms; send_offer_ack(c, true, c->recv_base); return;
        }
        if (c->state == NL_ST_RUN) return;   // busy with a DIFFERENT transfer — ignore the newcomer (no hijack)
        const uint8_t *q = buf + NLINK_HDR;
        c->session    = session;
        c->total_size = get32(q);      q += 4;
        c->chunk_size = get16(q);      q += 2;
        c->n_chunks   = get32(q);      q += 4;
        c->crc32      = get32(q);      q += 4;
        c->mode       = *q++;
        int nl = *q++; if (nl > NLINK_NAME_MAX) nl = NLINK_NAME_MAX;
        memcpy(c->name, q, nl); c->name[nl] = 0;
        memcpy(c->peer, peer6, 6);
        c->offer_pending = true; c->last_rx_ms = now_ms;
        nlink_evt_t e = {0};
        e.peer = peer6; e.name = c->name;
        e.offer.session = c->session; e.offer.total_size = c->total_size; e.offer.n_chunks = c->n_chunks;
        e.offer.crc32 = c->crc32; e.offer.chunk_size = c->chunk_size; e.offer.mode = c->mode;
        strncpy(e.offer.name, c->name, NLINK_NAME_MAX);
        event(c, NL_EV_OFFER, &e);
        return;
    }
    if (session != c->session) return;                  // stray frame from another transfer

    switch (type) {
        case NL_OFFER_ACK:
            if (c->role != NL_ROLE_SEND || len < NLINK_HDR + 6) return;
            c->last_rx_ms = now_ms;
            if (!buf[NLINK_HDR]) { fail(c, NL_R_DECLINED); return; }
            { uint32_t hp = get32(buf + NLINK_HDR + 2); if (hp > c->n_chunks) hp = c->n_chunks;
              c->base = c->next = hp; }
            c->state = NL_ST_RUN;
            if (c->base == c->n_chunks) { c->state = NL_ST_DRAIN; send_done(c); }
            else pump_window(c);
            break;
        case NL_DATA:  on_data(c, buf, len); break;
        case NL_ACK:   on_ack(c, buf, len);  break;
        case NL_DONE:                                   // receiver nudge: re-ACK or re-FIN
            if (c->role == NL_ROLE_RECV) {
                if (c->recv_base == c->n_chunks) { send_ack(c); send_fin(c, 1); }
                else send_ack(c);
            }
            break;
        case NL_FIN:   on_fin(c, buf, len);  break;
        case NL_ABORT: fail(c, NL_R_ABORT);  break;
        default: break;
    }
}

void nlink_tick(nlink_ctx_t *c, uint32_t now_ms) {
    c->now_ms = now_ms;
    if (c->state != NL_ST_RUN && c->state != NL_ST_DRAIN && c->state != NL_ST_OFFER) return;

    if (now_ms - c->last_rx_ms > NL_IDLE_MS) { fail(c, NL_R_TIMEOUT); return; }

    if (c->role == NL_ROLE_SEND) {
        if (c->state == NL_ST_OFFER) {                  // OFFER lost -> re-offer
            if (now_ms - c->last_done_ms >= NL_RTO_MS) { send_offer(c); c->last_done_ms = now_ms; }
            return;
        }
        for (uint32_t s = c->base; s < c->next; s++) {
            uint32_t i = s % NLINK_WINDOW;
            if (c->win[i].used && !c->win[i].acked && now_ms - c->win[i].sent_ms >= NL_RTO_MS) {
                if (++c->win[i].tries > NL_MAX_RETRY) { fail(c, NL_R_TIMEOUT); return; }
                c->win[i].sent_ms = now_ms;
                send_data(c, s);
            }
        }
        if (c->state == NL_ST_DRAIN && now_ms - c->last_done_ms >= NL_DONE_MS) send_done(c);
    } else if (c->role == NL_ROLE_RECV && c->state == NL_ST_RUN) {
        if (now_ms - c->last_ack_ms >= NL_RTO_MS) send_ack(c);  // re-advertise gaps if ACKs were lost
    }
}
