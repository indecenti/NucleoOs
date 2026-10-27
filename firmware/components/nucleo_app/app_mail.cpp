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
#include "nucleo_i18n.h"        // TR(it,en): hints follow the system language
#include "nucleo_board.h"       // NUCLEO_SD_MOUNT
#include "nucleo_touch.h"
#include "app_large.h"          // two screens: tabs, lists, fields on the panel; the Cardputer as companion

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SENT_LOG NUCLEO_SD_MOUNT "/system/mail/sent.log"
#define REC_FILE NUCLEO_SD_MOUNT "/system/mail/recents.txt"

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
static const char *s_edlabel = "", *s_edlabel_en = ""; static EdT s_edt = ED_NONE;
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
    mkdir(NUCLEO_SD_MOUNT "/system", 0775); mkdir(NUCLEO_SD_MOUNT "/system/mail", 0775);
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
    if (def < 0 || !nucleo_mailcfg_get(def, &acc)) { set_status(TR("Nessun account", "No account")); return; }
    if (!M->to[0] || !M->body[0]) { set_status(TR("A: e Testo obbligatori", "To and Text required")); return; }
    if (!strchr(M->to, '@') || !strchr(M->to, '.')) { set_status(TR("Indirizzo non valido", "Invalid address")); return; }
    s_sending = true; s_send_done = false; s_send_phase = 1;
    set_status(TR("Invio in corso...", "Sending..."));
}

// ===========================================================================
// editor
static void editor_open(const char *label, const char *label_en, char *buf, size_t cap, bool secret, bool numeric, bool wiz, EdT t) {
    s_ed = true; s_edbuf = buf; s_edcap = cap; s_edsecret = secret; s_ednum = numeric; s_edwiz = wiz;
    s_edlabel = label; s_edlabel_en = label_en; s_edt = t; s_blink = true;
    nucleo_app_set_hint(t == ED_TO ? TR5("scrivi   tab contatto   invio ok", "type   tab contact   enter ok", "escr.   tab contacto   enter ok", "taper   tab contact   entree ok", "tippe   tab kontakt   enter ok")
                                   : TR5("scrivi   invio ok   esc annulla", "type   enter ok   esc cancel", "escribe   enter ok   esc cancel", "taper   entree ok   esc annuler", "tippe   enter ok   esc abbr."));
    mark();
}
static void editor_close(void) {
    s_ed = false; s_edbuf = NULL; s_edt = ED_NONE;
    nucleo_app_set_hint(TR5("tab scheda   esc esci", "tab switch   esc back", "tab pestana   esc volver", "tab onglet   esc retour", "tab reiter   esc zuruck")); mark();
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
        case WS_HOST:  editor_open("Host SMTP", "SMTP host", M->edit.host, sizeof M->edit.host, false, false, true, ED_W_HOST); break;
        case WS_PORT:  snprintf(M->portbuf, sizeof M->portbuf, "%d", M->edit.port);
                       editor_open("Porta", "Port", M->portbuf, sizeof M->portbuf, false, true, true, ED_W_PORT); break;
        case WS_EMAIL: editor_open("Email", "Email", M->edit.user, sizeof M->edit.user, false, false, true, ED_W_EMAIL); break;
        case WS_PASS:  editor_open(s_edit_idx < 0 ? "Password app" : "Password (vuoto=invariata)",
                                   s_edit_idx < 0 ? "App password" : "Password (empty=keep)",
                                   M->edit.pass, sizeof M->edit.pass, true, false, true, ED_W_PASS); break;
        case WS_NAME:  editor_open("Nome mittente", "Sender name", M->edit.from_name, sizeof M->edit.from_name, false, false, true, ED_W_NAME); break;
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
    set_status(r >= 0 ? TR("Account salvato", "Account saved") : TR("Errore salvataggio", "Save error"));
}
static void wiz_next(void) {
    if (s_wseq[s_wi] == WS_CONFIRM) { wiz_save(); return; }
    if (s_wi < s_wn - 1) { s_wi++; wiz_show(); }
}
static void wiz_back(void) {
    s_ed = false;
    if (s_wi > 0) { s_wi--; wiz_show(); }
    else { s_wiz = false; nucleo_app_set_hint(TR5("tab scheda   esc esci", "tab switch   esc back", "tab pestana   esc volver", "tab onglet   esc retour", "tab reiter   esc zuruck")); mark(); }
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
    if (s_edt == ED_TO || s_edt == ED_SUBJ || s_edt == ED_BODY) { nucleo_app_set_hint(TR5("tab scheda   esc esci", "tab switch   esc back", "tab pestana   esc volver", "tab onglet   esc retour", "tab reiter   esc zuruck")); mark(); }
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
    draw_header(s_edwiz ? TR("Configura", "Setup") : TR("Scrivi", "Write"));
    if (s_edwiz) draw_dots(HDR + 8, s_wi, s_wn);
    int y = s_edwiz ? HDR + 18 : CT + 2;
    txt(8, y, TR(s_edlabel, s_edlabel_en), 2, MUTED); y += 22;
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
    const char *foot = (s_edt == ED_W_NAME) ? TR("Invio = salta/avanti", "Enter = skip/next") : TR("Invio = avanti", "Enter = next");
    if (s_edt == ED_TO || s_edt == ED_SUBJ || s_edt == ED_BODY) foot = TR("Invio = conferma", "Enter = confirm");
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
    draw_header(TR("Conferma", "Confirm")); draw_dots(HDR + 8, s_wi, s_wn);
    int y = HDR + 18; char l[64];
    snprintf(l, sizeof l, "%s", M->edit.name[0] ? M->edit.name : M->edit.host); txt(8, y, l, 2, ACC); y += 20;
    snprintf(l, sizeof l, "%.30s", M->edit.user); txt(8, y, l, 2, FG); y += 18;
    snprintf(l, sizeof l, "%s:%d", M->edit.host, M->edit.port); txt(8, y, l, 1, MUTED); y += 14;
    txt(8, y, M->edit.pass[0] ? TR("Password: impostata", "Password: set")
                             : (s_edit_idx < 0 ? TR("Password: MANCANTE", "Password: MISSING") : TR("Password: invariata", "Password: unchanged")),
        1, (M->edit.pass[0] || s_edit_idx >= 0) ? C_GREEN : C_RED);
    d.fillRoundRect(8, CB - 26, W - 16, 22, 6, ACC);
    ctxt(CB - 21, TR("INVIO = Salva", "ENTER = Save"), 2, WHITE);
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
    items[s_nacct] = TR("+ Nuovo account", "+ New account"); dim[s_nacct] = true;
    draw_list(items, dim, n, s_asel, CT);
    if (s_status[0]) txt(8, CB - 13, s_status, 1, C_GREEN);
    else txt(8, CB - 13, TR("Invio=apri  X=canc  T=default", "Enter=open  X=del  T=default"), 1, MUTED);
}
static void draw_topick(void) {
    d.fillScreen(BG); draw_header(TR("Destinatario", "Recipient"));
    static const char *items[7]; static bool dim[7];
    items[0] = TR("+ Nuovo indirizzo", "+ New address"); dim[0] = true;
    for (int i = 0; i < s_nrec; ++i) { items[i + 1] = M->rec[i]; dim[i + 1] = false; }
    draw_list(items, dim, s_nrec + 1, s_tpsel, CT);
}
static void draw_write(void) {
    d.fillScreen(BG); draw_header(TR("Scrivi", "Write"));
    if (s_sending) { ctxt((CB + HDR) / 2 - 8, TR("Invio in corso...", "Sending..."), 2, ACC); return; }
    static const char *pre[3]    = { "A:  ", "Ogg: ", "Txt: " };
    static const char *pre_en[3] = { "To: ", "Sub: ", "Txt: " };
    const char *val[3] = { M->to[0] ? M->to : TR("(scegli)", "(pick)"), M->subj[0] ? M->subj : TR("(vuoto)", "(empty)"), M->body[0] ? M->body : TR("(vuoto)", "(empty)") };
    int y = HDR + 4, rh = 22;
    for (int i = 0; i < 3; ++i) {
        bool s = (s_wfield == i);
        if (s) d.fillRoundRect(4, y, W - 8, rh - 1, 5, ACC);
        char b[30]; snprintf(b, sizeof b, "%s%.21s", TR(pre[i], pre_en[i]), val[i]);
        for (char *q = b; *q; ++q) if (*q == '\n') *q = ' ';
        txt(8, y + 4, b, 2, s ? WHITE : FG); y += rh;
    }
    bool s = (s_wfield == 3);
    d.fillRoundRect(8, y + 1, W - 16, rh, 6, s ? C_GREEN : THEME_LINE);
    ctxt(y + 5, TR("INVIA", "SEND"), 2, s ? WHITE : FG); y += rh + 2;
    if (s_status[0]) txt(6, y, s_status, 1, s_send_ok ? C_GREEN : C_RED);
}
static void draw_sent(void) {
    d.fillScreen(BG); draw_header(TR("Inviate", "Sent"));
    FILE *f = fopen(SENT_LOG, "r");
    if (!f) { ctxt((CB + HDR) / 2, TR("Nessuna inviata", "None sent"), 2, MUTED); return; }
    int n = 0; char buf[120];
    while (fgets(buf, sizeof buf, f)) {
        size_t L = strlen(buf); while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        if (!L) continue;
        strncpy(M->sent[n % 8], buf, 103); M->sent[n % 8][103] = 0; n++;
    }
    fclose(f);
    if (!n) { ctxt((CB + HDR) / 2, TR("Nessuna inviata", "None sent"), 2, MUTED); return; }
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
static void draw_companion(void);
static void on_draw(void) {
    if (!s_dirty) return;
    s_dirty = false;
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    if (!M) { d.fillScreen(BG); ctxt(CB / 2, TR("Memoria insufficiente", "Out of memory"), 2, C_RED); return; }
    if (s_ed)          draw_editor();
    else if (s_wiz)    draw_wizard();
    else if (s_topick) draw_topick();
    else if (s_tab == TAB_ACCT)  draw_acct();
    else if (s_tab == TAB_WRITE) draw_write();
    else               draw_sent();
}

// ===========================================================================
// input
static bool lg_key(int key, char ch);
static void on_key(int key, char ch) {
    if (!M || s_sending) return;
    if (lg_key(key, ch)) return;                  // the panel's taps (two screens)
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
            if (s_tpsel == 0) { s_topick = false; editor_open("A", "To", M->to, sizeof M->to, false, false, false, ED_TO); }
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
            nucleo_mailcfg_delete(s_asel); reload(); set_status(TR("Eliminato", "Deleted"));
        } else if (key == NK_CHAR && (ch == 't' || ch == 'T') && s_asel < s_nacct) {
            nucleo_mailcfg_set_default(s_asel); reload(); set_status(TR("Default impostato", "Default set"));
        }
        return;
    }
    if (s_tab == TAB_WRITE) {
        if (key == NK_UP && s_wfield > 0) { s_wfield--; mark(); }
        else if (key == NK_DOWN && s_wfield < W_FIELDS - 1) { s_wfield++; mark(); }
        else if (key == NK_ENTER || key == NK_RIGHT) {
            if (s_wfield == 0) { if (s_nrec) { s_topick = true; s_tpsel = 0; mark(); }
                                 else editor_open("A", "To", M->to, sizeof M->to, false, false, false, ED_TO); }
            else if (s_wfield == 1) editor_open("Oggetto", "Subject", M->subj, sizeof M->subj, false, false, false, ED_SUBJ);
            else if (s_wfield == 2) editor_open("Testo", "Text", M->body, sizeof M->body, false, false, false, ED_BODY);
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
            nucleo_app_set_direct_draw(false); nucleo_screen_acquire(); set_status(TR("Memoria insufficiente", "Out of memory")); return;
        }
        strncpy(job->to, M->to, sizeof job->to - 1); job->to[sizeof job->to - 1] = 0;
        strncpy(job->subj, M->subj, sizeof job->subj - 1); job->subj[sizeof job->subj - 1] = 0;
        strncpy(job->body, M->body, sizeof job->body - 1); job->body[sizeof job->body - 1] = 0;
        if (xTaskCreate(smtp_task, "smtpw", 8192, job, 4, NULL) != pdPASS) {
            free(job); s_sending = false; s_send_phase = 0;
            nucleo_app_set_direct_draw(false); nucleo_screen_acquire(); set_status(TR("Memoria insufficiente", "Out of memory"));
        }
        return;
    }
    if (s_send_done) {
        s_send_done = false; s_send_phase = 0;
        nucleo_app_set_direct_draw(false); nucleo_screen_acquire();
        nucleo_mailcfg_log_sent(M->to, M->subj, s_send_ok);
        if (s_send_ok) { add_recent(M->to); set_status(TR("Inviata!", "Sent!")); M->to[0] = M->subj[0] = M->body[0] = 0; s_wfield = 0; }
        else { char m[64]; snprintf(m, sizeof m, TR("Errore: %.40s", "Error: %.40s"), s_send_err[0] ? s_send_err : "?"); set_status(m); }
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
    nucleo_app_set_hint(M ? TR5("tab scheda   esc esci", "tab switch   esc back", "tab pestana   esc volver", "tab onglet   esc retour", "tab reiter   esc zuruck")
                          : TR5("memoria insufficiente", "out of memory", "memoria insuf.", "memoire insuf.", "speicher unzur."));
    if (M) { reload(); load_recents(); }
    mark(); nucleo_app_request_draw();
}
static void on_exit(void) {
    // A send may still be running in the worker; it owns its own job and never touches M, so freeing
    // M here is safe. The framework's close safety-net restores exclusive mode + canvas.
    free(M); M = NULL;
}


// ===========================================================================
// two screens
// The panel carries the mail app laid out for touch: Account / Write / Sent as tabs under the header; the
// accounts as a list with the picked one's card (Edit / Default / Delete); the message as three tappable fields
// and a Send button; the sent log as a list; the editors (fields, setup wizard) as one large field with Cancel /
// OK and, for the recipient, the matching contacts to tap. The typing stays on the Cardputer, whose companion
// shows the field being typed in large type, or what is picked, and the keys.
// Panel-only actions arrive as NK_CHAR with private control codes (below 0x10: never typing, never list rows).
enum { LGK_TAB0 = 0x01 /* ..0x03 */, LGK_FIELD0 = 0x04 /* ..0x07: To, Subject, Text, Send */, LGK_SUGG0 = 0x0A /* ..0x0C */ };
static int s_lg_sscroll, s_lg_ssel;             // the Sent list (the other lists reuse the app's own scrolls)

static int lg_sent_load(void)                     // the sent log into M->sent (ring of the newest 8); returns the count
{
    FILE *f = fopen(SENT_LOG, "r");
    if (!f) return 0;
    int n = 0; char buf[120];
    while (fgets(buf, sizeof buf, f)) {
        size_t L = strlen(buf); while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        if (!L) continue;
        strncpy(M->sent[n % 8], buf, 103); M->sent[n % 8][103] = 0; n++;
    }
    fclose(f);
    return n;
}
// One sent-log line split in place of a copy: "time | OK | to | subject".
static void lg_sent_split(const char *ln, char *ts, char *to, char *subj, bool *ok)
{
    char b[104]; snprintf(b, sizeof b, "%s", ln);
    ts[0] = to[0] = subj[0] = 0; *ok = strstr(b, " | OK | ") != NULL;
    char *p2 = strstr(b, " | ");
    if (!p2) { snprintf(subj, 64, "%s", b); return; }
    *p2 = 0; snprintf(ts, 20, "%s", b);
    char *p3 = strstr(p2 + 3, " | ");
    if (!p3) return;
    char *t = p3 + 3, *p4 = strstr(t, " | ");
    if (p4) { *p4 = 0; snprintf(subj, 64, "%s", p4 + 3); }
    snprintf(to, 64, "%s", t);
}

static bool lg_key(int key, char ch)
{
    if (!M || s_sending) return false;
    if (nucleo_touch_is_row(key, ch)) {                                   // a tapped list row: pick it, again = open
        int r = 0;
        if (s_ed) return false;
        if (s_wiz && s_wseq[s_wi] == WS_PROV) { r = lg::row_tap(key, ch, s_pscroll, nucleo_mailcfg_preset_count(), &s_psel); if (r == 2) wiz_pick_provider(s_psel); }
        else if (s_wiz) return false;
        else if (s_topick) { r = lg::row_tap(key, ch, s_tpscroll, s_nrec + 1, &s_tpsel); if (r == 2) { on_key(NK_ENTER, 0); return true; } }
        else if (s_tab == TAB_ACCT) { r = lg::row_tap(key, ch, s_ascroll, s_nacct + 1, &s_asel); if (r == 2) { on_key(NK_ENTER, 0); return true; } }
        else if (s_tab == TAB_SENT) { int n = lg_sent_load(); if (n > 8) n = 8; r = lg::row_tap(key, ch, s_lg_sscroll, n, &s_lg_ssel); }
        mark();
        return true;
    }
    if (key != NK_CHAR || ch <= 0 || ch >= NUCLEO_TOUCH_ROW) return false;
    if (s_ed) {
        if (ch >= LGK_SUGG0 && ch < LGK_SUGG0 + 3 && s_edt == ED_TO) {
            int idx[3]; const int c = mail_suggest(idx, 3), i = ch - LGK_SUGG0;
            if (i < c) { strncpy(M->to, M->rec[idx[i]], sizeof M->to - 1); M->to[sizeof M->to - 1] = 0; mark(); }
            return true;
        }
        return false;
    }
    if (s_wiz || s_topick) return false;
    if (ch >= LGK_TAB0 && ch < LGK_TAB0 + NTABS) { s_tab = ch - LGK_TAB0; s_asel = 0; s_wfield = 0; s_status[0] = 0; mark(); return true; }
    if (s_tab == TAB_WRITE && ch >= LGK_FIELD0 && ch < LGK_FIELD0 + W_FIELDS) { s_wfield = ch - LGK_FIELD0; on_key(NK_ENTER, 0); return true; }
    return false;
}

static const char *lg_acct_name(int i) { return M->accts[i].name[0] ? M->accts[i].name : M->accts[i].user; }

static void lg_acct_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    if (i >= s_nacct) { lg::row_text(x, y, w, h, focus, bg, TR("+ Nuovo account", "+ New account"), nullptr, C_GREEN); return; }
    lg::row_text(x, y, w, h, focus, bg, lg_acct_name(i), i == s_def ? "*" : nullptr, FG);
}
static void lg_prov_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    lg::row_text(x, y, w, h, focus, bg, nucleo_mailcfg_preset(i)->name, nullptr, FG);
}
static void lg_pick_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    if (i == 0) { lg::row_text(x, y, w, h, focus, bg, TR("+ Nuovo indirizzo", "+ New address"), nullptr, C_GREEN); return; }
    lg::row_text(x, y, w, h, focus, bg, M->rec[i - 1], nullptr, FG);
}
static void lg_sent_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *ud)
{
    const int n = *(const int *)ud;
    int idx = (n - 1 - i) % 8; if (idx < 0) idx += 8;                // newest first
    char ts[20], to[64], subj[64]; bool ok;
    lg_sent_split(M->sent[idx], ts, to, subj, &ok);
    d.fillCircle(x + 12, y + 13, 4, focus ? INK : (ok ? C_GREEN : C_RED));
    char t[64];
    lg::font(0); d.setTextDatum(top_right); d.setTextColor(focus ? INK : MUTED, bg);
    const char *hm = strlen(ts) > 11 ? ts + 5 : ts;                     // "MM-DD HH:MM"
    d.drawString(hm, x + w - 8, y + 5);
    const int rw = d.textWidth(hm) + 12;
    d.setTextDatum(top_left); d.setTextColor(focus ? INK : FG, bg);
    lg::fit(t, sizeof t, to, w - 28 - rw); d.drawString(t, x + 24, y + 5);
    d.setTextColor(focus ? INK : MUTED, bg);
    lg::fit(t, sizeof t, ok ? subj : TR("non inviata", "not sent"), w - 32); d.drawString(t, x + 24, y + 23);
}

// The open editor: its label, the value in a large box, the contacts that match (recipient), Cancel / OK.
static void lg_editor(void)
{
    const int W = d.width(), H = d.height();
    char step[12] = "";
    if (s_edwiz) snprintf(step, sizeof step, "%d / %d", s_wi + 1, s_wn);
    lg::header(s_edwiz ? TR("Configura account", "Account setup") : TR("Scrivi", "Write"), step, ACC, NK_BACK);
    d.fillRect(0, lg::HEAD, W, H - lg::HEAD, BG);
    lg::font(1); d.setTextColor(FG, BG); d.drawString(TR(s_edlabel, s_edlabel_en), 14, lg::HEAD + 8);
    const bool body = s_edt == ED_BODY;
    const int by = lg::HEAD + 32, bh = body ? H - by - 56 : 44;
    d.fillRoundRect(10, by, W - 20, bh, 8, INK); d.drawRoundRect(10, by, W - 20, bh, 8, ACC);
    const char *v = s_edbuf ? s_edbuf : "";
    if (body) {                                                          // the text: wrapped, its end in view (no copy)
        lg::font(0); d.setTextColor(FG, INK);
        const int lines = (bh - 12) / 18;
        const char *t = v;
        while (strlen(t) > (size_t)(lines * 30)) { const char *nl = strpbrk(t, " \n"); if (!nl) break; t = nl + 1; }
        if (!*t) { d.setTextColor(DIM, INK); d.drawString(TR("scrivi sul Cardputer...", "type on the Cardputer..."), 20, by + 8); }
        else lg::wrap(t, 20, by + 8, W - 40, 18, lines);
    } else {
        char shown[128];
        if (s_edsecret) { int n = (int)strlen(v); if (n > 40) n = 40; memset(shown, '*', n); shown[n] = 0; }
        else snprintf(shown, sizeof shown, "%s", v);
        lg::font(2); d.setTextDatum(middle_left); d.setTextColor(FG, INK);
        if (d.textWidth(shown) > W - 56) lg::font(1);                   // a long address: the smaller bold face first
        const char *t = shown; while (*t && d.textWidth(t) > W - 56) t++;
        d.drawString(t, 20, by + bh / 2 + 1);
        d.fillRect(20 + d.textWidth(t) + 2, by + 10, 3, bh - 20, ACC);
        d.setTextDatum(top_left);
    }
    if (s_edt != ED_TO && !body) {
        lg::font(0); d.setTextColor(MUTED, BG);
        lg::wrap(s_edt == ED_W_PASS ? TR("La password per app del provider (non quella normale): la scrivi sul Cardputer.",
                                         "The provider's app password (not the normal one): type it on the Cardputer.")
                                    : TR("Scrivi sulla tastiera del Cardputer.", "Type on the Cardputer's keyboard."), 14, by + bh + 10, W - 28, 17, 3);
    }
    if (s_edt == ED_TO && s_nrec) {                                      // the matching contacts: tap = fill it
        int idx[3]; const int c = mail_suggest(idx, 3);
        for (int i = 0; i < c; i++) {
            const int ry = by + bh + 6 + i * 26;
            if (ry + 24 > H - 50) break;
            d.fillRoundRect(10, ry, W - 20, 24, 7, lg::card());
            lg::row_text(10, ry, W - 20, 24, false, lg::card(), M->rec[idx[i]], i == 0 ? "TAB" : nullptr, i == 0 ? ACC : FG);
            nucleo_touch_add(10, ry, W - 20, 24, NK_CHAR, (char)(LGK_SUGG0 + i));
        }
    }
    const int bw = (W - 30) / 2;
    const char *ok = s_edwiz ? TR("Avanti", "Next") : "OK";
    lg::button(10, H - 46, bw, 38, s_edwiz && s_wi > 0 ? TR("Indietro", "Back") : TR("Annulla", "Cancel"), LINE, FG, NK_BACK, 0);
    lg::button(20 + bw, H - 46, bw, 38, ok, ACC, INK, NK_ENTER, 0);
}

static void lg_wizard(void)
{
    const int W = d.width(), H = d.height();
    char step[12]; snprintf(step, sizeof step, "%d / %d", s_wi + 1, s_wn);
    if (s_wseq[s_wi] == WS_PROV) {                                       // the provider: list + its card
        lg::header(TR("Scegli il provider", "Pick the provider"), step, ACC, NK_BACK);
        const int LW = 150, y = lg::HEAD + 2;
        lg::list(0, y, LW, H - y - 2, lg::ROW, nucleo_mailcfg_preset_count(), s_psel, &s_pscroll, lg_prov_row, nullptr, ACC);
        d.fillRect(LW, lg::HEAD, W - LW, H - lg::HEAD, BG);
        const smtp_preset_t *p = nucleo_mailcfg_preset(s_psel);
        const int cx = LW + 4, cw = W - LW - 10, cy = y + 4, ch = H - cy - 8;
        lg::panel(cx, cy, cw, ch);
        const unsigned short cb = lg::card();
        lg::font(1); d.setTextColor(ACC, cb); d.drawString(p ? p->name : "", cx + 10, cy + 10);
        lg::font(0); d.setTextColor(MUTED, cb);
        if (p && p->host[0]) { char h[64]; lg::fit(h, sizeof h, p->host, cw - 20); d.drawString(h, cx + 10, cy + 36); }
        d.setTextColor(FG, cb);
        if (p) lg::wrap(p->hint, cx + 10, cy + 60, cw - 20, 18, 4);
        lg::button(cx + 8, cy + ch - 40, cw - 16, 32, TR("Scegli", "Choose"), ACC, INK, NK_ENTER, 0);
        return;
    }
    lg::header(TR("Conferma account", "Confirm account"), step, ACC, NK_BACK);   // WS_CONFIRM
    d.fillRect(0, lg::HEAD, W, H - lg::HEAD, BG);
    const int cy = lg::HEAD + 6, ch = H - cy - 56;
    lg::panel(10, cy, W - 20, ch);
    const unsigned short cb = lg::card();
    lg::font(1); d.setTextColor(ACC, cb); d.drawString(M->edit.name[0] ? M->edit.name : M->edit.host, 22, cy + 10);
    char l[80]; int ly = cy + 40;
    lg::kv(22, ly, W - 44, "Email", M->edit.user, FG, cb); ly += 24;
    snprintf(l, sizeof l, "%s:%d", M->edit.host, M->edit.port);
    lg::kv(22, ly, W - 44, "Server", l, FG, cb); ly += 24;
    lg::kv(22, ly, W - 44, TR("Mittente", "Sender"), M->edit.from_name[0] ? M->edit.from_name : "-", FG, cb); ly += 24;
    const bool pw_ok = M->edit.pass[0] || s_edit_idx >= 0;
    lg::kv(22, ly, W - 44, "Password", M->edit.pass[0] ? TR("impostata", "set") : (s_edit_idx < 0 ? TR("MANCANTE", "MISSING") : TR("invariata", "unchanged")),
           pw_ok ? C_GREEN : C_RED, cb);
    const int bw = (W - 30) / 2;
    lg::button(10, H - 46, bw, 38, TR("Indietro", "Back"), LINE, FG, NK_BACK, 0);
    lg::button(20 + bw, H - 46, bw, 38, TR("Salva", "Save"), ACC, INK, NK_ENTER, 0);
}

static void lg_field(int y, int h, int i, const char *label, const char *val, bool empty)
{
    const int W = d.width();
    const bool f = s_wfield == i;
    lg::panel(8, y, W - 16, h);
    const unsigned short cb = lg::card();
    if (f) { d.drawRoundRect(8, y, W - 16, h, 8, ACC); d.drawRoundRect(9, y + 1, W - 18, h - 2, 7, ACC); }
    lg::font(0); d.setTextColor(f ? ACC : MUTED, cb); d.drawString(label, 18, y + 8);
    d.setTextColor(empty ? DIM : FG, cb);
    if (i == 2) lg::wrap(val, 84, y + 8, W - 108, 17, (h - 8) / 17);
    else { char t[128]; lg::fit(t, sizeof t, val, W - 108); d.drawString(t, 84, y + 8); }
    nucleo_touch_add(8, y, W - 16, h, NK_CHAR, (char)(LGK_FIELD0 + i));
}

static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    if (!M) {
        lg::header("Mail", nullptr, ACC, NK_BACK);
        lg::message(0, lg::HEAD, W, H - lg::HEAD, TR("Memoria insufficiente", "Out of memory"), TR("Chiudi qualche app e riprova", "Close some apps and retry"), C_RED);
        nucleo_touch_commit(); return;
    }
    if (s_ed)       { lg_editor(); nucleo_touch_commit(); return; }
    if (s_wiz)      { lg_wizard(); nucleo_touch_commit(); return; }
    if (s_topick) {
        lg::header(TR("Destinatario", "Recipient"), nullptr, ACC, NK_BACK);
        lg::list(0, lg::HEAD + 2, W, H - lg::HEAD - 2, lg::ROW + 4, s_nrec + 1, s_tpsel, &s_tpscroll, lg_pick_row, nullptr, ACC);
        nucleo_touch_commit(); return;
    }
    const char *from = s_def >= 0 && s_def < s_nacct ? M->accts[s_def].user : TR("nessun account", "no account");
    lg::header("Mail", from, ACC, NK_BACK);
    static const char *names_it[NTABS] = { "Account", "Scrivi", "Inviate" }, *names_en[NTABS] = { "Account", "Write", "Sent" };
    const char *names[NTABS]; for (int i = 0; i < NTABS; i++) names[i] = TR(names_it[i], names_en[i]);
    static const char keys[NTABS] = { LGK_TAB0, LGK_TAB0 + 1, LGK_TAB0 + 2 };
    const int y = lg::tabs(lg::HEAD + 2, names, NTABS, s_tab, ACC, keys);
    const int bh = H - y;
    if (s_tab == TAB_ACCT) {
        const int LW = 140;
        lg::list(0, y, LW, bh - 2, lg::ROW, s_nacct + 1, s_asel, &s_ascroll, lg_acct_row, nullptr, ACC);
        d.fillRect(LW, y, W - LW, bh, BG);
        const int cx = LW + 4, cw = W - LW - 10, ch = bh - 6;
        lg::panel(cx, y, cw, ch);
        const unsigned short cb = lg::card();
        if (s_asel >= s_nacct) {                                          // "+ New account": what a tap does
            lg::font(1); d.setTextColor(C_GREEN, cb); d.drawString(TR("Nuovo account", "New account"), cx + 10, y + 10);
            lg::font(0); d.setTextColor(MUTED, cb);
            lg::wrap(TR("Scegli il provider, poi email e password per app: le scrivi sul Cardputer.",
                        "Pick the provider, then the email and app password: you type them on the Cardputer."), cx + 10, y + 36, cw - 20, 17, 5);
            lg::button(cx + 8, y + ch - 40, cw - 16, 32, TR("Configura", "Set up"), ACC, INK, NK_ENTER, 0);
        } else {
            const smtp_account_t *a = &M->accts[s_asel];
            lg::font(1); d.setTextColor(ACC, cb); char t[48]; lg::fit(t, sizeof t, lg_acct_name(s_asel), cw - 20); d.drawString(t, cx + 10, y + 8);
            lg::font(0); d.setTextColor(FG, cb); lg::fit(t, sizeof t, a->user, cw - 20); d.drawString(t, cx + 10, y + 32);
            char hp[80]; snprintf(hp, sizeof hp, "%s:%d", a->host, a->port);
            d.setTextColor(MUTED, cb); lg::fit(t, sizeof t, hp, cw - 20); d.drawString(t, cx + 10, y + 50);
            d.setTextColor(s_asel == s_def ? C_GREEN : DIM, cb);
            d.drawString(s_asel == s_def ? TR("* predefinito", "* default") : TR("non predefinito", "not default"), cx + 10, y + 68);
            const int b3 = (cw - 24) / 2, by = y + ch - 76;
            lg::button(cx + 8, by, cw - 16, 32, TR("Modifica", "Edit"), ACC, INK, NK_ENTER, 0);
            lg::button(cx + 8, by + 38, b3, 30, TR("Predef.", "Default"), LINE, s_asel == s_def ? DIM : FG, NK_CHAR, 't');
            lg::button(cx + 16 + b3, by + 38, b3, 30, TR("Elimina", "Delete"), LINE, C_RED, NK_CHAR, 'x');
        }
        if (s_status[0]) { lg::font(0); d.setTextColor(C_GREEN, BG); char t[64]; lg::fit(t, sizeof t, s_status, LW - 12); d.drawString(t, 8, H - 18); }
    } else if (s_tab == TAB_WRITE) {
        d.fillRect(0, y, W, bh, BG);
        if (s_sending) lg::message(0, y, W, bh, TR("Invio in corso...", "Sending..."), M->to, ACC);
        else {
            lg_field(y + 2, 30, 0, TR("A", "To"), M->to[0] ? M->to : TR("tocca per scegliere", "tap to pick"), !M->to[0]);
            lg_field(y + 36, 30, 1, TR("Oggetto", "Subject"), M->subj[0] ? M->subj : TR("(vuoto)", "(empty)"), !M->subj[0]);
            lg_field(y + 70, bh - 112, 2, TR("Testo", "Text"), M->body[0] ? M->body : TR("tocca per scrivere", "tap to write"), !M->body[0]);
            const int sy = H - 38;
            if (s_status[0]) { lg::font(0); d.setTextColor(s_send_ok ? C_GREEN : C_RED, BG); char t[64]; lg::fit(t, sizeof t, s_status, W - 150); d.drawString(t, 10, sy + 9); }
            const bool ready = M->to[0] && M->body[0];
            lg::button(W - 130, sy, 120, 34, TR("Invia", "Send"), ready ? C_GREEN : LINE, ready ? INK : MUTED, NK_CHAR, (char)(LGK_FIELD0 + 3));
            if (s_wfield == 3) d.drawRoundRect(W - 132, sy - 2, 124, 38, 10, ACC);
        }
    } else {
        int n = lg_sent_load(); if (n > 8) n = 8;
        if (!n) lg::message(0, y, W, bh, TR("Nessuna inviata", "None sent"), TR("Le mail inviate da qui compaiono in questa lista.", "Mail sent from here shows up in this list."), MUTED);
        else { if (s_lg_ssel >= n) s_lg_ssel = n - 1; lg::list(0, y, W, bh, 42, n, s_lg_ssel, &s_lg_sscroll, lg_sent_row, &n, ACC); }
    }
    nucleo_touch_commit();
}

// The Cardputer while the panel shows the mail: the field being typed in large type, or what is picked.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("MAIL", ACC);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (!M) { d.setTextColor(C_RED, BG); d.drawString(TR("Memoria insufficiente", "Out of memory"), 8, y + 4); return; }
    if (s_ed) {                                                          // the line being typed, large
        d.setTextColor(MUTED, BG); d.drawString(TR(s_edlabel, s_edlabel_en), 8, y + 2);
        char shown[64]; const char *v = s_edbuf ? s_edbuf : "";
        if (s_edsecret) { int n = (int)strlen(v); if (n > 17) n = 17; memset(shown, '*', n); shown[n] = 0; v = shown; }
        const char *line = v; for (const char *c = v; *c; c++) if (*c == '\n') line = c + 1;
        const int n = (int)strlen(line);
        const char *tail = n > 17 ? line + n - 17 : line;
        d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(8, y + 20); d.print(tail);
        d.fillRect(8 + (int)strlen(tail) * 12, y + 18, 3, 18, ACC); d.setTextSize(1);
        if (s_edt == ED_TO && s_nrec) { int idx[1]; if (mail_suggest(idx, 1)) { char s[48]; snprintf(s, sizeof s, "TAB > %.38s", M->rec[idx[0]]); d.setTextColor(ACC, BG); d.drawString(s, 8, y + 46); } }
        char m[40]; snprintf(m, sizeof m, TR("%d caratteri", "%d characters"), (int)strlen(s_edbuf ? s_edbuf : "")); d.setTextColor(MUTED, BG); d.drawString(m, 8, y + 62);
        lg::comp_keys(TR("scrivi  INVIO ok  ESC annulla", "type  ENTER ok  ESC cancel"));
        return;
    }
    if (s_sending) { d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(ACC, BG); d.drawString(TR("Invio in corso...", "Sending..."), 8, y + 6); return; }
    d.setFont(&fonts::FreeSansBold9pt7b);
    if (s_wiz) {
        const smtp_preset_t *p = nucleo_mailcfg_preset(s_psel);
        d.setTextColor(FG, BG); d.drawString(s_wseq[s_wi] == WS_PROV ? (p ? p->name : "") : TR("Conferma account", "Confirm account"), 8, y + 6);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        char m[48]; snprintf(m, sizeof m, TR("Configura account  %d di %d", "Account setup  %d of %d"), s_wi + 1, s_wn); d.drawString(m, 8, y + 32);
        lg::comp_keys(TR("su/giu  INVIO avanti  ESC indietro", "up/dn  ENTER next  ESC back"));
        return;
    }
    if (s_topick) {
        d.setTextColor(FG, BG); lg::wrap(s_tpsel == 0 ? TR("+ Nuovo indirizzo", "+ New address") : M->rec[s_tpsel - 1], 8, y + 6, 224, 20, 2);
        lg::comp_keys(TR("su/giu  1-6 scegli  INVIO ok  ESC", "up/dn  1-6 pick  ENTER ok  ESC"));
        return;
    }
    if (s_tab == TAB_ACCT) {
        d.setTextColor(s_asel >= s_nacct ? C_GREEN : FG, BG);
        d.drawString(s_asel >= s_nacct ? TR("+ Nuovo account", "+ New account") : lg_acct_name(s_asel), 8, y + 6);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        if (s_asel < s_nacct) d.drawString(M->accts[s_asel].user, 8, y + 30);
        if (s_status[0]) { d.setTextColor(C_GREEN, BG); d.drawString(s_status, 8, y + 46); }
        lg::comp_keys(TR("INVIO apri  x elim  t predef  TAB", "ENTER open  x del  t default  TAB"));
    } else if (s_tab == TAB_WRITE) {
        static const char *fit[W_FIELDS] = { "A", "Oggetto", "Testo", "Invia" }, *fen[W_FIELDS] = { "To", "Subject", "Text", "Send" };
        char fl[32]; snprintf(fl, sizeof fl, "> %s", TR(fit[s_wfield], fen[s_wfield]));
        d.setTextColor(ACC, BG); d.drawString(fl, 8, y + 4);
        d.setFont(&fonts::Font0);
        char l[60];
        snprintf(l, sizeof l, "%s %.34s", TR("A:", "To:"), M->to[0] ? M->to : "-"); d.setTextColor(FG, BG); d.drawString(l, 8, y + 28);
        snprintf(l, sizeof l, "%s %.30s", TR("Ogg:", "Subj:"), M->subj[0] ? M->subj : "-"); d.drawString(l, 8, y + 40);
        snprintf(l, sizeof l, TR("Testo: %d caratteri", "Text: %d characters"), (int)strlen(M->body)); d.setTextColor(MUTED, BG); d.drawString(l, 8, y + 52);
        if (s_status[0]) { d.setTextColor(s_send_ok ? C_GREEN : C_RED, BG); d.drawString(s_status, 8, y + 66); }
        lg::comp_keys(TR("su/giu campo  INVIO modifica  TAB", "up/dn field  ENTER edit  TAB"));
    } else {
        const int n = lg_sent_load();
        char m[40]; snprintf(m, sizeof m, TR("%d inviate", "%d sent"), n);
        d.setTextColor(FG, BG); d.drawString(m, 8, y + 6);
        if (n > 0) {                                                       // the newest one
            char ts[20], to[64], subj[64]; bool ok;
            lg_sent_split(M->sent[(n - 1) % 8], ts, to, subj, &ok);
            d.setFont(&fonts::Font0);
            char l[64]; snprintf(l, sizeof l, "%s  %s", ts, ok ? "OK" : TR("ERRORE", "ERROR"));
            d.setTextColor(ok ? C_GREEN : C_RED, BG); d.drawString(l, 8, y + 30);
            snprintf(l, sizeof l, "%.38s", to); d.setTextColor(FG, BG); d.drawString(l, 8, y + 44);
            snprintf(l, sizeof l, "%.38s", subj); d.setTextColor(MUTED, BG); d.drawString(l, 8, y + 56);
        }
        lg::comp_keys(TR("TAB scheda  ESC esci", "TAB tab  ESC exit"));
    }
}

extern "C" void nucleo_register_mail(void) {
    static const nucleo_app_def_t app = {
        "mail", "Mail", "Messaging", "Invia email via Gmail/SMTP",
        'M', C_BLUE,
        on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
