// ui-host stand-in for xz_core.c (the XiaoZhi protocol): a view the scenes set, so the gallery renders every look of
// app_xiaozhi.cpp without a network, a microphone or a codec. ui_host_scene(n) picks the state.
#include "xz_core.h"
#include <stdio.h>
#include <string.h>

static xz_view_t V;
static xz_turn_t H[XZ_HISTORY];
static int NH;

static void set(xz_state_t s, const char *status) { V.state = s; snprintf(V.status, sizeof V.status, "%s", status); V.seq++; }

void xz_init(const char *, const char *) { memset(&V, 0, sizeof V); NH = 0; V.heap_free = 87364; V.heap_block = 31744; set(XZ_IDLE, "ready"); }
void xz_shutdown(void) {}
const xz_view_t *xz_view(void) { return &V; }
bool xz_poll(void) { return false; }
void xz_talk(bool on) { if (on) { set(XZ_RECORDING, "listening"); V.rec_ms = 1800; V.level = 55; } else set(XZ_ENCODING, "encoding"); }
bool xz_replay(void) { set(XZ_PLAYING, "speaking"); return true; }
bool xz_set_server(const char *) { set(XZ_IDLE, "ready"); return true; }
void xz_forget(void) {}
int xz_history(const xz_turn_t **t) { if (t) *t = H; return NH; }

static void turn(const char *u, const char *a)
{
    snprintf(H[NH].user, sizeof H[0].user, "%s", u);
    snprintf(H[NH].reply, sizeof H[0].reply, "%s", a);
    NH++;
}

extern "C" void ui_host_scene(int n)
{
    xz_init("", "");
    switch (n) {
    case 1:                                                  // listening, the meter up
        set(XZ_RECORDING, "listening"); V.rec_ms = 2300; V.level = 70; break;
    case 2:                                                  // a conversation: two turns before, this one done
        turn("Che ore sono?", "Sono le dieci e venti di domenica mattina.");
        turn("Mi dici in una frase cos'e' un ornitorinco?",
             "Un ornitorinco e' un mammifero semiacquatico australiano che depone uova e ha un becco simile a quello di un'anatra.");
        snprintf(V.user, sizeof V.user, "E che cosa mangia di solito?");
        snprintf(V.reply, sizeof V.reply, "Mangia soprattutto piccoli animali d'acqua dolce: larve di insetti, gamberetti e "
                 "vermi, che trova frugando sul fondo con il becco a occhi chiusi.");
        set(XZ_IDLE, "ready"); break;
    case 3:                                                  // the same, speaking the answer
        ui_host_scene(2); V.progress = 40; set(XZ_PLAYING, "speaking"); break;
    case 4:                                                  // first start: choose the server
        set(XZ_SETUP, "choose a server"); break;
    case 5:                                                  // activation on xiaozhi.me
        snprintf(V.code, sizeof V.code, "424242"); set(XZ_ACTIVATE, "bind the device"); break;
    case 6:                                                  // waiting for the answer
        snprintf(V.user, sizeof V.user, "Raccontami una barzelletta sui gatti"); set(XZ_WAITING, "thinking"); break;
    default: break;                                          // 0: ready, nothing said yet
    }
}
