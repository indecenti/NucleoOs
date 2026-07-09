// app_link.cpp — "Vicino": device-to-device file & command exchange over ESP-NOW.
//
// Two modes (selectable in OPZ): NUCLEO (evolved: window+ACK+CRC+resume) and BRUCE (wire-compatible
// with real Bruce devices). The engine (nucleo_link_espnow.c) owns all the radio/SD state and self-pumps
// on its own task; this file is the UI.
//
// Visual language is now the OS-standard native look shared with Files / Music / WiFi:
//   • a segmented tab bar (TAB/RIGHT pages it) with a filled/outline active pill;
//   • a size-2 accent section header + hairline underline under the bar (app_ui_title style);
//   • scrolling lists (PEER, SEND) use the shared app_ui_list widget, so the focused row and its
//     neighbours are the big readable size-2 pill, with the launcher's 1-9 quick-jump + type-ahead
//     for free — no more size-1 hand-rolled rows.
//
// RAM: registered with exclusive_flags = NX_NET_APP, so the framework frees ~70KB (httpd/mDNS/voice/L1)
// for Vicino's whole foreground life while Wi-Fi STA — which ESP-NOW rides — stays up.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"            // shared focused-list widget + type-ahead/quick-select nav
#include <M5GFX.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_link_espnow.h"
#include "nucleo_storage.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
const char *nucleo_setup_device_name(void);
}

// ---- palette (Connect accent) ----------------------------------------------
static const unsigned short
    SURF = 0x10A2, CAP = 0x1A8B,
    ACC  = C_BLUE, GRN = C_GREEN, AMB = C_YELLOW, REDC = C_RED, PUR = C_PURPLE;

// ---- tabs ------------------------------------------------------------------
#define T_SEND 0
#define T_RECV 1
#define T_PEER 2
#define T_CMD  3
#define T_OPT  4
#define T_HELP 5
#define NTABS  6
static const char *const TABS[NTABS]    = { "INVIA","RICEVI","PEER","CMD","OPZ","?" };
static const char *const TABS_EN[NTABS] = { "SEND","RECV","PEER","CMD","OPT","?" };

// ---- state -----------------------------------------------------------------
static int   s_tab = T_SEND, s_sel = -1;
// System UI language (settings.json ui.language) — shared with the Control Center + web shell, so
// this app's existing IT/EN labels follow the global setting instead of a private toggle.
extern "C" bool nucleo_i18n_is_en(void);
extern "C" void nucleo_i18n_set_en(bool en);
static bool  s_en = false;   // seeded from the system language on enter
static int   s_proto = NLINK_PROTO_NUCLEO;   // OPZ setting
static bool  s_auto = false;                 // auto-accept incoming (trusted)
static int   s_peer_sel = 0;                 // chosen target peer
static char  s_dir[160] = NUCLEO_SD_MOUNT "/data";
static char  s_msg[48]; static int s_msg_t = 0;

// file browser
struct Ent { char name[64]; bool dir; uint32_t size; };
#define ENT_MAX 64
static Ent *s_ent = nullptr; static int s_nent = 0, s_fsel = 0;

// command input
static char s_cbuf[120]; static int s_clen = 0;

static void toast(const char *it, const char *en) { snprintf(s_msg, sizeof s_msg, "%s", s_en ? en : it); s_msg_t = 20; }
static const char *tab_label(int i) { return s_en ? TABS_EN[i] : TABS[i]; }

// ---- draw primitives -------------------------------------------------------
static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}
static void draw_tabbar(bool header) {
    d.fillRect(0, 0, W, 24, BG);
    int seg = W / NTABS;
    for (int i = 0; i < NTABS; i++) {
        int x = i * seg; const char *lab = tab_label(i); int tw = (int)strlen(lab) * 6;
        if (i == s_tab && header) { d.fillRoundRect(x + 2, 3, seg - 4, 17, 7, ACC); txt(x + (seg - tw) / 2, 8, lab, INK, ACC, 1); }
        else if (i == s_tab)      { d.drawRoundRect(x + 2, 3, seg - 4, 17, 7, ACC); txt(x + (seg - tw) / 2, 8, lab, ACC, BG, 1); }
        else                      { txt(x + (seg - tw) / 2, 8, lab, MUTED, BG, 1); }
    }
    d.drawFastHLine(0, 23, W, LINE);
}

// Section header just under the tab bar — same visual language as app_ui_title (size-2 accent
// title + hairline + accent underline) but anchored below our 24px tab bar. Returns first content y.
static int link_head(const char *title, const char *right) {
    const int y = 24;
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(10, y + 3); d.print(title);
    if (right && right[0]) {
        d.setTextSize(1); d.setTextColor(MUTED, BG);
        int rw = (int)strlen(right) * 6; d.setCursor(238 - rw, y + 8); d.print(right);
    }
    d.drawFastHLine(10, y + 21, 220, LINE);
    int tw = (int)strlen(title) * 12; if (tw > 220) tw = 220;
    d.fillRect(10, y + 21, tw, 2, ACC);
    return y + 26;   // 50
}

// ---- OPZ settings list (focus-enlarged rows, like app_wifi) ----------------
enum { RV_TEXT = 0, RV_TOGGLE, RV_ACTION, RV_SEG };
struct Row { const char *label; char val[24]; int kind; bool on; int seg; };
static const char *s_seg2[2];
#define ROWH_F 44
#define ROWH_N 26
static void draw_row(int y, bool focus, const Row *r) {
    if (!focus) {
        d.fillCircle(12, y + ROWH_N / 2, 2, r->kind == RV_ACTION ? ACC : MUTED);
        txt(20, y + ROWH_N / 2 - 3, r->label, MUTED, BG, 1);
        char rb[24];
        if (r->kind == RV_TOGGLE) snprintf(rb, sizeof rb, "%s", r->on ? "On" : "Off");
        else if (r->kind == RV_SEG) snprintf(rb, sizeof rb, "%s", s_seg2[r->seg & 1]);
        else if (r->kind == RV_ACTION) snprintf(rb, sizeof rb, ">");
        else snprintf(rb, sizeof rb, "%.16s", r->val);
        if (rb[0]) { int w = (int)strlen(rb) * 6; uint16_t vc = r->kind == RV_TOGGLE ? (r->on ? GRN : DIM) : r->kind == RV_ACTION ? ACC : DIM;
            txt(W - 12 - w, y + ROWH_N / 2 - 3, rb, vc, BG, 1); }
        return;
    }
    int h = ROWH_F;
    d.fillRoundRect(4, y, W - 8, h - 2, 9, CAP);
    d.fillRoundRect(4, y + 3, 5, h - 8, 2, ACC);
    txt(16, y + (h - 16) / 2 - 1, r->label, FG, CAP, 2);
    if (r->kind == RV_SEG) {
        int gap = 3, pw = 56, ph = 20, n = 2, totw = n * pw + gap, bx = W - 10 - totw, vy = y + (h - ph) / 2;
        for (int i = 0; i < n; i++) { int px = bx + i * (pw + gap); bool sel = (i == r->seg);
            d.fillRoundRect(px, vy, pw, ph, ph / 2, sel ? ACC : SURF);
            int tw = (int)strlen(s_seg2[i]) * 6; txt(px + (pw - tw) / 2, vy + (ph - 8) / 2, s_seg2[i], sel ? INK : MUTED, sel ? ACC : SURF, 1); }
        return;
    }
    if (r->kind == RV_TOGGLE) {
        int sw = 42, sh = 20, bx = W - 10 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, r->on ? GRN : SURF);
        d.fillCircle(r->on ? bx + sw - sh / 2 - 1 : bx + sh / 2 + 1, vy + sh / 2, sh / 2 - 3, r->on ? INK : MUTED);
        return;
    }
    if (r->kind == RV_ACTION) {
        int bw = 28, bh = 22, bx = W - 10 - bw, vy = y + (h - bh) / 2;
        d.fillRoundRect(bx, vy, bw, bh, 6, ACC);
        int ax = bx + bw / 2 - 2, ay = vy + bh / 2; d.fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, INK);
        return;
    }
    if (r->val[0]) {
        char b[18]; snprintf(b, sizeof b, "%.15s", r->val); int vw = (int)strlen(b) * 12 + 14, vh = 22;
        int bx = W - 10 - vw, vy = y + (h - vh) / 2; d.fillRoundRect(bx, vy, vw, vh, 6, SURF);
        d.setTextSize(2); d.setTextColor(FG, SURF); d.setCursor(bx + 7, vy + 3); d.print(b);
    }
}
static void draw_list(int top, int ch, Row *it, int n, int sel) {
    d.setClipRect(0, top, W, ch - top);
    if (sel < 0) { int y = top + 2; for (int i = 0; i < n && y < ch - 6; i++) { draw_row(y, false, &it[i]); y += ROWH_N; } d.clearClipRect(); return; }
    int cy = (top + ch) / 2, half = ROWH_F / 2;
    for (int i = 0; i < n; i++) {
        int dist = i - sel, y;
        if (dist == 0) y = cy - half; else if (dist < 0) y = cy - half + dist * ROWH_N; else y = cy + half + (dist - 1) * ROWH_N;
        int h = (dist == 0) ? ROWH_F : ROWH_N;
        if (y + h > top && y < ch) draw_row(y, i == sel, &it[i]);
    }
    d.clearClipRect();
}

// ---- progress bar (shared, drawn near the bottom while a transfer runs) -----
static void draw_progress(int ch) {
    nlink_status_t st; nlink_svc_status(&st);
    if (!st.active && st.state != NL_ST_DONE && st.state != NL_ST_FAIL) return;
    int y = ch - 30; d.fillRoundRect(6, y, W - 12, 26, 6, SURF);
    const char *verb = st.state == NL_ST_DONE ? (s_en ? "Done" : "Fatto")
                     : st.state == NL_ST_FAIL ? (s_en ? "Failed" : "Errore")
                     : st.sending ? (s_en ? "Sending" : "Invio") : (s_en ? "Receiving" : "Ricezione");
    uint16_t vc = st.state == NL_ST_DONE ? GRN : st.state == NL_ST_FAIL ? REDC : ACC;
    char hd[40]; snprintf(hd, sizeof hd, "%s %.18s", verb, st.name);
    txt(12, y + 3, hd, vc, SURF, 1);
    int bx = 12, bw = W - 24, by = y + 15, bh = 7;
    d.fillRoundRect(bx, by, bw, bh, bh / 2, BG);
    if (st.total) { int onw = (int)((uint64_t)st.done * bw / st.total); if (onw > bw) onw = bw; if (onw > 0) d.fillRoundRect(bx, by, onw, bh, bh / 2, vc); }
    char rt[28];
    if (st.active && st.rate_bps) snprintf(rt, sizeof rt, "%uKB/s  %u%%", (unsigned)(st.rate_bps / 1024), st.total ? (unsigned)(100ULL * st.done / st.total) : 0);
    else snprintf(rt, sizeof rt, "%u/%u KB", (unsigned)(st.done / 1024), (unsigned)(st.total / 1024));
    txt(W - 12 - (int)strlen(rt) * 6, y + 3, rt, MUTED, SURF, 1);
}

// ---- file browser ----------------------------------------------------------
static int ent_cmp(const void *a, const void *b) {
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void read_dir(void) {
    s_nent = 0; s_fsel = 0;
    if (!s_ent) return;
    DIR *dp = opendir(s_dir); if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) && s_nent < ENT_MAX) {
        if (de->d_name[0] == '.') continue;
        char full[224]; snprintf(full, sizeof full, "%s/%s", s_dir, de->d_name);
        struct stat stt; int sr = stat(full, &stt);
        bool isdir = (sr == 0) && S_ISDIR(stt.st_mode);
        snprintf(s_ent[s_nent].name, sizeof s_ent[s_nent].name, "%s", de->d_name);
        s_ent[s_nent].dir = isdir;
        s_ent[s_nent].size = (sr == 0 && !isdir) ? (uint32_t)stt.st_size : 0;
        s_nent++;
    }
    closedir(dp);
    qsort(s_ent, s_nent, sizeof(Ent), ent_cmp);
}
static bool at_root(void) { return strcmp(s_dir, NUCLEO_SD_MOUNT) == 0 || strcmp(s_dir, NUCLEO_SD_MOUNT "/") == 0; }
static void dir_up(void) { char *s = strrchr(s_dir, '/'); if (s && s != s_dir) { *s = 0; if (!s_dir[0]) snprintf(s_dir, sizeof s_dir, "%s", NUCLEO_SD_MOUNT); read_dir(); } }

// Virtual file list: index 0 is the ".." parent row when not at the SD root, entries follow.
static int  snd_extra(void)  { return at_root() ? 0 : 1; }
static int  snd_count(void)  { return s_nent + snd_extra(); }
static bool snd_is_up(int i) { return snd_extra() && i == 0; }
static Ent *snd_at(int i)    { int e = i - snd_extra(); return (e >= 0 && e < s_nent) ? &s_ent[e] : nullptr; }
static const char *snd_label(int i, void *) { if (snd_is_up(i)) return ".."; const Ent *e = snd_at(i); return e ? e->name : ""; }
static const char *snd_right(int i, void *) {
    if (snd_is_up(i)) return ">";
    const Ent *e = snd_at(i); if (!e) return nullptr; if (e->dir) return ">";
    static char b[12]; uint32_t kb = (e->size + 1023) / 1024;
    if (kb < 1000) snprintf(b, sizeof b, "%uK", (unsigned)kb);
    else           snprintf(b, sizeof b, "%u.%uM", (unsigned)(kb / 1024), (unsigned)((kb % 1024) * 10 / 1024));
    return b;
}
static unsigned short snd_col(int i, void *) { if (snd_is_up(i)) return AMB; const Ent *e = snd_at(i); return (e && e->dir) ? AMB : ACC; }

// ---- peer list -------------------------------------------------------------
static const char *pr_label(int i, void *) { return nlink_svc_peer_name(i); }
static const char *pr_right(int i, void *) { return nlink_svc_peer_proto(i) == NLINK_PROTO_BRUCE ? "BRUCE" : "NUCLEO"; }
static unsigned short pr_col(int i, void *) {
    if (i == s_peer_sel) return GRN;   // chosen target stands out
    return nlink_svc_peer_proto(i) == NLINK_PROTO_BRUCE ? PUR : ACC;
}
// Name of the chosen send/command target, clamped in case the peer list shrank under us.
static const char *peer_target_name(void) {
    int n = nlink_svc_peer_count(); if (n <= 0) return s_en ? "no peer" : "nessun peer";
    int i = s_peer_sel; if (i < 0 || i >= n) i = 0;
    return nlink_svc_peer_name(i);
}

// ---- tab draws -------------------------------------------------------------
static void draw_send(int ch) {
    char rt[24]; snprintf(rt, sizeof rt, "-> %.13s", peer_target_name());
    const char *base = strrchr(s_dir, '/'); base = (base && base[1]) ? base + 1 : "SD";
    int y0 = link_head(base, rt);
    int tn = snd_count();
    if (tn == 0) { txt(14, y0 + 12, s_en ? "Empty folder" : "Cartella vuota", DIM, BG, 1); return; }
    app_ui_list(y0, ch - y0, tn, s_fsel, snd_label, snd_right, snd_col, nullptr);
    draw_progress(ch);
}
static void draw_peer(int ch) {
    int n = nlink_svc_peer_count();
    char rt[16]; snprintf(rt, sizeof rt, "ch%d", nlink_svc_channel());
    int y0 = link_head(s_en ? "Devices" : "Dispositivi", rt);
    bool hf = (s_sel < 0);
    d.fillRoundRect(6, y0, W - 12, 20, 7, hf ? CAP : SURF);
    if (hf) d.fillRoundRect(6, y0 + 3, 5, 14, 2, ACC);
    txt(16, y0 + 6, s_en ? "Scan for peers" : "Cerca dispositivi", hf ? FG : MUTED, hf ? CAP : SURF, 1);
    txt(W - 52, y0 + 6, s_en ? "ENTER" : "INVIO", hf ? ACC : DIM, hf ? CAP : SURF, 1);
    int ly = y0 + 24;
    if (!n) { txt(16, ly + 8, s_en ? "No peers yet." : "Nessun peer trovato.", DIM, BG, 1);
              txt(16, ly + 22, s_en ? "Press ENTER to scan." : "Premi INVIO per cercare.", MUTED, BG, 1);
              draw_progress(ch); return; }
    app_ui_list(ly, ch - ly, n, s_sel, pr_label, pr_right, pr_col, nullptr);
    draw_progress(ch);
}
// Green/red decision chips, shared by RECV (offer) and CMD (command). Draw at (x,y).
static void draw_yn_chips(int x, int y) {
    const int bw = 78, bh = 20, gap = 8;
    d.fillRoundRect(x, y, bw, bh, 6, GRN);        txt(x + 8, y + 6, s_en ? "Y Accept" : "Y Accetta", INK, GRN, 1);
    d.fillRoundRect(x + bw + gap, y, bw, bh, 6, REDC); txt(x + bw + gap + 8, y + 6, s_en ? "N Reject" : "N Rifiuta", INK, REDC, 1);
}
static void draw_recv(int ch) {
    const char *ib = nlink_svc_inbox(); const char *ibn = strrchr(ib, '/'); ibn = ibn ? ibn + 1 : ib;
    int y0 = link_head(s_en ? "Receive" : "Ricevi", ibn);
    char name[64], from[40]; uint32_t sz;
    if (nlink_svc_offer_pending(name, sizeof name, from, sizeof from, &sz)) {
        int cx = 8, cw = W - 16, cy = y0 + 2, chh = 66;
        d.fillRoundRect(cx, cy, cw, chh, 9, CAP);
        d.drawRoundRect(cx, cy, cw, chh, 9, ACC);
        char h[48]; snprintf(h, sizeof h, s_en ? "%.14s wants to send:" : "%.14s vuole inviarti:", from);
        txt(cx + 12, cy + 7, h, MUTED, CAP, 1);
        char f[24]; snprintf(f, sizeof f, "%.16s", name); txt(cx + 12, cy + 20, f, AMB, CAP, 2);
        char szs[20]; snprintf(szs, sizeof szs, "%u KB", (unsigned)(sz / 1024)); txt(cx + cw - 12 - (int)strlen(szs) * 6, cy + 22, szs, DIM, CAP, 1);
        draw_yn_chips(cx + 12, cy + chh - 26);
        return;
    }
    txt(14, y0 + 8, s_en ? "Listening for offers..." : "In ascolto di offerte...", ACC, BG, 1);
    txt(14, y0 + 24, s_en ? "Received files land in the inbox above." : "I file ricevuti vanno nell'inbox.", DIM, BG, 1);
    draw_progress(ch);
}
static void draw_cmd(int ch) {
    char cmd[160], from[40];
    if (nlink_svc_cmd_pending(cmd, sizeof cmd, from, sizeof from)) {
        char fr[24]; snprintf(fr, sizeof fr, "%.14s", from);
        int y0 = link_head(s_en ? "Command" : "Comando", fr);
        int cx = 8, cw = W - 16, cy = y0 + 2, chh = 66;
        d.fillRoundRect(cx, cy, cw, chh, 9, CAP);
        d.drawRoundRect(cx, cy, cw, chh, 9, AMB);
        txt(cx + 12, cy + 7, s_en ? "Requested command:" : "Comando richiesto:", MUTED, CAP, 1);
        char c[24]; snprintf(c, sizeof c, "%.18s", cmd); txt(cx + 12, cy + 20, c, AMB, CAP, 2);
        draw_yn_chips(cx + 12, cy + chh - 26);
        return;
    }
    char rt[24]; snprintf(rt, sizeof rt, "-> %.13s", peer_target_name());
    int y0 = link_head(s_en ? "Command" : "Comando", rt);
    txt(12, y0 + 4, s_en ? "Type, ENTER to send:" : "Scrivi, INVIO per inviare:", MUTED, BG, 1);
    int by = y0 + 18;
    d.fillRoundRect(8, by, W - 16, 30, 7, SURF);
    int vis = 16, off = s_clen > vis ? s_clen - vis : 0;
    char sh[20]; int k = 0; for (int i = off; i < s_clen && k < vis; i++, k++) sh[k] = s_cbuf[i]; sh[k] = 0;
    d.setTextSize(2); d.setTextColor(FG, SURF); d.setCursor(14, by + 8); d.print(sh);
    d.fillRect(14 + k * 12, by + 7, 3, 16, ACC);
    txt(12, by + 40, s_en ? "Receiver confirms before it runs." : "Il ricevente conferma prima di eseguire.", DIM, BG, 1);
}
#define O_PROTO 0
#define O_AUTO  1
#define O_CONF  2
#define O_INBOX 3
#define O_CHAN  4
#define O_NAME  5
#define O_LANG  6
static void build_opt(Row *it, int *n) {
    s_seg2[0] = "Nucleo"; s_seg2[1] = "Bruce";
    int k = 0;
    it[k].label = s_en ? "Protocol" : "Protocollo"; it[k].kind = RV_SEG; it[k].seg = s_proto; k++;
    it[k].label = s_en ? "Auto-accept" : "Auto-accetta"; it[k].kind = RV_TOGGLE; it[k].on = s_auto; k++;
    it[k].label = s_en ? "Confirm cmds" : "Conferma cmd"; it[k].kind = RV_TOGGLE; it[k].on = true; k++;
    it[k].label = "Inbox"; it[k].kind = RV_TEXT; { const char *b = strrchr(nlink_svc_inbox(), '/'); snprintf(it[k].val, sizeof it[k].val, "%s", b ? b + 1 : nlink_svc_inbox()); } k++;
    it[k].label = s_en ? "Channel" : "Canale"; it[k].kind = RV_TEXT; snprintf(it[k].val, sizeof it[k].val, "ch%d", nlink_svc_channel()); k++;
    it[k].label = s_en ? "Name" : "Nome"; it[k].kind = RV_TEXT; snprintf(it[k].val, sizeof it[k].val, "%.15s", nlink_svc_name()); k++;
    it[k].label = s_en ? "Language" : "Lingua"; it[k].kind = RV_TEXT; snprintf(it[k].val, sizeof it[k].val, "%s", s_en ? "EN" : "IT"); k++;
    *n = k;
}
static void draw_help(int ch) {
    int y0 = link_head(s_en ? "Guide" : "Guida", "Vicino");
    const char *it[] = {
        "TAB / DESTRA: cambia scheda",
        "1-9 salto rapido, lettere = cerca",
        "PEER: INVIO cerca, INVIO sceglie",
        "INVIA: sfoglia, INVIO invia il file",
        "RICEVI / CMD: Y accetta, N rifiuta",
        "OPZ: Nucleo affidabile / Bruce compat.", 0 };
    const char *en[] = {
        "TAB / RIGHT: switch tab",
        "1-9 quick-jump, letters = search",
        "PEER: ENTER scan, ENTER pick target",
        "SEND: browse, ENTER sends the file",
        "RECV / CMD: Y accept, N reject",
        "OPT: Nucleo reliable / Bruce compat.", 0 };
    const char **m = s_en ? en : it;
    for (int i = 0; m[i]; i++) txt(10, y0 + 2 + i * 12, m[i], i == 0 || i == 1 ? FG : MUTED, BG, 1);
    (void)ch;
}

static int opt_rows(void) { Row it[8]; int n; build_opt(it, &n); return n; }

static void update_hint(void) {
    const char *h;
    switch (s_tab) {
        case T_SEND: h = s_en ? "UP/DN  ENTER send  DEL up" : "SU/GIU  INVIO invia  DEL su"; break;
        case T_PEER: h = (s_sel < 0) ? (s_en ? "DOWN list  ENTER scan" : "GIU lista  INVIO cerca")
                                     : (s_en ? "UP/DN  ENTER set target" : "SU/GIU  INVIO scegli"); break;
        case T_RECV: h = s_en ? "Y accept   N reject" : "Y accetta   N rifiuta"; break;
        case T_CMD:  h = s_en ? "type   ENTER send   Y/N" : "scrivi   INVIO invia   Y/N"; break;
        case T_OPT:  h = (s_sel < 0) ? (s_en ? "DOWN rows  TAB tab" : "GIU righe  TAB scheda")
                                     : (s_en ? "UP/DN  ENTER change" : "SU/GIU  INVIO cambia"); break;
        default:     h = s_en ? "TAB switch tab   ESC exit" : "TAB scheda   ESC esci"; break;
    }
    nucleo_app_set_hint(h);
}

// ---- on_draw ---------------------------------------------------------------
static void on_draw(void) {
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, BG);
    // Bar pill is filled only while focus is on the bar; once focus is inside a list it goes to outline.
    bool bar_focus = !((s_tab == T_SEND) || ((s_tab == T_PEER || s_tab == T_OPT) && s_sel >= 0));
    draw_tabbar(bar_focus);
    Row it[8]; int n;
    switch (s_tab) {
        case T_SEND: draw_send(ch); break;
        case T_RECV: draw_recv(ch); break;
        case T_PEER: draw_peer(ch); break;
        case T_CMD:  draw_cmd(ch);  break;
        case T_OPT:  { int y0 = link_head(s_en ? "Options" : "Opzioni", nullptr); build_opt(it, &n); draw_list(y0, ch, it, n, s_sel); } break;
        default:     draw_help(ch); break;
    }
    if (s_msg_t > 0) { int iy = ch - 20; d.fillRoundRect(6, iy, W - 12, 18, 5, SURF); txt(12, iy + 5, s_msg, AMB, SURF, 1); }
}

// ---- input -----------------------------------------------------------------
static void page_tab(void) {
    s_tab = (s_tab + 1) % NTABS;
    int rows = (s_tab == T_OPT) ? opt_rows() : 0;
    s_sel = (s_sel >= 0 && rows > 0) ? 0 : -1;
    if (s_tab == T_SEND) read_dir();
    update_hint(); nucleo_app_request_draw();
}
static void send_selected(void) {
    int tn = snd_count(); if (tn == 0) return;
    if (snd_is_up(s_fsel)) { dir_up(); return; }
    Ent *e = snd_at(s_fsel); if (!e) return;
    if (e->dir) { char nx[224]; snprintf(nx, sizeof nx, "%s/%s", s_dir, e->name); snprintf(s_dir, sizeof s_dir, "%.159s", nx); read_dir(); return; }
    if (!nlink_svc_peer_count()) { toast("Scegli un peer", "Pick a peer"); return; }
    char path[224]; snprintf(path, sizeof path, "%s/%s", s_dir, e->name);
    if (nlink_svc_send_file(s_peer_sel, path, (nlink_proto_t)s_proto)) toast("Invio avviato", "Sending");
    else toast("Occupato/illeggibile", "Busy/unreadable");
}
static void opt_activate(int row) {
    switch (row) {
        case O_PROTO: s_proto ^= 1; toast(s_proto ? "Modo Bruce" : "Modo Nucleo", s_proto ? "Bruce mode" : "Nucleo mode"); break;
        case O_AUTO:  s_auto = !s_auto; toast(s_auto ? "Auto-accetta ON" : "Auto-accetta OFF", s_auto ? "Auto-accept ON" : "Auto-accept OFF"); break;
        case O_LANG:  nucleo_i18n_set_en(!nucleo_i18n_is_en()); s_en = nucleo_i18n_is_en(); update_hint(); break;
        default: break;
    }
}
static void on_key(int key, char ch) {
    if (s_msg_t > 0 && key != NK_NONE) s_msg_t = 1;

    // global tab paging
    if (key == NK_RIGHT) { page_tab(); return; }

    // Y/N answers (RECV offer, CMD pending) — only when something is actually pending.
    if ((s_tab == T_RECV || s_tab == T_CMD) && (ch == 'y' || ch == 'Y' || ch == 'n' || ch == 'N')) {
        bool ok = (ch == 'y' || ch == 'Y');
        char tmp[8], tf[8];
        if (s_tab == T_RECV && nlink_svc_offer_pending(tmp, 0, tf, 0, 0)) { nlink_svc_offer_answer(ok); toast(ok ? "Accettato" : "Rifiutato", ok ? "Accepted" : "Rejected"); nucleo_app_request_draw(); return; }
        if (s_tab == T_CMD && nlink_svc_cmd_pending(tmp, 0, tf, 0)) { nlink_svc_cmd_confirm(ok); toast(ok ? "Eseguito" : "Scartato", ok ? "Run" : "Discarded"); nucleo_app_request_draw(); return; }
    }

    // CMD input typing (only while no command is pending confirmation)
    if (s_tab == T_CMD) {
        char p[8], f[8];
        bool pending = nlink_svc_cmd_pending(p, sizeof p, f, sizeof f);
        if (!pending) {
            if (key == NK_CHAR && ch >= 32 && ch < 127) { if (s_clen < (int)sizeof(s_cbuf) - 1) { s_cbuf[s_clen++] = ch; s_cbuf[s_clen] = 0; } nucleo_app_request_draw(); return; }
            if (key == NK_DEL) { if (s_clen > 0) s_cbuf[--s_clen] = 0; nucleo_app_request_draw(); return; }
            if (key == NK_ENTER) {
                if (!nlink_svc_peer_count()) { toast("Scegli un peer", "Pick a peer"); return; }
                if (s_clen && nlink_svc_send_cmd(s_peer_sel, s_cbuf, (nlink_proto_t)s_proto)) { toast("Comando inviato", "Command sent"); s_clen = 0; s_cbuf[0] = 0; }
                nucleo_app_request_draw(); return;
            }
        }
    }

    // SEND: shared list nav (up/dn + 1-9 quick-jump + type-ahead), ENTER opens/sends, DEL goes up.
    if (s_tab == T_SEND) {
        int tn = snd_count();
        if (app_ui_list_key(key, ch, &s_fsel, tn, snd_label, nullptr)) { nucleo_app_request_draw(); return; }
        if (key == NK_ENTER) send_selected();
        else if (key == NK_DEL && !at_root()) dir_up();
        nucleo_app_request_draw(); return;
    }
    // PEER: header row (scan) above the shared peer list.
    if (s_tab == T_PEER) {
        int n = nlink_svc_peer_count();
        if (n == 0) s_sel = -1; else if (s_sel >= n) s_sel = n - 1;
        if (s_sel < 0) {
            if (key == NK_ENTER) { nlink_svc_discover((nlink_proto_t)s_proto); toast("Cerco...", "Scanning..."); }
            else if (key == NK_DOWN && n > 0) s_sel = 0;
        } else {
            if (key == NK_UP && s_sel == 0) s_sel = -1;                 // back up onto the scan row
            else if (app_ui_list_key(key, ch, &s_sel, n, pr_label, nullptr)) { /* moved */ }
            else if (key == NK_ENTER) { s_peer_sel = s_sel; toast("Peer scelto", "Peer set"); }
        }
        update_hint(); nucleo_app_request_draw(); return;
    }
    if (s_tab == T_OPT) {
        int n = opt_rows();
        if (s_sel == -1) { if (key == NK_DOWN && n > 0) s_sel = 0; }
        else {
            if (key == NK_UP)   { if (s_sel > 0) s_sel--; else s_sel = -1; }
            else if (key == NK_DOWN) { if (s_sel < n - 1) s_sel++; }
            else if (key == NK_ENTER) opt_activate(s_sel);
        }
        update_hint(); nucleo_app_request_draw(); return;
    }
    nucleo_app_request_draw();
}

// Back/Left: inside the file browser, go up a folder; inside a focused list, drop focus; else close.
static bool on_back(int key) {
    if (s_tab == T_SEND && !at_root()) { dir_up(); nucleo_app_request_draw(); return true; }
    if (s_sel >= 0) { s_sel = -1; update_hint(); nucleo_app_request_draw(); return true; }
    (void)key; return false;   // let the framework close the app
}

// 5Hz refresh, but only when something on screen actually changes (a transfer, an incoming
// offer/command, a toast, or the peers arriving on the discovery tabs). Static tabs stay idle.
static void on_tick(void) {
    nlink_status_t st; nlink_svc_status(&st);
    bool dyn = st.active || st.state == NL_ST_DONE || st.state == NL_ST_FAIL
            || s_tab == T_PEER || s_tab == T_RECV || s_tab == T_CMD || s_msg_t > 0;
    if (dyn) nucleo_app_request_draw();
}

static void on_enter(void) {
    s_en = nucleo_i18n_is_en();           // follow the system language
    s_tab = T_SEND; s_sel = -1; s_clen = 0; s_cbuf[0] = 0;
    snprintf(s_dir, sizeof s_dir, "%s", NUCLEO_SD_MOUNT "/data");
    if (!s_ent) s_ent = (Ent *)calloc(ENT_MAX, sizeof *s_ent);   // heap-resident only while open; ZERO RAM at boot
    nucleo_app_set_tab_handler(page_tab);     // TAB pages the tabs (the "tab system")
    nucleo_app_set_back_handler(on_back);
    if (!nlink_svc_start()) { toast("ESP-NOW non avviato", "ESP-NOW failed"); return; }
    nlink_svc_listen(NLINK_PROTO_NUCLEO);     // always ready to receive
    nlink_svc_discover(NLINK_PROTO_NUCLEO);
    read_dir(); update_hint();
}
static void on_exit(void) { nlink_svc_stop(); free(s_ent); s_ent = nullptr; s_nent = 0; }

extern "C" void nucleo_register_link(void) {
    static const nucleo_app_def_t app = {
        "link", "Vicino", "Communication",
        "Scambia file e comandi con altri device (Nucleo o Bruce) via ESP-NOW",
        'V', C_BLUE,
        on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP,                            // declarative exclusive: ~70KB freed, Wi-Fi STA stays up
    };
    nucleo_app_register(&app);
}
