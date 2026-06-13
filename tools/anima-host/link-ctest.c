// link-ctest.c — host gate for nucleo_link. Compiles the firmware's pure protocol core with
// nucleo_link_proto.c + nucleo_link_bruce.c (npm run link:test) and proves, on the PC, the two
// things Bruce's naive share cannot do:
//   1. EVOLVED Nucleo<->Nucleo transfer is LOSSLESS through a packet-dropping/reordering/duplicating
//      channel (sliding window + selective ACK + retransmit + whole-file CRC32).
//   2. An interrupted transfer RESUMES from the contiguous prefix already on disk (no re-send of
//      chunks the receiver already has).
// Plus a Bruce-codec round-trip so "Bruce mode" stays wire-compatible with real Bruce devices.
#include "nucleo_link.h"
#include "nucleo_link_bruce.h"
#include <stdio.h>
#include <string.h>

// ---- deterministic PRNG (no Math.random equivalent; reproducible runs) ------
static uint32_t RNG = 0x12345678u;
static void     seed(uint32_t s) { RNG = s ? s : 1; }
static uint32_t rnd(void) { RNG ^= RNG << 13; RNG ^= RNG >> 17; RNG ^= RNG << 5; return RNG; }
static int      chance(int pct) { return pct > 0 && (int)(rnd() % 100) < pct; }

// ---- simulated radio + storage world ---------------------------------------
#define QMAX  40000
#define FBUF  (256 * 1024)
typedef struct { int to; int len; uint8_t buf[256]; } pkt_t;

static struct {
    uint8_t  src[FBUF], dst[FBUF];
    uint32_t src_len, dst_cap, recv_total;
    pkt_t    q[QMAX]; int qn;
    int      loss_pct, dup_pct, reorder;
    long     sent, dropped, duped, data_delivered;
    uint32_t min_data_seq;
    int      auto_accept; uint32_t have_prefix;
    int      a_complete, a_failed, b_complete, b_failed, b_reason;
    int      peer_found; char peer_name[64];
    uint8_t  last_offer[256]; int last_offer_len; int offer_events;
    nlink_ctx_t *A, *B;
} W;

typedef struct { int id; } node_t;              // 0 = A (sender), 1 = B (receiver)
static node_t NA = {0}, NB = {1};
static const uint8_t MAC_A[6] = {0xA, 0xA, 0xA, 0xA, 0xA, 0xA};
static const uint8_t MAC_B[6] = {0xB, 0xB, 0xB, 0xB, 0xB, 0xB};

static int io_send(void *user, const uint8_t *peer, const uint8_t *buf, int len) {
    (void)peer; node_t *n = user; int to = n->id ^ 1;
    if (len > 256) len = 256;
    if (to == 1 && len >= 4 && buf[0] == 'N' && buf[3] == NL_OFFER) { memcpy(W.last_offer, buf, len); W.last_offer_len = len; }
    W.sent++;
    if (chance(W.loss_pct)) { W.dropped++; return 0; }
    if (W.qn < QMAX) { W.q[W.qn].to = to; W.q[W.qn].len = len; memcpy(W.q[W.qn].buf, buf, len); W.qn++; }
    if (chance(W.dup_pct) && W.qn < QMAX) { W.duped++; W.q[W.qn] = W.q[W.qn - 1]; W.qn++; }
    return 0;
}
static int io_read(void *user, uint32_t seq, uint8_t *buf) {
    (void)user; uint32_t off = seq * NLINK_CHUNK;
    if (off >= W.src_len) return 0;
    uint32_t len = W.src_len - off; if (len > NLINK_CHUNK) len = NLINK_CHUNK;
    memcpy(buf, W.src + off, len); return (int)len;
}
static int io_write(void *user, uint32_t seq, const uint8_t *buf, int len) {
    (void)user; uint32_t off = seq * NLINK_CHUNK;
    if (off + (uint32_t)len > W.dst_cap) return -1;
    memcpy(W.dst + off, buf, len); return 0;
}
static uint32_t io_verify(void *user) { (void)user; return nlink_crc32(0, W.dst, W.recv_total); }
static void io_event(void *user, int ev, const nlink_evt_t *e) {
    node_t *n = user;
    if (n->id == 1) {                                   // receiver
        if (ev == NL_EV_OFFER) {
            W.offer_events++;
            W.recv_total = e->offer.total_size;
            nlink_recv_accept(W.B, W.auto_accept ? true : false, W.have_prefix);
        } else if (ev == NL_EV_COMPLETE) W.b_complete = 1;
        else if (ev == NL_EV_FAILED) { W.b_failed = 1; W.b_reason = e->reason; }
    } else {                                            // sender
        if (ev == NL_EV_COMPLETE) W.a_complete = 1;
        else if (ev == NL_EV_FAILED) W.a_failed = 1;
        else if (ev == NL_EV_PEER) { W.peer_found = 1; strncpy(W.peer_name, e->name ? e->name : "", 63); }
    }
}
static const nlink_io_t IOA = { io_send, io_read, io_write, io_verify, io_event, &NA };
static const nlink_io_t IOB = { io_send, io_read, io_write, io_verify, io_event, &NB };

static void deliver_one(uint32_t now) {
    if (W.qn == 0) return;
    int idx = W.reorder ? (int)(rnd() % (uint32_t)W.qn) : 0;     // random pick == reorder
    pkt_t p = W.q[idx];
    W.q[idx] = W.q[--W.qn];
    if (p.to == 1) {
        if (p.len >= 12 && p.buf[0] == 'N' && p.buf[1] == 'L' && p.buf[3] == NL_DATA) {
            uint32_t seq = (uint32_t)p.buf[8] | ((uint32_t)p.buf[9] << 8) |
                           ((uint32_t)p.buf[10] << 16) | ((uint32_t)p.buf[11] << 24);
            if (seq < W.min_data_seq) W.min_data_seq = seq;
            W.data_delivered++;
        }
        nlink_on_frame(W.B, MAC_A, p.buf, p.len, now);
    } else {
        nlink_on_frame(W.A, MAC_B, p.buf, p.len, now);
    }
}

static int run(uint32_t max_steps) {
    uint32_t now = 0;
    for (uint32_t s = 0; s < max_steps; s++) {
        now += 5;
        for (int k = 0; k < 4; k++) deliver_one(now);
        nlink_tick(W.A, now); nlink_tick(W.B, now);
        if (W.a_complete && W.b_complete) return (int)s;
        if (W.a_failed || W.b_failed) { for (int k = 0; k < 8; k++) deliver_one(now); return (int)s; }
    }
    return -1;
}

static void wsetup(uint32_t flen, int loss, int dup, int reorder, int accept, uint32_t have_prefix) {
    nlink_ctx_t *A = W.A, *B = W.B;
    memset(&W, 0, sizeof(W)); W.A = A; W.B = B;
    W.src_len = flen; W.dst_cap = FBUF;
    W.loss_pct = loss; W.dup_pct = dup; W.reorder = reorder;
    W.auto_accept = accept; W.have_prefix = have_prefix; W.min_data_seq = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < flen; i++) W.src[i] = (uint8_t)rnd();
    if (have_prefix) {
        uint32_t pb = have_prefix * NLINK_CHUNK; if (pb > flen) pb = flen;
        memcpy(W.dst, W.src, pb);
    }
}

static int FAILS = 0;
static void check(const char *name, int cond, const char *detail) {
    printf("  [%s] %s%s%s\n", cond ? "ok" : "FAIL", name, detail ? " — " : "", detail ? detail : "");
    if (!cond) FAILS++;
}

// ---- transfer scenario -----------------------------------------------------
static void scenario(const char *name, uint32_t flen, int loss, int dup, int reorder, uint32_t prefix) {
    nlink_ctx_t A, B; W.A = &A; W.B = &B;
    wsetup(flen, loss, dup, reorder, 1, prefix);
    nlink_init(&A, &IOA, "Nucleo-A");
    nlink_init(&B, &IOB, "Nucleo-B");
    nlink_recv_listen(&B);
    uint32_t crc = nlink_crc32(0, W.src, W.src_len);
    nlink_send_begin(&A, MAC_B, 0xC0FFEEu, name, W.src_len, NL_MODE_FILE, crc);

    int steps = run(2000000);
    uint32_t n_chunks = (flen + NLINK_CHUNK - 1) / NLINK_CHUNK;
    int ok_done   = steps >= 0 && W.a_complete && W.b_complete;
    int ok_bytes  = memcmp(W.src, W.dst, flen) == 0;
    int ok_crc    = nlink_crc32(0, W.dst, flen) == crc;

    char d[160];
    snprintf(d, sizeof d, "%uB/%u chunks  steps=%d sent=%ld dropped=%ld dup=%ld data_dlv=%ld",
             flen, n_chunks, steps, W.sent, W.dropped, W.duped, W.data_delivered);
    check(name, ok_done && ok_bytes && ok_crc, d);
    if (!(ok_done && ok_bytes && ok_crc))
        printf("    DBG A.state=%d base=%u next=%u aC=%d aF=%d | B.state=%d rbase=%u rmask=%08x bC=%d bF=%d r=%d\n",
               A.state, A.base, A.next, W.a_complete, W.a_failed,
               B.state, B.recv_base, B.recv_mask, W.b_complete, W.b_failed, W.b_reason);
    if (prefix) {
        char r[96]; snprintf(r, sizeof r, "no DATA seq < %u re-sent (min_seq=%u)", prefix,
                             W.min_data_seq == 0xFFFFFFFFu ? prefix : W.min_data_seq);
        check("  resume-skips-prefix", W.min_data_seq >= prefix, r);
    }
}

static void test_decline(void) {
    nlink_ctx_t A, B; W.A = &A; W.B = &B;
    wsetup(4000, 0, 0, 0, /*accept*/0, 0);
    nlink_init(&A, &IOA, "Nucleo-A"); nlink_init(&B, &IOB, "Nucleo-B");
    nlink_recv_listen(&B);
    uint32_t crc = nlink_crc32(0, W.src, W.src_len);
    nlink_send_begin(&A, MAC_B, 0xBEEFu, "nope.bin", W.src_len, NL_MODE_FILE, crc);
    run(4000);
    check("decline", W.a_failed && W.b_failed && W.b_reason == NL_R_DECLINED, "sender sees DECLINED");
}

// A retransmitted OFFER (our OFFER_ACK was lost) must NOT re-prompt a receiver that already accepted —
// it must be re-ACKed silently. Guards against the duplicate-offer re-trigger / recv_base reset.
static void test_dup_offer(void) {
    nlink_ctx_t A, B; W.A = &A; W.B = &B;
    wsetup(5000, 0, 0, 0, /*accept*/1, 0);
    nlink_init(&A, &IOA, "Nucleo-A"); nlink_init(&B, &IOB, "Nucleo-B");
    nlink_recv_listen(&B);
    uint32_t crc = nlink_crc32(0, W.src, W.src_len);
    nlink_send_begin(&A, MAC_B, 0xD00Du, "dup.bin", W.src_len, NL_MODE_FILE, crc);  // emits OFFER (captured)
    nlink_on_frame(&B, MAC_A, W.last_offer, W.last_offer_len, 10);                  // 1st: prompt + auto-accept -> RUN
    int after_first = W.offer_events, st_first = B.state;
    nlink_on_frame(&B, MAC_A, W.last_offer, W.last_offer_len, 20);                  // duplicate OFFER (ack lost)
    char d[96]; snprintf(d, sizeof d, "events %d (not 2), state RUN both (%d,%d)", W.offer_events, st_first, B.state);
    check("dup-offer no re-prompt", after_first == 1 && W.offer_events == 1 && st_first == NL_ST_RUN && B.state == NL_ST_RUN, d);
}

static void test_discovery(void) {
    nlink_ctx_t A, B; W.A = &A; W.B = &B;
    wsetup(0, 0, 0, 1, 1, 0);
    nlink_init(&A, &IOA, "Nucleo-A"); nlink_init(&B, &IOB, "Nucleo-B");
    nlink_recv_listen(&B);
    nlink_discover(&A);
    run(400);
    char d[80]; snprintf(d, sizeof d, "peer='%s'", W.peer_name);
    check("discovery ping/pong", W.peer_found && strcmp(W.peer_name, "Nucleo-B") == 0, d);
}

// ---- Bruce-mode codec round-trip (wire-compat with real Bruce) --------------
static void test_bruce(void) {
    bruce_msg_t m;
    bruce_build_ping(&m); check("bruce ping", m.ping == 1 && m.pong == 0 && m.isFile == 0, NULL);
    bruce_build_pong(&m); check("bruce pong", m.pong == 1 && m.ping == 0, NULL);

    uint8_t file[320]; for (int i = 0; i < 320; i++) file[i] = (uint8_t)(i * 7 + 3);
    uint8_t recv[320]; uint32_t rlen = 0;
    bruce_file_init(&m, "photo.jpg", "/sd/img", sizeof file);
    int meta_ok = m.isFile == 1 && m.totalBytes == 320 &&
                  strcmp(m.filename, "photo.jpg") == 0 && strcmp(m.filepath, "/sd/img") == 0;
    check("bruce file meta", meta_ok, NULL);

    uint32_t off = 0; int done_ok = 1;
    while (off < sizeof file) {
        uint32_t n = (uint32_t)sizeof file - off; if (n > BRUCE_DATA_SIZE) n = BRUCE_DATA_SIZE;
        bruce_file_chunk(&m, file + off, n);
        uint8_t wire[BRUCE_MSG_SIZE]; memcpy(wire, &m, BRUCE_MSG_SIZE);    // "transmit" 248 bytes
        if (!bruce_is_msg(wire, sizeof wire)) done_ok = 0;
        const bruce_msg_t *r = (const bruce_msg_t *)wire;                  // receiver casts back
        memcpy(recv + rlen, r->data, r->dataSize); rlen += r->dataSize;
        off += n;
        if (r->done && r->bytesSent != r->totalBytes) done_ok = 0;
    }
    check("bruce file round-trip", done_ok && rlen == 320 && memcmp(recv, file, 320) == 0,
          "320B in 150B chunks reconstructs identically");
}

int main(void) {
    seed(0xA11CE);
    printf("nucleo_link host gate\n");
    printf("Bruce wire-compat (compile-time layout asserts passed: sizeof Message = %d)\n", BRUCE_MSG_SIZE);
    test_bruce();
    printf("Evolved Nucleo<->Nucleo transfer:\n");
    scenario("clean-1k",        1000,   0,  0, 0, 0);
    scenario("clean-20k",      20000,   0,  0, 0, 0);
    scenario("loss20-reorder", 20000,  20,  0, 1, 0);
    scenario("loss40-dup-reord",64000, 40, 10, 1, 0);
    scenario("zero-byte",          0,   0,  0, 0, 0);
    scenario("resume@40",      20000,  15,  0, 1, 40);
    printf("Control:\n");
    test_decline();
    test_dup_offer();
    test_discovery();
    printf(FAILS ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", FAILS);
    return FAILS ? 1 : 0;
}
