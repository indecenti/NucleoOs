// brawler_net.cpp — SCORRIBANDA co-op 2 giocatori su ESP-NOW (host-authoritative).
//
// Stesso schema di app_pong.cpp / app_tanks.cpp: nucleo_pnet best-effort (niente ACK, l'ultimo
// snapshot supera quello perso). Il CREATORE della stanza e' l'HOST (giocatore 0): possiede TUTTA
// la simulazione (posizioni, hp, nemici, ondate, livello, punteggio) e trasmette uno snapshot
// compatto ~30 Hz. L'OSPITE (giocatore 1) invia solo il proprio input ~30 Hz e applica lo snapshot
// ricevuto a g.f[] cosi' i due Cardputer vedono la stessa scena per pochissima banda.
//
// Pairing/canale: ESP-NOW segue il canale Wi-Fi corrente (STA o SoftAP) — due Cardputer sulla
// stessa rete si trovano da soli; altrimenti canale 1. La lobby (HELLO/JOIN/ACCEPT) e' qui dentro
// cosi' il menu puo' restare semplice: bnet_start avvia ESP-NOW, bnet_available dice se c'e' un peer.
//
// Solo-play NON chiama mai bnet_poll(). Sotto NX_NET_APP il Wi-Fi STA (che ESP-NOW cavalca) resta su.

#include "brawler.h"
#include "nucleo_kbd.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

extern "C" {
#include "nucleo_pnet.h"
#include "esp_timer.h"
}

// ============================ protocollo ====================================
#define SB_M0  'S'
#define SB_M1  'B'
#define SB_VER 1
enum { SB_HELLO = 1, SB_JOIN, SB_ACCEPT, SB_STATE, SB_INPUT, SB_BYE };
enum { SB_HS_IDLE = 0, SB_HS_HOSTING };

// bottoni input (bitmask): rising-edge → combat_begin_attack
#define SB_BTN_J 0x01   // punch
#define SB_BTN_K 0x02   // kick
#define SB_BTN_L 0x04   // jump

// Eroe nello snapshot: x,z float, hp int16, stato/dir/anim/player/kind in byte. 15 byte.
typedef struct __attribute__((packed)) {
    float   x, z;
    int16_t hp;
    uint8_t st;       // BrState
    uint8_t dir;      // 0 = -1, 1 = +1
    uint8_t anim;     // anim * 255
    uint8_t player;
    uint8_t kind;     // indice personaggio (l'ospite ricostruisce la build/colore giusti)
} sb_hero_t;

// Nemico nello snapshot: kind, x(int16), z(uint8*255), hp(int16), st. 7 byte.
typedef struct __attribute__((packed)) {
    uint8_t kind;
    int16_t x;
    uint8_t z;
    int16_t hp;
    uint8_t st;
} sb_enemy_t;

typedef struct __attribute__((packed)) {
    uint8_t  m0, m1, ver, type;
    uint8_t  status;          // SB_HS_*
    char     name[22];
} sb_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t  m0, m1, ver, type;
    char     name[22];
    uint8_t  kind;            // personaggio scelto dall'ospite (l'host configura lo slot 1)
} sb_join_t;

typedef struct __attribute__((packed)) {
    uint8_t  m0, m1, ver, type;
    uint32_t session;
} sb_accept_t;

// HOST → OSPITE ~30 Hz. ~ 4+4 + 2*14 + 1 + 6*7 + 4 + 4 + 1 + 1 + 4 = 95 byte (< 200).
typedef struct __attribute__((packed)) {
    uint8_t   m0, m1, ver, type;
    uint32_t  seq;
    sb_hero_t hero[2];
    uint8_t   nenemy;
    sb_enemy_t enemy[BR_MAXENEMY];
    float     camx;
    float     gatex;
    uint8_t   wave;
    uint8_t   level;
    int32_t   score;
    uint8_t   hostscreen;     // schermo dell'host (BrScreen): l'ospite segue play/pausa/over/clear
} sb_state_t;

// OSPITE → HOST ~30 Hz.
typedef struct __attribute__((packed)) {
    uint8_t  m0, m1, ver, type;
    int8_t   mvx;             // -1/0/+1 lungo la strada (A/D)
    int8_t   mvz;             // -1/0/+1 profondita' (W=lontano, S=vicino)
    uint8_t  buttons;         // SB_BTN_*
    uint32_t seq;
} sb_input_t;

// ============================ stato modulo ==================================
static bool     s_on;                 // ESP-NOW avviato
static uint8_t  s_peer[6];
static bool     s_haspeer;
static uint32_t s_session;
static uint32_t s_txseq, s_rxseq;
static uint32_t s_last_tx_ms, s_last_rx_ms, s_last_hello_ms;
static uint8_t  s_prev_btn;           // host: rilevamento rising-edge sull'input ospite
static int8_t   s_guest_mvx, s_guest_mvz;  // host: ultimo movimento richiesto dall'ospite

#define SB_TICK_MS   33               // ~30 Hz
#define SB_HELLO_MS  400
#define SB_LOST_MS   3500             // peer considerato perso

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ============================ send ==========================================
static void send_hello(void) {
    sb_hello_t h = { SB_M0, SB_M1, SB_VER, SB_HELLO, SB_HS_HOSTING, { 0 } };
    snprintf(h.name, sizeof h.name, "%s", pnet_name());
    pnet_send(NULL, &h, sizeof h);
}
static void send_join(void) {
    sb_join_t j = { SB_M0, SB_M1, SB_VER, SB_JOIN, { 0 }, 0 };
    snprintf(j.name, sizeof j.name, "%s", pnet_name());
    j.kind = (uint8_t)g.hero_pick[1];     // l'ospite e' il giocatore 1
    pnet_send(s_peer, &j, sizeof j);
}
static void send_accept(void) {
    sb_accept_t a = { SB_M0, SB_M1, SB_VER, SB_ACCEPT, s_session };
    pnet_send(s_peer, &a, sizeof a);
}
static void send_bye(void) {
    if (!s_haspeer) return;
    uint8_t b[4] = { SB_M0, SB_M1, SB_VER, SB_BYE };
    pnet_send(s_peer, b, 4);
}

// HOST: comprime g.f[] in uno snapshot.
static void send_state(void) {
    sb_state_t s;
    memset(&s, 0, sizeof s);
    s.m0 = SB_M0; s.m1 = SB_M1; s.ver = SB_VER; s.type = SB_STATE;
    s.seq = ++s_txseq;
    for (int p = 0; p < 2; p++) {
        sb_hero_t *h = &s.hero[p];
        const Fighter *fr = br_hero(p);
        h->player = (uint8_t)p;
        if (fr && fr->on) {
            h->x = fr->x; h->z = fr->z;
            h->hp = (int16_t)fr->hp;
            h->st = (uint8_t)fr->st;
            h->dir = (fr->dir >= 0) ? 1 : 0;
            float a = fr->anim; if (a < 0) a = 0; if (a > 1) a = 1;
            h->anim = (uint8_t)(a * 255.0f);
            h->kind = (uint8_t)fr->kind;
        } else {
            h->hp = 0; h->st = (uint8_t)BS_DOWN;   // assente/KO
        }
    }
    int n = 0;
    for (int i = 0; i < BR_MAXF && n < BR_MAXENEMY; i++) {
        const Fighter *fr = &g.f[i];
        if (!fr->on || fr->is_hero) continue;
        sb_enemy_t *e = &s.enemy[n++];
        e->kind = (uint8_t)fr->kind;
        e->x = (int16_t)fr->x;
        float z = fr->z; if (z < 0) z = 0; if (z > 1) z = 1;
        e->z = (uint8_t)(z * 255.0f);
        e->hp = (int16_t)fr->hp;
        e->st = (uint8_t)fr->st;
    }
    s.nenemy = (uint8_t)n;
    s.camx = g.camx; s.gatex = g.gatex;
    s.wave = (uint8_t)g.wave; s.level = (uint8_t)g.level;
    s.score = (int32_t)g.score;
    s.hostscreen = (uint8_t)g.screen;     // l'ospite specchia play/pausa/over/clear
    pnet_send(s_peer, &s, sizeof s);
    s_last_tx_ms = now_ms();
}

// OSPITE: legge i tasti locali e li manda all'host.
static void send_input(void) {
    sb_input_t in;
    memset(&in, 0, sizeof in);
    in.m0 = SB_M0; in.m1 = SB_M1; in.ver = SB_VER; in.type = SB_INPUT;
    int8_t mvx = 0, mvz = 0;
    uint8_t btn = 0;
    if (nucleo_kbd_char_down('a')) mvx -= 1;
    if (nucleo_kbd_char_down('d')) mvx += 1;
    if (nucleo_kbd_char_down('e')) mvz -= 1;   // E = su / piu' lontano (stesso schema del giocatore locale)
    if (nucleo_kbd_char_down('s')) mvz += 1;   // S = giu / piu' vicino
    if (nucleo_kbd_char_down('j')) btn |= SB_BTN_J;
    if (nucleo_kbd_char_down('k')) btn |= SB_BTN_K;
    if (nucleo_kbd_char_down('l') || nucleo_kbd_char_down(' ')) btn |= SB_BTN_L;
    in.mvx = mvx; in.mvz = mvz; in.buttons = btn;
    in.seq = ++s_txseq;
    pnet_send(s_peer, &in, sizeof in);
    s_last_tx_ms = now_ms();
}

// ============================ apply (ricezione) =============================
// HOST: applica l'input dell'ospite all'eroe giocatore 1 (movimento + attacchi su rising-edge).
static void host_apply_input(const sb_input_t *in) {
    if (in->seq && in->seq <= s_rxseq) return;   // scarta riordini/duplicati
    s_rxseq = in->seq;
    s_guest_mvx = in->mvx;
    s_guest_mvz = in->mvz;

    Fighter *fr = br_hero(1);
    if (fr && fr->on && !fighter_busy(fr)) {
        const HeroDef *def = brawler_hero(fr->kind);
        float spd = def ? def->speed : 60.0f;
        // muovi solo se non e' bloccato; la fisica vera resta a combat_step/host loop,
        // qui imponiamo l'intenzione di movimento dell'ospite.
        if (in->mvx) { fr->vx = (float)in->mvx * spd; fr->dir = (in->mvx > 0) ? 1 : -1; }
        else fr->vx = 0;
        if (in->mvz) fr->vz = (float)in->mvz * 0.9f;   // z e' 0..1 (unita' belt), NON pixel
        else fr->vz = 0;
        if (in->mvx || in->mvz) { if (fr->st == BS_IDLE) fr->st = BS_WALK; }
        else if (fr->st == BS_WALK) fr->st = BS_IDLE;

        // rising-edge → inizia attacchi (un colpo per pressione)
        uint8_t rise = (uint8_t)(in->buttons & ~s_prev_btn);
        if (rise & SB_BTN_J) combat_begin_attack(fr, BS_PUNCH);
        else if (rise & SB_BTN_K) combat_begin_attack(fr, BS_KICK);
        else if (rise & SB_BTN_L) {
            // salto: se gia' in aria parte la jumpkick, altrimenti salto
            if (fr->yoff > 0.5f) combat_begin_attack(fr, BS_JKICK);
            else { fr->st = BS_JUMP; fr->vy = -260.0f; }
        }
    }
    s_prev_btn = in->buttons;
}

// OSPITE: applica lo snapshot dell'host a g.f[] (autorita' = host).
static void guest_apply_state(const sb_state_t *s) {
    if (s->seq && s->seq < s_rxseq) return;      // vecchio: superato dal prossimo
    s_rxseq = s->seq;

    g.camx = s->camx;
    g.gatex = s->gatex;
    g.wave = s->wave;
    g.level = s->level;
    g.score = (long)s->score;

    // eroi: ricostruiamo gli slot 0/1 da capo (host autoritativo) — kind incluso cosi' la build/colore
    // dell'ospite combaciano con quelli dell'host.
    g.nplayers = 2;
    for (int p = 0; p < 2; p++) {
        const sb_hero_t *h = &s->hero[p];
        Fighter *fr = &g.f[p];
        fr->on = true; fr->is_hero = true; fr->player = (uint8_t)p;
        fr->kind = h->kind;
        fr->x = h->x; fr->z = h->z;
        fr->hp = (int)h->hp;
        const HeroDef *hd = brawler_hero(h->kind);
        fr->maxhp = hd ? hd->maxhp : 100;
        fr->st = (BrState)h->st;
        fr->dir = h->dir ? 1 : -1;
        fr->anim = (float)h->anim / 255.0f;
    }

    // nemici: ricostruiamo gli slot non-eroe da capo (host autoritativo).
    int idx = 0;
    for (int i = 0; i < BR_MAXF; i++) {
        Fighter *fr = &g.f[i];
        if (fr->is_hero) continue;
        if (idx < s->nenemy) {
            const sb_enemy_t *e = &s->enemy[idx++];
            fr->on = true;
            fr->is_hero = false;
            fr->kind = e->kind;
            fr->x = (float)e->x;
            fr->z = (float)e->z / 255.0f;
            fr->hp = (int)e->hp;
            fr->st = (BrState)e->st;
            const EnemyDef *def = brawler_enemy(e->kind);
            fr->maxhp = def ? def->maxhp : (e->hp > 0 ? e->hp : 1);
        } else {
            fr->on = false;
        }
    }

    // Specchia lo schermo dell'host (solo se siamo gia' in-game) cosi' l'ospite segue pausa/over/clear.
    if (g.screen == SC_PLAY || g.screen == SC_PAUSE || g.screen == SC_OVER || g.screen == SC_CLEAR) {
        BrScreen hs = (BrScreen)s->hostscreen;
        if (hs == SC_PLAY || hs == SC_PAUSE || hs == SC_OVER || hs == SC_CLEAR) {
            g.screen = hs;
            g.paused = (hs == SC_PAUSE);
        }
    }
    s_last_rx_ms = now_ms();
}

// ============================ dispatch =======================================
static void net_handle(const pnet_pkt_t *p) {
    if (p->len < 4 || p->buf[0] != SB_M0 || p->buf[1] != SB_M1 || p->buf[2] != SB_VER) return;
    int type = p->buf[3];

    // Lobby: prima dell'accoppiamento.
    if (!s_haspeer) {
        if (g.is_host) {
            // host: accetta una richiesta di JOIN
            if (type == SB_JOIN && p->len >= (int)sizeof(sb_join_t)) {
                const sb_join_t *j = (const sb_join_t *)p->buf;
                g.hero_pick[1] = (int)j->kind;        // l'host adotta il personaggio scelto dall'ospite
                memcpy(s_peer, p->mac, 6);
                s_haspeer = true;
                s_session = (now_ms() | 1u);
                s_rxseq = 0; s_prev_btn = 0;
                send_accept();
                s_last_rx_ms = now_ms();
            }
        } else {
            // ospite: l'host ha accettato
            if (type == SB_ACCEPT && p->len >= (int)sizeof(sb_accept_t)) {
                const sb_accept_t *a = (const sb_accept_t *)p->buf;
                memcpy(s_peer, p->mac, 6);
                s_haspeer = true;
                s_session = a->session;
                s_rxseq = 0;
                s_last_rx_ms = now_ms();
            }
            // ospite vede l'host annunciarsi → mandiamo JOIN al suo MAC
            else if (type == SB_HELLO && p->len >= (int)sizeof(sb_hello_t)) {
                const sb_hello_t *h = (const sb_hello_t *)p->buf;
                if (h->status == SB_HS_HOSTING) {
                    memcpy(s_peer, p->mac, 6);
                    send_join();
                    s_last_rx_ms = now_ms();
                }
            }
        }
        return;
    }

    // In partita: ignora pacchetti da MAC diversi dal peer.
    if (memcmp(p->mac, s_peer, 6) != 0) return;

    if (type == SB_BYE) { s_haspeer = false; return; }

    if (g.is_host) {
        if (type == SB_INPUT && p->len >= (int)sizeof(sb_input_t))
            host_apply_input((const sb_input_t *)p->buf);
    } else {
        if (type == SB_STATE && p->len >= (int)sizeof(sb_state_t))
            guest_apply_state((const sb_state_t *)p->buf);
    }
}

// ============================ API contratto =================================
bool bnet_start(void) {
    s_haspeer = false;
    s_session = 0;
    s_txseq = s_rxseq = 0;
    s_prev_btn = 0;
    s_guest_mvx = s_guest_mvz = 0;
    uint32_t t = now_ms();
    s_last_tx_ms = s_last_rx_ms = t;
    s_last_hello_ms = 0;
    s_on = pnet_start();
    return s_on;
}

void bnet_stop(void) {
    if (!s_on) return;
    send_bye();
    pnet_stop();
    s_on = false;
    s_haspeer = false;
}

bool bnet_available(void) { return s_on && s_haspeer; }

void bnet_poll(void) {
    if (!s_on) return;
    uint32_t t = now_ms();

    // drena tutto cio' che e' arrivato in questo frame
    pnet_pkt_t p;
    while (pnet_recv(&p)) net_handle(&p);

    // lobby: l'host si annuncia finche' non ha un peer
    if (!s_haspeer) {
        if (g.is_host && t - s_last_hello_ms > SB_HELLO_MS) {
            send_hello();
            s_last_hello_ms = t;
        }
        return;
    }

    // peer perso? rilascia (la shell tornera' a solo/menu osservando bnet_available()).
    if (t - s_last_rx_ms > SB_LOST_MS) { s_haspeer = false; return; }

    // streaming periodico
    if (t - s_last_tx_ms >= SB_TICK_MS) {
        if (g.is_host) send_state();
        else           send_input();
    }
}
