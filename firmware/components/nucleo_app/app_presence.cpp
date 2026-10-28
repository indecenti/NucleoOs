// app_presence.cpp — Security > Presence: senses people through Wi-Fi (CSI), no camera, no IR.
//
// Ported from Cardputer-CSI-Human-Detector and reworked for NucleoOS. While the OS is joined to a Wi-Fi
// network the radio reports Channel State Information for each frame it receives; a body moving through a
// radio path changes it. The detector is presence_detect.c, every pixel is presence_view.cpp (both
// host-tested: npm run presence:test); this file is only the glue — radio, keys, touch, settings.
//
// Two links, two zones:
//  - router: CSI of the frames the associated router sends us. A light ICMP echo to the gateway (50/s)
//    keeps them coming, so the rate does not depend on whatever traffic is on the air;
//  - Faro (optional): a second Cardputer running this app in Faro mode broadcasts a tiny ESP-NOW frame
//    ("NCSI" + counter) every 10 ms. Espressif rates a dedicated transmitter at a fixed rate as the most
//    accurate CSI setup, and a second path tells WHICH side of the room someone crosses. The receiver
//    recognises the Faro by that payload and runs a second detector on its frames.
// Both devices must be on the same Wi-Fi (same channel). Promiscuous mode is on with a management-only
// filter (ESP-NOW frames are action frames) and a no-op callback: it only widens what the CSI hook sees.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "presence_view.h"
#include "presence_detect.h"
#include "presence_alarm.h"
#include <M5GFX.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet_chksum.h"
extern "C" {
#include "nucleo_wifiatk.h"
#include "nucleo_i18n.h"
#include "nucleo_touch.h"
#include "nucleo_audio.h"
}

M5Canvas *nucleo_screen_wide(int w);   // nucleo_ui.cpp: the shared canvas seen w px wide (no allocation)

#define CFG "/sd/system/config/presence.json"
#define FARO_MAGIC "NCSI"
#define FARO_TTL_MS 3000               // a Faro unheard for this long is gone

static pd_state_t s_pd, s_faro;
static uint8_t    s_bssid[6], s_faro_mac[6];
static volatile bool s_bssid_ok, s_faro_ready, s_faro_new;
static volatile uint32_t s_faro_seen_ms;
static bool       s_csi_on, s_promisc;
static pv_status_t s_status;
static pv_view_t  s_view;
static char       s_ssid[33];
static float      s_thr = 0.35f;
static float      s_sweep, s_glyph = 0.5f;
static int64_t    s_last_us, s_rate_us, s_retry_us, s_ping_us;
static uint32_t   s_rate_frames, s_faro_frames;
static int        s_rate, s_faro_rate;
static int        s_sock = -1;
static uint32_t   s_gw;                 // gateway IPv4, network order
static uint16_t   s_ping_seq;
// Faro (transmitter) side
static bool       s_beacon, s_now_owned;
static esp_timer_handle_t s_btimer;
static volatile uint32_t s_sent;
static uint32_t   s_sent_prev;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ── receiver ──────────────────────────────────────────────────────────────────
static bool has_magic(const uint8_t *p, int n)
{
    if (!p) return false;
    if (n > 48) n = 48;
    for (int i = 0; i + 4 <= n; i++) if (!memcmp(p + i, FARO_MAGIC, 4)) return true;
    return false;
}

// Wi-Fi task: route each frame to its link's detector; everything else is ignored.
static void IRAM_ATTR csi_cb(void *, wifi_csi_info_t *info)
{
    if (!info || !info->buf) return;
    if (s_bssid_ok && !memcmp(info->mac, s_bssid, 6)) {
        pd_feed(&s_pd, info->buf, info->len, info->rx_ctrl.rssi, info->first_word_invalid);
    } else if (s_faro_ready && !memcmp(info->mac, s_faro_mac, 6)) {
        s_faro_seen_ms = now_ms();
        pd_feed(&s_faro, info->buf, info->len, info->rx_ctrl.rssi, info->first_word_invalid);
    } else if (!s_faro_new && has_magic(info->payload, info->payload_len)) {
        memcpy(s_faro_mac, info->mac, 6);   // a (new) Faro: the UI task sets its detector up
        s_faro_seen_ms = now_ms();
        s_faro_new = true;
    }
}

static void IRAM_ATTR promisc_cb(void *, wifi_promiscuous_pkt_type_t) {}

static bool csi_enable(void)
{
    wifi_promiscuous_filter_t pf = {};
    pf.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    s_promisc = esp_wifi_set_promiscuous_rx_cb(promisc_cb) == ESP_OK && esp_wifi_set_promiscuous_filter(&pf) == ESP_OK
             && esp_wifi_set_promiscuous(true) == ESP_OK;
    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true; cfg.htltf_en = true; cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true; cfg.channel_filter_en = true;
    return esp_wifi_set_csi_config(&cfg) == ESP_OK && esp_wifi_set_csi_rx_cb(csi_cb, nullptr) == ESP_OK
        && esp_wifi_set_csi(true) == ESP_OK;
}

static void csi_disable(void)
{
    if (s_csi_on) { esp_wifi_set_csi(false); esp_wifi_set_csi_rx_cb(nullptr, nullptr); s_csi_on = false; }
    if (s_promisc) { esp_wifi_set_promiscuous(false); esp_wifi_set_promiscuous_rx_cb(nullptr); s_promisc = false; }
    s_faro_ready = false; s_faro_new = false;
}

static void pump_close(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}

// The traffic pump: a non-blocking raw ICMP socket, an echo request every 20 ms (polled from the ~50 Hz
// run loop), replies drained and dropped. Each reply from the router is one more CSI frame.
static void pump_open(void)
{
    pump_close();
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.gw.addr) return;
    s_gw = ip.gw.addr;
    s_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s_sock < 0) return;
    int fl = fcntl(s_sock, F_GETFL, 0);
    fcntl(s_sock, F_SETFL, fl | O_NONBLOCK);
}

static void pump_tick(int64_t now)
{
    if (s_sock < 0 || now - s_ping_us < 20000) return;   // 50/s: a steady CSI rate matters more than the maths
    s_ping_us = now;
    struct { uint8_t type, code; uint16_t sum, id, seq; uint8_t pad[28]; } pkt = {};
    pkt.type = 8;
    pkt.id = htons(0x4E50);
    pkt.seq = htons(++s_ping_seq);
    pkt.sum = inet_chksum(&pkt, sizeof pkt);
    struct sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = s_gw;
    sendto(s_sock, &pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to);
    uint8_t rx[96];
    while (recv(s_sock, rx, sizeof rx, 0) > 0) {}
}

// ── Faro (transmitter) ────────────────────────────────────────────────────────
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void beacon_cb(void *)
{
    uint8_t f[8];
    memcpy(f, FARO_MAGIC, 4);
    uint32_t n = s_sent;
    memcpy(f + 4, &n, 4);
    if (esp_now_send(BCAST, f, sizeof f) == ESP_OK) s_sent = n + 1;
}

static void beacon_stop(void)
{
    if (s_btimer) { esp_timer_stop(s_btimer); esp_timer_delete(s_btimer); s_btimer = nullptr; }
    if (s_now_owned) { esp_now_deinit(); s_now_owned = false; }   // only if we brought ESP-NOW up
    s_beacon = false;
}

// ESP-NOW is a global singleton another service may hold (Swarm, Nearby): EXIST is success and then it is
// not ours to tear down. The transmitter senses nothing, so CSI and the pump are off while it runs.
static bool beacon_start(void)
{
    esp_err_t e = esp_now_init();
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) return false;
    s_now_owned = e == ESP_OK;
    if (!esp_now_is_peer_exist(BCAST)) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, BCAST, 6);
        p.ifidx = WIFI_IF_STA;
        if (esp_now_add_peer(&p) != ESP_OK) { beacon_stop(); return false; }
    }
    esp_timer_create_args_t ta = {};
    ta.callback = beacon_cb;
    ta.dispatch_method = ESP_TIMER_TASK;
    ta.name = "faro";
    ta.skip_unhandled_events = true;
    if (esp_timer_create(&ta, &s_btimer) != ESP_OK || esp_timer_start_periodic(s_btimer, 10000) != ESP_OK) { beacon_stop(); return false; }
    csi_disable(); pump_close();
    s_sent = s_sent_prev = 0;
    s_beacon = true;
    return true;
}

// Follow the OS connection: (re)arm on join, recalibrate on a new router, report what blocks us.
static void link_check(int64_t now)
{
    if (now - s_retry_us < 1000000 && s_status != PV_OK) return;
    s_retry_us = now;
    if (nucleo_wifiatk_sniffer_running()) {   // a radio tool owns the radio: never fight it
        csi_disable(); pump_close(); beacon_stop(); s_status = PV_BUSY; return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        s_bssid_ok = false; pump_close(); s_status = PV_NOWIFI; return;
    }
    snprintf(s_ssid, sizeof s_ssid, "%s", (const char *)ap.ssid);
    if (s_beacon) { s_status = PV_OK; return; }
    if (!s_bssid_ok || memcmp(ap.bssid, s_bssid, 6) != 0) {
        memcpy(s_bssid, ap.bssid, 6);
        s_bssid_ok = true;
        pd_calibrate(&s_pd, now_ms());
    }
    if (!s_csi_on && !(s_csi_on = csi_enable())) { s_status = PV_NOCSI; return; }
    if (s_sock < 0) pump_open();
    s_status = PV_OK;
}

static bool faro_fresh(void) { return s_faro_ready && now_ms() - s_faro_seen_ms < FARO_TTL_MS; }

// ── frame clock ───────────────────────────────────────────────────────────────
static void alarm_siren_feed(void);
static void alarm_step(uint32_t t);
static void save_cfg(void);

static bool poll(void)
{
    alarm_siren_feed();
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < 80000) { if (!s_beacon) pump_tick(now); return false; }   // 12.5 Hz frames
    float dt = (now - s_last_us) / 1e6f;
    s_last_us = now;
    link_check(now);
    if (s_faro_new) {                                   // a Faro was just heard: give it its own detector
        s_faro_ready = false;
        pd_init(&s_faro, s_thr, now_ms());
        s_faro_frames = 0;
        s_faro_ready = true;
        s_faro_new = false;
    }
    if (s_status == PV_OK && !s_beacon) pump_tick(now);
    if (now - s_rate_us >= 1000000) {
        if (s_beacon) { s_rate = (int)(s_sent - s_sent_prev); s_sent_prev = s_sent; }
        else { s_rate = (int)(s_pd.frames - s_rate_frames); s_rate_frames = s_pd.frames; }
        s_faro_rate = faro_fresh() ? (int)(s_faro.frames - s_faro_frames) : 0;
        s_faro_frames = s_faro.frames;
        s_rate_us = now;
    }
    if (s_status == PV_OK && !s_beacon) {
        uint32_t t = now_ms();
        pd_step(&s_pd, t);
        if (faro_fresh()) pd_step(&s_faro, t);
        if (s_rate >= 2 || s_faro_rate >= 2) s_sweep = fmodf(s_sweep + dt * 1.8f, 6.2832f);
        if (s_pd.moving || (faro_fresh() && s_faro.moving))   // the glyph wanders while there is motion (not a position)
            s_glyph += (sinf(now / 7.0e5f) * 0.5f + 0.5f - s_glyph) * dt * 0.8f;
    } else if (s_beacon) s_sweep = 0;
    if (!s_beacon) alarm_step(now_ms());   // always: a siren must time out even if the Wi-Fi dropped
    return true;
}

static int s_sel;

// ── alarm ─────────────────────────────────────────────────────────────────────
// The PIN is the Alarm app's (one code for the device): its FNV-1a hash in alarm.json, "0000" until set
// there. Read-only here — it is changed in Alarm, behind its current-PIN challenge.
#define ALARM_CFG "/sd/system/config/alarm.json"
static pa_t     s_alarm;
static char     s_pin[9];
static int      s_pin_len;
static bool     s_pin_bad;
static unsigned s_pin_hash;
static int      s_saved_vol = -1;

static unsigned pin_hash(const char *p)
{
    unsigned h = 2166136261u;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h;
}

static void pin_load(void)
{
    s_pin_hash = pin_hash("0000");
    FILE *f = fopen(ALARM_CFG, "rb");
    if (!f) return;
    char b[64]; int n = fread(b, 1, sizeof b - 1, f); fclose(f);
    b[n > 0 ? n : 0] = 0;
    unsigned h = 0;
    if (sscanf(b, "{\"pin\":%u", &h) == 1 && h) s_pin_hash = h;
}

static void pin_clear(void) { memset(s_pin, 0, sizeof s_pin); s_pin_len = 0; }

static void siren_off(void)
{
    nucleo_audio_siren_stop();
    if (s_saved_vol >= 0) { nucleo_audio_set_volume(s_saved_vol); s_saved_vol = -1; }
}

static void alarm_disarm(void)
{
    pa_disarm(&s_alarm, now_ms());
    siren_off();
    pin_clear();
    s_pin_bad = false;
}

// Runs every loop iteration (~50 Hz): the siren must be fed back-to-back to stay gapless.
static void alarm_siren_feed(void)
{
    if (s_alarm.st == PA_TRIGGERED) nucleo_audio_siren(60);
}

// Once per detector step (12.5 Hz): advance the state machine, turn its events into sound.
static void alarm_step(uint32_t t)
{
    bool moving = s_status == PV_OK
               && ((s_pd.phase == PD_RUN && s_pd.moving) || (faro_fresh() && s_faro.phase == PD_RUN && s_faro.moving));
    switch (pa_step(&s_alarm, t, moving)) {
        case PA_EV_TICK: {                                   // exit delay: a beep a second, higher at the end
            int left = pa_left_s(&s_alarm, t);
            nucleo_audio_tone(left <= 3 ? 3200 : 2200, left <= 3 ? 70 : 35, 60);
            break;
        }
        case PA_EV_ARMED:
            nucleo_audio_tone(2600, 60, 70); nucleo_audio_tone(3400, 90, 70);
            break;
        case PA_EV_TRIGGER:
            if (s_saved_vol < 0) { s_saved_vol = nucleo_audio_volume(); nucleo_audio_set_volume(100); }
            pin_clear();
            break;
        case PA_EV_AUTO_OFF:
            siren_off();
            pin_clear();
            break;
        default: break;
    }
}

static pv_model_t model(void)
{
    pv_model_t m = {};
    m.pd = &s_pd; m.status = s_status; m.view = s_view; m.sel = s_sel; m.now_ms = now_ms();
    m.rate = s_rate; m.sweep = s_sweep; m.glyph = s_glyph; m.ssid = s_ssid;
    m.faro = !s_beacon && faro_fresh() ? &s_faro : nullptr;
    m.faro_rate = s_faro_rate;
    m.beacon = s_beacon; m.beacon_sent = s_sent;
    m.alarm = &s_alarm; m.pin_len = s_pin_len; m.pin_bad = s_pin_bad;
    return m;
}

// ── settings ──────────────────────────────────────────────────────────────────
static void load_cfg(void)
{
    FILE *f = fopen(CFG, "rb");
    if (!f) return;
    char b[96]; int n = fread(b, 1, sizeof b - 1, f); fclose(f);
    b[n > 0 ? n : 0] = 0;
    float t; int ai = s_alarm.arm_i, di = s_alarm.dur_i;
    int k = sscanf(b, "{\"threshold\":%f,\"arm\":%d,\"dur\":%d", &t, &ai, &di);
    if (k >= 1 && t >= 0.05f && t <= 0.95f) s_thr = t;
    if (k == 3) pa_init(&s_alarm, ai, di);
}

static void save_cfg(void)
{
    mkdir("/sd/system", 0775); mkdir("/sd/system/config", 0775);
    FILE *f = fopen(CFG, "wb");
    if (!f) return;
    fprintf(f, "{\"threshold\":%.2f,\"arm\":%d,\"dur\":%d}\n", s_thr, s_alarm.arm_i, s_alarm.dur_i);
    fclose(f);
}

// ── input ─────────────────────────────────────────────────────────────────────
// Radar: up/down = sensitivity, OK = recalibrate, TAB = the control centre. Control centre: up/down pick a
// row (1-8 jump), left/right change a value, OK runs the row. ESC goes up one level; on the radar it closes.
// Armed in any form, the keyboard is a PIN pad: digits, DEL, OK. ESC and TAB are swallowed.

static void set_hint(void)
{
    if (pa_locked(&s_alarm))
        nucleo_app_set_hint(TR5("PIN + ok disarma   del cancella", "PIN + ok disarms   del erases", "PIN + ok desarma   del borra", "PIN + ok desarme   del efface", "PIN + ok aus   del loescht"));
    else if (s_view == PV_MENU)
        nucleo_app_set_hint(TR5("su/giu scegli  < > cambia  ok", "up/dn pick  < > change  ok", "ar/ab elige  < > cambia  ok", "haut/bas  < > change  ok", "auf/ab  < > aendern  ok"));
    else if (s_view != PV_RADAR)
        nucleo_app_set_hint(TR5("esc menu   tab radar", "esc menu   tab radar", "esc menu   tab radar", "esc menu   tab radar", "esc Menue   tab Radar"));
    else if (nucleo_app_two_screens())
        nucleo_app_set_hint(TR5("radar sullo schermo grande", "radar on the big screen", "radar en pantalla grande", "radar sur le grand ecran", "Radar auf grossem Schirm"));
    else
        nucleo_app_set_hint(TR5("tab menu  su/giu sensib.  ok cal", "tab menu  up/dn sens.  ok calib", "tab menu  ar/ab sens.  ok cal", "tab menu  haut/bas sens.  ok", "tab Menue  auf/ab  ok kal."));
}

static void go(pv_view_t v)
{
    s_view = v;
    set_hint();
    nucleo_app_request_draw();
}

// Sensitivity up = threshold down. `wrap` (a tap on a tile) cycles past the end instead of stopping.
static void sensitivity(int dir, bool wrap = false)
{
    float t = s_thr - dir * 0.05f;
    if (wrap && t < 0.049f) t = 0.95f;
    s_thr = t < 0.05f ? 0.05f : t > 0.95f ? 0.95f : t;
    s_pd.thr = s_faro.thr = s_thr;
    save_cfg();
}

static void option(int *idx, int n, int dir, bool wrap)
{
    int v = *idx + dir;
    *idx = wrap ? (v + n) % n : v < 0 ? 0 : v >= n ? n - 1 : v;
    save_cfg();
}

static void adjust(int row, int dir, bool wrap)
{
    if (row == PV_ROW_THRESHOLD) sensitivity(dir, wrap);
    else if (row == PV_ROW_ARMDELAY) option(&s_alarm.arm_i, PA_N_ARM, dir, wrap);
    else if (row == PV_ROW_DURATION) option(&s_alarm.dur_i, PA_N_DUR, dir, wrap);
}

static void calibrate(void)
{
    uint32_t t = now_ms();
    pd_calibrate(&s_pd, t);
    if (s_faro_ready) pd_calibrate(&s_faro, t);
}

static void on_tab(void)
{
    if (pa_locked(&s_alarm)) return;
    go(s_view == PV_RADAR ? PV_MENU : PV_RADAR);
}

static void run_row(void)
{
    switch (s_sel) {
        case PV_ROW_ALARM:
            if (s_status == PV_OK && !s_beacon) {        // nothing to guard without a working radar
                pin_clear(); s_pin_bad = false;
                pa_arm(&s_alarm, now_ms());
                go(PV_RADAR);
            } else nucleo_audio_tone(300, 120, 50);
            break;
        case PV_ROW_THRESHOLD:
        case PV_ROW_ARMDELAY:
        case PV_ROW_DURATION:  adjust(s_sel, +1, true); break;
        case PV_ROW_CALIBRATE: calibrate(); go(PV_RADAR); break;
        case PV_ROW_FARO:
            if (s_beacon) { beacon_stop(); s_retry_us = 0; link_check(esp_timer_get_time()); }
            else beacon_start();
            go(PV_RADAR);
            break;
        case PV_ROW_DETAILS:   go(PV_DETAILS); break;
        default:               go(PV_INFO); break;
    }
}

static bool on_back(int key)
{
    if (pa_locked(&s_alarm)) {                           // the PIN is the only way out
        if (key == NK_BACK && s_pin_len) { s_pin[--s_pin_len] = 0; nucleo_app_request_draw(); }
        return true;
    }
    if (s_view == PV_MENU && key == NK_LEFT) {           // LEFT is routed here: it is the "<" of the panel
        adjust(s_sel, -1, false);
        nucleo_app_request_draw();
        return true;
    }
    if (s_view == PV_DETAILS || s_view == PV_INFO) { go(PV_MENU); return true; }
    if (s_view == PV_MENU) { go(PV_RADAR); return true; }
    return false;                                        // radar: close the app
}

static void pin_key(int key, char ch)
{
    if (key == NK_CHAR && ch >= '0' && ch <= '9' && s_pin_len < 8) { s_pin[s_pin_len++] = ch; s_pin_bad = false; }
    else if (key == NK_DEL && s_pin_len) s_pin[--s_pin_len] = 0;
    else if (key == NK_ENTER && s_pin_len) {
        if (pin_hash(s_pin) == s_pin_hash) { alarm_disarm(); set_hint(); nucleo_audio_tone(1800, 60, 50); }
        else { s_pin_bad = true; pin_clear(); if (s_alarm.st != PA_TRIGGERED) nucleo_audio_tone(250, 200, 60); }
    }
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (pa_locked(&s_alarm)) { pin_key(key, ch); return; }
    bool plus = key == NK_CHAR && ch == '+', minus = key == NK_CHAR && ch == '-';
    if (key == NK_CHAR && (ch == 'c' || ch == 'C')) { if (!s_beacon) calibrate(); go(PV_RADAR); return; }
    if (plus || minus) { sensitivity(plus ? +1 : -1); nucleo_app_request_draw(); return; }   // the panel's touch buttons
    if (s_view == PV_MENU) {
        if (key == NK_UP)    s_sel = (s_sel + PV_NROWS - 1) % PV_NROWS;
        else if (key == NK_DOWN)  s_sel = (s_sel + 1) % PV_NROWS;
        else if (key == NK_RIGHT) adjust(s_sel, +1, false);
        else if (key == NK_ENTER) { run_row(); return; }
        else if (key == NK_CHAR && ch >= '1' && ch < '1' + PV_NROWS) { s_sel = ch - '1'; run_row(); return; }
    } else if (s_view == PV_RADAR) {
        if (key == NK_UP)         sensitivity(+1);
        else if (key == NK_DOWN)  sensitivity(-1);
        else if (key == NK_ENTER && !s_beacon) calibrate();
    } else if (key == NK_ENTER) go(PV_MENU);
    nucleo_app_request_draw();
}

// ── lifecycle ─────────────────────────────────────────────────────────────────
static void on_enter(void)
{
    pa_init(&s_alarm, 1, 2);                             // defaults: arm after 10 s, siren for 30 s
    load_cfg();
    pin_load(); pin_clear(); s_pin_bad = false;
    pd_init(&s_pd, s_thr, now_ms());
    s_bssid_ok = false; s_csi_on = false; s_status = PV_NOWIFI; s_view = PV_RADAR; s_sel = 0;
    s_faro_ready = false; s_faro_new = false; s_beacon = false; s_faro_rate = 0;
    s_last_us = s_rate_us = esp_timer_get_time();
    s_retry_us = 0; s_rate = 0; s_rate_frames = 0;
    link_check(s_last_us);
    set_hint();
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_keep_awake(true);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    alarm_disarm();
    beacon_stop();
    csi_disable();
    pump_close();
    s_bssid_ok = false;
}

static void on_draw(void)
{
    static int s_two = -1;   // the panel can be plugged / unplugged under us: keep the hint truthful
    static bool s_locked = false;
    if (s_two != (int)nucleo_app_two_screens() || s_locked != pa_locked(&s_alarm)) {
        s_two = nucleo_app_two_screens(); s_locked = pa_locked(&s_alarm); set_hint();
    }
    pv_model_t m = model();
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    if (s_two && s_view == PV_RADAR) pv_draw_guide(d, top, h, m);
    else pv_draw_compact(d, top, h, m);
}

// External panel: composited in 320-wide strips through the shared 32 KB canvas (no extra RAM), each strip
// a finished slice, so the panel never shows a cleared-then-redrawn frame. Direct only if the canvas is gone.
static void on_draw_large(void)
{
    // ~6 fps is plenty for a radar and leaves the SPI bus (shared with the SD card) mostly free; the panel
    // keeps the last frame meanwhile. A repaint the framework forces (overlay gone, panel replugged) passes,
    // and so does any key (the control centre must answer a tap at once).
    static int64_t s_next_us;
    static unsigned s_gen;
    static int s_seen_sel = -1, s_seen_view = -1;
    int64_t now = esp_timer_get_time();
    bool changed = s_seen_sel != s_sel || s_seen_view != (int)s_view;
    if (now < s_next_us && s_gen == nucleo_app_repaint_gen() && !changed && s_view != PV_MENU) return;
    s_next_us = now + 160000;
    s_gen = nucleo_app_repaint_gen();
    s_seen_sel = s_sel; s_seen_view = s_view;
    pv_model_t m = model();
    M5Canvas *c = nucleo_screen_wide(320);
    if (c) {
        for (int oy = 0; oy < 240; oy += c->height()) {
            pv_draw_large(*c, oy, m);
            int h = 240 - oy < c->height() ? 240 - oy : c->height();
            d.setClipRect(0, oy, 320, h);
            c->pushSprite(0, oy);
            d.clearClipRect();
        }
    } else {
        pv_draw_large(d, 0, m);
    }
    nucleo_touch_clear();
    if (!pa_locked(&s_alarm)) nucleo_touch_add(0, 0, 50, 42, NK_BACK, 0);   // the Back button (an armed alarm keeps you here)
    if (m.view == PV_MENU && !pa_locked(&s_alarm)) {
        for (int r = 0; r < PV_NROWS; r++)                 // a tile = its quick-select digit: select + run
            nucleo_touch_add(PV_TILE_X(r), PV_TILE_Y(r), PV_TILE_W, PV_TILE_H, NK_CHAR, (char)('1' + r));
        nucleo_touch_add(50, 0, 270, 42, NK_TAB, 0);        // the rest of the status bar closes it again
    } else if (!pa_locked(&s_alarm) && !s_beacon && s_status == PV_OK) {
        nucleo_touch_add(PV_MENU_X, PV_MENU_Y, PV_MENU_W, PV_MENU_H, NK_TAB, 0);
    }
    nucleo_touch_commit();
}

extern "C" void nucleo_register_presence(void)
{
    static const nucleo_app_def_t app = {
        "presence", "Presence", "Security",
        "Senses people moving through Wi-Fi (CSI), with an intrusion alarm",
        'P', 0xF9E4, on_enter, on_key, nullptr, on_draw, on_exit, 0, nullptr, on_draw_large
    };
    nucleo_app_register(&app);
}
