// app_wifi.cpp — NucleoOS Settings panel + Wi-Fi manager.
//
// Visual language mirrors the Music / Video settings sheet (app_video.cpp):
//   • themed palette (THEME_BG &c.) so the panel matches every other app and the
//     active theme — no hard-coded background;
//   • a segmented tab bar with a tiny per-tab status dot (at-a-glance health);
//   • LIST tabs use the shared focused-row look: the selected row grows to text
//     size 2 in a capsule with an accent rail, neighbours shrink and dim — exactly
//     like Files / Music / Video;
//   • everything is clipped to the OS content area [0, content_height); the OS owns
//     the hint bar below it, so nothing spills off the bottom any more.
//
// Tabs:  STATO · RETE · AP · LUCE · SYS

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"   // W/H/HINT + themed BG/FG/MUTED/DIM/LINE/INK + C_* accents
#include "app_gfx.h"
#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "nucleo_storage.h"
#include "nucleo_voice.h"
}
#include <dirent.h>
#include <unistd.h>

// ---- platform hooks -------------------------------------------------------
extern "C" {
const char *nucleo_setup_mode(void);
const char *nucleo_setup_ssid(void);
const char *nucleo_setup_ip(void);
int         nucleo_setup_rssi(void);
int         nucleo_setup_channel(void);
const char *nucleo_setup_device_name(void);
bool        nucleo_setup_time_synced(void);
int         nucleo_setup_scan(void);
int         nucleo_setup_scan_count(void);
const char *nucleo_setup_scan_ssid(int i);
int         nucleo_setup_scan_rssi(int i);
int         nucleo_setup_scan_channel(int i);
int         nucleo_setup_scan_secure(int i);
const char *nucleo_setup_scan_auth_label(int i);
bool        nucleo_setup_join(const char *ssid, const char *pass);
void        nucleo_setup_start_ap(void);
void        nucleo_setup_stop_ap(void);
void        nucleo_setup_forget(void);
bool        nucleo_setup_net_is_known(const char *ssid);   // saved (auto-rejoin) network?
bool        nucleo_setup_net_has_password(const char *ssid); // saved AND has a usable password
void        nucleo_setup_forget_ssid(const char *ssid);    // forget one saved network
int         nucleo_setup_net_count(void);                  // how many networks are saved
void        nucleo_setup_set_device_name(const char *name);
const char *nucleo_setup_ap_ssid(void);
const char *nucleo_setup_ap_pass(void);
void        nucleo_setup_set_ap_ssid(const char *ssid);
void        nucleo_setup_set_ap_pass(const char *pass);
int         nucleo_setup_apply_network(void);
const char *nucleo_auth_pin(void);
int         nucleo_audio_volume(void);
void        nucleo_audio_set_volume(int pct);
void        nucleo_audio_set_mute(bool muted);
bool        nucleo_audio_is_muted(void);
}

// ---- surfaces (subtle tints layered over the themed BG, like app_video) ----
static const unsigned short
    SURF = 0x10A2,   // raised surface / slider track
    CAP  = 0x1A8B,   // focused-row capsule
    ACC  = C_BLUE,   // panel accent (matches the launcher tile)
    GRN  = C_GREEN,
    AMB  = C_YELLOW,
    REDC = C_RED;

// ---- tabs ------------------------------------------------------------------
#define T_STATO 0
#define T_RETE  1
#define T_AP    2
#define T_DISP  3
#define T_SYS   4
#define T_RESET 5
#define NTABS   6
static const char *const TABS[NTABS]   = { "STATO","RETE","AP","DISP","SYS","RESET" };
static const char *const TABS_EN[NTABS]= { "HOME","WIFI","AP","DISP","SYS","RESET" };

// ---- state -----------------------------------------------------------------
static int  s_tab    = T_STATO;
static int  s_sel    = -1;     // -1 = tab header (RIGHT pages, DOWN enters rows)
static bool s_edit   = false;  // slider adjust mode (LUCE), mirrors Video s_set_edit
// System UI language (settings.json ui.language) — shared with the Control Center + web shell, so
// this app's existing IT/EN labels follow the global setting instead of a private toggle.
extern "C" bool nucleo_i18n_is_en(void);
extern "C" void nucleo_i18n_set_en(bool en);
static bool s_en     = false;  // English labels (seeded from the system language on enter)
static bool s_rconf  = false;  // reboot double-confirm armed
static bool s_fconf  = false;  // "forget all networks" double-confirm armed

enum { IM_NONE=0, IM_PASS, IM_NAME, IM_APSSID, IM_APPASS };
static int  s_im = IM_NONE;
static char s_ibuf[80]; static int s_ilen = 0;
static char s_join_ssid[33];

enum { OP_NONE=0, OP_SCAN, OP_JOIN };
static volatile int  s_op   = OP_NONE;
static volatile bool s_busy = false, s_done = false, s_join_ok = false;
static char s_join_pass[80];
static TaskHandle_t  s_task = nullptr;
static int  s_anim    = 0;

static char    s_msg[48]; static int s_msg_t = 0;
static uint8_t s_hist[40]; static int s_histn = 0, s_tickn = 0;
static char    s_skey[80];

static void toast(const char *it, const char *en) {
    snprintf(s_msg, sizeof s_msg, "%s", s_en ? en : it); s_msg_t = 20;
}

// ---- small helpers ---------------------------------------------------------
static int squality(int rssi) {
    if (!rssi) return 0;
    int q = 2 * (rssi + 100); return q < 0 ? 0 : q > 100 ? 100 : q;
}
static uint16_t qcol(int q)   { return q >= 60 ? GRN : q >= 33 ? AMB : REDC; }
static bool     connected(void){ return !strcmp(nucleo_setup_mode(),"sta") && nucleo_setup_ip()[0]; }
static bool     ap_on(void)    { return !strcmp(nucleo_setup_mode(),"ap"); }
static const char *tab_label(int i){ return s_en ? TABS_EN[i] : TABS[i]; }

static void txt(int x,int y,const char*s,uint16_t fg,uint16_t bg,int sz){
    d.setTextSize(sz); d.setTextColor(fg,bg); d.setCursor(x,y); d.print(s);
}

// ---- tab bar ---------------------------------------------------------------
// When `header` (s_sel == -1) the focus is ON the bar, so the active tab is a filled
// pill. Once you press DOWN into the rows, the focus leaves the bar: the active tab
// becomes a hollow outline so the eye follows the focus down into the content.
static void draw_tabbar(bool header){
    d.fillRect(0,0,W,24,BG);
    int seg = W / NTABS;                                     // 7 tabs -> 34 px each
    for(int i=0;i<NTABS;i++){
        int x=i*seg; const char*lab=tab_label(i); int tw=(int)strlen(lab)*6;
        if(i==s_tab && header){
            d.fillRoundRect(x+2,3,seg-4,17,7,ACC);          // focused bar: filled pill
            txt(x+(seg-tw)/2,8,lab,INK,ACC,1);
        }else if(i==s_tab){
            d.drawRoundRect(x+2,3,seg-4,17,7,ACC);          // active but focus is in rows: outline
            txt(x+(seg-tw)/2,8,lab,ACC,BG,1);
        }else{
            txt(x+(seg-tw)/2,8,lab,MUTED,BG,1);             // inactive tab: legible grey, not near-black DIM
        }
    }
    d.drawFastHLine(0,23,W,LINE);
}

// ---- generic settings list -------------------------------------------------
enum { RV_TEXT=0, RV_TOGGLE, RV_SLIDER, RV_ACTION, RV_SEG };
// Segment labels for the focused RV_SEG row (set before draw_list).
static const char *s_seg[4]; static int s_seg_n = 0;

// One settings row. `val` carries the right-hand text for RV_TEXT; other kinds derive it.
struct Row { const char *label; char val[20]; int kind; bool on; int slider; };

// Compact right-aligned value text for an UNFOCUSED row (launcher style: dot + text).
static void row_value_str(char*b,int cap,const char*val,int kind,bool on,int slider){
    if(kind==RV_TOGGLE)      snprintf(b,cap,"%s",on?"On":"Off");
    else if(kind==RV_SLIDER) snprintf(b,cap,"%d%%",slider);
    else if(kind==RV_SEG)    snprintf(b,cap,"%s",(slider>=0&&slider<s_seg_n&&s_seg[slider])?s_seg[slider]:"");
    else if(kind==RV_ACTION) snprintf(b,cap,">");
    else                     snprintf(b,cap,"%.14s",val?val:"");
}

// Decide ONCE per list whether labels can use the big size-2 font: only when the longest
// label still leaves room for a value and the per-row pitch is tall enough. Labels are kept
// short enough (e.g. "Dimentica", not "Dimentica tutte") that every tab — Luce/AP/SYS —
// clears this check and renders the same big rows; only a genuinely long label falls back.
static bool list_big(Row*it,int n,int pitch){
    if(pitch<22) return false;
    int maxlen=0; for(int i=0;i<n;i++){ int l=(int)strlen(it[i].label); if(l>maxlen)maxlen=l; }
    return maxlen*12 + 16 + 46 <= W;            // label(size2) + left margin + min value room
}

// One list row of height h at top y. The SELECTED row is always the big font in a capsule
// with the live widget; other rows follow `big` (size-2 on short lists, size-1 on long ones)
// and show a compact value. Widgets are vertically centred so they fit any row height.
static void draw_list_row(int y,int h,Row&r,bool big,bool sel){
    int cy=y+h/2;
    uint16_t bg = sel?CAP:BG;
    if(sel){
        d.fillRoundRect(4,y+1,W-8,h-3,8,CAP);                // focus capsule
        d.fillRoundRect(8,y+5,4,h-11,2,ACC);                 // accent rail
    }
    bool lbig = sel || big;                                  // selected row is always enlarged
    int lsz = lbig?2:1;
    if(!lbig) d.fillCircle(12,cy,2,(r.kind==RV_ACTION)?ACC:MUTED);   // dot only on slim rows
    int lx = lbig?16:20;
    uint16_t labc = (sel||big)?FG:MUTED;
    d.setTextSize(lsz); d.setTextColor(labc,bg);
    d.setCursor(lx, cy-(lsz==2?8:3)); d.print(r.label);

    if(sel){
        if(r.kind==RV_SEG){
            int nn=s_seg_n>0?s_seg_n:1, gap=2, pw=44, ph=20;
            int totw=nn*pw+(nn-1)*gap, bx=W-12-totw, vy=cy-ph/2;
            for(int i=0;i<nn;i++){
                int px=bx+i*(pw+gap); bool s=(i==r.slider);
                d.fillRoundRect(px,vy,pw,ph,ph/2,s?ACC:SURF);
                const char*sl=s_seg[i]?s_seg[i]:"?"; int tw=(int)strlen(sl)*6;
                int tx=px+(pw-tw)/2; if(tx<px+2)tx=px+2;
                txt(tx,vy+(ph-8)/2,sl,s?INK:MUTED,s?ACC:SURF,1);
            }
        }else if(r.kind==RV_SLIDER){
            int sw=92,sh=12,bx=W-12-sw,vy=cy-sh/2;
            d.fillRoundRect(bx,vy,sw,sh,sh/2,SURF);
            int onw=r.slider*sw/100; if(onw<0)onw=0; if(onw>sw)onw=sw;
            if(onw>0) d.fillRoundRect(bx,vy,onw,sh,sh/2,GRN);
            int kx=bx+onw; if(kx<bx+6)kx=bx+6; if(kx>bx+sw-6)kx=bx+sw-6;
            d.fillCircle(kx,vy+sh/2,sh/2+2,FG);
            char pv[8]; snprintf(pv,sizeof pv,"%d%%",r.slider);   // live % readout left of the track
            d.setTextSize(1); d.setTextColor(FG,CAP); d.setCursor(bx-6-(int)strlen(pv)*6,cy-3); d.print(pv);
        }else if(r.kind==RV_TOGGLE){
            int sw=42,sh=20,bx=W-12-sw,vy=cy-sh/2;
            d.fillRoundRect(bx,vy,sw,sh,sh/2,r.on?GRN:SURF);
            int kx=r.on?bx+sw-sh/2-1:bx+sh/2+1;
            d.fillCircle(kx,vy+sh/2,sh/2-3,r.on?INK:MUTED);
        }else if(r.kind==RV_ACTION){
            int bw=30,bh=22,bx=W-12-bw,vy=cy-bh/2;
            d.fillRoundRect(bx,vy,bw,bh,6,ACC);
            int ax=bx+bw/2-2;
            d.fillTriangle(ax,cy-5,ax,cy+5,ax+5,cy,INK);
        }else if(r.val[0]){                                  // RV_TEXT chip
            int vw=(int)strlen(r.val)*12+12, vh=20;
            int maxw=W-12-(16+(int)strlen(r.label)*12+8);
            if(vw>maxw){ int mc=(maxw-12)/12; if(mc<1)mc=1; vw=mc*12+12; }
            int bx=W-12-vw,vy=cy-vh/2;
            d.fillRoundRect(bx,vy,vw,vh,5,SURF);
            int mc=(vw-12)/12; if(mc>19)mc=19;
            char b[20]; snprintf(b,sizeof b,"%.*s",mc,r.val);
            d.setTextSize(2); d.setTextColor(FG,SURF); d.setCursor(bx+6,vy+2); d.print(b);
        }
        return;
    }
    // unselected: compact, right-aligned value
    char rb[20]; row_value_str(rb,sizeof rb,r.val,r.kind,r.on,r.slider);
    if(rb[0]){ int w=(int)strlen(rb)*6;
        uint16_t vc = r.kind==RV_TOGGLE?(r.on?GRN:MUTED):r.kind==RV_ACTION?ACC:(big?FG:MUTED);
        d.setTextSize(1); d.setTextColor(vc,BG); d.setCursor(W-12-w,cy-3); d.print(rb);
    }
}

// Adaptive, space-filling list. Few rows (Luce/AP) stretch to fill the band with big
// size-2 labels; many rows (SYS) keep a compact size-1 pitch and scroll. The block is laid
// out top-to-bottom and vertically centred, so short lists never float with empty space.
// sel == -1 (tab header) simply means no row carries the capsule.
static void draw_list(int ch,Row*it,int n,int sel){
    if(n<=0) return;
    const int top=26, bot=ch-3, band=bot-top;          // content band under the tab bar
    const int HMIN=22, HMAX=40;
    int pitch=band/n; if(pitch>HMAX)pitch=HMAX;
    bool scroll=(pitch<HMIN); if(scroll)pitch=HMIN;
    int visible=scroll?(band/pitch):n; if(visible>n)visible=n;
    bool big=list_big(it,n,pitch);

    // scroll window that keeps the selection on screen (roughly centred)
    int first=0;
    if(scroll){
        int cur=(sel<0)?0:sel;
        first=cur-visible/2;
        if(first>n-visible)first=n-visible;
        if(first<0)first=0;
    }
    int shown=scroll?visible:n;
    int y0=top+(band-shown*pitch)/2; if(y0<top)y0=top; // centre the block when it doesn't fill

    d.setClipRect(0,top,W,band);
    for(int k=0;k<shown;k++){
        int i=first+k; if(i>=n)break;
        draw_list_row(y0+k*pitch,pitch,it[i],big,i==sel);
    }
    d.clearClipRect();

    if(scroll){                                        // slim scrollbar (watch-style)
        int trk=band-4, kh=trk*visible/n; if(kh<12)kh=12;
        int maxoff=n-visible, ky=top+2+(maxoff>0?(trk-kh)*first/maxoff:0);
        d.fillRoundRect(W-3,top+2,2,trk,1,LINE);
        d.fillRoundRect(W-3,ky,2,kh,1,ACC);
    }
}

// ---- STATO (dashboard, bounded to content) ---------------------------------
static void draw_stato(int ch){
    int rssi=nucleo_setup_rssi(),q=squality(rssi);

    // signal ring (watchface arc)
    int cx=40,cy=58,r1=27,r0=19;
    uint16_t rc=(connected()||ap_on())?qcol(q):DIM;
    d.fillArc(cx,cy,r0,r1,135.0f,405.0f,SURF);
    if(connected()||ap_on()){
        float e=135.0f+q*270.0f/100.0f; if(e>405.0f)e=405.0f;
        d.fillArc(cx,cy,r0,r1,135.0f,e,rc);
    }
    char b[24];
    if(ap_on())          txt(cx-6,cy-8,"AP",ACC,BG,2);
    else if(connected()){snprintf(b,4,"%d",q); txt(cx-(int)strlen(b)*6,cy-8,b,qcol(q),BG,2);}
    else                 txt(cx-6,cy-8,"--",DIM,BG,2);

    // right column — primary values size 2
    int rx=80;
    if(ap_on()){
        txt(rx,30,nucleo_setup_ap_ssid(),AMB,BG,2);
        txt(rx,52,"192.168.4.1",FG,BG,2);
        txt(rx,74,s_en?"Hotspot active":"Hotspot attivo",GRN,BG,1);
    }else{
        const char*ss=nucleo_setup_ssid()[0]?nucleo_setup_ssid():(s_en?"No network":"Nessuna rete");
        char sb[16]; snprintf(sb,sizeof sb,"%.13s",ss);
        txt(rx,30,sb,FG,BG,2);
        char ib[16]; snprintf(ib,sizeof ib,"%.13s",connected()?nucleo_setup_ip():"--");
        txt(rx,52,ib,connected()?FG:DIM,BG,2);
        snprintf(b,sizeof b,"ch%d  %ddBm%s",nucleo_setup_channel(),rssi,
                 nucleo_setup_time_synced()?"  NTP":"");
        txt(rx,74,b,MUTED,BG,1);
    }

    // sparkline strip
    int gx=rx,gy=86,gw=W-rx-8,gh=12;
    d.drawRoundRect(gx,gy,gw,gh,2,LINE);
    int ns=s_histn<gw-4?s_histn:gw-4;
    for(int i=0;i<ns;i++){int v=s_hist[s_histn-ns+i],bh=v*(gh-4)/100;
        d.drawFastVLine(gx+2+i,gy+gh-2-bh,bh,qcol(v));}

    // complications row: PIN | SD  (kept above the hint bar, with a small bottom margin)
    int fy=ch-18;
    d.drawFastHLine(0,fy-3,W,LINE);
    const char*pin=nucleo_auth_pin();
    txt(8,fy+2,"PIN",MUTED,BG,1);
    txt(30,fy,pin[0]?pin:"------",pin[0]?GRN:DIM,BG,2);
    const nucleo_storage_info_t*st=nucleo_storage_info();
    if(st&&st->mounted){
        uint32_t fmb=(uint32_t)(st->free_bytes/(1024*1024));
        if(fmb>=1000) snprintf(b,sizeof b,"SD %.1fGB",(float)fmb/1024.0f);
        else          snprintf(b,sizeof b,"SD %uMB",(unsigned)fmb);
        txt(W-(int)strlen(b)*6-8,fy+2,b,ACC,BG,1);
    }else txt(W-5*6-8,fy+2,"SD --",REDC,BG,1);
}

// ---- RETE (scan list, focus-enlarged) --------------------------------------
static void draw_rete(int ch){
    if(s_busy&&s_op==OP_SCAN){
        const char*dt[]={".","..","...",""};
        char b[28]; snprintf(b,sizeof b,"%s%s",s_en?"Scanning":"Scansione",dt[s_anim%4]);
        txt(56,56,b,ACC,BG,2);
        d.fillArc(28,60,8,14,0.0f,(float)(s_anim*40%360),ACC);
        return;
    }
    int n=nucleo_setup_scan_count();
    // header row (sel==-1): scan/rescan (manual — never auto-runs)
    bool hf=(s_sel==-1);
    d.fillRoundRect(4,26,W-8,18,6,hf?CAP:BG);
    txt(12,30,n?(s_en?"Rescan networks":"Aggiorna reti"):(s_en?"Scan networks":"Cerca reti"),hf?FG:MUTED,hf?CAP:BG,1);
    if(hf) txt(W-40,30,"INVIO",ACC,CAP,1);
    if(!n){
        txt(12,62,s_en?"No networks yet.":"Nessuna rete trovata.",MUTED,BG,1);
        txt(12,76,s_en?"Press ENTER to scan.":"Premi INVIO per cercare.",MUTED,BG,1);
        return;
    }

    // Top-anchored, space-filling list: the block is centred when every network fits, and
    // scrolls to keep the selection on screen when it doesn't — no more floating in the
    // middle. Non-focused rows now show the SSID at size 2, so the names are easy to read.
    int top=48, band=(ch-3)-top, HF=44, HN=28;
    int total=0; for(int i=0;i<n;i++) total += (i==s_sel)?HF:HN;
    int y0;
    if(total<=band){ y0=top+(band-total)/2; }              // all fit: centre the block
    else{                                                  // scroll so the selection stays visible
        int selTop=0; for(int i=0;i<(s_sel<0?0:s_sel);i++) selTop+=HN;
        int selH=(s_sel<0)?HN:HF, scroll=0;
        if(selTop+selH>band) scroll=selTop+selH-band;
        if(selTop<scroll)     scroll=selTop;
        if(scroll>total-band) scroll=total-band;
        if(scroll<0)          scroll=0;
        y0=top-scroll;
    }

    d.setClipRect(0,46,W,ch-46);
    int y=y0;
    for(int i=0;i<n;i++){
        bool f=(i==s_sel); int h=f?HF:HN;
        if(y+h>46 && y<ch-2){
            int rssi=nucleo_setup_scan_rssi(i),q=squality(rssi);
            bool cur=connected()&&!strcmp(nucleo_setup_scan_ssid(i),nucleo_setup_ssid());
            bool known=nucleo_setup_net_is_known(nucleo_setup_scan_ssid(i));   // saved -> auto-rejoin
            d.fillRoundRect(4,y,W-8,h-2,7,f?CAP:BG);
            if(f) d.fillRoundRect(4,y+3,5,h-8,2,qcol(q));
            // signal bars
            int bx=14,by=y+(h-13)/2; int lit=q>=75?4:q>=50?3:q>=25?2:q>0?1:0;
            for(int k=0;k<4;k++){int bh=3+k*3; d.fillRect(bx+k*4,by+13-bh,3,bh,k<lit?(f?qcol(q):MUTED):LINE);}
            if(nucleo_setup_scan_secure(i)){d.drawRoundRect(34,by+2,5,5,1,f?FG:MUTED);d.fillRect(33,by+5,7,5,f?FG:MUTED);}
            const char*ss=nucleo_setup_scan_ssid(i);
            if(f){
                char sb[14]; snprintf(sb,sizeof sb,"%.12s",ss[0]?ss:"(hidden)");
                txt(46,y+6,sb,cur?GRN:FG,CAP,2);
                char det[40]; snprintf(det,sizeof det,"%s%s  ch%d  %ddBm",
                    known?(s_en?"saved ":"salv. "):"",
                    nucleo_setup_scan_auth_label(i),nucleo_setup_scan_channel(i),rssi);
                txt(46,y+26,det,qcol(q),CAP,1);
                if(known) d.fillCircle(W-16,y+11,3,ACC);          // saved-network pip
            }else{
                char sb[16]; snprintf(sb,sizeof sb,"%.13s",ss[0]?ss:"(hidden)");
                txt(46,y+(h-16)/2,sb,cur?GRN:MUTED,BG,2);          // size-2 name (was tiny size-1)
                if(known) d.fillCircle(W-12,y+h/2,3,ACC);          // saved-network pip
            }
        }
        y+=h;
    }
    d.clearClipRect();
    // slim scrollbar when the list overflows
    if(total>band){
        int trk=band, kh=trk*band/total; if(kh<12)kh=12;
        int scrolled=top-y0, maxs=total-band;
        int ky=top+(maxs>0?(trk-kh)*scrolled/maxs:0);
        d.fillRoundRect(W-3,top,2,trk,1,LINE);
        d.fillRoundRect(W-3,ky,2,kh,1,ACC);
    }
}

// ---- AP --------------------------------------------------------------------
static void build_ap(Row*it,int*n){
    int k=0;
    it[k].label=s_en?"Hotspot":"Hotspot"; it[k].kind=RV_TOGGLE; it[k].on=ap_on(); k++;
    it[k].label="SSID"; it[k].kind=RV_TEXT; snprintf(it[k].val,20,"%s",nucleo_setup_ap_ssid()); k++;
    it[k].label="Pass"; it[k].kind=RV_TEXT; snprintf(it[k].val,20,"%s",nucleo_setup_ap_pass()); k++;
    it[k].label="URL";  it[k].kind=RV_TEXT; snprintf(it[k].val,20,"192.168.4.1"); k++;
    *n=k;
}

// ---- DISP ------------------------------------------------------------------
#define DISP_BRIGHT 0
#define DISP_VOL    1
#define DISP_MUTE   2
#define DISP_VOICE  3

static void build_disp(Row*it,int*n){
    it[DISP_BRIGHT].label=s_en?"Bright":"Luce";   it[DISP_BRIGHT].kind=RV_SLIDER; it[DISP_BRIGHT].slider=nucleo_app_brightness();
    it[DISP_VOL].label="Volume";                  it[DISP_VOL].kind=RV_SLIDER;    it[DISP_VOL].slider=nucleo_audio_volume();
    it[DISP_MUTE].label=s_en?"Mute":"Muto";       it[DISP_MUTE].kind=RV_TOGGLE;   it[DISP_MUTE].on=nucleo_audio_is_muted();
    it[DISP_VOICE].label=s_en?"Voice PTT":"Voce"; it[DISP_VOICE].kind=RV_TOGGLE;  it[DISP_VOICE].on=nucleo_voice_always_on();
    *n=4;
}

// ---- factory reset ---------------------------------------------------------
static void rm_tree(const char *path){
    DIR *dir=opendir(path);
    if(!dir){ unlink(path); return; }
    struct dirent *e;
    while((e=readdir(dir))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char sub[256]; snprintf(sub,sizeof sub,"%s/%s",path,e->d_name);
        rm_tree(sub);
    }
    closedir(dir); rmdir(path);
}
static void do_reset_soft(void){
    rm_tree("/sd/system/config");
    rm_tree("/sd/system/sessions");
    rm_tree("/sd/system/log");
    esp_restart();
}
static void do_reset_hard(void){
    rm_tree("/sd/system/config");
    rm_tree("/sd/system/sessions");
    rm_tree("/sd/system/log");
    rm_tree("/sd/system/keys");
    rm_tree("/sd/data/anima/learned");
    unlink("/sd/data/anima/teacher.json");
    unlink("/sd/data/anima/telemetry.ndjson");
    unlink("/sd/data/anima/session.txt");
    unlink("/sd/data/anima/sessions.json");
    unlink("/sd/data/anima/workspace.json");
    rm_tree("/sd/config");
    rm_tree("/sd/backups");
    rm_tree("/sd/journal");
    esp_restart();
}

// ---- RESET tab -------------------------------------------------------------
#define RST_SOFT 0
#define RST_HARD 1
static int s_rst_n    = 0;   // confirm counter: 0=idle, 1,2=counting
static int s_rst_type = 0;   // 1=soft, 2=hard

static void build_reset(Row*it,int*n){
    it[RST_SOFT].label=s_en?"Reset config":"Reset config"; it[RST_SOFT].kind=RV_ACTION;
    memset(it[RST_SOFT].val,0,20);
    if(s_rst_type==1&&s_rst_n>0){ it[RST_SOFT].kind=RV_TEXT; snprintf(it[RST_SOFT].val,20,"x%d",3-s_rst_n); }

    it[RST_HARD].label=s_en?"Factory reset":"Reset totale"; it[RST_HARD].kind=RV_ACTION;
    memset(it[RST_HARD].val,0,20);
    if(s_rst_type==2&&s_rst_n>0){ it[RST_HARD].kind=RV_TEXT; snprintf(it[RST_HARD].val,20,"x%d",3-s_rst_n); }
    *n=2;
}

// ---- SYS -------------------------------------------------------------------
static const char *theme_name(void){
    int cnt=0; const nucleo_theme_t*all=nucleo_theme_get_all(&cnt);
    const char*cur=nucleo_theme_get_current();
    for(int i=0;i<cnt;i++) if(all[i].id&&cur&&!strcmp(all[i].id,cur)) return all[i].name;
    return cur?cur:"?";
}
static void theme_cycle(void){
    int cnt=0; const nucleo_theme_t*all=nucleo_theme_get_all(&cnt);
    if(cnt<=0) return;
    const char*cur=nucleo_theme_get_current(); int idx=0;
    for(int i=0;i<cnt;i++) if(all[i].id&&cur&&!strcmp(all[i].id,cur)){ idx=i; break; }
    nucleo_theme_set(all[(idx+1)%cnt].id);
}
static void build_sys(Row*it,int*n){
    bool sta=!strcmp(nucleo_setup_mode(),"sta");
    int k=0;
    it[k].label=s_en?"Name":"Nome";  it[k].kind=RV_TEXT; snprintf(it[k].val,20,"%s",nucleo_setup_device_name()); k++;
    it[k].label=s_en?"Language":"Lingua"; it[k].kind=RV_TEXT; snprintf(it[k].val,20,"%s",s_en?"EN":"IT"); k++;
    it[k].label=s_en?"Theme":"Tema"; it[k].kind=RV_TEXT; snprintf(it[k].val,20,"%s",theme_name()); k++;
    it[k].label=sta?(s_en?"Disconnect":"Disconnetti"):(s_en?"Reconnect":"Riconnetti");
        it[k].kind=RV_ACTION; k++;
    // "Dimentica" (not "Dimentica tutte"): short enough that SYS clears list_big's width
    // check and renders with the same big size-2 rows as the other tabs, while still
    // leaving room for the "sicuro?" confirm chip beside it.
    it[k].label=s_en?"Forget all":"Dimentica"; it[k].kind=RV_TEXT;
        { int nn=nucleo_setup_net_count();
          if(s_fconf) snprintf(it[k].val,20,"%s",s_en?"sure?":"sicuro?");
          else        snprintf(it[k].val,20,nn?"%d":"",nn); } k++;
    it[k].label=s_en?"Restart":"Riavvia"; it[k].kind=RV_TEXT;
        snprintf(it[k].val,20,"%s",s_rconf?(s_en?"sure?":"sicuro?"):""); k++;
    // SD info (read-only)
    const nucleo_storage_info_t*st=nucleo_storage_info();
    it[k].label="SD"; it[k].kind=RV_TEXT;
    if(st&&st->mounted){
        uint32_t f=(uint32_t)(st->free_bytes/(1024*1024)),t=(uint32_t)(st->total_bytes/(1024*1024));
        if(t>=1000) snprintf(it[k].val,20,"%.1f/%.1fG",(float)f/1024.0f,(float)t/1024.0f);
        else        snprintf(it[k].val,20,"%u/%uM",(unsigned)f,(unsigned)t);
    }else snprintf(it[k].val,20,"%s",s_en?"none":"assente");
    k++;
    it[k].label=s_en?"Manual":"Manuale"; it[k].kind=RV_ACTION; k++;
    *n=k;
}
#define SYS_NAME 0
#define SYS_LANG 1
#define SYS_THEME 2
#define SYS_WIFI 3
#define SYS_FORGET 4
#define SYS_RESTART 5
#define SYS_SD 6
#define SYS_MANUAL 7

// ---- manual ----------------------------------------------------------------
static bool s_manual=false;
static void draw_manual(int ch){
    d.fillRect(0,0,W,ch,BG);
    txt(8,4,s_en?"Settings - help":"Impostazioni - guida",ACC,BG,1);
    const char*it[]={
        "SX / DX / TAB: cambia scheda",
        "GIU: entra nelle voci  SU: torna su",
        "INVIO: attiva   Esc: esci dall'app",
        "DISP: INVIO su slider, poi < >",
        "DISP: Voce=toggle sempre attiva",
        "SYS: tema, lingua, riavvia",
        "RESET: INVIO 3x (config o totale)",0};
    const char*en[]={
        "LEFT / RIGHT / TAB: switch tab",
        "DOWN: enter rows   UP: go back up",
        "ENTER: act   Esc: exit the app",
        "DISP: ENTER on slider, then < >",
        "DISP: Voice=always-on toggle",
        "SYS: theme, language, restart",
        "RESET: ENTER 3x (config or full)",0};
    const char**m=s_en?en:it;
    for(int i=0;m[i];i++) txt(8,20+i*14,m[i],FG,BG,1);
}

// ---- rows-per-tab ----------------------------------------------------------
static int tab_rows(int t){
    Row it[8]; int n=0;
    if(t==T_AP) build_ap(it,&n);
    else if(t==T_DISP) build_disp(it,&n);
    else if(t==T_RESET) build_reset(it,&n);
    else if(t==T_SYS) build_sys(it,&n);
    else if(t==T_RETE) n=nucleo_setup_scan_count();
    return n;   // T_STATO / T_DIAG = 0 (no row navigation)
}

// ---- on_draw ---------------------------------------------------------------
static void on_draw(void){
    int ch=nucleo_app_content_height();
    if(s_manual){ draw_manual(ch); return; }
    d.fillRect(0,0,W,ch,BG);
    draw_tabbar(s_sel==-1);

    if(s_busy&&s_op==OP_JOIN){
        const char*dt[]={".","..","...",""};
        char b[40]; snprintf(b,sizeof b,"%s%s",s_en?"Connecting":"Connessione",dt[s_anim%4]);
        txt(40,52,b,ACC,BG,2);
        char sb[28]; snprintf(sb,sizeof sb,"%.26s",s_join_ssid); txt(40,76,sb,MUTED,BG,1);
        return;
    }

    Row it[8]; int n;
    switch(s_tab){
    case T_STATO: draw_stato(ch); break;
    case T_RETE:  draw_rete(ch);  break;
    case T_AP:    build_ap(it,&n);    draw_list(ch,it,n,s_sel); break;
    case T_DISP:  build_disp(it,&n); draw_list(ch,it,n,s_sel); break;
    case T_RESET: build_reset(it,&n);draw_list(ch,it,n,s_sel); break;
    default:      build_sys(it,&n);  draw_list(ch,it,n,s_sel); break;
    }

    // inline editor overlay
    if(s_im!=IM_NONE){
        int iy=ch-26; d.fillRoundRect(6,iy,W-12,22,5,CAP);
        const char*lab = s_im==IM_PASS  ? "Password:"
                       : s_im==IM_APSSID ? "AP SSID:"
                       : s_im==IM_APPASS ? "AP Pass:"
                       : (s_en?"Name:":"Nome:");
        bool mask = (s_im==IM_PASS||s_im==IM_APPASS);
        txt(12,iy+7,lab,ACC,CAP,1);
        int vx=12+(int)strlen(lab)*6+6, k=0, from=s_ilen>24?s_ilen-24:0;
        char sh[40]; for(int i=from;i<s_ilen&&k<24;i++,k++) sh[k]=mask?'*':s_ibuf[i];
        sh[k]=0; txt(vx,iy+7,sh,FG,CAP,1); d.fillRect(vx+k*6,iy+6,2,9,ACC);
    }
    // toast — sits at the bottom, but lifts above the inline editor when one is open so
    // a validation message (e.g. "Min 8 chars") never covers the field you are typing in.
    if(s_msg_t>0){ int iy=(s_im!=IM_NONE)?ch-48:ch-22; d.fillRoundRect(6,iy,W-12,18,5,SURF); txt(12,iy+5,s_msg,AMB,SURF,1); }
}

// ---- worker ----------------------------------------------------------------
static void wifi_task(void*){
    if(s_op==OP_SCAN)      nucleo_setup_scan();
    else if(s_op==OP_JOIN) s_join_ok=nucleo_setup_join(s_join_ssid,s_join_pass);
    s_done=true; s_task=nullptr; vTaskDelete(nullptr);
}
static void start_op(int op){
    if(s_busy) return;
    s_op=op; s_busy=true; s_done=false; s_anim=0;
    if(xTaskCreate(wifi_task,"wifi",4096,nullptr,tskIDLE_PRIORITY+2,&s_task)!=pdPASS){
        s_busy=false; s_task=nullptr; toast("Errore task","Task error");
    }
    nucleo_app_request_draw();
}

// ---- hint (state-based: header / rows / slider-edit — mirrors Video) -------
static int tab_rows(int t);
static void update_hint(void){
    if(s_edit){ nucleo_app_set_hint(s_en?"L/R adjust   ENTER done":"L/R regola   INVIO ok"); return; }
    if(s_sel==-1){
        if(s_tab==T_STATO)      nucleo_app_set_hint(s_en?"L/R tab   esc":"L/R scheda   esc");
        else if(s_tab==T_RETE)  nucleo_app_set_hint(s_en?"L/R tab   ENTER scan   DOWN list":"L/R scheda   INVIO cerca   GIU lista");
        else if(s_tab==T_RESET)    nucleo_app_set_hint(s_en?"L/R tab   DOWN sel   ENTER 3x confirm":"L/R scheda   GIU selezione   INVIO 3x conferma");
        else if(tab_rows(s_tab)>0) nucleo_app_set_hint(s_en?"L/R tab   DOWN rows   esc":"L/R scheda   GIU voci   esc");
        else                       nucleo_app_set_hint(s_en?"L/R tab   esc back":"L/R scheda   esc esci");
        return;
    }
    if(s_tab==T_RETE) nucleo_app_set_hint(s_en?"ENTER join   DEL forget   L/R tab":"INVIO connetti   CANC dimentica   L/R scheda");
    else              nucleo_app_set_hint(s_en?"UP/DN row   ENTER ok   L/R tab":"SU/GIU voce   INVIO ok   L/R scheda");
}

// Forward (RIGHT / TAB) pager — keeps the row level like Video's settings_key.
static void page_tab(void){
    if(s_im!=IM_NONE||s_manual||s_busy) return;
    s_tab=(s_tab+1)%NTABS; s_edit=false; s_rconf=false; s_fconf=false;
    int n=tab_rows(s_tab);
    s_sel=(s_sel>=0 && n>0)?0:-1;            // stay in rows if we were in rows, else header
    update_hint(); nucleo_app_request_draw();   // NB: scan is manual (ENTER on RETE header)
}
static void on_tab(void){ page_tab(); }

// Backward (LEFT) pager — mirror of page_tab so the arrows cycle the tabs both ways.
static void page_tab_back(void){
    if(s_im!=IM_NONE||s_manual||s_busy) return;
    s_tab=(s_tab+NTABS-1)%NTABS; s_edit=false; s_rconf=false; s_fconf=false;
    int n=tab_rows(s_tab);
    s_sel=(s_sel>=0 && n>0)?0:-1;            // stay in rows if we were in rows, else header
    update_hint(); nucleo_app_request_draw();
}

// ---- inline input ----------------------------------------------------------
static void input_key(int k,char ch){
    if(k==NK_CHAR&&ch>=32&&ch<127){ if(s_ilen<(int)sizeof(s_ibuf)-1){s_ibuf[s_ilen++]=ch;s_ibuf[s_ilen]=0;} }
    else if(k==NK_DEL){ if(s_ilen>0)s_ibuf[--s_ilen]=0; }
    else if(k==NK_ENTER){
        if(s_im==IM_PASS){ snprintf(s_join_pass,sizeof s_join_pass,"%s",s_ibuf); s_im=IM_NONE; start_op(OP_JOIN); }
        else if(s_im==IM_NAME){ nucleo_setup_set_device_name(s_ibuf); s_im=IM_NONE; toast("Salvato","Saved"); }
        else if(s_im==IM_APSSID){ nucleo_setup_set_ap_ssid(s_ibuf); s_im=IM_NONE; toast("SSID salvato","SSID saved"); }
        else if(s_im==IM_APPASS){
            // empty = open AP; otherwise WPA2 wants >= 8 chars
            if(s_ilen==0||s_ilen>=8){ nucleo_setup_set_ap_pass(s_ibuf); s_im=IM_NONE;
                toast(s_ilen?"Password salvata":"AP aperto",s_ilen?"Password saved":"AP open"); }
            else { toast("Min 8 caratteri","Min 8 chars"); }
        }
        if(s_im==IM_NONE){ memset(s_ibuf,0,sizeof s_ibuf); s_ilen=0; }
    }
    nucleo_app_request_draw();
}

// ---- ENTER at a tab HEADER (s_sel == -1) -----------------------------------
static void header_enter(void){
    if(s_tab==T_RETE) start_op(OP_SCAN);
}

// ---- activate (ENTER on a focused ROW, s_sel >= 0) -------------------------
static void activate(void){
    switch(s_tab){
    case T_RETE:
        snprintf(s_join_ssid,sizeof s_join_ssid,"%s",nucleo_setup_scan_ssid(s_sel));
        // Ask for the password when the network is secured and we don't already hold a usable one
        // (i.e. not saved, or saved without a stored password). Open networks, or saved nets whose
        // password we have, join straight away (the firmware reuses the stored password).
        if(nucleo_setup_scan_secure(s_sel) && !nucleo_setup_net_has_password(s_join_ssid)){ s_im=IM_PASS; s_ibuf[0]=0; s_ilen=0; }
        else { s_join_pass[0]=0; start_op(OP_JOIN); }
        break;
    case T_AP:
        if(s_sel==0){ if(ap_on())nucleo_setup_stop_ap(); else nucleo_setup_start_ap();   // OFF must flip mode->sta, not re-apply "ap"
                      toast(s_en?"Done":"Fatto",s_en?"Done":"Fatto"); }
        else if(s_sel==1){ s_im=IM_APSSID; snprintf(s_ibuf,sizeof s_ibuf,"%s",nucleo_setup_ap_ssid()); s_ilen=(int)strlen(s_ibuf); }
        else if(s_sel==2){ s_im=IM_APPASS; snprintf(s_ibuf,sizeof s_ibuf,"%s",nucleo_setup_ap_pass()); s_ilen=(int)strlen(s_ibuf); }
        break;
    case T_DISP:
        if(s_sel==DISP_BRIGHT||s_sel==DISP_VOL){ s_edit=true; update_hint(); }
        else if(s_sel==DISP_MUTE){ nucleo_audio_set_mute(!nucleo_audio_is_muted()); nucleo_app_persist_prefs();
                           toast(nucleo_audio_is_muted()?(s_en?"Muted":"Muto"):(s_en?"Sound":"Audio"),
                                 nucleo_audio_is_muted()?"Muted":"Sound"); }
        else if(s_sel==DISP_VOICE){ nucleo_voice_set_always_on(!nucleo_voice_always_on());
                           toast(nucleo_voice_always_on()?(s_en?"Voice on":"Voce attiva"):(s_en?"Voice off":"Voce off"),
                                 nucleo_voice_always_on()?"Voice on":"Voice off"); }
        break;
    case T_RESET:
        if(s_rst_n>0&&s_rst_type!=(s_sel+1)){ s_rst_n=0; s_rst_type=0; }
        if(s_rst_n==0){      s_rst_type=s_sel+1; s_rst_n=1;
            toast(s_en?"ENTER twice more":"INVIO ancora 2 volte",s_en?"ENTER twice more":"INVIO ancora 2 volte"); }
        else if(s_rst_n==1){ s_rst_n=2;
            toast(s_en?"ENTER once more":"INVIO ancora 1 volta",s_en?"ENTER once more":"INVIO ancora 1 volta"); }
        else{ if(s_rst_type==1) do_reset_soft(); else do_reset_hard(); }
        break;
    case T_SYS:
        if(s_sel==SYS_NAME){ s_im=IM_NAME; snprintf(s_ibuf,sizeof s_ibuf,"%s",nucleo_setup_device_name()); s_ilen=(int)strlen(s_ibuf); }
        else if(s_sel==SYS_LANG){ nucleo_i18n_set_en(!nucleo_i18n_is_en()); s_en=nucleo_i18n_is_en(); update_hint(); }
        else if(s_sel==SYS_THEME){ theme_cycle(); toast(theme_name(),theme_name()); }
        else if(s_sel==SYS_WIFI){
            if(!strcmp(nucleo_setup_mode(),"sta")) nucleo_setup_start_ap();
            else nucleo_setup_stop_ap();   // same fix: turning AP off must return to STA, not re-apply "ap"
            toast(s_en?"Done":"Fatto",s_en?"Done":"Fatto");
        }
        else if(s_sel==SYS_FORGET){
            if(s_fconf){ nucleo_setup_forget(); s_fconf=false; toast("Reti dimenticate","All forgotten"); }
            else { s_fconf=true; toast(s_en?"ENTER again":"INVIO ancora",s_en?"ENTER again":"INVIO ancora"); }
        }
        else if(s_sel==SYS_RESTART){
            if(s_rconf) esp_restart();
            else { s_rconf=true; toast(s_en?"ENTER again":"INVIO ancora",s_en?"ENTER again":"INVIO ancora"); }
        }
        else if(s_sel==SYS_MANUAL){ s_manual=true; }
        break;
    }
    nucleo_app_request_draw();
}

// ---- slider adjust (LUCE edit mode) ----------------------------------------
static void slider_adjust(int delta){
    if(s_tab==T_DISP){
        if(s_sel==DISP_BRIGHT) nucleo_app_set_brightness(nucleo_app_brightness()+delta);
        else if(s_sel==DISP_VOL) nucleo_audio_set_volume(nucleo_audio_volume()+delta);
    }
    nucleo_app_request_draw();
}

// ---- on_key (mirrors app_video settings_key) -------------------------------
// LEFT and Esc never arrive here — the framework routes them to wifi_back.
static void on_key(int k,char ch){
    if(s_manual){ s_manual=false; nucleo_app_request_draw(); return; }
    if(s_busy) return;
    if(s_im!=IM_NONE){ input_key(k,ch); return; }
    if(s_rconf&&k!=NK_ENTER){ s_rconf=false; }
    if(s_fconf&&k!=NK_ENTER){ s_fconf=false; }
    if(s_rst_n>0&&k!=NK_ENTER){ s_rst_n=0; s_rst_type=0; }

    if(s_edit){                                              // slider adjust mode
        if(k==NK_RIGHT||k==NK_UP) slider_adjust(+5);
        else if(k==NK_DOWN)       slider_adjust(-5);
        else if(k==NK_ENTER){ s_edit=false; nucleo_app_persist_prefs(); update_hint(); nucleo_app_request_draw(); }  // save adjusted brightness/volume on exit
        return;
    }
    if(k==NK_RIGHT){ page_tab(); return; }                   // horizontal pager from anywhere

    int n=tab_rows(s_tab);
    if(s_sel==-1){                                           // tab header
        if(k==NK_DOWN && n>0){ s_sel=0; update_hint(); }
        else if(k==NK_ENTER){ header_enter(); }
        nucleo_app_request_draw(); return;
    }
    if(k==NK_UP){ s_sel=(s_sel>0)?s_sel-1:-1; update_hint(); }  // row 0 -> header
    else if(k==NK_DOWN){ if(s_sel<n-1)s_sel++; }
    else if(k==NK_DEL && s_tab==T_RETE){                        // forget a saved network from the list
        const char*ss=nucleo_setup_scan_ssid(s_sel);
        if(nucleo_setup_net_is_known(ss)){ nucleo_setup_forget_ssid(ss); toast("Rete dimenticata","Forgotten"); }
    }
    else if(k==NK_ENTER){ activate(); }
    nucleo_app_request_draw();
}

// ---- back (Esc) / LEFT --------------------------------------------------------
// Both keys land here (the framework routes Esc and Left to the back handler). LEFT
// is a backward tab pager and never closes the app; Esc does the hierarchical pop
// (row -> header -> close) so the launcher is reached only with Esc.
static bool wifi_back(int key){
    if(s_manual){ s_manual=false; nucleo_app_request_draw(); return true; }
    if(s_im!=IM_NONE){ s_im=IM_NONE; memset(s_ibuf,0,sizeof s_ibuf); s_ilen=0; nucleo_app_request_draw(); return true; }
    if(s_busy) return true;
    if(s_edit){
        if(key==NK_LEFT) slider_adjust(-5);                  // Left = value down
        else { s_edit=false; nucleo_app_persist_prefs(); update_hint(); nucleo_app_request_draw(); }   // Esc = done (save adjusted value)
        return true;
    }
    // LEFT pages the tabs backward (mirror of RIGHT); it must never close the app —
    // only Esc returns to the launcher.
    if(key==NK_LEFT){ page_tab_back(); return true; }
    // Esc: hierarchical pop — a selected row returns to the header, the header closes the app.
    if(s_sel>=0){ s_sel=-1; update_hint(); nucleo_app_request_draw(); return true; }  // row -> header
    return false;   // header + Esc -> let the framework close the app
}

// ---- on_tick ---------------------------------------------------------------
static void on_tick(void){
    s_tickn++;
    if(s_msg_t>0&&--s_msg_t==0) nucleo_app_request_draw();
    if(s_busy){
        s_anim++;
        if(s_done){
            s_busy=false; s_done=false; int op=s_op; s_op=OP_NONE;
            if(op==OP_JOIN){
                memset(s_join_pass,0,sizeof s_join_pass);
                if(s_join_ok){ toast("Connessa","Connected"); s_tab=T_STATO; s_sel=-1; update_hint(); }
                else toast("Connessione fallita","Connect failed");
            }else if(op==OP_SCAN){
                s_sel=nucleo_setup_scan_count()>0?0:-1;
            }
        }
        nucleo_app_request_draw(); return;
    }
    if(s_tickn%5==0){
        int q=squality(nucleo_setup_rssi());
        if(s_histn<(int)sizeof s_hist) s_hist[s_histn++]=(uint8_t)q;
        else{ memmove(s_hist,s_hist+1,sizeof s_hist-1); s_hist[sizeof s_hist-1]=(uint8_t)q; }
        char key[80]; snprintf(key,sizeof key,"%s|%s|%d|%s",
            nucleo_setup_mode(),nucleo_setup_ip(),nucleo_setup_rssi(),nucleo_setup_ssid());
        if(s_tab==T_STATO||strcmp(key,s_skey)){ snprintf(s_skey,sizeof s_skey,"%s",key); nucleo_app_request_draw(); }
    }
}

// ---- lifecycle -------------------------------------------------------------
static void enter(void){
    s_en = nucleo_i18n_is_en();           // follow the system language
    s_tab=T_STATO; s_sel=-1; s_im=IM_NONE; s_manual=false;
    s_msg_t=0; s_histn=0; s_skey[0]=0; s_rconf=false; s_fconf=false;
    s_rst_n=0; s_rst_type=0;
    if(!s_task){ s_busy=false; s_op=OP_NONE; s_done=false; }
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(wifi_back);
    update_hint(); nucleo_app_request_draw();
}
static void leave(void){
    memset(s_join_pass,0,sizeof s_join_pass);
    memset(s_ibuf,0,sizeof s_ibuf);
}

extern "C" void nucleo_register_wifi(void){
    static const nucleo_app_def_t app={
        "wifi","Impostazioni","System","WiFi, AP, display, voce, sistema, reset",
        'W',ACC,enter,on_key,on_tick,on_draw,leave
    };
    nucleo_app_register(&app);
}
