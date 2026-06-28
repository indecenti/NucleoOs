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
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

static const unsigned short CAP = 0x1A8B;   // focused-row capsule (same tint as the Settings panel)

static int s_sel = 0;                        // highlighted theme (not necessarily the active one)

static int theme_count(void){ int c=0; nucleo_theme_get_all(&c); return c; }
static int active_index(void){
    int c=0; const nucleo_theme_t *all=nucleo_theme_get_all(&c);
    const char *cur=nucleo_theme_get_current();
    for(int i=0;i<c;i++) if(cur && all[i].id && !strcmp(all[i].id,cur)) return i;
    return 0;
}

// Left/Right do nothing here (no tabs); only Esc closes — matching the Settings-app convention.
static bool on_back(int key){ return key==NK_LEFT; }   // consume Left, let Esc fall through to close

static void on_enter(void){
    nucleo_app_set_direct_draw(true);   // static list UI: draw direct, free the 32 KB menu buffer
    s_sel = active_index();
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR("SU/GIU scegli   INVIO applica   Esc esci", "UP/DN pick   ENTER apply   Esc exit"));
    nucleo_app_request_draw();
}

static void on_key(int k,char ch){
    int n=theme_count(); if(n<=0) return;
    if(k==NK_UP)        s_sel=(s_sel-1+n)%n;
    else if(k==NK_DOWN) s_sel=(s_sel+1)%n;
    else if(k==NK_ENTER){
        int c=0; const nucleo_theme_t *all=nucleo_theme_get_all(&c);
        if(s_sel>=0 && s_sel<c) nucleo_theme_set(all[s_sel].id);   // applies live + persists
    } else return;
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

static void on_draw(void){
    int ch=nucleo_app_content_height();
    d.fillRect(0,0,W,ch,BG);

    // header: "Theme" + the active theme's name, in the live accent so it reflects the choice
    d.setTextSize(2); d.setTextColor(THEME_ACC,BG); d.setCursor(10,4); d.print(TR("Tema","Theme"));
    int c=0; const nucleo_theme_t *all=nucleo_theme_get_all(&c);
    int ai=active_index();
    if(c>0 && all[ai].name){ const char *cn=all[ai].name; int w=(int)strlen(cn)*6;
        d.setTextSize(1); d.setTextColor(MUTED,BG); d.setCursor(W-10-w,11); d.print(cn); }
    d.drawFastHLine(10,22,W-20,LINE);

    if(c<=0){ d.setTextSize(1); d.setTextColor(MUTED,BG); d.setCursor(10,40); d.print(TR("Nessun tema","No themes")); return; }

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
        'T',0xFBB6,on_enter,on_key,nullptr,on_draw,nullptr
    };
    nucleo_app_register(&app);
}
