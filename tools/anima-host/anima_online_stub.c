// Host stub for ANIMA's online tier (the only part that touches the network: HTTPS +
// mbedtls + the live Wi-Fi IP — none of which exist on a PC build). It presents the same
// interface nucleo_anima.c links against, but always reports "offline / honest miss", so
// the harness exercises the PURE offline cascade (L0 intents + L1 retrieval + HDC) exactly
// as a device with Wi-Fi switched off. Swap this out later for a real fetch if you want to
// test the online path on host.
#include "nucleo_anima_online.h"

static bool s_enabled = true;   // mirrors the device default (master switch ON)

bool nucleo_anima_online_available(void) { return false; }       // no internet on host
void nucleo_anima_set_online(bool on)    { s_enabled = on; }
bool nucleo_anima_online_enabled(void)   { return s_enabled; }
bool nucleo_anima_teacher_configured(void) { return false; }     // no cloud key on host -> L1 always serves (gates run offline)

int nucleo_anima_online_entity(const char *input, bool en,
                               char *entity, int entity_cap, char *slug, int slug_cap) {
    (void)input; (void)en; (void)entity; (void)entity_cap; (void)slug; (void)slug_cap;
    return 0;
}
int nucleo_anima_online_answer(const char *entity, const char *slug, bool en, anima_result_t *out) {
    (void)entity; (void)slug; (void)en; (void)out; return 0;
}
int nucleo_anima_online_live(const char *input, bool en, anima_result_t *out) {
    (void)input; (void)en; (void)out; return 0;
}
bool nucleo_anima_online_is_live(const char *input, bool en) {
    (void)input; (void)en; return false;
}
int nucleo_anima_online_recall(const char *query, bool en, anima_result_t *out) {
    (void)query; (void)en; (void)out; return 0;
}
int nucleo_anima_online_fact(const char *input, bool en, anima_result_t *out) {
    (void)input; (void)en; (void)out; return 0;
}
bool nucleo_anima_online_is_fact(const char *input, bool en) {
    (void)input; (void)en; return false;
}
int nucleo_anima_online_entity_bare(const char *input, bool en, anima_result_t *out) {
    (void)input; (void)en; (void)out; return 0;
}
int nucleo_anima_online_teacher(const char *input, bool en, anima_result_t *out) {
    (void)input; (void)en; (void)out; return 0;
}
void nucleo_anima_online_upgrade(const char *topic, bool en) { (void)topic; (void)en; }   // no internet on host
bool nucleo_anima_online_is_about(const char *input, bool en) { (void)input; (void)en; return false; }
int nucleo_anima_online_chat(const char *input, const char *ctx_q, const char *ctx_a, bool en, anima_result_t *out) { (void)input;(void)ctx_q;(void)ctx_a;(void)en;(void)out; return 0; }
int nucleo_anima_online_chat_ctx(const char *input, const anima_turn_t *turns, int nturns, bool en, anima_result_t *out) { (void)input;(void)turns;(void)nturns;(void)en;(void)out; return 0; }
int nucleo_anima_online_code(const char *input, bool en, anima_result_t *out) { (void)input;(void)en;(void)out; return 0; }   // no internet on host
