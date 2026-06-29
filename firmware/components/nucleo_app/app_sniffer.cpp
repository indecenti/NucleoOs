// WiFi Sniffer app — on-device UI for the promiscuous packet capture engine (nucleo_wifiatk sniffer_*).
//
// AUTHORIZED USE ONLY. Captures 802.11 frames off the air to a standard .pcap on the SD card (open in
// Wireshark / hcxpcapngtool). For security-awareness demos, auditing networks you own or have written
// permission to test, and CTF/lab work. Opens on a consent screen; will not start until acknowledged.
//
// Modes: Tutti / Beacon / Probe / Handshake (EAPOL) / Deauth. Channel: Hop the band or a fixed 1..13.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
int         nucleo_wifiatk_sniffer_start(int mode, int channel);
void        nucleo_wifiatk_sniffer_stop(void);
bool        nucleo_wifiatk_sniffer_running(void);
unsigned    nucleo_wifiatk_sniffer_pkts(void);
unsigned    nucleo_wifiatk_sniffer_drops(void);
int         nucleo_wifiatk_sniffer_channel(void);
const char *nucleo_wifiatk_sniffer_path(void);
unsigned    nucleo_wifiatk_sniffer_beacons(void);
unsigned    nucleo_wifiatk_sniffer_probes(void);
unsigned    nucleo_wifiatk_sniffer_data(void);
unsigned    nucleo_wifiatk_sniffer_eapol(void);
unsigned    nucleo_wifiatk_sniffer_deauth(void);
}

#include "app_gfx.h"

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            LINE = 0x2945, INK = 0x0000, SNF = 0x07FF /*cyan*/, GRN = 0x8FF3, YEL = 0xFE8C;

enum { ST_CONSENT, ST_CONFIG, ST_RUNNING, ST_ARMING, ST_STOPPING };
enum { R_MODE, R_CHAN, R_GO, R_NROWS };
static const char *MODE_NAME[] = { "Tutti", "Beacon", "Probe", "Handshake", "Deauth" };
#define N_MODE 5

static int  s_state, s_row, s_mode, s_chan;   // s_chan: 0 = hop, 1..13 fixed
static bool s_consented, s_arm_armed, s_stop_armed;
static char s_err[40];
static uint32_t s_last;
static bool s_pulse;

static void set_hint(void)
{
    if (s_state == ST_CONSENT)       nucleo_app_set_hint("invio accetto   esc esci");
    else if (s_state == ST_ARMING)   nucleo_app_set_hint("avvio...");
    else if (s_state == ST_STOPPING) nucleo_app_set_hint("salvo e chiudo...");
    else if (s_state == ST_RUNNING)  nucleo_app_set_hint("invio: ferma e salva");
    else                             nucleo_app_set_hint("su/giu  destra cambia  invio avvia");
}

static void enter(void)
{
    s_err[0] = 0;
    if (nucleo_wifiatk_sniffer_running()) s_state = ST_RUNNING;
    else if (s_consented)                 s_state = ST_CONFIG;
    else                                  s_state = ST_CONSENT;
    set_hint(); nucleo_app_request_draw();
}

// The capture queue needs ~6KB CONTIGUOUS; on the ADV the live heap leaves only ~3KB free at app
// open, so free the 32KB launcher canvas first (same trick KARMA/Recorder use) — the running screen
// draws direct meanwhile. Restored on stop / start-failure.
static bool s_screen_freed;
static void free_screen(void)
{
    if (s_screen_freed) return;
    nucleo_app_set_direct_draw(true);
    nucleo_screen_release();
    s_screen_freed = true;
}
static void restore_screen(void)
{
    if (!s_screen_freed) return;
    s_screen_freed = false;
    nucleo_app_set_direct_draw(false);
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    nucleo_app_request_draw();
}

static void tick(void)
{
    if (s_state == ST_ARMING) {
        if (!s_arm_armed) return;
        free_screen();                       // +32KB contiguous BEFORE the queue allocs
        int rc = nucleo_wifiatk_sniffer_start(s_mode, s_chan);
        if (rc == 0) { s_state = ST_RUNNING; s_err[0] = 0; }
        else { restore_screen(); snprintf(s_err, sizeof s_err, "Avvio fallito (%d)", rc); s_state = ST_CONFIG; }
        set_hint(); nucleo_app_request_draw();
        return;
    }
    if (s_state == ST_STOPPING) {
        if (!s_stop_armed) return;
        nucleo_wifiatk_sniffer_stop();
        restore_screen();
        nucleo_app_exit();
        return;
    }
    if (s_state != ST_RUNNING) return;
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    if (t != s_last) { s_last = t; s_pulse = !s_pulse; nucleo_app_request_draw(); }
}

static void on_key(int key, char ch)
{
    (void)ch;
    if (s_state == ST_CONSENT) {
        if (key == NK_ENTER) { s_consented = true; s_state = ST_CONFIG; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    if (s_state == ST_ARMING || s_state == ST_STOPPING) return;
    if (s_state == ST_RUNNING) {
        if (key == NK_ENTER) { s_state = ST_STOPPING; s_stop_armed = false; set_hint(); nucleo_app_request_draw(); }
        return;
    }
    // ST_CONFIG
    if (key == NK_UP)        s_row = (s_row + R_NROWS - 1) % R_NROWS;
    else if (key == NK_DOWN) s_row = (s_row + 1) % R_NROWS;
    else if (key == NK_RIGHT) {
        if (s_row == R_MODE) s_mode = (s_mode + 1) % N_MODE;
        else if (s_row == R_CHAN) s_chan = (s_chan + 1) % 14;   // 0=hop, 1..13
    } else if (key == NK_ENTER) {
        if (s_row == R_GO) { s_state = ST_ARMING; s_arm_armed = false; set_hint(); nucleo_app_request_draw(); return; }
        if (s_row == R_MODE) s_mode = (s_mode + 1) % N_MODE;
        else if (s_row == R_CHAN) s_chan = (s_chan + 1) % 14;
    } else return;
    nucleo_app_request_draw();
}

// ---- drawing ----------------------------------------------------------------
static void row(int y, bool focus, const char *label, const char *val)
{
    if (focus) {
        d.fillRoundRect(6, y, 228, 22, 6, SNF);
        d.setTextSize(2); d.setTextColor(INK, SNF); d.setCursor(12, y + 4); d.print(label);
        int w = (int)strlen(val) * 6; d.setTextSize(1); d.setTextColor(INK, SNF);
        d.setCursor(226 - w, y + 13); d.print(val);
    } else {
        d.fillCircle(13, y + 11, 3, SNF);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(24, y + 3); d.print(label);
        int w = (int)strlen(val) * 6; d.setTextColor(DIM, BG); d.setCursor(226 - w, y + 13); d.print(val);
    }
}

static void draw_consent(void)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, "AUTH");
    d.setTextSize(2); d.setTextColor(YEL, BG); d.setCursor(10, 28); d.print("Test autorizzati");
    const char *L[] = { "Cattura i pacchetti WiFi", "in aria e li salva in .pcap", "su SD (Wireshark/hashcat).",
                        "", "Solo reti tue o con permesso." };
    unsigned short C[] = { FG, FG, FG, BG, MUTED };
    d.setTextSize(1);
    for (int i = 0; i < 5; i++) { d.setTextColor(C[i], BG); d.setCursor(10, 52 + i * 11); d.print(L[i]); }
    if (s_err[0]) { d.setTextColor(0xF800, BG); d.setCursor(10, h - 10); d.print(s_err); }
}

static void draw_config(void)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, s_err[0] ? "ERR" : "");
    char cv[12]; if (s_chan == 0) snprintf(cv, sizeof cv, "Hop banda"); else snprintf(cv, sizeof cv, "Canale %d", s_chan);
    row(24, s_row == R_MODE, "Modo", MODE_NAME[s_mode]);
    row(50, s_row == R_CHAN, "Canale", cv);
    bool go = (s_row == R_GO);
    if (go) { d.fillRoundRect(6, 76, 228, 22, 6, GRN); d.setTextSize(2); d.setTextColor(INK, GRN); d.setCursor(88, 80); d.print("AVVIA"); }
    else    { d.drawRoundRect(6, 76, 228, 22, 6, GRN); d.setTextSize(2); d.setTextColor(GRN, BG); d.setCursor(88, 80); d.print("AVVIA"); }
    // Error / hint line, kept INSIDE the content area (above the footer).
    d.setTextSize(1);
    if (s_err[0]) { d.setTextColor(0xF800, BG); d.setCursor(10, h - 10); d.print(s_err); }
    else          { d.setTextColor(DIM, BG);    d.setCursor(10, h - 10); d.print("salva in /sd/sniffer/*.pcap"); }
}

static void draw_busy(const char *t, const char *m)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, "");
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, h / 2 - 14); d.print(t);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, h / 2 + 6); d.print(m);
}

static void draw_running(void)
{
    int h = nucleo_app_content_height();
    app_ui_title("WiFi Sniffer", SNF, MODE_NAME[s_mode]);
    if (s_pulse) d.fillCircle(150, 9, 4, SNF);
    d.setTextSize(1); d.setTextColor(SNF, BG); d.setCursor(160, 7); d.print("REC");
    int cc = nucleo_wifiatk_sniffer_channel();
    char cb[10]; snprintf(cb, sizeof cb, s_chan == 0 ? "hop c%d" : "ch %d", cc);
    d.setTextColor(MUTED, BG); d.setCursor(238 - (int)strlen(cb) * 6, 7); d.print(cb);

    unsigned pk = nucleo_wifiatk_sniffer_pkts(), dr = nucleo_wifiatk_sniffer_drops();
    char ps[16]; snprintf(ps, sizeof ps, "%u", pk);
    d.setTextSize(3); d.setTextColor(FG, BG); d.setCursor(10, 34); d.print(ps);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(12 + (int)strlen(ps) * 18, 48); d.print("pacchetti");

    char ds[20]; snprintf(ds, sizeof ds, "persi %u", dr);
    d.setTextColor(dr ? YEL : DIM, BG); d.setCursor(150, 40); d.print(ds);

    // Live breakdown: beacons / probes / data + EAPOL + deauth. EAPOL highlighted (crackable material).
    unsigned ea = nucleo_wifiatk_sniffer_eapol();
    char bd[44]; snprintf(bd, sizeof bd, "B%u P%u D%u X%u", nucleo_wifiatk_sniffer_beacons(),
                          nucleo_wifiatk_sniffer_probes(), nucleo_wifiatk_sniffer_data(),
                          nucleo_wifiatk_sniffer_deauth());
    d.setTextColor(MUTED, BG); d.setCursor(10, 64); d.print(bd);
    if (ea) { char es[16]; snprintf(es, sizeof es, "HS %u", ea);
              d.setTextColor(GRN, BG); d.setCursor(170, 64); d.print(es); }

    d.drawFastHLine(10, 78, 220, LINE);
    const char *p = nucleo_wifiatk_sniffer_path(); const char *base = p;
    for (const char *q = p; *q; q++) if (*q == '/') base = q + 1;
    d.setTextColor(SNF, BG); d.setCursor(10, 86); d.print(base[0] ? base : "...");
    d.setTextColor(YEL, BG); d.setCursor(10, h - 9); d.print("invio: ferma  (B/P/D/X/HS = tipi)");
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_state == ST_CONSENT)       draw_consent();
    else if (s_state == ST_ARMING)   { draw_busy("Avvio...", "Apro la cattura."); s_arm_armed = true; }
    else if (s_state == ST_STOPPING) { draw_busy("Salvo...", "Chiudo il .pcap e ripristino rete."); s_stop_armed = true; }
    else if (s_state == ST_RUNNING)  draw_running();
    else                             draw_config();
}

static void leave(void) { }   // sniffer keeps running in the background if you leave; reopen to stop

extern "C" void nucleo_register_sniffer(void)
{
    static const nucleo_app_def_t app = {
        "sniffer", "WiFi Sniffer", "Security", "Promiscuous 802.11 capture to .pcap for authorized testing",
        'P', SNF, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
