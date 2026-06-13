// app_ir.cpp — native on-device IR remote for NucleoOS (Wear-OS-style launcher app).
//
// Three views (TAB cycles): Remotes (browse the SD preset catalog, drill into a device, send a
// button), TV-B-Gone (sweep the catalog's power codes by region) and Favourites (the SAME ⭐ the
// web app saved to /sd/data/ir/userdata.json — so a favourite starred in the browser is one tap
// away on the device). The big catalog + user data live on the SD card and are loaded ON DEMAND
// when the app opens and freed on exit — they never sit in firmware RAM, honouring the rule that a
// backgrounded native app must cost nothing while the web UI is the active surface.
//
// Drawing goes straight through the `d` macro (app_gfx.h), tick-based and non-blocking: the
// TV-B-Gone sweep sends one code per on_tick() so the UI stays responsive.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include "nucleo_ir.h"
#include "nucleo_ir_proto.h"
#include "nucleo_board.h"   // NUCLEO_SD_MOUNT
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRESETS_PATH NUCLEO_SD_MOUNT "/system/ir/presets.json"
#define UDATA_PATH   NUCLEO_SD_MOUNT "/data/ir/userdata.json"

// ---- palette (RGB565) ----
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, LINE = 0x2945,
                            ACC = 0x2EDA, PWR = 0xF9A6, GRN = 0x8FF3, STAR = 0xFE60, SELBG = 0x1A8B;

// ---- bounded model (malloc'd on enter, freed on exit) ----
#define MAX_REMOTES 16
#define MAX_BTN     18
#define MAX_TVP     48
#define MAX_FAV     24

typedef struct { char key[14]; uint32_t cmd; } ir_btn_t;
typedef struct { char name[28]; nir_proto_t proto; uint32_t addr; int nbtn; ir_btn_t btn[MAX_BTN]; } ir_remote_t;
typedef struct { char brand[20]; char region[8]; nir_proto_t proto; uint32_t addr, cmd; } ir_tvp_t;
typedef struct { char label[30]; nir_proto_t proto; uint32_t addr, cmd; } ir_fav_t;

static ir_remote_t *s_rem;  static int s_nrem;
static ir_tvp_t    *s_tvp;  static int s_ntvp;
static ir_fav_t    *s_fav;  static int s_nfav;

// ---- UI state ----
static int  s_view;            // 0 = Remotes, 1 = TV-B-Gone, 2 = Favourites
static int  s_level;           // remotes: 0 = device list, 1 = button list
static int  s_sel;             // selection in the current list
static int  s_dev;             // chosen device index (level 1)
static int  s_region;          // 0=all 1=us 2=eu 3=asia
static char s_status[40];

static bool s_sweep; static int s_sweep_i, s_sweep_n; static int s_sweep_idx[MAX_TVP];

static const char *const REGIONS[] = { "all", "us", "eu", "asia" };
#define NREGION 4
static const char *const VIEW_TITLE[] = { "IR \xB7 Remotes", "IR \xB7 TV-B-Gone", "IR \xB7 Favourites" };

// ---- load helpers ----
static char *slurp(const char *path, long cap) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > cap) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1); if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, sz, f); fclose(f); buf[got] = 0; return buf;
}

static void load_catalog(void) {
    // 96 KB cap: enter() now releases the 32 KB canvas before this load, so the slurp has headroom;
    // keep the original ceiling so large imported .ir catalogs (Flipper) aren't truncated mid-JSON.
    char *buf = slurp(PRESETS_PATH, 96 * 1024); if (!buf) return;
    cJSON *root = cJSON_Parse(buf); free(buf); if (!root) return;
    s_rem = (ir_remote_t *)calloc(MAX_REMOTES, sizeof(ir_remote_t));
    s_tvp = (ir_tvp_t *)calloc(MAX_TVP, sizeof(ir_tvp_t));
    if (!s_rem || !s_tvp) {   // partial alloc: free whatever succeeded, leave a clean (empty) state
        free(s_rem); free(s_tvp); s_rem = NULL; s_tvp = NULL;
        cJSON_Delete(root);
        snprintf(s_status, sizeof s_status, "catalog OOM");
        return;
    }

    cJSON *rem = cJSON_GetObjectItem(root, "remotes"), *it;
    if (cJSON_IsArray(rem)) cJSON_ArrayForEach(it, rem) {
        if (s_nrem >= MAX_REMOTES) break;
        cJSON *nm = cJSON_GetObjectItem(it, "name"), *pr = cJSON_GetObjectItem(it, "protocol"),
              *ad = cJSON_GetObjectItem(it, "address"), *bs = cJSON_GetObjectItem(it, "buttons");
        if (!cJSON_IsString(nm) || !cJSON_IsString(pr) || !cJSON_IsObject(bs)) continue;
        ir_remote_t *R = &s_rem[s_nrem];
        snprintf(R->name, sizeof R->name, "%s", nm->valuestring);
        R->proto = nir_proto_from_name(pr->valuestring);
        R->addr  = cJSON_IsNumber(ad) ? (uint32_t)ad->valuedouble : 0;
        R->nbtn  = 0;
        cJSON *b;
        cJSON_ArrayForEach(b, bs) {
            if (R->nbtn >= MAX_BTN) break;
            if (!cJSON_IsNumber(b) || !b->string) continue;
            snprintf(R->btn[R->nbtn].key, sizeof R->btn[R->nbtn].key, "%s", b->string);
            R->btn[R->nbtn].cmd = (uint32_t)b->valuedouble;
            R->nbtn++;
        }
        if (R->proto < NIR_PROTO__COUNT && R->nbtn > 0) s_nrem++;
    }
    cJSON *tvp = cJSON_GetObjectItem(root, "tvpower");
    if (cJSON_IsArray(tvp)) cJSON_ArrayForEach(it, tvp) {
        if (s_ntvp >= MAX_TVP) break;
        cJSON *br = cJSON_GetObjectItem(it, "brand"), *pr = cJSON_GetObjectItem(it, "protocol"),
              *ad = cJSON_GetObjectItem(it, "address"), *cm = cJSON_GetObjectItem(it, "command"),
              *rg = cJSON_GetObjectItem(it, "region");
        if (!cJSON_IsString(br) || !cJSON_IsString(pr) || !cJSON_IsNumber(cm)) continue;
        ir_tvp_t *T = &s_tvp[s_ntvp];
        snprintf(T->brand, sizeof T->brand, "%s", br->valuestring);
        snprintf(T->region, sizeof T->region, "%s", cJSON_IsString(rg) ? rg->valuestring : "all");
        T->proto = nir_proto_from_name(pr->valuestring);
        T->addr  = cJSON_IsNumber(ad) ? (uint32_t)ad->valuedouble : 0;
        T->cmd   = (uint32_t)cm->valuedouble;
        if (T->proto < NIR_PROTO__COUNT) s_ntvp++;
    }
    cJSON_Delete(root);
}

static void load_favorites(void) {
    char *buf = slurp(UDATA_PATH, 64 * 1024); if (!buf) return;
    cJSON *root = cJSON_Parse(buf); free(buf); if (!root) return;
    cJSON *favs = cJSON_GetObjectItem(root, "favs");
    if (cJSON_IsArray(favs)) {
        s_fav = (ir_fav_t *)calloc(MAX_FAV, sizeof(ir_fav_t));
        if (s_fav) { cJSON *it; cJSON_ArrayForEach(it, favs) {
            if (s_nfav >= MAX_FAV) break;
            cJSON *lb = cJSON_GetObjectItem(it, "label"), *cd = cJSON_GetObjectItem(it, "code");
            if (!cJSON_IsObject(cd)) continue;
            cJSON *pr = cJSON_GetObjectItem(cd, "protocol"), *ad = cJSON_GetObjectItem(cd, "address"), *cm = cJSON_GetObjectItem(cd, "command");
            if (!cJSON_IsString(pr) || !cJSON_IsNumber(cm)) continue;   // skip raw favourites
            ir_fav_t *F = &s_fav[s_nfav];
            snprintf(F->label, sizeof F->label, "%s", cJSON_IsString(lb) ? lb->valuestring : "fav");
            F->proto = nir_proto_from_name(pr->valuestring);
            F->addr  = cJSON_IsNumber(ad) ? (uint32_t)ad->valuedouble : 0;
            F->cmd   = (uint32_t)cm->valuedouble;
            if (F->proto < NIR_PROTO__COUNT) s_nfav++;
        } }
    }
    cJSON_Delete(root);
}

static void free_all(void) {
    free(s_rem); free(s_tvp); free(s_fav);
    s_rem = NULL; s_tvp = NULL; s_fav = NULL; s_nrem = s_ntvp = s_nfav = 0;
}

// ---- helpers ----
static int cur_count(void) {
    if (s_view == 1) return NREGION;
    if (s_view == 2) return s_nfav;
    return s_level == 0 ? s_nrem : (s_dev < s_nrem ? s_rem[s_dev].nbtn : 0);
}
static void tx(int x, int y, const char *s, unsigned short fg, unsigned short bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// ---- drawing ----
static void on_draw(void) {
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    tx(8, top + 4, VIEW_TITLE[s_view], s_view == 2 ? STAR : ACC, BG, 1);
    if (s_view == 1) { char r[24]; snprintf(r, sizeof r, "region: %s", REGIONS[s_region]); tx(150, top + 4, r, MUTED, BG, 1); }
    d.drawFastHLine(0, top + 16, 240, LINE);

    if (s_nrem == 0 && s_ntvp == 0 && s_nfav == 0) {
        tx(8, top + h / 2 - 4, "No catalog on SD", MUTED, BG, 1);
        tx(8, top + h / 2 + 8, "/system/ir/presets.json", MUTED, BG, 1);
        return;
    }
    if (s_view == 2 && s_nfav == 0) {
        tx(8, top + h / 2 - 4, "No favourites yet", MUTED, BG, 1);
        tx(8, top + h / 2 + 8, "star buttons in the web app", MUTED, BG, 1);
        return;
    }

    int n = cur_count(), rows = (h - 30) / 14; if (rows < 1) rows = 1;
    int first = s_sel - rows / 2; if (first < 0) first = 0; if (n > rows && first > n - rows) first = n - rows;
    int y = top + 22;
    for (int i = first; i < n && i < first + rows; i++) {
        bool foc = (i == s_sel);
        if (foc) d.fillRoundRect(4, y - 2, 232, 14, 4, SELBG);
        char label[44]; unsigned short col = foc ? FG : MUTED;
        if (s_view == 1)        snprintf(label, sizeof label, "%s", REGIONS[i]);
        else if (s_view == 2)   snprintf(label, sizeof label, "\x05 %s", s_fav[i].label);
        else if (s_level == 0)  snprintf(label, sizeof label, "%s", s_rem[i].name);
        else { snprintf(label, sizeof label, "%s", s_rem[s_dev].btn[i].key);
               if (!strcmp(s_rem[s_dev].btn[i].key, "power")) col = foc ? PWR : 0xC408; }
        tx(12, y, label, col, foc ? SELBG : BG, 1);
        y += 14;
    }

    const char *hint = s_status[0] ? s_status
        : (s_view == 1 ? (s_sweep ? "sweeping  esc to stop" : "enter: sweep   tab: next")
        :  s_view == 2 ? "enter: send   tab: next"
        : (s_level == 0 ? "enter: open   tab: next" : "enter: send   esc: back"));
    tx(8, top + h - 12, hint, s_status[0] ? GRN : MUTED, BG, 1);
}

// ---- actions ----
static void start_sweep(void) {
    s_sweep_n = 0;
    for (int i = 0; i < s_ntvp && s_sweep_n < MAX_TVP; i++)
        if (s_region == 0 || !strcmp(s_tvp[i].region, REGIONS[s_region])) s_sweep_idx[s_sweep_n++] = i;
    if (!s_sweep_n) { snprintf(s_status, sizeof s_status, "no codes"); return; }
    s_sweep_i = 0; s_sweep = true; s_status[0] = 0;
}
static void send_now(nir_proto_t proto, uint32_t addr, uint32_t cmd, const char *label) {
    esp_err_t rc = nucleo_ir_send_proto(proto, addr, cmd, 0);
    if (rc == ESP_OK) snprintf(s_status, sizeof s_status, "sent %s", label);
    else              snprintf(s_status, sizeof s_status, "send error");
    nucleo_app_request_draw();
}
static void activate(void) {
    if (s_view == 1) {
        if (s_sweep) { s_sweep = false; s_status[0] = 0; } else { s_region = s_sel; start_sweep(); }
        nucleo_app_request_draw(); return;
    }
    if (s_view == 2) { if (s_sel < s_nfav) send_now(s_fav[s_sel].proto, s_fav[s_sel].addr, s_fav[s_sel].cmd, s_fav[s_sel].label); return; }
    if (s_level == 0) { if (s_sel < s_nrem) { s_dev = s_sel; s_level = 1; s_sel = 0; nucleo_app_request_draw(); } return; }
    ir_remote_t *R = &s_rem[s_dev]; if (s_sel < R->nbtn) send_now(R->proto, R->addr, R->btn[s_sel].cmd, R->btn[s_sel].key);
}

// ---- input ----
static void on_key(int k, char ch) {
    (void)ch;
    int n = cur_count();
    if (k == NK_UP)        { if (s_sel > 0) s_sel--; s_status[0] = 0; nucleo_app_request_draw(); }
    else if (k == NK_DOWN) { if (s_sel < n - 1) s_sel++; s_status[0] = 0; nucleo_app_request_draw(); }
    else if (k == NK_ENTER) activate();
}
static bool ir_back(int key) {
    (void)key;
    if (s_sweep) { s_sweep = false; s_status[0] = 0; nucleo_app_request_draw(); return true; }
    if (s_view == 0 && s_level == 1) { s_level = 0; s_sel = s_dev; nucleo_app_request_draw(); return true; }
    return false;   // Esc at the top level -> framework closes the app
}
static void ir_tab(void) { s_view = (s_view + 1) % 3; s_level = 0; s_sel = 0; s_status[0] = 0; nucleo_app_request_draw(); }

// ---- lifecycle ----
static void on_tick(void) {
    if (!s_sweep) return;
    if (nucleo_ir_busy()) return;
    if (s_sweep_i >= s_sweep_n) { s_sweep = false; snprintf(s_status, sizeof s_status, "swept %d", s_sweep_n); nucleo_app_request_draw(); return; }
    ir_tvp_t *T = &s_tvp[s_sweep_idx[s_sweep_i]];
    nucleo_ir_send_proto(T->proto, T->addr, T->cmd, 0);
    snprintf(s_status, sizeof s_status, "%s %d/%d", T->brand, s_sweep_i + 1, s_sweep_n);
    s_sweep_i++;
    nucleo_app_request_draw();
}
static void enter(void) {
    s_view = 0; s_level = 0; s_sel = 0; s_dev = 0; s_region = 0; s_sweep = false; s_status[0] = 0;
    // Free the 32 KB shared canvas BEFORE parsing the SD catalog: the slurp buffer + the cJSON DOM
    // briefly coexist, and on a full OS that peak can starve the open. The launcher re-acquires the
    // canvas lazily on the first on_draw (this app is not direct-draw and draws nothing during load).
    nucleo_app_release_buffers();
    load_catalog();       // may set s_status (e.g. "catalog OOM") — keep it for on_draw
    load_favorites();
    nucleo_app_set_back_handler(ir_back);
    nucleo_app_set_tab_handler(ir_tab);
    nucleo_app_request_draw();
}
static void on_exit(void) { s_sweep = false; free_all(); }

extern "C" void nucleo_register_ir(void) {
    static const nucleo_app_def_t app = {
        "ir", "IR Remote", "Tools", "TV-B-Gone, brand remotes & favourites (SD catalog)",
        'I', ACC,
        enter, on_key, on_tick, on_draw, on_exit
    };
    nucleo_app_register(&app);
}
