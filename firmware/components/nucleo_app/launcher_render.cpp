// Launcher rendering. See launcher_render.h.
#include "launcher_render.h"
#include "launcher_menu.h"
#include "nucleo_kbd.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_system.h"      // esp_restart() for the SISTEMA > Riavvia action
#include "esp_heap_caps.h"   // free-heap readout for the SISTEMA > RAM row

// The real display (defined in nucleo_ui.cpp). The launcher always draws to it directly;
// only the animated list band is composited off-screen first. We do NOT include app_gfx.h
// here on purpose — that header redefines `d` as the movable app-draw target, but the
// launcher chrome must always hit the physical screen.
extern M5GFX d;

// Shared off-screen back-buffer (nucleo_ui.cpp). The launcher list band composites into it and
// blits the band region with a clipped push; apps reuse the same canvas. See app_gfx.h.
M5Canvas *nucleo_screen(void);
void      nucleo_screen_release(void);

// Network info + pairing PIN (resolved at link; no component dependency).
extern "C" const char *nucleo_setup_mode(void);        // "sta" | "ap"
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_setup_ip(void);
extern "C" const char *nucleo_setup_device_name(void);
extern "C" const char *nucleo_auth_pin(void);

// Evil Portal live state, so the status bar can flag it while it runs in the background.
extern "C" bool nucleo_evilportal_running(void);
extern "C" int  nucleo_evilportal_captures(void);
extern "C" bool          nucleo_wifiatk_deauth_running(void);   // background radio-offensive ops: alert bar
extern "C" unsigned long nucleo_wifiatk_frames(void);
extern "C" bool          nucleo_wifiatk_beacon_running(void);
extern "C" int           nucleo_wifiatk_beacon_count(void);

// Quick-settings the Control Center drives: the shared backlight + the audio engine.
extern "C" void     nucleo_app_set_brightness(int pct);
extern "C" int      nucleo_app_brightness(void);
extern "C" bool     nucleo_audio_is_playing(void);
extern "C" bool     nucleo_audio_is_paused(void);
extern "C" const char *nucleo_audio_path(void);
extern "C" uint32_t nucleo_audio_elapsed(void);
extern "C" void     nucleo_audio_toggle_pause(void);
extern "C" int      nucleo_audio_volume(void);
extern "C" void     nucleo_audio_set_volume(int pct);
extern "C" bool     nucleo_audio_is_muted(void);
extern "C" void     nucleo_audio_set_mute(bool muted);

// Cardputer / NucleoOS / ANIMA management hooks the Control Center drives. These live in other
// components (nucleo_setup, nucleo_usbmsc, nucleo_anima, nucleo_voice, nucleo_tts); per the
// nucleo_app CMake note a REQUIRES on some of them would cycle, but the symbols resolve at the
// final link (httpd + app_anima already pull them in), so a forward extern "C" decl is enough.
extern "C" int      nucleo_setup_rssi(void);                 // joined-AP signal in dBm (0 = not associated)
extern "C" bool     nucleo_setup_time_synced(void);          // NTP clock synced?
extern "C" int      nucleo_power_battery_pct(void);          // real cell level 0..100, -1 = unknown
extern "C" void     nucleo_usbmsc_request(void);             // reboot into USB Mass-Storage mode (no return)
extern "C" void     nucleo_anima_set_online(bool on);        // ANIMA network tiers master switch
extern "C" bool     nucleo_anima_online_enabled(void);
extern "C" int      nucleo_anima_l1_get_mode(void);          // 0=AUTO 1=ON 2=OFF (offline L1 brain)
extern "C" void     nucleo_anima_l1_set_mode(int mode);
extern "C" void     nucleo_anima_reset_session(void);        // forget ANIMA conversational state
extern "C" bool     nucleo_anima_teacher_info(char *provider, int pcap, char *model, int mcap);
extern "C" bool     nucleo_anima_teacher_configured(void);
extern "C" void     nucleo_voice_set_always_on(bool on);     // PTT "always listen"
extern "C" bool     nucleo_voice_always_on(void);
extern "C" void     nucleo_tts_set_enabled(bool on);         // on-device spoken replies
extern "C" bool     nucleo_tts_enabled(void);
extern "C" bool     nucleo_tts_available(void);

// ---- hint + instruction text ------------------------------------------------
static char s_hint[48] = "";
static char s_instr[44] = "";          // one-line description of the focused row
static unsigned short s_hint_bg = INK, s_hint_fg = MUTED;   // hint-bar theme (app-overridable)
void        launcher_render_set_hint(const char *h) { strncpy(s_hint, h ? h : "", sizeof(s_hint) - 1); }
const char *launcher_render_hint(void) { return s_hint; }
void        launcher_render_set_hint_colors(unsigned short bg, unsigned short fg) { s_hint_bg = bg; s_hint_fg = fg; }
void        launcher_render_reset_hint_colors(void) { s_hint_bg = INK; s_hint_fg = MUTED; }

void launcher_render_update_chrome(void)
{
    // Description line: what the focused row does (falls back to the menu's own blurb).
    const MenuNode *cur = launcher_focused();
    const char *desc = (cur && cur->desc && cur->desc[0]) ? cur->desc : launcher_node()->desc;
    strncpy(s_instr, desc ? desc : "", sizeof(s_instr) - 1);
    s_instr[sizeof(s_instr) - 1] = 0;

    // Hint line: the controls that actually do something right now. Honest and contextual —
    // no numeric quick-select (it was removed), and `/` is labelled as "options" (it opens
    // the focused app's menu), not lumped into "move".
    // Segments are joined with a 2-space gap (not 3): the worst-case home hint
    // "scroll  enter open  / options  esc back" is then 39 chars (~234 px) and fits the
    // 240 px width, so the rightmost segment is never clipped off the screen edge.
    if (launcher_filter()[0]) {
        strncpy(s_hint, "type to filter  enter open  esc clear", sizeof(s_hint) - 1);
        s_hint[sizeof(s_hint) - 1] = 0;
    } else {
        bool back = launcher_depth() > 0;
        bool ctx  = cur && cur->kind == N_APP;
        snprintf(s_hint, sizeof(s_hint), "scroll  enter open%s%s",
                 ctx ? "  / options" : "", back ? "  esc back" : "");
    }
}

// Signature gate for the CHROME (status + hint bars). They are STATIC while you scroll WITHIN a menu —
// the clock is the 1 Hz tick's job, and the per-item description lives in the hero card (the buffered
// LIST), not here. The old code marked the chrome dirty on EVERY launcher key, so it re-wiped the top and
// bottom bars (direct fillRect = clear-then-draw) on each press = visible flicker. Latch a signature of
// what the bars actually SHOW and report a change only when it differs: depth/category, the /filter, the
// focused item's kind (drives the hint), and the Wi-Fi bar level + mode. Per ANTI-FLICKER.md (gate on real
// content change). The clock/date are excluded on purpose — they ride the 1 Hz in-place tick, not a wipe.
bool launcher_render_chrome_changed(void)
{
    const MenuNode *node = launcher_node();
    const MenuNode *foc  = launcher_focused();
    bool sta  = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];
    int  rssi = nucleo_setup_rssi();
    int  wlvl = (!sta || rssi == 0) ? 1 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;  // same map as draw_wifi
    uint32_t sig = 2166136261u;                       // FNV-1a over the chrome-visible state
    #define MIX(v) do { sig = (sig ^ (uint32_t)(v)) * 16777619u; } while (0)
    MIX(launcher_depth());
    MIX((uintptr_t)(node ? node->id : 0));            // category identity (breadcrumb glyph/name/count)
    MIX(foc ? (int)foc->kind + 1 : 0);                // hint shows "/ options" only for an app
    MIX(sta ? 1 : 0); MIX(wlvl);                      // Wi-Fi gauge (coarse bars, not dBm noise)
    for (const char *f = launcher_filter(); *f; f++) MIX((unsigned char)*f);  // /filter chip + filter hint
    #undef MIX
    static uint32_t s_last = 0; static bool s_init = false;
    if (s_init && sig == s_last) return false;
    s_last = sig; s_init = true; return true;
}

// ---- chrome (drawn directly; static during a scroll) ------------------------
// Number of children in a menu node (for the "how many inside" badge). 0 for apps/actions.
static int node_child_count(const MenuNode *m)
{
    if (!m || m->kind != N_MENU || !m->items) return 0;
    int n = 0; while (m->items[n]) n++; return n;
}

// Honest Wi-Fi indicator: four rising bars whose FILL tracks the real signal. STA = green, AP/setup
// = amber; bars above the measured level are drawn dim so the glyph reads as a strength gauge (the
// smartwatch idiom) rather than a flat icon. rssi 0 (not associated) -> 1 amber bar. There is no
// battery on bare M5GFX, so the top-right is spent entirely on this. Occupies a 19x9 box at (x,y).
static void draw_wifi(int x, int y, bool sta, int rssi)
{
    unsigned short on = sta ? C_GREEN : C_YELLOW;
    // Map dBm to 0..4 lit bars (>=-55 full, <=-88 one). AP mode / unknown -> a single amber bar.
    int lvl;
    if (!sta || rssi == 0) lvl = 1;
    else if (rssi >= -55)  lvl = 4;
    else if (rssi >= -67)  lvl = 3;
    else if (rssi >= -78)  lvl = 2;
    else                   lvl = 1;
    for (int i = 0; i < 4; i++) {
        int bh = 2 + i * 2;                                  // 2,4,6,8 px tall
        d.fillRect(x + i * 5, y + 9 - bh, 3, bh, i < lvl ? on : LINE);
    }
}

// Compact battery gauge, sibling to draw_wifi. Real cell level from nucleo_power (ADC G10).
// Fill tracks charge: red < 15 %, amber < 40 %, green above — the smartwatch idiom, same as the
// dialog header. pct < 0 = unknown reading -> a dim hollow icon, never a faked full bar. Occupies
// a 15x9 box at (x,y): a 13x8 body + a 2px terminal nub.
static void draw_battery(int x, int y)
{
    int pct = nucleo_power_battery_pct();
    int bw = 13, bh = 8;
    unsigned short frame = (pct < 0) ? LINE : FG;
    d.drawRect(x, y + 1, bw, bh, frame);
    d.fillRect(x + bw, y + 3, 2, 4, frame);                  // terminal nub
    if (pct < 0) return;                                     // unknown -> hollow, no fill
    int fw = (bw - 2) * pct / 100;
    if (fw < 1 && pct > 0) fw = 1;                           // a sliver so >0 % is never blank
    if (fw > bw - 2) fw = bw - 2;
    unsigned short col = (pct < 15) ? C_RED : (pct < 40) ? C_YELLOW : C_GREEN;
    d.fillRect(x + 1, y + 2, fw, bh - 2, col);
}

// ---- vector app / category icons -------------------------------------------------------
// Tiny line-art glyphs drawn inside a box of half-size `r` around (cx,cy). On a 240x135 panel
// detail only reads at the focused row's ~14 px badge and the breadcrumb, so that is the only
// place we spend it; smaller rows use a colour dot. Keyed by node id (stable). Any unmapped id
// falls back to the letter glyph, so a new app never draws blank.
template <typename T> static void ui_icon(T *g, int cx, int cy, int r, const char *id, char letter, uint16_t col)
{
    if (!id) id = "";
    #define HL(x,y,w) g->drawFastHLine((x),(y),(w),col)
    #define VL(x,y,h) g->drawFastVLine((x),(y),(h),col)

    if (!strcmp(id, "clock")) {
        g->drawCircle(cx, cy, r, col);
        VL(cx, cy - r + 2, r - 2); HL(cx, cy, r - 2);              // hour + minute hands
        g->fillCircle(cx, cy, 1, col);
    } else if (!strcmp(id, "calc")) {
        g->drawRoundRect(cx - r, cy - r, 2 * r, 2 * r, 2, col);
        HL(cx - r + 2, cy - r / 2, 2 * r - 4);                     // screen
        for (int jy = 0; jy < 2; jy++) for (int ix = 0; ix < 3; ix++)
            g->fillRect(cx - r + 2 + ix * (r - 1), cy + 1 + jy * 3, 1, 1, col);   // keypad
    } else if (!strcmp(id, "files")) {
        g->fillRect(cx - r, cy - r + 2, r, 2, col);               // folder tab
        g->fillRect(cx - r, cy - r + 3, 2 * r, 2 * r - 4, col);   // body
    } else if (!strcmp(id, "calendar")) {
        g->drawRect(cx - r, cy - r + 1, 2 * r, 2 * r - 1, col);
        VL(cx - r / 2, cy - r - 1, 3); VL(cx + r / 2, cy - r - 1, 3);  // rings
        HL(cx - r, cy - r + 4, 2 * r);                            // header
        g->fillRect(cx - 1, cy, 3, 3, col);                       // a marked day
    } else if (!strcmp(id, "notepad")) {
        g->drawRect(cx - r + 1, cy - r, 2 * r - 2, 2 * r, col);
        HL(cx - r + 3, cy - r + 3, 2 * r - 6);
        HL(cx - r + 3, cy,          2 * r - 6);
        HL(cx - r + 3, cy + r - 3,  2 * r - 7);
    } else if (!strcmp(id, "usb")) {
        g->fillRoundRect(cx - r, cy - 3, 2 * r - 2, 6, 1, col);   // stick body
        g->fillRect(cx + r - 2, cy - 1, 2, 2, col);              // connector
    } else if (!strcmp(id, "music")) {
        g->fillCircle(cx - 3, cy + r - 3, 2, col);               // note head
        VL(cx - 1, cy - r + 1, 2 * r - 4);                       // stem
        g->drawLine(cx - 1, cy - r + 1, cx + r - 2, cy - r + 3, col);  // flag
    } else if (!strcmp(id, "video") || !strcmp(id, "Media")) {
        g->fillTriangle(cx - r + 2, cy - r + 1, cx - r + 2, cy + r - 1, cx + r - 1, cy, col);  // play
    } else if (!strcmp(id, "photos")) {
        g->drawRect(cx - r, cy - r, 2 * r, 2 * r, col);
        g->fillCircle(cx - r + 3, cy - r + 3, 1, col);           // sun
        g->fillTriangle(cx - r + 1, cy + r - 1, cx, cy, cx + r - 1, cy + r - 1, col);  // hill
    } else if (!strcmp(id, "recorder")) {
        g->fillRoundRect(cx - 2, cy - r, 4, r + 2, 2, col);       // mic capsule
        VL(cx, cy + r - 1, 2); HL(cx - 2, cy + r, 4);            // stand
    } else if (!strcmp(id, "info") || !strcmp(id, "Connect")) {
        for (int i = 0; i < 3; i++) { int bh = 2 + i * 2; g->fillRect(cx - 4 + i * 3, cy + r - bh, 2, bh, col); }  // wifi bars
    } else if (!strcmp(id, "sysmon") || !strcmp(id, "System")) {
        int bh[3] = { r, 2 * r - 2, r - 2 };                     // activity bars
        for (int i = 0; i < 3; i++) g->fillRect(cx - r + 1 + i * (r), cy + r - 1 - bh[i], 2, bh[i], col);
    } else if (!strcmp(id, "radio")) {
        g->drawRoundRect(cx - r, cy - 1, 2 * r, r + 1, 1, col);   // receiver body
        g->drawLine(cx + r - 3, cy - 1, cx + r, cy - r, col);     // antenna
        g->fillCircle(cx - r + 3, cy + r / 2, 1, col);          // tuning dot
        g->drawCircle(cx + r - 3, cy + r / 2, 1, col);          // speaker
    } else if (!strcmp(id, "remote")) {
        g->drawRoundRect(cx - r, cy - r, r, 2 * r, 1, col);      // handset
        g->drawLine(cx + 1, cy - 2, cx + 3, cy, col); g->drawLine(cx + 3, cy, cx + 1, cy + 2, col);  // wave
        g->drawLine(cx + 3, cy - 4, cx + 6, cy, col); g->drawLine(cx + 6, cy, cx + 3, cy + 4, col);
    } else if (!strcmp(id, "Tools")) {
        g->drawCircle(cx, cy, r - 2, col); g->fillCircle(cx, cy, 1, col);  // gear hub
        VL(cx, cy - r, 2); VL(cx, cy + r - 2, 2); HL(cx - r, cy, 2); HL(cx + r - 2, cy, 2);  // teeth
    } else {
        g->setTextSize(r >= 6 ? 2 : 1); g->setTextColor(col);    // fallback: the letter glyph
        int cw = (r >= 6 ? 12 : 6), ch = (r >= 6 ? 16 : 8);
        g->setCursor(cx - cw / 2 + 1, cy - ch / 2); g->print(letter);
    }
    #undef HL
    #undef VL
}

// Red alert label for ANY background radio-offensive op (Evil Portal / Deauth Flood / Beacon Spam).
// All three keep running after you leave their app and suspend the OS network/web — the bar makes that
// unmissable. Returns the label (into buf) or NULL when nothing offensive is armed.
static const char *offensive_alert(char *buf, int cap)
{
    if (nucleo_evilportal_running())     { snprintf(buf, cap, "EVIL PORTAL  %d catt.", nucleo_evilportal_captures()); return buf; }
    if (nucleo_wifiatk_deauth_running()) { snprintf(buf, cap, "DEAUTH FLOOD  %lu fr", nucleo_wifiatk_frames());        return buf; }
    if (nucleo_wifiatk_beacon_running()) { snprintf(buf, cap, "BEACON SPAM  %d SSID", nucleo_wifiatk_beacon_count()); return buf; }
    return NULL;
}

void launcher_render_status_bar(void)
{
    // While any radio-offensive op runs in the background the whole top bar becomes a red alert — an
    // unmissable reminder that the device is attacking (and that the OS network/web UI are suspended).
    // Shown at any menu depth.
    char ab[28]; const char *alert = offensive_alert(ab, sizeof ab);
    if (alert) {
        d.fillRect(0, 0, W, BAR, C_RED);
        d.fillCircle(9, BAR / 2, 3, INK);
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4); d.print(alert);
        d.drawFastHLine(0, BAR - 1, W, LINE);
        return;
    }
    d.fillRect(0, 0, W, BAR, INK);
    d.setTextSize(1);
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];

    const MenuNode *node = launcher_node();
    if (launcher_depth() > 0) {
        // Breadcrumb: category glyph + name on the left; battery, Wi-Fi and the item count chip
        // packed from the right edge inward.
        ui_icon(&d, 10, 7, 5, node->id, node->icon, node->color);
        d.setTextSize(1); d.setTextColor(FG, INK); d.setCursor(19, 4);
        char b[18]; snprintf(b, sizeof(b), "%.13s", node->label); d.print(b);
        int rx = W - 2;
        draw_battery(rx - 15, 4);                          rx -= 15 + 4;   // battery far right
        draw_wifi(rx - 19, 4, sta, nucleo_setup_rssi());   rx -= 19 + 4;   // signal stays visible in a category too
        int cnt = node_child_count(node);
        if (cnt > 0) {
            char cc[12]; snprintf(cc, sizeof cc, "%d app", cnt);
            d.setTextColor(MUTED, INK); d.setCursor(rx - (int)strlen(cc) * 6, 4); d.print(cc);   // left of the gauges
        }
    } else {
        // Home (smartwatch face): a bold clock anchors the LEFT in the 16px-tall Font2 face; the
        // date, network name, Wi-Fi gauge and battery form ONE right-aligned cluster, packed from
        // the right edge inward, so the "what + where + signal + charge" status reads as a single
        // group. Drawn only when no filter chip claims the right.
        time_t now = time(NULL); struct tm *tm = localtime(&now);
        char t[8]; snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, 0); d.print(t);  // big white clock
        int clock_r = 6 + (int)d.textWidth(t);
        d.setFont(&fonts::Font0); d.setTextSize(1);

        if (!launcher_filter()[0]) {
            // Right cluster, laid out from the edge: [date]  [ssid] [|||]. Each piece is dropped if
            // it would crowd the clock, so a long SSID never overlaps the time.
            int rx = W - 2;
            draw_battery(rx - 15, 4);                          rx -= 15 + 4;   // battery far right
            draw_wifi(rx - 19, 4, sta, nucleo_setup_rssi());   rx -= 19 + 5;   // antenna gauge
            char net[12]; snprintf(net, sizeof net, "%.10s", sta ? nucleo_setup_ssid() : "Setup AP");
            int nw = (int)strlen(net) * 6;
            if (rx - nw > clock_r + 8) {
                d.setTextColor(sta ? MUTED : C_YELLOW, INK); d.setCursor(rx - nw, 4); d.print(net);
                rx -= nw + 8;
            }
            static const char *const WD[7]  = { "dom","lun","mar","mer","gio","ven","sab" };
            static const char *const MO[12] = { "gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic" };
            char dt[16] = "";
            if (tm && now > 1672531200) snprintf(dt, sizeof dt, "%s %d %s", WD[tm->tm_wday], tm->tm_mday, MO[tm->tm_mon]);
            int dw = (int)strlen(dt) * 6;
            if (dt[0] && rx - dw > clock_r + 6) { d.setTextColor(C_YELLOW, INK); d.setCursor(rx - dw, 4); d.print(dt); }
        }
    }

    if (launcher_filter()[0]) {
        // Filter chip, right-aligned so it never collides with the title/clock.
        char fb[20]; snprintf(fb, sizeof fb, "/%.10s", launcher_filter());
        int fw = (int)strlen(fb) * 6;
        d.fillRect(W - fw - 4, 0, fw + 4, BAR - 1, 0x1926);
        d.setTextColor(C_GREEN, 0x1926); d.setCursor(W - fw - 1, 4); d.print(fb);
    }

    d.drawFastHLine(0, BAR - 1, W, LINE);
}

// Once-a-second clock refresh WITHOUT wiping the bar. The full chrome repaint did a fillRect
// over all three bars every second, flashing them black; here we overwrite only the HH:MM
// digits in place (opaque background) so the top bar stays steady. At menu depth the bar shows
// a breadcrumb instead of a clock, so there is nothing to tick.
void launcher_render_clock_tick(void)
{
    if (launcher_depth() > 0) return;
    char ab[28]; const char *alert = offensive_alert(ab, sizeof ab);
    if (alert) {                                   // alert bar owns the strip: refresh the count in place
        d.fillRect(18, 0, W - 18, BAR - 1, C_RED);
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4); d.print(alert);
        return;
    }
    char t[8]; time_t now = time(NULL); struct tm *tm = localtime(&now);
    snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    d.fillRect(0, 0, 56, BAR - 1, INK);                       // wipe the clock cell (Font2 is wider than the old 6x8)
    d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, 0); d.print(t);
    d.setFont(&fonts::Font0); d.setTextSize(1);               // leave the global font at the framework default

    // Keep the battery glyph live while idle on the home face (it only sits in the right cluster
    // when no filter chip is up). Refresh just its 18px cell so the rest of the bar stays steady.
    if (!launcher_filter()[0]) {
        d.fillRect(W - 18, 0, 18, BAR - 1, INK);
        draw_battery(W - 17, 4);
    }
}

// Draw one footer line, centered inside its dark band and clipped to the band width so it
// can never bleed below its strip or run off the right edge. The classic font cell is 8 px
// tall (descenders included), so the vertical inset that keeps equal margins top and bottom
// is (band_h - 8) / 2. Horizontally we copy only as many 6-px glyphs as fit before the right
// margin, so a long string is hard-truncated instead of spilling past the screen.
static void footer_line(int y, int band_h, unsigned short bg, unsigned short fg,
                        int x, const char *text)
{
    int maxch = (W - x) / 6;                // 6 px per glyph; last column lands inside the screen
    if (maxch < 0) maxch = 0;
    if (maxch > 47) maxch = 47;
    char b[48];
    snprintf(b, sizeof b, "%.*s", maxch, text ? text : "");
    int ty = y + (band_h - 8) / 2;          // 8 px font cell -> equal top/bottom margin
    d.setTextSize(1); d.setTextColor(fg, bg); d.setCursor(x, ty); d.print(b);
}

// Retired: the focused-row description now lives inside the hero card (draw_list), reclaiming this
// strip for the list band. Kept as a no-op so the header symbol and call sites stay valid.
void launcher_render_instr_bar(void) { }

void launcher_render_hint_bar(void)
{
    int y = H - HINT;
    d.fillRect(0, y, W, HINT, s_hint_bg); d.drawFastHLine(0, y, W, LINE);
    footer_line(y, HINT, s_hint_bg, s_hint_fg, 5, s_hint);
}

void launcher_render_chrome(void) { launcher_render_status_bar(); launcher_render_hint_bar(); }

// ---- smooth-scroll animation state -----------------------------------------
static float s_smooth_y = 0.0f;

bool launcher_render_step_scroll(void)
{
    float target = (float)launcher_sel();
    s_smooth_y = target;
    return false;
}

// Hard-truncate `s` to the widest prefix that fits `w` px in the target's CURRENT font (set it
// before calling). Cheap O(n) shrink — n is a short menu label, so per-frame cost is negligible.
template <typename T> static void fit_text(T *c, const char *s, int w, char *out, int cap)
{
    int n = (int)strlen(s); if (n > cap - 1) n = cap - 1;
    for (; n > 0; n--) { memcpy(out, s, n); out[n] = 0; if ((int)c->textWidth(out) <= w) return; }
    out[0] = 0;
}

// ---- the focused-list band (Wear-OS style hero carousel) -------------------
// Rendered into a band-local coordinate space (top = `base`). For the off-screen canvas base = 0
// and the result is pushed at y = LIST_TOP; for the direct fallback base = LIST_TOP.
//
// Layout (the "use the whole panel" win): the selected app is a tall HERO card centred in the band
// — real anti-aliased FreeSans title + its one-line description right inside the card, so the screen
// explains the app without the user opening it. Neighbours flow above/below as slim rows that fade
// with distance and clip softly at the band edges, giving the scrollable-list feel of a smartwatch
// instead of three lonely pills on a near-empty screen.
template <typename T> static void draw_list(T *c, int base)
{
    if (nucleo_theme_has_bg_image()) {
        nucleo_theme_draw_bg_slice(c, 0, base, W, LIST_BAND_H);
    } else {
        c->fillRect(0, base, W, LIST_BAND_H, BG);
    }
    int n = launcher_visible_count();
    int sel = launcher_sel();
    if (n == 0) {
        c->setFont(&fonts::Font2); c->setTextColor(DIM, BG);
        c->setCursor(54, base + LIST_BAND_H / 2 - 8); c->print("Nessuna app");
        c->setFont(&fonts::Font0); c->setTextSize(1);
        return;
    }

    const int HERO_H = 38, ROW_H = 19;
    const int cy = base + LIST_BAND_H / 2;
    const int heroTop = cy - HERO_H / 2;

    c->setClipRect(0, base, W, LIST_BAND_H);                    // partials clip softly at the band edges

    for (int i = 0; i < n; i++) {
        const MenuNode *it = launcher_nth_visible(i);
        int dist = i - sel, y0, rh;
        if (dist == 0)      { y0 = heroTop;                          rh = HERO_H; }
        else if (dist < 0)  { rh = ROW_H; y0 = heroTop - (-dist) * ROW_H; }
        else                { rh = ROW_H; y0 = heroTop + HERO_H + (dist - 1) * ROW_H; }
        if (y0 + rh <= base || y0 >= base + LIST_BAND_H) continue;

        if (dist == 0) {
            // HERO card: rounded accent pill, circular icon badge, FreeSans title + a small desc line.
            c->fillRoundRect(6, y0, W - 12, HERO_H, 11, it->color);
            int bcy = y0 + HERO_H / 2;
            c->fillCircle(24, bcy, 13, INK);
            ui_icon(c, 24, bcy, 8, it->id, it->icon, it->color);   // vector glyph in the badge

            // Right-edge affordance: submenus show "N >" (how many inside), apps a lone chevron.
            int rx = W - 16;
            c->setFont(&fonts::Font0); c->setTextColor(INK, it->color);
            if (it->kind == N_MENU) {
                c->setTextSize(2); c->setCursor(W - 20, bcy - 8); c->print(">");
                int cnt = node_child_count(it);
                if (cnt > 0) {
                    char cb[4]; snprintf(cb, sizeof cb, "%d", cnt);
                    int cw = (int)strlen(cb) * 6;
                    c->setTextSize(1); c->setCursor(W - 24 - cw, bcy - 3); c->print(cb);
                    rx = W - 24 - cw - 4;
                } else rx = W - 24;
            } else {
                c->setTextSize(1); c->setCursor(W - 13, bcy - 3); c->print(">");
                rx = W - 17;
            }

            const int lx = 44, availw = rx - lx;
            const char *desc = (it->desc && it->desc[0]) ? it->desc : nullptr;
            char lb[28];
            c->setFont(&fonts::FreeSansBold9pt7b); c->setTextColor(INK, it->color);
            fit_text(c, it->label, availw, lb, sizeof lb);
            if (desc) {
                c->setCursor(lx, y0 + 3); c->print(lb);            // title (top)
                char db[44];
                c->setFont(&fonts::Font0); c->setTextSize(1);
                fit_text(c, desc, availw, db, sizeof db);
                c->setCursor(lx, y0 + HERO_H - 11); c->print(db);  // description (inside the card)
            } else {
                c->setCursor(lx, y0 + (HERO_H - 18) / 2); c->print(lb);   // single, vertically centred
            }
        } else {
            // Neighbour row: colour dot + label, fading with distance for the scrollable-list feel.
            bool near = (dist == -1 || dist == 1);
            int ny = y0 + ROW_H / 2;
            c->fillCircle(24, ny, near ? 4 : 3, near ? it->color : DIM);
            char lb[34];
            c->setFont(&fonts::Font0); c->setTextSize(1); c->setTextColor(near ? FG : DIM, BG);
            fit_text(c, it->label, W - 48, lb, sizeof lb);
            c->setCursor(36, ny - 3); c->print(lb);
        }
    }
    c->clearClipRect();

    // Scroll indicator: a rounded track + knob, tinted with the current menu's accent.
    if (n > 3) {
        int track = LIST_BAND_H - 10;
        int kh = track / n; if (kh < 10) kh = 10;
        int ky = base + 5 + (int)((track - kh) * (sel / (float)(n - 1)));
        c->fillRoundRect(W - 4, base + 5, 3, track, 1, LINE);
        c->fillRoundRect(W - 4, ky, 3, kh, 1, launcher_node()->color);
    }
    c->setFont(&fonts::Font0); c->setTextSize(1);              // leave the font at the framework default
}

// The list band composites into the shared back-buffer (see nucleo_ui.cpp). We draw the band
// into the top of that canvas and blit only the band region with a destination-clipped push,
// so the chrome below it is untouched. Decoder apps call nucleo_screen_release() directly to
// hand the canvas RAM to the codec; it re-acquires lazily here.
void launcher_render_list(void)
{
    M5Canvas *c = nucleo_screen();
    if (c) {
        draw_list(c, 0);                                        // band-local: rows 0..LIST_BAND_H of the canvas
        d.setClipRect(0, LIST_TOP, W, LIST_BAND_H);            // land it in the band region only...
        c->pushSprite(0, LIST_TOP);                            // ...one blit -> no flicker (rows below are clipped)
        d.clearClipRect();
    } else {
        // No canvas (heap fragmented after a radio session): draw direct, batched into ONE SPI
        // transaction so the clear→rows repaint is as quick as possible. Paired with the snap-scroll
        // above (no per-frame animation) this keeps the menu clean instead of a flickery mess.
        d.startWrite();
        draw_list(&d, LIST_TOP);
        d.endWrite();
    }
}

// ---- Control Center overlay (tabbed quick settings) -------------------------
// A full-screen sheet raised with TAB from anywhere, with navigation IDENTICAL to the Music /
// Video settings sheets (app_player.cpp / app_video.cpp):
//   · a persistent segmented tab bar; the header (s_cc_row == -1) is the tab cursor;
//   · RIGHT / LEFT page to the next / previous tab from ANYWHERE (header or a row), never close;
//   · DOWN from the header drops into the rows; UP from row 0 returns to the header;
//   · UP/DOWN walk the rows; ENTER acts (toggle / cycle / play-pause, or arms a slider);
//   · a slider arms an EDIT mode on ENTER — then UP/RIGHT raise, DOWN/LEFT lower, ENTER/Esc end;
//   · Esc/Back pops one level hierarchically: edit -> row -> header -> close the sheet (Tab also closes).
// The look mirrors the Music/Video sheet too: same palette, segmented tabs, accent-railed
// carousel rows, slider knobs, toggle pills and value chips. The RETE tab renders the IP and
// pairing PIN at size-2 so they read across the room.
//
// Palette mirrors app_player.cpp / app_video.cpp so the sheet matches the Music/Video look.
static const unsigned short CC_BG   = 0x0841;  // void-blue sheet background
static const unsigned short CC_SURF = 0x10A2;  // raised surface / slider track
static const unsigned short CC_CAP  = 0x1A8B;  // focused capsule background
static const unsigned short CC_ACC  = 0x4DDF;  // bright-blue accent (tab + rail)
static const unsigned short CC_GRN  = 0x8FF3;  // positive / playing

// launcher_render_control_center_key return codes (so the launcher can pop/close hierarchically).
enum { CC_NONE = 0, CC_REDRAW = 1, CC_CLOSE = 2 };

enum { CC_TEXT = 0, CC_NOTE, CC_SLIDER, CC_TOGGLE, CC_CYCLE, CC_ACTION, CC_BIG, CC_PLAY };   // row kinds
// Action ids — the durable identity of each interactive row, so the key handler dispatches on
// MEANING (not on a fragile tab+row coordinate that shifts when a dynamic row appears).
enum { A_NONE = 0, A_BRIGHT, A_VOLUME, A_MUTE, A_PLAY,
       A_ONLINE, A_L1, A_RESET, A_TTS, A_LISTEN, A_THEME, A_USB, A_REBOOT };

static const char *const CC_TABS[5] = { "RAPIDE", "ANIMA", "VOCE", "RETE", "SISTEMA" };
static const int CC_NTABS = 5;
static const char *const CC_L1_LABEL[3] = { "AUTO", "ON", "OFF" };

static int  s_cc_tab     = 0;       // 0=RAPIDE 1=ANIMA 2=VOCE 3=RETE 4=SISTEMA
static int  s_cc_row     = -1;      // -1 = tab header, 0..n-1 = row
static bool s_cc_edit    = false;   // a slider is in adjust mode (UP/DN/L/R change it)
static int  s_cc_confirm = A_NONE;  // a destructive action awaiting a second ENTER (USB/Riavvia)

struct CcItem { const char *label; char val[24]; int kind; int act; int slider; bool on; unsigned short col; };

// Theme cycling (SISTEMA tab): the OS theme registry is the single source of truth.
static const char *cc_theme_name(void)
{
    int cnt = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&cnt);
    const char *cur = nucleo_theme_get_current();
    for (int i = 0; i < cnt; i++) if (cur && all[i].id && !strcmp(all[i].id, cur)) return all[i].name;
    return cur ? cur : "?";
}
static void cc_theme_cycle(int dir)
{
    int cnt = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&cnt);
    if (!all || cnt <= 0) return;
    const char *cur = nucleo_theme_get_current();
    int idx = 0; for (int i = 0; i < cnt; i++) if (cur && all[i].id && !strcmp(all[i].id, cur)) { idx = i; break; }
    idx = (idx + dir + cnt) % cnt;
    nucleo_theme_set(all[idx].id);
}

// Build the rows of the active tab into `it` (max 6). Returns the row count.
static int cc_build(CcItem *it)
{
    memset(it, 0, sizeof(CcItem) * 6);
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0];
    if (s_cc_tab == 0) {                                       // RAPIDE — luce, audio
        int n = 0;
        it[n].label = "Luminosita"; it[n].kind = CC_SLIDER; it[n].act = A_BRIGHT; it[n].slider = nucleo_app_brightness(); it[n].col = C_YELLOW; n++;
        it[n].label = "Volume";     it[n].kind = CC_SLIDER; it[n].act = A_VOLUME; it[n].slider = nucleo_audio_volume();   it[n].col = CC_GRN;   n++;
        it[n].label = "Muto";       it[n].kind = CC_TOGGLE; it[n].act = A_MUTE;   it[n].on = nucleo_audio_is_muted();     it[n].col = CC_GRN;   n++;
        if (nucleo_audio_is_playing()) {
            const char *p = nucleo_audio_path(); const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
            it[n].label = "In riprod."; it[n].kind = CC_PLAY; it[n].act = A_PLAY;
            snprintf(it[n].val, sizeof it[n].val, "%.21s", base);
            it[n].col = nucleo_audio_is_paused() ? C_YELLOW : CC_GRN; n++;
        }
        return n;
    }
    if (s_cc_tab == 1) {                                       // ANIMA — il cervello dell'OS
        it[0].label = "Online"; it[0].kind = CC_TOGGLE; it[0].act = A_ONLINE; it[0].on = nucleo_anima_online_enabled(); it[0].col = CC_GRN;
        int lm = nucleo_anima_l1_get_mode(); if (lm < 0 || lm > 2) lm = 0;
        it[1].label = "Cervello"; it[1].kind = CC_CYCLE; it[1].act = A_L1; it[1].col = C_BLUE;
        snprintf(it[1].val, sizeof it[1].val, "%s", CC_L1_LABEL[lm]);
        char prov[20] = "", model[24] = "";
        it[2].label = "Modello"; it[2].kind = CC_TEXT; it[2].col = MUTED;
        if (nucleo_anima_teacher_info(prov, sizeof prov, model, sizeof model)) {
            // "anthropic" -> Claude, otherwise show the provider word as-is (Grok/Groq/Gemini).
            const char *brand = !strcmp(prov, "anthropic") ? "Claude" : (prov[0] ? prov : "cloud");
            snprintf(it[2].val, sizeof it[2].val, "%.18s", brand);
        } else snprintf(it[2].val, sizeof it[2].val, "locale");
        it[3].label = "Reset chat"; it[3].kind = CC_ACTION; it[3].act = A_RESET; it[3].col = C_YELLOW;
        return 4;
    }
    if (s_cc_tab == 2) {                                       // VOCE — TTS + ascolto
        it[0].label = "Voce TTS"; it[0].kind = CC_TOGGLE; it[0].act = A_TTS;
        it[0].on = nucleo_tts_enabled(); it[0].col = CC_GRN;
        it[1].label = "Ascolto"; it[1].kind = CC_TOGGLE; it[1].act = A_LISTEN;
        it[1].on = nucleo_voice_always_on(); it[1].col = CC_GRN;
        it[2].label = "Pacchetto"; it[2].kind = CC_TEXT; it[2].col = MUTED;
        snprintf(it[2].val, sizeof it[2].val, "%s", nucleo_tts_available() ? "installato" : "assente");
        return 3;
    }
    if (s_cc_tab == 3) {                                       // RETE — indirizzo, PIN, segnale
        it[0].label = "Rete"; it[0].kind = CC_NOTE; it[0].col = FG;   // compact, small line
        snprintf(it[0].val, sizeof it[0].val, "%.20s", sta ? nucleo_setup_ssid() : "Setup AP");
        it[1].label = "IP"; it[1].kind = CC_BIG; it[1].col = CC_GRN;
        snprintf(it[1].val, sizeof it[1].val, "%s", sta ? nucleo_setup_ip() : "192.168.4.1");
        it[2].label = "PIN"; it[2].kind = CC_BIG; it[2].col = C_YELLOW;
        snprintf(it[2].val, sizeof it[2].val, "%s", nucleo_auth_pin());
        it[3].label = "Segnale"; it[3].kind = CC_TEXT; it[3].col = MUTED;
        int rssi = nucleo_setup_rssi();
        if (sta && rssi < 0) snprintf(it[3].val, sizeof it[3].val, "%d dBm", rssi);
        else                 snprintf(it[3].val, sizeof it[3].val, "%s", sta ? "ok" : "AP");
        return 4;
    }
    // SISTEMA — tema, USB, riavvio, stato
    it[0].label = "Tema"; it[0].kind = CC_CYCLE; it[0].act = A_THEME; it[0].col = C_BLUE;
    snprintf(it[0].val, sizeof it[0].val, "%.10s", cc_theme_name());
    it[1].label = "USB Drive"; it[1].kind = CC_ACTION; it[1].act = A_USB;    it[1].col = C_BLUE;
    it[2].label = "Riavvia";   it[2].kind = CC_ACTION; it[2].act = A_REBOOT; it[2].col = C_RED;
    it[3].label = "RAM"; it[3].kind = CC_TEXT; it[3].col = MUTED;
    snprintf(it[3].val, sizeof it[3].val, "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    it[4].label = "Nome"; it[4].kind = CC_TEXT; it[4].col = MUTED;
    snprintf(it[4].val, sizeof it[4].val, "%.10s", nucleo_setup_device_name());
    char tt[8]; time_t now = time(NULL); struct tm *tm = localtime(&now);
    snprintf(tt, sizeof tt, "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    it[5].label = "Ora"; it[5].kind = CC_TEXT; it[5].col = MUTED;
    snprintf(it[5].val, sizeof it[5].val, "%s%s", tt, nucleo_setup_time_synced() ? "" : " ?");
    return 6;
}

void launcher_render_control_center_open(void) { s_cc_tab = 0; s_cc_row = -1; s_cc_edit = false; s_cc_confirm = A_NONE; }

// Adjust the focused slider by delta, dispatching on the row's action id (robust to layout).
static void cc_slider_adjust(int delta)
{
    CcItem it[6]; int n = cc_build(it);
    if (s_cc_row < 0 || s_cc_row >= n) return;
    if (it[s_cc_row].act == A_VOLUME) nucleo_audio_set_volume(nucleo_audio_volume() + delta);
    else                              nucleo_app_set_brightness(nucleo_app_brightness() + delta);
}

int launcher_render_control_center_key(int key, char ch)
{
    (void)ch;
    CcItem it[6]; int n = cc_build(it);
    if (s_cc_row >= n) s_cc_row = n - 1;

    // --- slider adjust mode (identical to Music/Video edit) ---
    if (s_cc_edit) {
        if      (key == NK_RIGHT || key == NK_UP)   cc_slider_adjust(+5);
        else if (key == NK_LEFT  || key == NK_DOWN) cc_slider_adjust(-5);
        else if (key == NK_ENTER || key == NK_BACK) s_cc_edit = false;
        return CC_REDRAW;
    }

    // Any non-ENTER keystroke abandons a pending destructive confirmation.
    if (key != NK_ENTER) s_cc_confirm = A_NONE;

    // --- RIGHT / LEFT page the tabs from anywhere (header or a row); they never close ---
    if (key == NK_RIGHT) {
        s_cc_tab = (s_cc_tab + 1) % CC_NTABS;
        if (s_cc_row >= 0) s_cc_row = 0;
        return CC_REDRAW;
    }
    if (key == NK_LEFT) {
        s_cc_tab = (s_cc_tab + CC_NTABS - 1) % CC_NTABS;
        if (s_cc_row >= 0) s_cc_row = 0;
        return CC_REDRAW;
    }

    // --- Esc/Back: pop one level (row -> header -> close the sheet) ---
    if (key == NK_BACK) {
        if (s_cc_row >= 0) { s_cc_row = -1; return CC_REDRAW; }
        return CC_CLOSE;
    }

    // --- header (tab cursor) ---
    if (s_cc_row == -1) {
        if (key == NK_DOWN) { s_cc_row = 0; return CC_REDRAW; }
        return CC_NONE;
    }

    // --- rows ---
    if (key == NK_UP)   { s_cc_row = (s_cc_row > 0) ? s_cc_row - 1 : -1; return CC_REDRAW; }  // row 0 -> header
    if (key == NK_DOWN) { if (s_cc_row < n - 1) s_cc_row++; return CC_REDRAW; }
    if (key == NK_ENTER) {
        int act = it[s_cc_row].act;
        if (it[s_cc_row].kind == CC_SLIDER) { s_cc_edit = true; return CC_REDRAW; }
        switch (act) {
            case A_MUTE:   nucleo_audio_set_mute(!nucleo_audio_is_muted());          return CC_REDRAW;
            case A_PLAY:   nucleo_audio_toggle_pause();                              return CC_REDRAW;
            case A_ONLINE: nucleo_anima_set_online(!nucleo_anima_online_enabled());  return CC_REDRAW;
            case A_L1:     nucleo_anima_l1_set_mode((nucleo_anima_l1_get_mode() + 1) % 3); return CC_REDRAW;
            case A_RESET:  nucleo_anima_reset_session();                             return CC_REDRAW;
            case A_TTS:    nucleo_tts_set_enabled(!nucleo_tts_enabled());            return CC_REDRAW;
            case A_LISTEN: nucleo_voice_set_always_on(!nucleo_voice_always_on());    return CC_REDRAW;
            case A_THEME:  cc_theme_cycle(+1);                                       return CC_REDRAW;
            case A_USB:    case A_REBOOT:
                // Two-step: first ENTER arms the confirm, second ENTER on the same row fires.
                if (s_cc_confirm == act) { if (act == A_USB) nucleo_usbmsc_request(); else esp_restart(); }
                else s_cc_confirm = act;
                return CC_REDRAW;
            default: return CC_NONE;
        }
    }
    return CC_NONE;
}

// Persistent segmented tab bar (mirrors app_video.cpp draw_tabbar).
template <typename T> static void cc_tabbar(T *g, int active, bool hdr)
{
    int seg = W / CC_NTABS;
    for (int i = 0; i < CC_NTABS; i++) {
        int x = i * seg, tw = (int)strlen(CC_TABS[i]) * 6;
        if (i == active) {
            g->fillRoundRect(x + 3, 3, seg - 6, 17, 8, hdr ? CC_ACC : CC_SURF);
            g->setTextSize(1); g->setTextColor(hdr ? INK : FG, hdr ? CC_ACC : CC_SURF);
            g->setCursor(x + (seg - tw) / 2, 8); g->print(CC_TABS[i]);
        } else {
            g->setTextSize(1); g->setTextColor(hdr ? MUTED : DIM, CC_BG);
            g->setCursor(x + (seg - tw) / 2, 8); g->print(CC_TABS[i]);
        }
    }
    g->drawFastHLine(0, 23, W, LINE);
}

// One settings row, styled exactly like the Music/Video sheet (app_video.cpp draw_set_row):
// rounded capsule + accent rail when focused, label at size-2, then a kind-specific control.
template <typename T> static void cc_set_row(T *g, int y, bool focus, const CcItem *it, bool edit)
{
    int h = focus ? 50 : 32;
    unsigned short rbg = focus ? CC_CAP : CC_BG;
    g->fillRoundRect(4, y, 232, h - 2, 9, rbg);
    if (focus) g->fillRoundRect(4, y + 3, 5, h - 8, 2, CC_ACC);   // accent rail

    if (it->kind == CC_BIG) {
        // Small label, then the value at size-2 below it — readability win for IP/PIN.
        g->setTextSize(1); g->setTextColor(focus ? FG : MUTED, rbg);
        g->setCursor(16, y + (focus ? 8 : 5)); g->print(it->label);
        g->setTextSize(2); g->setTextColor(it->col, rbg);
        g->setCursor(16, y + (focus ? 24 : 15)); g->print(it->val);
        return;
    }

    if (it->kind == CC_NOTE) {
        // Compact single line, all size-1: small grey label + small value (e.g. the SSID),
        // deliberately less prominent than the big IP/PIN rows below it.
        g->setTextSize(1);
        g->setTextColor(MUTED, rbg);
        g->setCursor(16, y + (h - 8) / 2); g->print(it->label);
        g->setTextColor(focus ? it->col : MUTED, rbg);
        g->setCursor(16 + ((int)strlen(it->label) + 1) * 6, y + (h - 8) / 2); g->print(it->val);
        return;
    }

    g->setTextSize(2); g->setTextColor(focus ? FG : MUTED, rbg);
    g->setCursor(16, y + (h - 16) / 2 - 1); g->print(it->label);

    if (it->kind == CC_SLIDER) {
        int sw = focus ? 96 : 64, sh = 12, bx = 230 - sw, vy = y + (h - sh) / 2;
        g->fillRoundRect(bx, vy, sw, sh, sh / 2, CC_SURF);
        int onw = it->slider * sw / 100; if (onw < 0) onw = 0; if (onw > sw) onw = sw;
        if (onw > 0) g->fillRoundRect(bx, vy, onw, sh, sh / 2, it->col);
        int kx = bx + onw; if (kx < bx + 6) kx = bx + 6; if (kx > bx + sw - 6) kx = bx + sw - 6;
        g->fillCircle(kx, vy + sh / 2, edit ? sh / 2 + 2 : sh / 2 + 1, FG);
        if (edit) g->drawRoundRect(bx - 2, vy - 2, sw + 4, sh + 4, (sh + 4) / 2, CC_ACC);
        return;
    }
    if (it->kind == CC_TOGGLE) {
        int sw = 42, sh = 20, bx = 230 - sw, vy = y + (h - sh) / 2;
        g->fillRoundRect(bx, vy, sw, sh, sh / 2, it->on ? CC_GRN : CC_SURF);
        int kx = it->on ? bx + sw - sh / 2 - 1 : bx + sh / 2 + 1;
        g->fillCircle(kx, vy + sh / 2, sh / 2 - 3, it->on ? INK : MUTED);
        return;
    }
    if (it->kind == CC_PLAY) {
        unsigned short fg = it->col;
        g->setTextSize(1); g->setTextColor(fg, rbg);
        g->setCursor(150, y + (h - 16) / 2);      g->print(it->col == C_YELLOW ? "|| pausa" : "> in play");
        g->setTextColor(focus ? FG : MUTED, rbg);
        g->setCursor(150, y + (h - 16) / 2 + 10); g->print(it->val);
        return;
    }
    if (it->kind == CC_ACTION) {
        // A chevron button by default; once armed it turns into a red "Confermi?" prompt.
        if (s_cc_confirm == it->act) {
            const char *q = "Confermi?";
            g->setTextSize(1); g->setTextColor(C_RED, rbg);
            g->setCursor(230 - (int)strlen(q) * 6, y + (h - 8) / 2); g->print(q);
        } else {
            int bw = 28, bh = 22, bx = 230 - bw, vy = y + (h - bh) / 2;
            g->fillRoundRect(bx, vy, bw, bh, 6, focus ? it->col : CC_SURF);
            unsigned short ar = focus ? INK : MUTED;
            int ax = bx + bw / 2 - 2, ay = vy + bh / 2;
            g->fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, ar);    // chevron
        }
        return;
    }
    if (it->kind == CC_CYCLE && it->val[0]) {
        // Cycled value: a size-2 pill, right-aligned (Tema / Cervello / ...).
        int vw = (int)strlen(it->val) * 12 + 14, vh = 22, bx = 230 - vw, vy = y + (h - vh) / 2;
        if (focus) g->fillRoundRect(bx, vy, vw, vh, 6, CC_SURF);
        g->setTextSize(2); g->setTextColor(it->col, focus ? CC_SURF : rbg);
        g->setCursor(bx + 7, vy + 3); g->print(it->val);
        return;
    }
    // CC_TEXT read-only value, size-1, right-aligned (RSSI / RAM / model / time ...).
    if (it->val[0]) {
        g->setTextSize(1); g->setTextColor(it->col, rbg);
        g->setCursor(230 - (int)strlen(it->val) * 6, y + (h - 8) / 2); g->print(it->val);
    }
}

// Compose the whole sheet into `g` (an off-screen canvas, or the display as a fallback).
// Layout mirrors app_video.cpp draw_settings: a dimmed top-down preview while the header is
// focused, then a centered carousel (focused row enlarged) once inside the rows.
template <typename T> static void cc_draw(T *g)
{
    const int CH = H;
    g->fillScreen(CC_BG);
    bool hdr = (s_cc_row == -1);
    cc_tabbar(g, s_cc_tab, hdr);

    CcItem it[6]; int n = cc_build(it);
    if (s_cc_row >= n) s_cc_row = n - 1;

    g->setClipRect(0, 24, W, CH - 36);
    if (hdr) {
        int y = 30;                                          // dimmed preview; DOWN to focus
        for (int i = 0; i < n && y < CH - 12; i++) { cc_set_row(g, y, false, &it[i], false); y += 34; }
    } else {
        int cy = (28 + CH) / 2, f = s_cc_row;
        for (int i = 0; i < n; i++) {
            int dist = i - f, h = (dist == 0) ? 50 : 32, y;
            if (dist == 0)     y = cy - h / 2;
            else if (dist < 0) y = cy - 25 + dist * 32;
            else               y = cy + 25 + (dist - 1) * 32;
            if (y + h > 24 && y < CH - 12)
                cc_set_row(g, y, i == f, &it[i], i == f && s_cc_edit);
        }
    }
    g->clearClipRect();

    // Footer hint (Italian, matches the Music/Video sheet style) — context aware.
    const char *hint; unsigned short hc = DIM;
    int fk = (!hdr && s_cc_row >= 0 && s_cc_row < n) ? it[s_cc_row].kind : -1;
    if      (s_cc_edit)          hint = "L/R regola   ENTER ok";
    else if (hdr)                hint = "L/R scheda   DOWN righe   ESC chiudi";
    else if (s_cc_confirm != A_NONE) { hint = "ENTER conferma   ESC annulla"; hc = C_RED; }
    else if (fk == CC_ACTION)    hint = "ENTER esegui   L/R scheda";
    else if (fk == CC_TOGGLE)    hint = "ENTER attiva/disattiva   L/R scheda";
    else if (fk == CC_CYCLE)     hint = "ENTER cambia   L/R scheda";
    else if (fk == CC_SLIDER)    hint = "ENTER regola   L/R scheda";
    else if (fk == CC_PLAY)      hint = "ENTER play/pausa   L/R scheda";
    else                         hint = "su/giu riga   L/R scheda   ESC indietro";
    g->setTextSize(1); g->setTextColor(hc, CC_BG); g->setCursor(8, CH - 10); g->print(hint);
}

// The sheet is a full-screen overlay repainted every second (clock + Now-Playing time) and on
// every adjust key. Drawing it straight to the display means fillScreen(INK) + a full redraw
// flash blank-then-paint = flicker. So we composite into the shared back-buffer and blit in ONE
// pushSprite. No separate allocation: the launcher list underneath isn't drawing while the
// sheet is up, so the one permanent buffer serves both (the buffer is taller than the screen;
// the panel clips the extra rows on push).
void launcher_render_control_center(void)
{
    M5Canvas *c = nucleo_screen();
    if (c) { cc_draw(c); c->pushSprite(0, 0); }            // one blit -> no flicker
    else   { cc_draw(&d); }                                // low-heap fallback: direct (may flicker)
}

void launcher_render_control_center_close(void) { }       // no-op: buffer is shared (kept for call sites)
