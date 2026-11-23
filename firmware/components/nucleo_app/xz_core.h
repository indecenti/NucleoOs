// xz_core — the XiaoZhi voice-assistant client without its UI (app_xiaozhi.cpp draws it): activation through the
// service's OTA endpoint, push-to-talk capture, Opus encoding, the WebSocket exchange and the spoken reply.
// Store only (ABI 10: nucleo_mic + nucleo_tcp_connect_tls). Host-tested by tools/xiaozhi-host against a fake server.
//
// One turn runs in phases so that the heavy pieces never share the heap (a Cardputer has no PSRAM):
//   record (mic -> PCM file) -> encode (Opus encoder, ~41 KB) -> exchange (WebSocket, TLS ~35 KB; the reply's Opus
//   packets go to a file) -> play (Opus decoder, ~26 KB, speaker). Mic and speaker share an I2S line: half duplex.
// Every step is bounded: xz_poll() does a slice of work and returns, so the UI keeps drawing.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_DEFAULT_OTA "https://api.tenclass.net/xiaozhi/ota/"   // the public service (xiaozhi.me)

typedef enum {
    XZ_SETUP = 0,     // no usable configuration yet
    XZ_OTA,           // asking the OTA endpoint for the WebSocket address
    XZ_ACTIVATE,      // waiting for the owner to bind the device on the service with the code shown
    XZ_IDLE,          // ready: hold to talk
    XZ_RECORDING,
    XZ_ENCODING,
    XZ_EXCHANGE,      // connecting, sending the question
    XZ_WAITING,       // receiving the answer
    XZ_PLAYING,
    XZ_ERROR,         // view.status says what; the next talk retries
} xz_state_t;

typedef struct {
    xz_state_t state;
    char status[72];      // one short line: what is happening or what went wrong
    char user[192];       // what the service heard (stt), this turn
    char reply[768];      // what it says (tts sentence_start, joined), this turn
    char code[16];        // activation code to type on the service's console
    char emotion[16];     // llm emotion ("happy", "thinking"...)
    char server[96];      // the configured server (OTA or ws URL), for the settings line
    int level;            // mic peak 0..100 while recording
    int progress;         // 0..100 while encoding / playing
    int rec_ms;           // length of the take
    uint32_t heap_free, heap_block;   // measured at each phase change (the prototype's numbers)
    uint32_t seq;         // bumps on every change, for redraw
} xz_view_t;

// The turns before this one, oldest first (the UI scrolls through them). Kept in RAM only, for this session.
#define XZ_HISTORY 4
typedef struct { char user[192]; char reply[768]; } xz_turn_t;
int xz_history(const xz_turn_t **turns);           // how many, and the array

void xz_init(const char *dir, const char *lang);   // data folder (config, takes), "en-US"...
void xz_shutdown(void);                            // closes everything (app exit)
const xz_view_t *xz_view(void);
bool xz_poll(void);                                // one bounded step; true when the view changed
void xz_talk(bool on);                             // push-to-talk: start / stop recording
bool xz_replay(void);                              // play the last answer again (false: none, or busy)
bool xz_set_server(const char *url);               // https://.../ota/ (activation) or ws(s)://... (direct)
void xz_forget(void);                              // drop the saved connection and identity (new activation)
void xz_text_fold(const char *src, char *dst, int cap);   // UTF-8 -> the fonts' ASCII (e', citta')

#ifdef __cplusplus
}
#endif
