// app_ir.cpp — native on-device IR remote for NucleoOS (Wear-OS-style launcher app).
//
// Three views (TAB cycles): Remotes (browse the SD preset catalog, drill into a device, send a
// button), TV-B-Gone (sweep the catalog's power codes by region) and Favourites (the SAME ⭐ the
// web app saved to /sd/data/ir/userdata.json — so a favourite starred in the browser is one tap
// away on the device). The catalog lives on the SD card as a packed binary (/sd/system/ir/presets.bin,
// built from presets.json by tools/ir-pack.mjs) and is read one fixed-width record at a time with
// fseek/fread — a catalog of thousands of remotes costs only a few hundred bytes of RAM, honouring
// the rule that a backgrounded native app must cost nothing while the web UI is the active surface.
//
// Drawing goes straight through the `d` macro (app_gfx.h), tick-based and non-blocking: the
// TV-B-Gone sweep sends one code per on_tick() so the UI stays responsive.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "app_gfx.h"
#include "app_ui.h"   // shared Wear-OS focused-row list + type-to-jump
#include <M5GFX.h>
#include "nucleo_ir.h"
#include "nucleo_ir_proto.h"
#include "nucleo_ir_pack.h"   // low-RAM seek-based reader for the packed catalog
#include "nucleo_board.h"     // NUCLEO_SD_MOUNT
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define PRESETS_BIN NUCLEO_SD_MOUNT "/system/ir/presets.bin"  // built from presets.json by tools/ir-pack.mjs
#define UDATA_PATH  NUCLEO_SD_MOUNT "/data/ir/userdata.json"

// ---- palette (RGB565) ----
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, LINE = 0x2945,
                            ACC = 0x2EDA, PWR = 0xF9A6, GRN = 0x8FF3, STAR = 0xFE60, INK = 0x0000;

// ---- model ----
// Remotes + TV-power codes are NOT loaded into RAM: they're read on demand from the packed SD
// catalog (presets.bin), one fixed-width record per list row / button press, so the catalog can
// hold thousands of devices for a few hundred bytes of working set (see nucleo_ir_pack.h). Only
// favourites — a short, user-curated list — are parsed into RAM, and that list is bounded.
#define MAX_FAV 24
typedef struct { char label[30]; nir_proto_t proto; uint32_t addr, cmd; } ir_fav_t;

static ir_pack_t        s_pack;     // open handle to presets.bin (valid while s_pack_ok)
static bool             s_pack_ok;
static ir_pack_remote_t s_curdev;   // the remote being browsed at level 2 (loaded on drill-in)
static ir_fav_t        *s_fav; static int s_nfav;

// Category index (v3 pack): remotes are packed SORTED by category, so each category is a contiguous
// [start,count] range — the REMOTES view browses category -> remotes -> buttons with plain seek math.
#define MAX_CAT 16
static struct { char name[IRPACK_CAT_LEN]; int start, count; } s_cats[MAX_CAT];
static int s_ncat;      // distinct categories found in the pack
static int s_catsel;    // chosen category (level 1/2)

// ---- UI state ----
static int  s_view;            // 0 = Remotes, 1 = TV-B-Gone, 2 = Favourites
static int  s_level;           // remotes: 0 = device list, 1 = button face
static int  s_sel;             // selection in the current list
static int  s_dev;             // chosen device index (level 1)
static int  s_region;          // 0=all 1=us 2=eu 3=asia
static char s_status[40];

static bool     s_sweep;        // TV-B-Gone sweep running
static uint32_t s_sweep_i;      // next tvpower index to consider
static int      s_sweep_done;   // codes actually sent this sweep
static int      s_sweep_total;  // region-matching codes (for the x/n readout)
static char     s_cur_brand[20]; // brand currently firing (big readout on the sweep screen)

static const char *const REGIONS[] = { "all", "us", "eu", "asia" };
#define NREGION 4
static int s_regcount[NREGION];   // tvpower codes per region (precomputed once on open)

enum { V_REM = 0, V_TVB, V_FIND, V_FAV };   // tab order (s_view)
#define NTABS 4
static const char *const TABS[NTABS] = { "REMOTES", "TV-B", "FIND", "FAVS" };

// ---- Auto-Find: guided universal-remote identification (the flagship) -----------------------
// Walks the TV-power codes one UNIQUE code at a time, firing each and asking the operator "did it
// turn off?". A yes identifies the set and jumps straight into that brand's full remote. Dedups by
// (proto,addr,cmd) so the dozens of EU brands that share RC5 0/12 are asked only ONCE.
static bool        s_find;            // wizard running
static uint32_t    s_find_i;          // next tvpower index to probe
static int         s_find_done;       // unique codes tried
static int         s_find_total;      // unique region-matching codes (denominator)
static char        s_find_brand[24];  // current candidate brand (big readout)
static nir_proto_t s_find_proto; static uint32_t s_find_addr, s_find_cmd;
static uint32_t    s_find_seen[96]; static int s_find_nseen;

static uint32_t find_key(uint8_t proto, uint32_t addr, uint32_t cmd) {
    return ((uint32_t)proto << 24) | ((addr & 0xFF) << 8) | (cmd & 0xFF);
}
static bool find_seen(uint32_t key) {
    for (int i = 0; i < s_find_nseen; i++) if (s_find_seen[i] == key) return true;
    return false;
}
// Dedup key for a tv-power entry: a RAW row keys on its (large) pool offset, a protocol row on
// (proto,addr,cmd) — the two ranges never collide (offsets are small, proto keys carry proto<<24).
static uint32_t tvp_key(const ir_pack_tvp_t *t) {
    return (t->proto == IRPACK_PROTO_RAW) ? ir_pack_tvp_raw_off(t) : find_key(t->proto, t->addr, t->cmd);
}

// ---- load helpers ----
static char *slurp(const char *path, long cap) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > cap) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1); if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, sz, f); fclose(f); buf[got] = 0; return buf;
}

// region filter: code 0 ("all") fires in every region; otherwise the selected region must match.
static bool region_hit(uint8_t code) { return s_region == 0 || code == 0 || code == (uint8_t)s_region; }

static void open_pack(void) {
    // O(1)-RAM: just read + validate the 32-byte header and keep the FILE open. Every row/button is
    // fetched on demand from the SD pack — the catalog never sits in the heap.
    s_pack_ok = ir_pack_open(&s_pack, PRESETS_BIN);
    for (int r = 0; r < NREGION; r++) s_regcount[r] = 0;
    if (!s_pack_ok) return;
    // One pass to count codes per region tab (so the region list shows a live badge without rescanning).
    for (uint32_t i = 0; i < s_pack.n_tvpower; i++) {
        ir_pack_tvp_t t;
        if (!ir_pack_tvpower(&s_pack, i, &t)) continue;
        s_regcount[0]++;                                    // "all"
        for (int r = 1; r < NREGION; r++) if (t.region == 0 || t.region == r) s_regcount[r]++;
    }
    // Build the category index. Remotes are packed sorted by category (ir-pack.mjs), so a run of the
    // same category is one contiguous [start,count] block; a v1/v2 pack (no category) lands in "all".
    s_ncat = 0;
    for (uint32_t i = 0; i < s_pack.n_remotes; i++) {
        ir_pack_remote_t r;
        if (!ir_pack_remote(&s_pack, i, &r)) continue;
        const char *c = r.category[0] ? r.category : "all";
        if (s_ncat > 0 && strcmp(s_cats[s_ncat - 1].name, c) == 0) { s_cats[s_ncat - 1].count++; continue; }
        if (s_ncat < MAX_CAT) { snprintf(s_cats[s_ncat].name, sizeof s_cats[s_ncat].name, "%s", c);
                                s_cats[s_ncat].start = (int)i; s_cats[s_ncat].count = 1; s_ncat++; }
        else s_cats[MAX_CAT - 1].count++;                   // overflow: lump the tail into the last bucket
    }
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
    if (s_pack_ok) { ir_pack_close(&s_pack); s_pack_ok = false; }
    free(s_fav); s_fav = NULL; s_nfav = 0;
}

// ---- helpers ----
static int cur_count(void) {
    if (s_view == V_TVB || s_view == V_FIND) return NREGION;
    if (s_view == V_FAV) return s_nfav;
    if (!s_pack_ok) return 0;
    if (s_level == 0) return s_ncat;                        // category list
    if (s_level == 1) return (s_catsel >= 0 && s_catsel < s_ncat) ? s_cats[s_catsel].count : 0;  // remotes in cat
    return (int)s_curdev.nbtn;                              // button face
}
static void tx(int x, int y, const char *s, unsigned short fg, unsigned short bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}
static void tx_center(int cx, int y, const char *s, unsigned short fg, unsigned short bg, int sz) {
    tx(cx - (int)strlen(s) * 3 * sz, y, s, fg, bg, sz);
}

// Emit one code: protocol-encoded, or — for a RAW record (proto 0) — the captured µs streamed from
// the pack (cmd_or_off is then the RAW-pool byte offset). This is what makes any protocol sendable.
static void ir_emit(uint8_t proto, uint32_t addr, uint32_t cmd_or_off) {
    if (proto == IRPACK_PROTO_RAW) {
        uint16_t car = 0; static uint16_t dur[400];
        int n = ir_pack_raw(&s_pack, cmd_or_off, &car, dur, 400);
        if (n > 0) nucleo_ir_send_raw(dur, n, car, 0);
    } else {
        nucleo_ir_send_proto((nir_proto_t)proto, addr, cmd_or_off, 0);
    }
}

// ---- app_ui row providers (one shared scratch buffer; each label/right is consumed before the
// next call, so a single static is safe — and every row is read straight from the SD pack) -------
static char s_rowbuf[40];
static const char *remote_label(int i, void *) {
    ir_pack_remote_t r; s_rowbuf[0] = 0;
    if (ir_pack_remote(&s_pack, (uint32_t)i, &r)) snprintf(s_rowbuf, sizeof s_rowbuf, "%s", r.name);
    return s_rowbuf;
}
static const char *remote_right(int i, void *) {
    ir_pack_remote_t r;
    if (ir_pack_remote(&s_pack, (uint32_t)i, &r)) return nir_proto_name((nir_proto_t)r.proto);
    return "";
}
// Category list (level 0) + remotes within the chosen category (level 1, index offset into the pack).
static const char *cat_label(int i, void *) {
    s_rowbuf[0] = 0;
    if (i >= 0 && i < s_ncat) { int k = 0;
        for (const char *p = s_cats[i].name; *p && k < 30; p++) s_rowbuf[k++] = (char)toupper((unsigned char)*p);
        s_rowbuf[k] = 0; }
    return s_rowbuf;
}
static const char *cat_right(int i, void *) {
    static char rb[8]; rb[0] = 0;
    if (i >= 0 && i < s_ncat) snprintf(rb, sizeof rb, "%d", s_cats[i].count);
    return rb;
}
static int catrem_index(int i) { return (s_catsel >= 0 && s_catsel < s_ncat) ? s_cats[s_catsel].start + i : i; }
static const char *catrem_label(int i, void *u) { return remote_label(catrem_index(i), u); }
static const char *catrem_right(int i, void *u) { return remote_right(catrem_index(i), u); }
static const char *btn_label(int i, void *) {
    ir_pack_btn_t b; s_rowbuf[0] = 0;
    if (ir_pack_button(&s_pack, &s_curdev, (uint32_t)i, &b)) snprintf(s_rowbuf, sizeof s_rowbuf, "%s", b.key);
    return s_rowbuf;
}
static unsigned short btn_color(int i, void *) {
    ir_pack_btn_t b;
    if (ir_pack_button(&s_pack, &s_curdev, (uint32_t)i, &b) && !strcmp(b.key, "power")) return PWR;
    return ACC;
}
static const char *fav_label(int i, void *) {
    s_rowbuf[0] = 0;
    if (i >= 0 && i < s_nfav) snprintf(s_rowbuf, sizeof s_rowbuf, "\x05 %s", s_fav[i].label);
    return s_rowbuf;
}
static unsigned short fav_color(int, void *) { return STAR; }
static const char *region_label(int i, void *) {
    static const char *const UP[NREGION] = { "ALL", "US", "EU", "ASIA" };
    return (i >= 0 && i < NREGION) ? UP[i] : "";
}
static const char *region_right(int i, void *) {
    s_rowbuf[0] = 0;
    if (i >= 0 && i < NREGION) snprintf(s_rowbuf, sizeof s_rowbuf, "%d", s_regcount[i]);
    return s_rowbuf;
}
// pick the active list's label provider for type-to-jump (app_ui_list_key).
static app_ui_text_fn cur_label_fn(void) {
    if (s_view == V_TVB || s_view == V_FIND) return region_label;
    if (s_view == V_FAV) return fav_label;
    return s_level == 0 ? cat_label : s_level == 1 ? catrem_label : btn_label;
}

// ---- segmented tab bar (app_wifi look): active tab = filled accent pill, others legible grey;
// a small alert dot on TV-B-GONE while a sweep is live. Occupies [top, top+24). ----------------
static void draw_tabbar(int top) {
    d.fillRect(0, top, 240, 24, BG);
    int seg = 240 / NTABS;
    for (int i = 0; i < NTABS; i++) {
        int x = i * seg; const char *lab = TABS[i]; int tw = (int)strlen(lab) * 6;
        int lx = x + (seg - tw) / 2, ly = top + 8;
        if (i == s_view) { d.fillRoundRect(x + 3, top + 3, seg - 6, 18, 8, ACC); tx(lx, ly, lab, INK, ACC, 1); }
        else             { tx(lx, ly, lab, MUTED, BG, 1); }
        if (i == 1 && s_sweep) d.fillCircle(x + seg - 8, top + 7, 3, PWR);   // live-sweep alert dot
    }
    d.drawFastHLine(0, top + 23, 240, LINE);
}

// ---- TV-B-Gone sweep screen: big brand readout + fat progress bar + counter -------------------
static void draw_sweep(int top, int h) {
    d.fillRect(0, top, 240, h, BG);
    int cy = top + h / 2;
    tx_center(120, top + 8, "SWEEPING", PWR, BG, 2);
    // current brand, large
    const char *b = s_cur_brand[0] ? s_cur_brand : "...";
    tx_center(120, cy - 30, b, FG, BG, 2);
    // fat progress bar
    int pct = s_sweep_total ? s_sweep_done * 100 / s_sweep_total : 0;
    int bx = 20, bw = 200, by = cy, bh = 16;
    d.drawRoundRect(bx, by, bw, bh, 6, LINE);
    d.fillRoundRect(bx + 2, by + 2, (bw - 4) * pct / 100, bh - 4, 4, PWR);
    char c[24]; snprintf(c, sizeof c, "%d / %d", s_sweep_done, s_sweep_total);
    tx_center(120, by + bh + 8, c, MUTED, BG, 2);
    tx_center(120, top + h - 12, "Enter / Esc: stop", MUTED, BG, 1);
}

// ---- Auto-Find wizard logic ----
static void find_send_current(void) { ir_emit((uint8_t)s_find_proto, s_find_addr, s_find_cmd); }

// Advance to the next UNIQUE, region-matching power code. Returns false when the list is exhausted.
static bool find_advance(void) {
    while (s_find_i < s_pack.n_tvpower) {
        ir_pack_tvp_t t; uint32_t idx = s_find_i++;
        if (!ir_pack_tvpower(&s_pack, idx, &t) || !region_hit(t.region)) continue;
        uint32_t key = tvp_key(&t);
        if (find_seen(key)) continue;
        if (s_find_nseen < (int)(sizeof s_find_seen / sizeof s_find_seen[0])) s_find_seen[s_find_nseen++] = key;
        if (t.proto == IRPACK_PROTO_RAW) { s_find_proto = (nir_proto_t)0; s_find_addr = 0; s_find_cmd = ir_pack_tvp_raw_off(&t); }
        else                            { s_find_proto = (nir_proto_t)t.proto; s_find_addr = t.addr; s_find_cmd = t.cmd; }
        snprintf(s_find_brand, sizeof s_find_brand, "%s", t.brand);
        s_find_done++;
        return true;
    }
    return false;
}
static void start_find(void) {
    if (!s_pack_ok) { snprintf(s_status, sizeof s_status, "no catalog"); return; }
    // First pass: count the unique region-matching codes (the denominator).
    s_find_nseen = 0; s_find_total = 0;
    for (uint32_t i = 0; i < s_pack.n_tvpower; i++) {
        ir_pack_tvp_t t;
        if (ir_pack_tvpower(&s_pack, i, &t) && region_hit(t.region)) {
            uint32_t k = tvp_key(&t);
            if (!find_seen(k)) { if (s_find_nseen < 96) s_find_seen[s_find_nseen++] = k; s_find_total++; }
        }
    }
    if (!s_find_total) { snprintf(s_status, sizeof s_status, "no codes"); return; }
    s_find_nseen = 0; s_find_i = 0; s_find_done = 0; s_find = true; s_status[0] = 0; s_find_brand[0] = 0;
    if (find_advance()) find_send_current();
}
static void find_no(void) {   // "didn't turn off" -> probe the next candidate
    if (find_advance()) find_send_current();
    else { s_find = false; snprintf(s_status, sizeof s_status, "not found (%d tried)", s_find_done); }
    nucleo_app_request_draw();
}
static void find_yes(void) {  // identified -> jump straight into that brand's full remote if we have it
    s_find = false;
    int match = -1;
    for (uint32_t i = 0; i < s_pack.n_remotes; i++) {
        ir_pack_remote_t r;
        if (ir_pack_remote(&s_pack, i, &r) && r.proto == (uint8_t)s_find_proto && r.addr == s_find_addr) { match = (int)i; break; }
    }
    if (match >= 0 && ir_pack_remote(&s_pack, (uint32_t)match, &s_curdev)) {
        s_dev = match; s_view = V_REM; s_level = 2; s_sel = 0;
        s_catsel = 0;                                       // resolve which category owns the matched remote
        for (int c = 0; c < s_ncat; c++) if (match >= s_cats[c].start && match < s_cats[c].start + s_cats[c].count) { s_catsel = c; break; }
        snprintf(s_status, sizeof s_status, "found: %s", s_find_brand);
    } else {
        snprintf(s_status, sizeof s_status, "found %s (power only)", s_find_brand);
    }
    nucleo_app_request_draw();
}
static void draw_find(int top, int h) {
    d.fillRect(0, top, 240, h, BG);
    tx_center(120, top + 6, "AUTO-FIND", ACC, BG, 2);
    tx_center(120, top + h / 2 - 28, s_find_brand[0] ? s_find_brand : "...", FG, BG, 2);
    char c[20]; snprintf(c, sizeof c, "%d / %d", s_find_done, s_find_total);
    tx_center(120, top + h / 2 - 6, c, MUTED, BG, 1);
    tx_center(120, top + h / 2 + 12, "TV off?", PWR, BG, 2);
    tx_center(120, top + h - 12, "Enter: YES   >: no   Esc: stop", MUTED, BG, 1);
}

// ---- drawing ----
static void on_draw(void) {
    int top = nucleo_app_content_top(), H = nucleo_app_content_height();
    d.fillRect(0, top, 240, H, BG);
    draw_tabbar(top);
    int cy = top + 26, band = H - 26;          // content band below the tab bar

    if (!s_pack_ok && (s_view != V_FAV || s_nfav == 0)) {
        tx_center(120, top + H / 2 - 10, "No catalog on SD", MUTED, BG, 2);
        tx_center(120, top + H / 2 + 12, "build presets.bin (ir:pack)", MUTED, BG, 1);
        return;
    }

    if (s_view == V_REM) {                      // REMOTES: categories -> remotes -> buttons
        if (s_level == 2) {
            ir_pack_remote_t r; r.name[0] = 0; ir_pack_remote(&s_pack, (uint32_t)s_dev, &r);
            const char *pn = nir_proto_name((nir_proto_t)r.proto);
            char nm[18]; snprintf(nm, sizeof nm, "%s", r.name);   // size-2 header: cap to fit 240px
            tx(10, cy, nm, ACC, BG, 2);
            tx(238 - (int)strlen(pn) * 6, cy + 4, pn, MUTED, BG, 1);
            app_ui_list(cy + 22, band - 22 - 14, (int)s_curdev.nbtn, s_sel, btn_label, nullptr, btn_color, nullptr);
            tx_center(120, top + H - 12, s_status[0] ? s_status : "Enter: send  \xB7  1-9 quick  \xB7  Esc: back",
                      s_status[0] ? GRN : MUTED, BG, 1);
        } else if (s_level == 1) {
            char hd[24]; snprintf(hd, sizeof hd, "\x11 %s", s_catsel < s_ncat ? cat_label(s_catsel, nullptr) : "");
            tx(10, cy, hd, ACC, BG, 2);
            int n = (s_catsel >= 0 && s_catsel < s_ncat) ? s_cats[s_catsel].count : 0;
            app_ui_list(cy + 22, band - 22 - 14, n, s_sel, catrem_label, catrem_right, nullptr, nullptr);
            tx_center(120, top + H - 12, s_status[0] ? s_status : "Enter: open  \xB7  type to search  \xB7  Esc: back",
                      s_status[0] ? GRN : MUTED, BG, 1);
        } else {
            app_ui_list(cy, band - 14, s_ncat, s_sel, cat_label, cat_right, nullptr, nullptr);
            tx_center(120, top + H - 12, s_status[0] ? s_status : "Enter: open category  \xB7  type to search",
                      s_status[0] ? GRN : MUTED, BG, 1);
        }
    } else if (s_view == V_TVB) {               // TV-B-GONE
        if (s_sweep) { draw_sweep(cy, band); return; }
        app_ui_list(cy, band - 14, NREGION, s_sel, region_label, region_right, nullptr, nullptr);
        tx_center(120, top + H - 12, s_status[0] ? s_status : "Enter: sweep region power-off",
                  s_status[0] ? GRN : MUTED, BG, 1);
    } else if (s_view == V_FIND) {              // AUTO-FIND
        if (s_find) { draw_find(cy, band); return; }
        app_ui_list(cy, band - 14, NREGION, s_sel, region_label, region_right, nullptr, nullptr);
        tx_center(120, top + H - 12, s_status[0] ? s_status : "Enter: start Auto-Find wizard",
                  s_status[0] ? GRN : MUTED, BG, 1);
    } else {                                    // FAVS
        if (s_nfav == 0) {
            tx_center(120, top + H / 2 - 10, "No favourites yet", MUTED, BG, 2);
            tx_center(120, top + H / 2 + 12, "star buttons in the web app", MUTED, BG, 1);
            return;
        }
        app_ui_list(cy, band - 14, s_nfav, s_sel, fav_label, nullptr, fav_color, nullptr);
        tx_center(120, top + H - 12, s_status[0] ? s_status : "Enter: send  \xB7  type to search",
                  s_status[0] ? GRN : MUTED, BG, 1);
    }
}

// ---- actions ----
static void start_sweep(void) {
    if (!s_pack_ok) { snprintf(s_status, sizeof s_status, "no catalog"); return; }
    s_sweep_total = 0;
    for (uint32_t i = 0; i < s_pack.n_tvpower; i++) {
        ir_pack_tvp_t t;
        if (ir_pack_tvpower(&s_pack, i, &t) && region_hit(t.region)) s_sweep_total++;
    }
    if (!s_sweep_total) { snprintf(s_status, sizeof s_status, "no codes"); return; }
    s_sweep_i = 0; s_sweep_done = 0; s_sweep = true; s_status[0] = 0;
}
static void send_now(nir_proto_t proto, uint32_t addr, uint32_t cmd, const char *label) {
    ir_emit((uint8_t)proto, addr, cmd);   // raw-aware (RAW button: cmd is the pool offset)
    snprintf(s_status, sizeof s_status, "sent %s", label);
    nucleo_app_request_draw();
}
static void activate(void) {
    if (s_view == V_TVB) {
        if (s_sweep) { s_sweep = false; s_status[0] = 0; } else { s_region = s_sel; start_sweep(); }
        nucleo_app_request_draw(); return;
    }
    if (s_view == V_FIND) {                       // region list -> start the guided wizard
        if (!s_find) { s_region = s_sel; start_find(); }
        nucleo_app_request_draw(); return;
    }
    if (s_view == V_FAV) { if (s_sel < s_nfav) send_now(s_fav[s_sel].proto, s_fav[s_sel].addr, s_fav[s_sel].cmd, s_fav[s_sel].label); return; }
    if (s_level == 0) {                          // pick a category
        if (s_pack_ok && s_sel < s_ncat) { s_catsel = s_sel; s_level = 1; s_sel = 0; nucleo_app_request_draw(); }
        return;
    }
    if (s_level == 1) {                          // pick a remote within the category
        int real = catrem_index(s_sel);
        if (s_pack_ok && real < (int)s_pack.n_remotes && ir_pack_remote(&s_pack, (uint32_t)real, &s_curdev)) {
            s_dev = real; s_level = 2; s_sel = 0; nucleo_app_request_draw();
        }
        return;
    }
    ir_pack_btn_t b;                             // level 2: send the focused button
    if (ir_pack_button(&s_pack, &s_curdev, s_sel, &b))
        send_now((nir_proto_t)s_curdev.proto, s_curdev.addr, b.cmd, b.key);
}

// ---- input ----
static void ir_tab(void);   // defined below (RIGHT also pages tabs)
static void on_key(int k, char ch) {
    // While a sweep runs, only Enter stops it (Esc routes to ir_back).
    if (s_view == V_TVB && s_sweep) { if (k == NK_ENTER) { s_sweep = false; s_status[0] = 0; nucleo_app_request_draw(); } return; }
    // Auto-Find wizard: Enter = "yes it turned off" (identify), RIGHT = "no, next candidate".
    if (s_view == V_FIND && s_find) { if (k == NK_ENTER) find_yes(); else if (k == NK_RIGHT) find_no(); return; }

    if (k == NK_RIGHT) { ir_tab(); return; }       // RIGHT pages tabs forward (LEFT == back handler)
    if (k == NK_ENTER) { activate(); return; }

    // Remote face: 0-9 hardware keys instantly fire the matching digit button (smartwatch quick-select).
    if (s_view == V_REM && s_level == 2 && k == NK_CHAR && ch >= '0' && ch <= '9') {
        char want[2] = { ch, 0 }; ir_pack_btn_t b;
        for (uint32_t j = 0; j < s_curdev.nbtn; j++)
            if (ir_pack_button(&s_pack, &s_curdev, j, &b) && !strcmp(b.key, want)) {
                s_sel = (int)j; send_now((nir_proto_t)s_curdev.proto, s_curdev.addr, b.cmd, b.key); return;
            }
        return;   // a digit with no matching button: do nothing (don't fall through to search)
    }

    // Everything else -> the shared list navigator (wrap-around UP/DOWN + type-to-jump search).
    int n = cur_count();
    if (app_ui_list_key(k, ch, &s_sel, n, cur_label_fn(), nullptr)) { s_status[0] = 0; nucleo_app_request_draw(); }
}
static bool ir_back(int key) {
    (void)key;
    if (s_sweep) { s_sweep = false; s_status[0] = 0; nucleo_app_request_draw(); return true; }
    if (s_find)  { s_find = false; s_status[0] = 0; nucleo_app_request_draw(); return true; }
    if (s_view == V_REM && s_level == 2) {                 // button face -> back to the category's remote list
        s_level = 1;
        s_sel = (s_catsel >= 0 && s_catsel < s_ncat) ? (s_dev - s_cats[s_catsel].start) : 0;
        if (s_sel < 0) s_sel = 0;
        nucleo_app_request_draw();
        return true;
    }
    if (s_view == V_REM && s_level == 1) { s_level = 0; s_sel = s_catsel; nucleo_app_request_draw(); return true; }
    return false;   // Esc at the top level -> framework closes the app
}
static void ir_tab(void) {
    s_view = (s_view + 1) % NTABS; s_level = 0; s_sel = 0; s_status[0] = 0;
    s_sweep = false; s_find = false;   // leaving a tab cancels any live sweep/wizard
    nucleo_app_request_draw();
}

// ---- lifecycle ----
static void on_tick(void) {
    if (!s_sweep) return;
    if (nucleo_ir_busy()) return;
    // Stream the next region-matching power code straight from the SD pack (no array in RAM).
    ir_pack_tvp_t t; bool found = false;
    while (s_sweep_i < s_pack.n_tvpower) {
        if (ir_pack_tvpower(&s_pack, s_sweep_i, &t) && region_hit(t.region)) { found = true; break; }
        s_sweep_i++;
    }
    if (!found) { s_sweep = false; snprintf(s_status, sizeof s_status, "swept %d", s_sweep_done); nucleo_app_request_draw(); return; }
    ir_emit(t.proto, t.addr, t.proto == IRPACK_PROTO_RAW ? ir_pack_tvp_raw_off(&t) : t.cmd);
    s_sweep_done++;
    snprintf(s_cur_brand, sizeof s_cur_brand, "%s", t.brand);   // big readout on the sweep screen
    s_sweep_i++;
    nucleo_app_request_draw();
}
static void enter(void) {
    s_view = 0; s_level = 0; s_sel = 0; s_dev = 0; s_catsel = 0; s_region = 0; s_sweep = false; s_find = false;
    s_status[0] = 0; s_cur_brand[0] = 0; s_find_brand[0] = 0;
    // No heavy load anymore: the catalog is read record-by-record from presets.bin, so there's no
    // multi-KB slurp to make room for — the shared canvas can stay put.
    open_pack();
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
