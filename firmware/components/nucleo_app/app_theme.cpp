// app_theme.cpp — NucleoOS "Theme": native theme picker (the System-menu "theme" app).
//
// Replaces the old stub in nucleo_setup.c, whose on_enter called the BLOCKING nucleo_ui_menu()
// with on_draw == NULL — that modal does not render inside the app run-loop, so the app opened
// to an empty screen. This is a proper framework app (on_enter/on_key/on_draw) that lists every
// registered theme with a live colour preview (background + "Aa" text + accent block) and an
// "in use" marker, and applies the chosen one on ENTER. nucleo_theme_set() recolours the whole
// OS immediately and persists the choice, so the app repaints in the new palette as confirmation.
//
// Same registration identity as the old stub (id "theme", System category) so it occupies the
// very same menu slot — there is exactly ONE theme app, no duplicate.

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"     // W/H/HINT + themed BG/FG/MUTED/DIM/LINE/INK + C_* accents (pulls nucleo_theme.h)
#include "nucleo_theme.h"       // theme registry: get_all / get_current / set + THEME_ACC
#include "app_gfx.h"
#include "nucleo_i18n.h"        // TR(it,en): UI labels follow the system language
#include "app_large.h"          // two screens: the list + live preview on the 320x240 panel, the Cardputer companion
#include "nucleo_touch.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

static const unsigned short CAP = 0x1A8B;   // focused-row capsule (same tint as the Settings panel)

static int s_sel = 0;                        // highlighted theme (not necessarily the active one)
static int s_lg_scroll = 0;                  // two screens: the panel list's first visible row

static int theme_count(void){ int c=0; nucleo_theme_get_all(&c); return c; }
static int active_index(void){
    int c=0; const nucleo_theme_t *all=nucleo_theme_get_all(&c);
    const char *cur=nucleo_theme_get_current();
    for(int i=0;i<c;i++) if(cur && all[i].id && !strcmp(all[i].id,cur)) return i;
    return 0;
}

static void apply_selected(void)             // apply the highlighted theme (live + persisted)
{
    int c=0; const nucleo_theme_t *all=nucleo_theme_get_all(&c);
    if(s_sel>=0 && s_sel<c) nucleo_theme_set(all[s_sel].id);
}

// Left/Right do nothing here (no tabs); only Esc closes — matching the Settings-app convention.
static bool on_back(int key){ return key==NK_LEFT; }   // consume Left, let Esc fall through to close

static void on_enter(void){
    nucleo_app_set_direct_draw(true);   // static list UI: draw direct, free the 32 KB menu buffer
    s_sel = active_index();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR5("SU/GIU scegli  INVIO applica  Esc", "UP/DN pick  ENTER apply  Esc", "AR/AB elige  INTRO aplica  Esc", "H/B choix  ENTREE appliq.  Esc", "AUF/AB Wahl  ENTER setzen  Esc"));
    nucleo_app_request_draw();
}

static void on_key(int k,char ch){
    int n=theme_count(); if(n<=0) return;
    if(nucleo_app_two_screens()){                                 // panel: tap a row to select, tap again to apply
        int t=lg::row_tap(k,ch,s_lg_scroll,n,&s_sel);
        if(t==2){ apply_selected(); nucleo_app_request_draw(); return; }
        if(t==1){ nucleo_app_request_draw(); return; }
    }
    if(k==NK_UP)        s_sel=(s_sel-1+n)%n;
    else if(k==NK_DOWN) s_sel=(s_sel+1)%n;
    else if(k==NK_ENTER){ apply_selected(); }                     // ENTER (and a preview tap) apply the theme
    else return;
    nucleo_app_request_draw();
}

// One theme row: "in use" marker + name (size 2, truncated to fit) + a live preview chip that
// renders the theme's own background, "Aa" in its text colour and an accent block.
static void draw_theme_row(int y,int h,const nucleo_theme_t *t,bool focus,bool active){
    int cy=y+h/2;
    if(focus) d.fillRoundRect(4,y+1,W-8,h-3,7,CAP);
    uint16_t bg = focus?CAP:BG;

    // active marker: filled green disc when in use, hollow grey ring otherwise
    if(active) d.fillCircle(11,cy,4,C_GREEN);
    else       d.drawCircle(11,cy,4,MUTED);

    // preview chip (right): the theme's own bg + "Aa" in its fg + an accent block
    int cw=36, cx=W-8-cw, chy=cy-8, chh=16;
    d.fillRoundRect(cx,chy,cw,chh,3,t->bg);
    d.drawRoundRect(cx,chy,cw,chh,3,MUTED);                       // border so bg-on-bg stays visible
    d.setTextSize(1); d.setTextColor(t->fg,t->bg); d.setCursor(cx+4,chy+4); d.print("Aa");
    d.fillRect(cx+cw-12,chy+3,9,chh-6,t->acc);

    // name (size 2), truncated to the room left of the chip
    int nx=20, maxw=cx-6-nx, maxc=maxw/12; if(maxc<1)maxc=1; if(maxc>23)maxc=23;
    char nm[24]; snprintf(nm,sizeof nm,"%.*s",maxc,t->name?t->name:"?");
    d.setTextSize(2); d.setTextColor((focus||active)?FG:MUTED,bg); d.setCursor(nx,cy-8); d.print(nm);
}

// ===== two screens ==============================================================================================
// The external panel shows the whole picker: the theme list on the left and a LIVE full-panel preview of the
// selected theme on the right — a little mock OS screen painted in that theme's own palette (background, title
// bar, a selected row, body + secondary text, a separator, an accent button), so the choice is obvious before
// it is applied. The Cardputer is the companion: the selected theme, its swatches and the keys. Touch: tap a
// row to select (the preview follows), tap again — or tap the preview — to apply it.
static const int T_LISTW = 150;

static void lg_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    int c = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&c);
    const bool active = (i == active_index());
    // swatch of the theme's accent at the row's right edge
    d.fillRoundRect(x + w - 22, y + h / 2 - 7, 16, 14, 3, all[i].acc);
    d.drawRoundRect(x + w - 22, y + h / 2 - 7, 16, 14, 3, focus ? INK : MUTED);
    lg::row_text(x, y, w - 26, h, focus, bg, all[i].name ? all[i].name : "?",
                 active ? TR("in uso", "in use") : nullptr, active ? C_GREEN : FG);
}

static void lg_preview(int x, int y, int w, int h, const nucleo_theme_t *t)
{
    d.fillRoundRect(x, y, w, h, 8, t->bg);
    d.drawRoundRect(x, y, w, h, 8, t->line);
    const int ix = x + 10, iw = w - 20;
    d.fillRoundRect(ix, y + 10, iw, 26, 6, t->acc);               // title bar
    lg::font(1); d.setTextDatum(middle_left); d.setTextColor(nucleo_theme_ink_on(t->acc, t->ink), t->acc);
    d.drawString("NucleoOS", ix + 8, y + 10 + 13);
    int ry = y + 46;                                              // a selected row + body + secondary text
    d.fillRoundRect(ix, ry, iw, 24, 5, t->sel);
    lg::font(0); d.setTextColor(nucleo_theme_ink_on(t->sel, t->fg), t->sel);
    d.drawString(TR("Voce selezionata", "Selected item"), ix + 8, ry + 12); ry += 30;
    d.setTextColor(t->fg, t->bg); d.drawString("Aa Bb Cc  0123", ix + 8, ry + 10); ry += 24;
    d.setTextColor(t->muted, t->bg); d.drawString(TR("testo secondario", "secondary text"), ix + 8, ry + 10); ry += 22;
    d.drawFastHLine(ix, ry, iw, t->line); ry += 2;
    d.setTextColor(t->dim, t->bg); d.drawString(TR("riga spenta", "dim line"), ix + 8, ry + 10);
    d.fillRoundRect(ix, y + h - 40, iw, 30, 8, t->acc);           // accent button
    d.setTextDatum(middle_center); d.setTextColor(nucleo_theme_ink_on(t->acc, t->ink), t->acc);
    d.drawString(TR("Pulsante", "Button"), ix + iw / 2, y + h - 40 + 15);
    d.setTextDatum(top_left);
}

static void draw_large(void)
{
    const int Wd = d.width(), Hd = d.height();
    nucleo_touch_clear();
    int c = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&c);
    const int ai = active_index();
    char r[28]; snprintf(r, sizeof r, "%s", (c > 0 && all[ai].name) ? all[ai].name : "");
    const int y = lg::header(TR("Tema", "Theme"), r, THEME_ACC, NK_BACK, 0);
    if (c <= 0) { lg::message(0, y, Wd, Hd - y, TR("Nessun tema", "No themes"), nullptr, MUTED); nucleo_touch_commit(); return; }
    if (s_sel >= c) s_sel = c - 1;
    lg::list(0, y + 2, T_LISTW, Hd - y - 4, lg::ROW, c, s_sel, &s_lg_scroll, lg_row, nullptr, THEME_ACC);
    d.fillRect(T_LISTW, y, Wd - T_LISTW, Hd - y, BG);
    const int px = T_LISTW + 4, py = y + 6, pw = Wd - T_LISTW - 10, ph = Hd - y - 12;
    lg_preview(px, py, pw, ph, &all[s_sel]);
    nucleo_touch_add(px, py, pw, ph, NK_ENTER, 0);               // tap the preview to apply the theme
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);
    const int y = lg::comp_head("TEMA", THEME_ACC);
    int c = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&c);
    if (c <= 0) { d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(MUTED, BG); d.drawString(TR("Nessun tema", "No themes"), 8, y + 10); return; }
    if (s_sel >= c) s_sel = c - 1;
    const nucleo_theme_t *t = &all[s_sel];
    const bool active = (s_sel == active_index());
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(active ? C_GREEN : FG, BG);
    d.drawString(t->name ? t->name : "?", 8, y + 8);
    // swatches: bg / fg / acc / sel
    const uint16_t sw[4] = { t->bg, t->fg, t->acc, t->sel };
    for (int i = 0; i < 4; i++) { d.fillRoundRect(8 + i * 30, y + 34, 26, 18, 3, sw[i]); d.drawRoundRect(8 + i * 30, y + 34, 26, 18, 3, MUTED); }
    char m[32]; snprintf(m, sizeof m, "%d / %d%s", s_sel + 1, c, active ? TR("  (in uso)", "  (in use)") : "");
    d.setFont(&fonts::Font0); d.setTextColor(DIM, BG); d.drawString(m, 8, y + 58);
    lg::comp_keys(TR("su/giu scegli  INVIO applica", "up/dn pick  ENTER apply"));
}

static void on_draw(void){
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int ch=nucleo_app_content_height();
    d.fillRect(0,0,W,ch,BG);

    // header: "Theme" + the active theme's name, in the live accent so it reflects the choice
    d.setTextSize(2); d.setTextColor(THEME_ACC,BG); d.setCursor(10,4); d.print(TR5("Tema","Theme", "Tema", "Theme", "Design"));
    int c=0; const nucleo_theme_t *all=nucleo_theme_get_all(&c);
    int ai=active_index();
    if(c>0 && all[ai].name){ const char *cn=all[ai].name; int w=(int)strlen(cn)*6;
        d.setTextSize(1); d.setTextColor(MUTED,BG); d.setCursor(W-10-w,11); d.print(cn); }
    d.drawFastHLine(10,22,W-20,LINE);

    if(c<=0){ d.setTextSize(1); d.setTextColor(MUTED,BG); d.setCursor(10,40); d.print(TR5("Nessun tema","No themes", "Sin temas", "Pas de themes", "Keine designs")); return; }

    // adaptive list: fill the band for the few themes we ship; scroll only if it ever grows large
    int top=26, band=ch-top;
    int pitch=band/c; if(pitch>40)pitch=40;
    bool scroll=(pitch<22); if(scroll)pitch=22;
    int visible=scroll?(band/pitch):c; if(visible>c)visible=c;
    int first=0;
    if(scroll){ first=s_sel-visible/2; if(first>c-visible)first=c-visible; if(first<0)first=0; }
    int shown=scroll?visible:c;
    int y0=top+(band-shown*pitch)/2; if(y0<top)y0=top;

    d.setClipRect(0,top,W,band);
    for(int k=0;k<shown;k++){
        int i=first+k; if(i>=c)break;
        draw_theme_row(y0+k*pitch,pitch,&all[i],i==s_sel,i==ai);
    }
    d.clearClipRect();

    if(scroll){                                                  // slim scrollbar (watch-style)
        int trk=band-4,kh=trk*visible/c; if(kh<12)kh=12;
        int mo=c-visible,ky=top+2+(mo>0?(trk-kh)*first/mo:0);
        d.fillRoundRect(W-3,top+2,2,trk,1,LINE);
        d.fillRoundRect(W-3,ky,2,kh,1,THEME_ACC);
    }
}

extern "C" void nucleo_register_theme(void){
    static const nucleo_app_def_t app={
        "theme","Theme","System","Change UI colors and look",
        'T',0xFBB6,on_enter,on_key,nullptr,on_draw,nullptr,0,nullptr,draw_large
    };
    nucleo_app_register(&app);
}
