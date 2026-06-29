// app_mail — native SMTP mail client for NucleoOS.
// Smartwatch-style UI: large fonts, scrollable lists, a step-by-step account wizard,
// blinking cursor, 1-9 quick-select, recents picker.
//
// RAM discipline (no-PSRAM, see docs/memory-budget.md): NOTHING big lives in .bss. All working
// buffers sit in a single heap struct allocated in on_enter and freed in on_exit — never at boot.
// The TLS send additionally runs in a worker task with its OWN heap job (independent of the app
// struct, so leaving the app mid-send is safe), after freeing httpd (NX_NET_APP) + the 32 KB canvas.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "nucleo_smtp.h"
#include "nucleo_mailcfg.h"
#include "nucleo_exclusive.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SENT_LOG "/sd/system/mail/sent.log"
#define REC_FILE "/sd/system/mail/recents.txt"

// ---- geometry ----
#define HDR    20
#define CT     (HDR + 4)
#define CB     (H - HINT)
#define ROW_H  26
#define ACC    THEME_ACC
#define WHITE  0xFFFF

// ---- tabs / wizard steps ----
enum { TAB_ACCT = 0, TAB_WRITE = 1, TAB_SENT = 2, NTABS = 3 };
enum { WS_PROV, WS_HOST, WS_PORT, WS_EMAIL, WS_PASS, WS_NAME, WS_CONFIRM };
enum EdT { ED_NONE, ED_TO, ED_SUBJ, ED_BODY, ED_W_HOST, ED_W_PORT, ED_W_EMAIL, ED_W_PASS, ED_W_NAME };

// ---- heap working set (allocated on_enter, freed on_exit — NEVER in .bss) ----
typedef struct {
    smtp_account_t accts[MAIL_MAX_ACCOUNTS];
    smtp_account_t edit;
    char rec[6][96];
    char to[96], subj[120], body[768];
    char portbuf[8];
    char rowbuf[MAIL_MAX_ACCOUNTS + 1][40];
    char sent[8][104];               // newest sent-log lines for the Inviate tab
} mail_state_t;
static mail_state_t *M = NULL;        // pointer only — 4 bytes of .bss

// ---- send job (worker-owned heap; independent of M) ----
typedef struct { smtp_account_t acc; char to[96], subj[120], body[768]; } mail_job_t;

// ---- small scalar UI state (negligible .bss) ----
static bool s_dirty = true;
static int  s_tab = TAB_ACCT;
static char s_status[64];
static int64_t s_status_until = 0;
static int  s_nacct = 0, s_def = -1, s_nrec = 0;
static int  s_asel = 0, s_ascroll = 0;
static int  s_wfield = 0;
#define W_FIELDS 4
static bool s_topick = false; static int s_tpsel = 0, s_tpscroll = 0;
// editor
static bool s_ed = false; static char *s_edbuf = NULL; static size_t s_edcap = 0;
static bool s_edsecret = false, s_ednum = false, s_edwiz = false;
static const char *s_edlabel = ""; static EdT s_edt = ED_NONE;
static bool s_blink = false; static int s_blink_n = 0;
// wizard
static bool s_wiz = false; static int s_wseq[8], s_wn = 0, s_wi = 0; static bool s_wcustom = false;
static int  s_edit_idx = -1; static int s_psel = 0, s_pscroll = 0;
// async send
static volatile bool s_sending = false, s_send_done = false, s_send_ok = false;
static int  s_send_phase = 0;
static char s_send_err[96];

// ===========================================================================
static void mark(void) { s_dirty = 1; nucleo_app_request_draw(); }
static void set_status(const char *s) { strncpy(s_status, s, sizeof s_status - 1); s_status[sizeof s_status - 1] = 0;
    s_status_until = esp_timer_get_time() + 3000000; mark(); }
static void txt(int x, int y, const char *s, int size, unsigned short fg) {
    d.setTextSize(size); d.setTextColor(fg); d.drawString(s, x, y); }
static int tw(const char *s, int size) { return (int)strlen(s) * 6 * size; }
static void ctxt(int y, const char *s, int size, unsigned short fg) { txt((W - tw(s, size)) / 2, y, s, size, fg); }

// case-insensitive substring (empty needle matches all)
static int ci_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *h = hay; *h; ++h) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}
// contacts (recents) matching what's typed in the "A" field; returns count, fills indices
static int mail_suggest(int *out, int max) {
    int c = 0;
    for (int i = 0; i < s_nrec && c < max; ++i) if (ci_contains(M->rec[i], M->to)) out[c++] = i;
    return c;
}

static void reload(void) {
    s_nacct = nucleo_mailcfg_count();
    for (int i = 0; i < s_nacct; ++i) nucleo_mailcfg_get_redacted(i, &M->accts[i]);
    s_def = nucleo_mailcfg_default();
    if (s_asel > s_nacct) s_asel = s_nacct;
}
static void load_recents(void) {
    s_nrec = 0;
    FILE *f = fopen(REC_FILE, "r"); if (!f) return;
    char line[96];
    while (s_nrec < 6 && fgets(line, sizeof line, f)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) { strncpy(M->rec[s_nrec], line, 95); M->rec[s_nrec][95] = 0; s_nrec++; }
    }
    fclose(f);
}
static void add_recent(const char *to) {
    if (!to || !to[0]) return;
    for (int i = 0; i < s_nrec; ++i) if (strcmp(M->rec[i], to) == 0) {
        for (int j = i; j > 0; --j) memcpy(M->rec[j], M->rec[j - 1], 96);
        strncpy(M->rec[0], to, 95); goto save;
    }
    for (int j = (s_nrec < 6 ? s_nrec : 5); j > 0; --j) memcpy(M->rec[j], M->rec[j - 1], 96);
    strncpy(M->rec[0], to, 95); M->rec[0][95] = 0;
    if (s_nrec < 6) s_nrec++;
save:
    mkdir("/sd/system", 0775); mkdir("/sd/system/mail", 0775);
    FILE *f = fopen(REC_FILE, "w"); if (!f) return;
    for (int i = 0; i < s_nrec; ++i) fprintf(f, "%s\n", M->rec[i]);
    fclose(f);
}

// ---- send worker (owns its job; does NOT touch M) ----
static void smtp_task(void *arg) {
    mail_job_t *job = (mail_job_t *)arg;
    smtp_msg_t m = { .to = job->to, .subject = job->subj, .body = job->body };
    char err[96] = { 0 };
    esp_err_t e = nucleo_smtp_send(&job->acc, &m, err, sizeof err);
    strncpy(s_send_err, err, sizeof s_send_err - 1); s_send_err[sizeof s_send_err - 1] = 0;
    free(job);
    s_send_ok = (e == ESP_OK); s_send_done = true; s_sending = false;
    vTaskDelete(NULL);
}
static void start_send(void) {
    if (s_sending) return;
    int def = nucleo_mailcfg_default();
    smtp_account_t acc;
    if (def < 0 || !nucleo_mailcfg_get(def, &acc)) { set_status("Nessun account"); return; }
    if (!M->to[0] || !M->body[0]) { set_status("A: e Testo obbligatori"); return; }
    if (!strchr(M->to, '@') || !strchr(M->to, '.')) { set_status("Indirizzo non valido"); return; }
    s_sending = true; s_send_done = false; s_send_phase = 1;
    set_status("Invio in corso...");
}

// ===========================================================================
// editor
static void editor_open(const char *label, char *buf, size_t cap, bool secret, bool numeric, bool wiz, EdT t) {
    s_ed = true; s_edbuf = buf; s_edcap = cap; s_edsecret = secret; s_ednum = numeric; s_edwiz = wiz;
    s_edlabel = label; s_edt = t; s_blink = true;
    nucleo_app_set_hint(t == ED_TO ? "Digita  TAB=contatto  OK=Invio" : "Digita  OK=Invio  Indietro=annulla");
    mark();
}
static void editor_close(void) {
    s_ed = false; s_edbuf = NULL; s_edt = ED_NONE;
    nucleo_app_set_hint("TAB scheda  Esc esci"); mark();
}
static void ed_putc(char c) {
    if (!s_ed || !s_edbuf || c < 0x20) return;
    if (s_ednum && !(c >= '0' && c <= '9')) return;
    size_t n = strlen(s_edbuf);
    if (n + 1 < s_edcap) { s_edbuf[n] = c; s_edbuf[n + 1] = 0; mark(); }
}

// ---- wizard ----
static void build_wseq(bool custom, bool editing) {
    int i = 0;
    if (!editing) s_wseq[i++] = WS_PROV;
    if (custom) { s_wseq[i++] = WS_HOST; s_wseq[i++] = WS_PORT; }
    s_wseq[i++] = WS_EMAIL; s_wseq[i++] = WS_PASS; s_wseq[i++] = WS_NAME; s_wseq[i++] = WS_CONFIRM;
    s_wn = i;
}
static void wiz_show(void) {
    s_ed = false;
    switch (s_wseq[s_wi]) {
        case WS_HOST:  editor_open("Host SMTP", M->edit.host, sizeof M->edit.host, false, false, true, ED_W_HOST); break;
        case WS_PORT:  snprintf(M->portbuf, sizeof M->portbuf, "%d", M->edit.port);
                       editor_open("Porta", M->portbuf, sizeof M->portbuf, false, true, true, ED_W_PORT); break;
        case WS_EMAIL: editor_open("Email", M->edit.user, sizeof M->edit.user, false, false, true, ED_W_EMAIL); break;
        case WS_PASS:  editor_open(s_edit_idx < 0 ? "Password app" : "Password (vuoto=invariata)",
                                   M->edit.pass, sizeof M->edit.pass, true, false, true, ED_W_PASS); break;
        case WS_NAME:  editor_open("Nome mittente", M->edit.from_name, sizeof M->edit.from_name, false, false, true, ED_W_NAME); break;
        default:       mark(); break;
    }
}
static void wiz_start_new(void) {
    memset(&M->edit, 0, sizeof M->edit); M->edit.port = 465; s_edit_idx = -1; s_wcustom = false;
    build_wseq(false, false); s_wi = 0; s_psel = 0; s_pscroll = 0; s_wiz = true; wiz_show();
}
static void wiz_start_edit(int idx) {
    nucleo_mailcfg_get_redacted(idx, &M->edit); s_edit_idx = idx; s_wcustom = true;
    build_wseq(true, true); s_wi = 0; s_wiz = true; wiz_show();
}
static void wiz_save(void) {
    if (M->edit.from[0] == 0) strncpy(M->edit.from, M->edit.user, sizeof M->edit.from - 1);
    int r = nucleo_mailcfg_set(s_edit_idx, &M->edit);
    s_wiz = false; s_ed = false; reload();
    set_status(r >= 0 ? "Account salvato" : "Errore salvataggio");
}
static void wiz_next(void) {
    if (s_wseq[s_wi] == WS_CONFIRM) { wiz_save(); return; }
    if (s_wi < s_wn - 1) { s_wi++; wiz_show(); }
}
static void wiz_back(void) {
    s_ed = false;
    if (s_wi > 0) { s_wi--; wiz_show(); }
    else { s_wiz = false; nucleo_app_set_hint("TAB scheda  Esc esci"); mark(); }
}
static void wiz_pick_provider(int i) {
    const smtp_preset_t *p = nucleo_mailcfg_preset(i); if (!p) return;
    strncpy(M->edit.name, p->name, sizeof M->edit.name - 1);
    strncpy(M->edit.host, p->host, sizeof M->edit.host - 1);
    M->edit.port = p->port; M->edit.tls = p->tls;
    s_wcustom = (p->host[0] == 0);
    build_wseq(s_wcustom, false); s_wi = 1; wiz_show();
}
static void editor_commit(void) {
    s_ed = false;
    if (s_edt == ED_W_PORT) { int v = atoi(M->portbuf); M->edit.port = (v > 0 && v < 65536) ? v : 465; }
    if (s_edt == ED_TO || s_edt == ED_SUBJ || s_edt == ED_BODY) { nucleo_app_set_hint("TAB scheda  Esc esci"); mark(); }
    else wiz_next();
}

// ===========================================================================
// drawing
static void draw_header(const char *title) {
    d.fillRect(0, 0, W, HDR, ACC);
    d.fillRect(0, HDR, W, 1, THEME_LINE);
    txt(8, 3, title, 2, WHITE);
    for (int i = 0; i < NTABS; ++i)
        d.fillCircle(W - 14 - (NTABS - 1 - i) * 12, HDR / 2, i == s_tab ? 4 : 2, i == s_tab ? WHITE : BG);
}
static int list_first(int sel, int count, int rows) {
    int first = (sel >= rows) ? (sel - rows + 1) : 0;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    if (sel < first) first = sel;
    return first;
}
static void draw_list(const char *items[], const bool dimmable[], int count, int sel, int top) {
    int rows = (CB - top) / ROW_H;
    int first = list_first(sel, count, rows);
    for (int r = 0; r < rows && first + r < count; ++r) {
        int idx = first + r, y = top + r * ROW_H; bool s = (idx == sel);
        if (s) d.fillRoundRect(4, y + 1, W - 8, ROW_H - 3, 6, ACC);
        char num[4]; if (idx < 9) snprintf(num, sizeof num, "%d", idx + 1); else num[0] = 0;
        if (num[0]) txt(10, y + 6, num, 2, s ? WHITE : MUTED);
        unsigned short fg = s ? WHITE : (dimmable && dimmable[idx] ? C_GREEN : FG);
        txt(30, y + 6, items[idx], 2, fg);
    }
    if (count > rows) {
        int th = CB - top, kh = th * rows / count, ky = top + th * first / count;
        d.fillRect(W - 3, top, 2, th, THEME_LINE);
        d.fillRect(W - 3, ky, 2, kh, ACC);
    }
}
static void draw_dots(int y, int cur, int n) {
    int x0 = (W - (n * 12 - 6)) / 2;
    for (int i = 0; i < n; ++i) d.fillCircle(x0 + i * 12, y, i == cur ? 4 : 2, i <= cur ? ACC : THEME_LINE);
}
static void draw_editor(void) {
    d.fillScreen(BG);
    draw_header(s_edwiz ? "Configura" : "Scrivi");
    if (s_edwiz) draw_dots(HDR + 8, s_wi, s_wn);
    int y = s_edwiz ? HDR + 18 : CT + 2;
    txt(8, y, s_edlabel, 2, MUTED); y += 22;
    char shown[160]; const char *v = s_edbuf ? s_edbuf : "";
    if (s_edsecret) { int n = (int)strlen(v); if (n > 24) n = 24; memset(shown, '*', n); shown[n] = 0; v = shown; }
    int maxc = 18, lh = 22, lines = 0, len = (int)strlen(v), p = 0; char line[22];
    while (p < len && lines < 3) {
        int take = (len - p > maxc) ? maxc : (len - p);
        memcpy(line, v + p, take); line[take] = 0;
        if (p + take >= len && s_blink && take < maxc) { line[take] = '|'; line[take + 1] = 0; }
        txt(8, y + lines * lh, line, 2, FG); p += take; lines++;
    }
    if (len == 0) txt(8, y, s_blink ? "|" : " ", 2, FG);
    // recipient autocomplete: show matching contacts, TAB fills/cycles them
    if (s_edt == ED_TO && s_nrec) {
        int idx[2]; int c = mail_suggest(idx, 2);
        int sy = CT + 2 + 44;
        for (int i = 0; i < c; ++i) {
            bool pick = (i == 0);   // TAB fills the best (first) match
            char b[30]; snprintf(b, sizeof b, "%s %.24s", pick ? ">" : " ", M->rec[idx[i]]);
            txt(8, sy + i * 16, b, 1, pick ? ACC : MUTED);
        }
    }
    const char *foot = (s_edt == ED_W_NAME) ? "Invio = salta/avanti" : "Invio = avanti";
    if (s_edt == ED_TO || s_edt == ED_SUBJ || s_edt == ED_BODY) foot = "Invio = conferma";
    txt(8, CB - 16, foot, 1, MUTED);
}
static void draw_wizard(void) {
    d.fillScreen(BG);
    if (s_wseq[s_wi] == WS_PROV) {
        draw_header("Provider"); draw_dots(HDR + 8, 0, s_wn);
        int np = nucleo_mailcfg_preset_count();
        int top = HDR + 16, rows = (CB - top) / ROW_H, first = list_first(s_psel, np, rows);
        for (int r = 0; r < rows && first + r < np; ++r) {
            int idx = first + r, y = top + r * ROW_H; bool s = (idx == s_psel);
            if (s) d.fillRoundRect(4, y + 1, W - 8, ROW_H - 3, 6, ACC);
            char num[4]; snprintf(num, sizeof num, "%d", idx + 1);
            txt(10, y + 6, num, 2, s ? WHITE : MUTED);
            txt(30, y + 6, nucleo_mailcfg_preset(idx)->name, 2, s ? WHITE : FG);
        }
        if (np > rows) { int th = CB - top, kh = th * rows / np, ky = top + th * first / np;
            d.fillRect(W - 3, top, 2, th, THEME_LINE); d.fillRect(W - 3, ky, 2, kh, ACC); }
        return;
    }
    draw_header("Conferma"); draw_dots(HDR + 8, s_wi, s_wn);
    int y = HDR + 18; char l[64];
    snprintf(l, sizeof l, "%s", M->edit.name[0] ? M->edit.name : M->edit.host); txt(8, y, l, 2, ACC); y += 20;
    snprintf(l, sizeof l, "%.30s", M->edit.user); txt(8, y, l, 2, FG); y += 18;
    snprintf(l, sizeof l, "%s:%d", M->edit.host, M->edit.port); txt(8, y, l, 1, MUTED); y += 14;
    txt(8, y, M->edit.pass[0] ? "Password: impostata" : (s_edit_idx < 0 ? "Password: MANCANTE" : "Password: invariata"),
        1, (M->edit.pass[0] || s_edit_idx >= 0) ? C_GREEN : C_RED);
    d.fillRoundRect(8, CB - 26, W - 16, 22, 6, ACC);
    ctxt(CB - 21, "INVIO = Salva", 2, WHITE);
}
static void draw_acct(void) {
    d.fillScreen(BG); draw_header("Account");
    int n = s_nacct + 1;
    static const char *items[MAIL_MAX_ACCOUNTS + 1]; static bool dim[MAIL_MAX_ACCOUNTS + 1];
    for (int i = 0; i < s_nacct; ++i) {
        snprintf(M->rowbuf[i], 40, "%s%.20s", (i == s_def) ? "* " : "",
                 M->accts[i].name[0] ? M->accts[i].name : M->accts[i].user);
        items[i] = M->rowbuf[i]; dim[i] = false;
    }
    items[s_nacct] = "+ Nuovo account"; dim[s_nacct] = true;
    draw_list(items, dim, n, s_asel, CT);
    if (s_status[0]) txt(8, CB - 13, s_status, 1, C_GREEN);
    else txt(8, CB - 13, "Invio=apri  X=canc  T=default", 1, MUTED);
}
static void draw_topick(void) {
    d.fillScreen(BG); draw_header("Destinatario");
    static const char *items[7]; static bool dim[7];
    items[0] = "+ Nuovo indirizzo"; dim[0] = true;
    for (int i = 0; i < s_nrec; ++i) { items[i + 1] = M->rec[i]; dim[i + 1] = false; }
    draw_list(items, dim, s_nrec + 1, s_tpsel, CT);
}
static void draw_write(void) {
    d.fillScreen(BG); draw_header("Scrivi");
    if (s_sending) { ctxt((CB + HDR) / 2 - 8, "Invio in corso...", 2, ACC); return; }
    static const char *pre[3] = { "A:  ", "Ogg: ", "Txt: " };
    const char *val[3] = { M->to[0] ? M->to : "(scegli)", M->subj[0] ? M->subj : "(vuoto)", M->body[0] ? M->body : "(vuoto)" };
    int y = HDR + 4, rh = 22;
    for (int i = 0; i < 3; ++i) {
        bool s = (s_wfield == i);
        if (s) d.fillRoundRect(4, y, W - 8, rh - 1, 5, ACC);
        char b[30]; snprintf(b, sizeof b, "%s%.21s", pre[i], val[i]);
        for (char *q = b; *q; ++q) if (*q == '\n') *q = ' ';
        txt(8, y + 4, b, 2, s ? WHITE : FG); y += rh;
    }
    bool s = (s_wfield == 3);
    d.fillRoundRect(8, y + 1, W - 16, rh, 6, s ? C_GREEN : THEME_LINE);
    ctxt(y + 5, "INVIA", 2, s ? WHITE : FG); y += rh + 2;
    if (s_status[0]) txt(6, y, s_status, 1, s_send_ok ? C_GREEN : C_RED);
}
static void draw_sent(void) {
    d.fillScreen(BG); draw_header("Inviate");
    FILE *f = fopen(SENT_LOG, "r");
    if (!f) { ctxt((CB + HDR) / 2, "Nessuna inviata", 2, MUTED); return; }
    int n = 0; char buf[120];
    while (fgets(buf, sizeof buf, f)) {
        size_t L = strlen(buf); while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        if (!L) continue;
        strncpy(M->sent[n % 8], buf, 103); M->sent[n % 8][103] = 0; n++;
    }
    fclose(f);
    if (!n) { ctxt((CB + HDR) / 2, "Nessuna inviata", 2, MUTED); return; }
    int show = (n < 4) ? n : 4, y = CT;
    for (int i = 0; i < show; ++i) {
        int idx = (n - 1 - i) % 8; if (idx < 0) idx += 8;
        char *ln = M->sent[idx];
        bool ok = strstr(ln, " | OK | ") != NULL;
        d.fillCircle(12, y + 11, 4, ok ? C_GREEN : C_RED);
        char *to = (char *)"", *subj = (char *)"";
        char *p2 = strstr(ln, " | ");
        if (p2) { char *p3 = strstr(p2 + 3, " | "); if (p3) { to = p3 + 3; char *p4 = strstr(to, " | "); if (p4) { *p4 = 0; subj = p4 + 3; } } }
        char t1[26]; snprintf(t1, sizeof t1, "%.22s", to); txt(24, y + 2, t1, 2, FG);
        char t2[34]; snprintf(t2, sizeof t2, "%.32s", subj); txt(24, y + 18, t2, 1, MUTED);
        y += 30;
    }
}
static void on_draw(void) {
    if (!s_dirty) return;
    s_dirty = false;
    if (!M) { d.fillScreen(BG); ctxt(CB / 2, "Memoria insufficiente", 2, C_RED); return; }
    if (s_ed)          draw_editor();
    else if (s_wiz)    draw_wizard();
    else if (s_topick) draw_topick();
    else if (s_tab == TAB_ACCT)  draw_acct();
    else if (s_tab == TAB_WRITE) draw_write();
    else               draw_sent();
}

// ===========================================================================
// input
static void on_key(int key, char ch) {
    if (!M || s_sending) return;
    if (s_ed) {
        if (key == NK_ENTER) editor_commit();
        else if (key == NK_DEL) { size_t n = strlen(s_edbuf); if (n) { s_edbuf[n - 1] = 0; mark(); } }
        else if (ch >= 0x20) ed_putc(ch);
        return;
    }
    if (s_wiz) {
        if (s_wseq[s_wi] == WS_PROV) {
            int np = nucleo_mailcfg_preset_count();
            if (key == NK_UP && s_psel > 0) { s_psel--; mark(); }
            else if (key == NK_DOWN && s_psel < np - 1) { s_psel++; mark(); }
            else if (key == NK_CHAR && ch >= '1' && ch <= '9' && (ch - '1') < np) wiz_pick_provider(ch - '1');
            else if (key == NK_ENTER || key == NK_RIGHT) wiz_pick_provider(s_psel);
        } else if (key == NK_ENTER || key == NK_RIGHT) wiz_save();
        return;
    }
    if (s_topick) {
        int n = s_nrec + 1;
        if (key == NK_UP && s_tpsel > 0) { s_tpsel--; mark(); }
        else if (key == NK_DOWN && s_tpsel < n - 1) { s_tpsel++; mark(); }
        else if (key == NK_CHAR && ch >= '1' && ch <= '9' && (ch - '0') <= s_nrec) {
            strncpy(M->to, M->rec[ch - '1'], sizeof M->to - 1); s_topick = false; mark();
        } else if (key == NK_ENTER || key == NK_RIGHT) {
            if (s_tpsel == 0) { s_topick = false; editor_open("A", M->to, sizeof M->to, false, false, false, ED_TO); }
            else { strncpy(M->to, M->rec[s_tpsel - 1], sizeof M->to - 1); s_topick = false; mark(); }
        }
        return;
    }
    if (s_tab == TAB_ACCT) {
        int n = s_nacct + 1;
        if (key == NK_UP && s_asel > 0) { s_asel--; mark(); }
        else if (key == NK_DOWN && s_asel < n - 1) { s_asel++; mark(); }
        else if (key == NK_CHAR && ch >= '1' && ch <= '9' && (ch - '1') < n) {
            s_asel = ch - '1'; if (s_asel == s_nacct) wiz_start_new(); else wiz_start_edit(s_asel);
        } else if (key == NK_ENTER || key == NK_RIGHT) {
            if (s_asel == s_nacct) wiz_start_new(); else wiz_start_edit(s_asel);
        } else if (key == NK_CHAR && (ch == 'x' || ch == 'X') && s_asel < s_nacct) {
            nucleo_mailcfg_delete(s_asel); reload(); set_status("Eliminato");
        } else if (key == NK_CHAR && (ch == 't' || ch == 'T') && s_asel < s_nacct) {
            nucleo_mailcfg_set_default(s_asel); reload(); set_status("Default impostato");
        }
        return;
    }
    if (s_tab == TAB_WRITE) {
        if (key == NK_UP && s_wfield > 0) { s_wfield--; mark(); }
        else if (key == NK_DOWN && s_wfield < W_FIELDS - 1) { s_wfield++; mark(); }
        else if (key == NK_ENTER || key == NK_RIGHT) {
            if (s_wfield == 0) { if (s_nrec) { s_topick = true; s_tpsel = 0; mark(); }
                                 else editor_open("A", M->to, sizeof M->to, false, false, false, ED_TO); }
            else if (s_wfield == 1) editor_open("Oggetto", M->subj, sizeof M->subj, false, false, false, ED_SUBJ);
            else if (s_wfield == 2) editor_open("Testo", M->body, sizeof M->body, false, false, false, ED_BODY);
            else start_send();
        }
        return;
    }
}
static void on_tick(void) {
    if (!M) return;
    if (s_sending && s_send_phase == 1) {
        s_send_phase = 2;
        nucleo_app_release_buffers();            // free the 32 KB canvas for the TLS handshake
        nucleo_app_set_direct_draw(true);
        draw_write();                            // "Invio in corso..." direct (canvas gone)
        mail_job_t *job = (mail_job_t *)malloc(sizeof(mail_job_t));
        int def = nucleo_mailcfg_default();
        if (!job || def < 0 || !nucleo_mailcfg_get(def, &job->acc)) {
            free(job); s_sending = false; s_send_phase = 0;
            nucleo_app_set_direct_draw(false); nucleo_screen_acquire(); set_status("Memoria insufficiente"); return;
        }
        strncpy(job->to, M->to, sizeof job->to - 1); job->to[sizeof job->to - 1] = 0;
        strncpy(job->subj, M->subj, sizeof job->subj - 1); job->subj[sizeof job->subj - 1] = 0;
        strncpy(job->body, M->body, sizeof job->body - 1); job->body[sizeof job->body - 1] = 0;
        if (xTaskCreate(smtp_task, "smtpw", 8192, job, 4, NULL) != pdPASS) {
            free(job); s_sending = false; s_send_phase = 0;
            nucleo_app_set_direct_draw(false); nucleo_screen_acquire(); set_status("Memoria insufficiente");
        }
        return;
    }
    if (s_send_done) {
        s_send_done = false; s_send_phase = 0;
        nucleo_app_set_direct_draw(false); nucleo_screen_acquire();
        nucleo_mailcfg_log_sent(M->to, M->subj, s_send_ok);
        if (s_send_ok) { add_recent(M->to); set_status("Inviata!"); M->to[0] = M->subj[0] = M->body[0] = 0; s_wfield = 0; }
        else { char m[64]; snprintf(m, sizeof m, "Errore: %.40s", s_send_err[0] ? s_send_err : "?"); set_status(m); }
    }
    if (s_status[0] && s_status_until && esp_timer_get_time() > s_status_until) { s_status[0] = 0; s_status_until = 0; mark(); }
    if (s_ed) { if (++s_blink_n >= 3) { s_blink_n = 0; s_blink = !s_blink; mark(); } }
}
static void on_tab(void) {
    if (s_ed) {                                   // TAB in the "A" editor = autocomplete to a contact
        if (s_edt == ED_TO && s_nrec) {
            int idx[2]; if (mail_suggest(idx, 1) > 0) {
                strncpy(M->to, M->rec[idx[0]], sizeof M->to - 1); M->to[sizeof M->to - 1] = 0; mark();
            }
        }
        return;
    }
    if (s_wiz || s_topick) return;
    s_tab = (s_tab + 1) % NTABS; s_asel = 0; s_wfield = 0; s_status[0] = 0; mark();
}
static bool on_back(int key) {
    if (s_ed) {
        if (key == NK_LEFT) { ed_putc(','); return true; }
        if (s_edwiz) wiz_back(); else editor_close();
        return true;
    }
    if (s_topick) { s_topick = false; mark(); return true; }
    if (s_wiz) { wiz_back(); return true; }
    if (key == NK_LEFT) { on_tab(); return true; }
    return false;
}
static void on_enter(void) {
    M = (mail_state_t *)calloc(1, sizeof(mail_state_t));   // heap-on-enter, NEVER .bss
    s_tab = TAB_ACCT; s_asel = 0; s_status[0] = 0; s_ed = false; s_wiz = false; s_topick = false;
    s_sending = false; s_send_done = false; s_send_phase = 0; s_wfield = 0;
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(M ? "TAB scheda  Esc esci" : "Memoria insufficiente");
    if (M) { reload(); load_recents(); }
    mark(); nucleo_app_request_draw();
}
static void on_exit(void) {
    // A send may still be running in the worker; it owns its own job and never touches M, so freeing
    // M here is safe. The framework's close safety-net restores exclusive mode + canvas.
    free(M); M = NULL;
}

extern "C" void nucleo_register_mail(void) {
    static const nucleo_app_def_t app = {
        "mail", "Mail", "Communication", "Invia email via Gmail/SMTP",
        'M', C_BLUE,
        on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP
    };
    nucleo_app_register(&app);
}
