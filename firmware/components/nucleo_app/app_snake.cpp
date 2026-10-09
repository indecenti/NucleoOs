// app_snake.cpp — Snake Duel: 1v1 over the network (ESP-NOW) or vs AI
// World 80×40 cells, CELL=8px, the camera follows your own snake (like Tank Duel).
// Host-authoritative. HUD with a minimap. NX_SOLO only: httpd is left untouched in Solo boot.
// Power-up: SPEED, SLOW, SHORT, GHOST, SHIELD.
//
// Play is fullscreen (HUD 23 px + 14 rows of cells = 135 px); menus keep the footer hint. Frames land in the
// shared 8bpp RGB332 canvas, so the colours sit on that grid. Text goes through game_text (5 languages).
// Esc in a match pauses (vs CPU the match freezes), Esc again leaves; ENTER resumes.
#include "app_gfx.h"
#include "game_sfx.h"
#include "game_text.h"
#include "game_ui.h"
#include "launcher_theme.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_pnet.h"
#include "nucleo_exclusive.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

// ─── world / camera ──────────────────────────────────────────────────────────
#define WORLD_W   80
#define WORLD_H   40
#define CELL       8      // px per cell
#define HUD_H     23
#define PLAY_Y    HUD_H
#define VIEW_W    30      // visible cells X: 240/8=30
#define VIEW_H    14      // visible cells Y: (135-23)/8=14 (fullscreen play)

// Minimap in the HUD
#define MM_X      (W-42)
#define MM_Y        1
#define MM_W       40     // 1px = 2 cells X (WORLD_W/MM_W = 2)
#define MM_H       20     // 1px = 2 cells Y (WORLD_H/MM_H = 2)

// ─── timing ───────────────────────────────────────────────────────────────────
#define MOVE_MS      200LL
#define FAST_MS      110LL
#define SLOW_MS      340LL
#define READY_MS    1800LL   // 3-2-1 before the snakes move
#define PU_TICKS       8
#define HELLO_US  400000LL
#define TX_US      40000LL
#define RX_TIMEOUT  3500000LL
#define JOIN_RETRY   350000LL
#define JOIN_TIMEOUT 4000000LL
#define FRAME_US   33333LL

// ─── limits ───────────────────────────────────────────────────────────────────
#define MAX_SEG   60
#define NET_SEG   38
#define MAX_HOSTS  6
#define N_PARTS   24
#define AI_REACH 120      // flood-fill budget (cells) when the AI checks a move for a dead end

// ─── directions ───────────────────────────────────────────────────────────────
enum { DUP=0, DRT=1, DDN=2, DLT=3 };
static const int8_t DX[4]={0,1,0,-1}, DY[4]={-1,0,1,0};
#define OPP(d_) ((d_)^2)

// ─── colours (RGB332-safe) ───────────────────────────────────────────────────
#define COL_P1    0x07E0              // green snake (you, or the host)
#define COL_P1D   0x0560
#define COL_P2    0xF81F              // magenta snake
#define COL_P2D   0xA815
#define COL_BGK   0x0000
#define COL_PANEL 0x0151              // navy card (0,40,136)
#define COL_EDGE  0x5D7F              // light blue border
#define COL_TXT   0xFFFF
#define COL_MUTE  0xB5B6
#define COL_DIMC  0x6B5F              // (109,109,255)-ish dim text
#define COL_GOLD  0xFE60
#define COL_ROCK  0x9A86              // (152,80,48)
#define COL_ROCKH 0xDD0C
#define COL_WALL  0x2110              // (36,36,128)
#define COL_WALLH 0x4A5F

// ─── power-up ─────────────────────────────────────────────────────────────────
enum { PU_NONE=0, PU_SPEED, PU_SLOW, PU_SHORT, PU_GHOST, PU_SHIELD, PU_COUNT };
static const uint16_t PU_COL[PU_COUNT]={0, 0xFFE0, 0xA01F, 0xFC1F, 0xBDF7, 0x07FF};
static const char     PU_SYM[PU_COUNT]={' ','F','L','X','G','S'};

// ─── states ───────────────────────────────────────────────────────────────────
enum { ST_MENU=0, ST_HOST, ST_BROWSE, ST_PLAY, ST_OVER, ST_HELP, ST_SCORES };
enum { MODE_AI=0, MODE_HOST=1, MODE_GUEST=2 };
#define N_MENU 5

// ─── protocol ─────────────────────────────────────────────────────────────────
#define SN_M0 'S'
#define SN_M1 'N'
#define SN_VER 2          // 2: rocks never block the start lanes (the map from a seed changed — v1 never pairs)
enum { SN_HELLO=1, SN_JOIN, SN_ACCEPT, SN_STATE, SN_INPUT, SN_BYE };

#pragma pack(push,1)
struct sn_hdr_t    { char m0,m1; uint8_t ver,type; };
struct sn_hello_t  { sn_hdr_t h; char name[12]; uint8_t status; };
struct sn_join_t   { sn_hdr_t h; char name[12]; };
struct sn_accept_t { sn_hdr_t h; uint32_t seed; char host[12]; };
struct sn_input_t  { sn_hdr_t h; uint8_t seq; uint8_t dir; };
struct sn_bye_t    { sn_hdr_t h; };
struct sn_state_t {
    sn_hdr_t h;
    uint8_t  tick;
    uint8_t  s1_len, s1_dir, s1_alive, s1_pu, s1_put, s1_score;
    uint8_t  s2_len, s2_dir, s2_alive, s2_pu, s2_put, s2_score;
    int8_t   fx, fy, fx2, fy2;   // 2 foods always present
    uint8_t  pu_type; int8_t pu_x, pu_y;
    uint8_t  phase;
    int8_t   segs[NET_SEG*4]; // s1 × NET_SEG × (x,y) then s2
};
#pragma pack(pop)
static_assert(sizeof(sn_state_t) <= PNET_MAXMSG, "State packet too large");

// ─── snake ────────────────────────────────────────────────────────────────────
struct Snake {
    int8_t  bx[MAX_SEG], by[MAX_SEG]; // [0]=head
    int     len;
    int8_t  dir, next_dir;
    int8_t  inq[2];                  // input queue: up to 2 buffered turns (responsive quick turns)
    uint8_t inq_n;
    bool    alive;
    uint8_t pu;
    int     pu_t;
    int     score;
    char    name[13];
    int64_t move_next_us;
};

// ─── obstacles ───────────────────────────────────────────────────────────────
// Heap-on-enter (was .bss ~3.1 KB): a Solo-boot game is closed during normal OS boot, so this map
// held boot RAM for nothing. calloc in on_enter(), freed on_exit; readers skip cleanly if null.
// Bit 0 = rock/wall; bits 6/7 are the AI's scratch marks (snake bodies / flood-fill visited).
static uint8_t (*s_obstacles)[WORLD_W] = nullptr;
#define OB_ROCK  0x01
#define OB_BODY  0x40
#define OB_SEEN  0x80

// ─── particles ───────────────────────────────────────────────────────────────
struct Part { float x,y,vx,vy; int life; uint16_t col; };

// ─── SFX ──────────────────────────────────────────────────────────────────────
// A chiptune pack rendered on the PC (tools/sfx-gen/games/snake.py -> /sd/data/snake/pack), played by
// game_sfx: nothing is synthesized on the device; a cue without its WAV plays a short tone at SFX_HZ.
static const int SFX_ENABLED = 1;
enum { SX_EAT=1, SX_PU, SX_DIE, SX_WIN, SX_NAV, SX_START, SX_COUNT=SX_START };
static const char *const SFX_NAME[SX_COUNT+1]={"","eat","pu","die","win","nav","start"};
static const uint16_t SFX_HZ[SX_COUNT+1]={0,880,1100,220,1047,440,880};
static const char* sn_sfx_name(int id) { return (id>0&&id<=SX_COUNT)?SFX_NAME[id]:"?"; }
static int sn_sfx_recipe(int id, notify_voice_t* v) {
    if(id<=0||id>SX_COUNT) return 0;
    notify__voice(&v[0],SFX_HZ[id],0,0.1f); return 1;
}
static bool sn_sfx_important(int id) { return id==SX_WIN || id==SX_DIE; }

static const game_sfx_t s_sfx = {
    .dir       = "/sd/data/snake",
    .name      = sn_sfx_name,
    .recipe    = sn_sfx_recipe,
    .count     = SX_COUNT,
    .ver       = 1,
    .rate      = 0,
    .important = sn_sfx_important,
    .enabled   = &SFX_ENABLED
};
#define SFX(id) game_sfx_play(&s_sfx,(id))

// ─── global state ────────────────────────────────────────────────────────────
static int      s_st;
static int      s_mode;
static Snake   *s_snk;                  // 2 snakes, per-app RAM (APP_RAM): 0 B when closed
#define s_s1 (s_snk[0])
#define s_s2 (s_snk[1])
static int8_t   s_fx, s_fy, s_fx2, s_fy2;  // 2 simultaneous foods
static uint8_t  s_pu_type;
static int8_t   s_pu_x, s_pu_y;
static int      s_pu_life;
static int      s_pu_next;
static uint32_t s_rng;
static uint8_t  s_tick;
static int64_t  s_last_us, s_go_us, s_pause_us;
static int      s_winner;
static int      s_wins1, s_wins2;       // persisted: matches won by you / by the rival
static int      s_hisc;                 // persisted: most food in one match
static int      s_flash;
static int      s_browse_sel;
static int      s_help_pg;
static gui::Menu s_menu, s_bmenu;          // main menu / match list (selection + glide)
static int      s_cam_x, s_cam_y;   // top-left of the viewport in world cells
static bool     s_paused, s_peerleft;
static unsigned s_anim;

static uint8_t  s_peer[6];
static int64_t  s_last_tx_us;
static int64_t  s_last_rx_us;
static int64_t  s_hello_us;
static int64_t  s_join_first_us;
static int64_t  s_join_retry_us;
static bool     s_join_pending;
static uint8_t  s_seq;

struct HostEntry { uint8_t mac[6]; char name[13]; int64_t ts; bool valid; };
static HostEntry *s_hosts;               // MAX_HOSTS entries, APP_RAM (only touched from poll/draw/keys)
static int       s_n_hosts;

static Part *s_parts;                    // N_PARTS entries, APP_RAM

// ─── RNG ──────────────────────────────────────────────────────────────────────
static uint32_t rng_next(void) {
    s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng;
}
static int rng_range(int lo, int hi) {
    return lo+(int)(rng_next()%(uint32_t)(hi-lo+1));
}

// ─── text ───────────────────────────────────────────────────────────
static void txt(int x,int y,int sz,uint16_t col,const char*s){ d.setTextSize(sz); d.setTextColor(col); d.setCursor(x,y); d.print(s); }
static void txc(int cx,int y,int sz,uint16_t col,const char*s){ txt(cx-(int)strlen(s)*3*sz,y,sz,col,s); }
static void txr(int rx,int y,int sz,uint16_t col,const char*s){ txt(rx-(int)strlen(s)*6*sz,y,sz,col,s); }

// ─── persistence (wins + record survive a relaunch) ─────────────────────────
#define STATS_PATH  "/sd/data/snake/stats.bin"
#define STATS_MAGIC 0x534E4B31u    // 'SNK1'
static void stats_save(void) {
    mkdir("/sd/data",0777); mkdir("/sd/data/snake",0777);
    FILE *f=fopen(STATS_PATH,"wb"); if(!f) return;
    int32_t v[4]={(int32_t)STATS_MAGIC,s_wins1,s_wins2,s_hisc}; fwrite(v,sizeof v,1,f); fclose(f);
}
static void stats_load(void) {
    s_wins1=s_wins2=s_hisc=0;
    FILE *f=fopen(STATS_PATH,"rb"); if(!f) return;
    int32_t v[4]; if(fread(v,sizeof v,1,f)==1&&(uint32_t)v[0]==STATS_MAGIC){ s_wins1=v[1]; s_wins2=v[2]; s_hisc=v[3]; }
    fclose(f);
}

// ─── camera ───────────────────────────────────────────────────────────────────
static void cam_update(void) {
    const Snake& own=(s_mode==MODE_GUEST)?s_s2:s_s1;
    if(!own.alive) return;
    int cx=(int)own.bx[0]-VIEW_W/2;
    int cy=(int)own.by[0]-VIEW_H/2;
    if(cx<0)cx=0;
    if(cx>WORLD_W-VIEW_W)cx=WORLD_W-VIEW_W;
    if(cy<0)cy=0;
    if(cy>WORLD_H-VIEW_H)cy=WORLD_H-VIEW_H;
    s_cam_x=cx; s_cam_y=cy;
}
// Screen coordinates from a world cell
static inline int sx_(int8_t c) { return (c-s_cam_x)*CELL; }
static inline int sy_(int8_t r) { return HUD_H+(r-s_cam_y)*CELL; }
static inline bool inv_(int8_t c, int8_t r) {
    return c>=s_cam_x&&c<s_cam_x+VIEW_W&&r>=s_cam_y&&r<s_cam_y+VIEW_H;
}

// ─── snake helpers ────────────────────────────────────────────────────────────
static void snake_init(Snake& s, int8_t hx, int8_t hy, int8_t dir, const char* nm) {
    char keep[13]; snprintf(keep,sizeof keep,"%s",nm);   // nm may point into s.name (memset below)
    memset(&s,0,sizeof(s));
    s.dir=s.next_dir=dir; s.alive=true; s.len=5;
    memcpy(s.name,keep,sizeof keep);
    for(int i=0;i<s.len;i++){s.bx[i]=hx-DX[dir]*i; s.by[i]=hy-DY[dir]*i;}
}
static bool snake_has(const Snake& s, int8_t x, int8_t y, int skip=0) {
    for(int i=skip;i<s.len;i++) if(s.bx[i]==x&&s.by[i]==y) return true;
    return false;
}
// Grow by n: the new tail segments sit on the current tail (they unfold as the snake moves). The old code
// only bumped len, which exposed a stale segment from an earlier, longer body for one step.
static void snake_grow(Snake& s, int n) {
    for(int k=0;k<n&&s.len<MAX_SEG;k++){ s.bx[s.len]=s.bx[s.len-1]; s.by[s.len]=s.by[s.len-1]; s.len++; }
}

// ─── obstacle map ─────────────────────────────────────────────────────────────
static void gen_obstacles(void) {
    if(!s_obstacles) return;
    memset(s_obstacles,0,(size_t)WORLD_H*WORLD_W);
    // Borders
    for(int c=0;c<WORLD_W;c++) s_obstacles[0][c]=s_obstacles[WORLD_H-1][c]=OB_ROCK;
    for(int r=0;r<WORLD_H;r++) s_obstacles[r][0]=s_obstacles[r][WORLD_W-1]=OB_ROCK;
    // Scattered rocks: ~15 random 2×2 blocks, never in the two start lanes (row WORLD_H/2 ±3 from the wall
    // to 10 cells past each start) — a rock there killed a snake before its player could steer.
    for(int i=0;i<15;i++) {
        int x=rng_range(4,WORLD_W-5), y=rng_range(4,WORLD_H-5);
        bool lane=(y+1>=WORLD_H/2-3&&y<=WORLD_H/2+3)&&(x<=24||x+1>=WORLD_W-25);
        if(lane) continue;
        for(int dy=0;dy<2;dy++) for(int dx=0;dx<2;dx++) s_obstacles[y+dy][x+dx]=OB_ROCK;
    }
}
static inline bool is_obstacle(int8_t x, int8_t y) {
    if(!s_obstacles) return true;
    return (x<0||x>=WORLD_W||y<0||y>=WORLD_H) ? true : (s_obstacles[y][x]&OB_ROCK);
}

// ─── particles ───────────────────────────────────────────────────────────────
static void parts_spawn(float px, float py, uint16_t col, int n) {
    int k=0;
    for(int i=0;i<N_PARTS;i++) {
        Part& p=s_parts[i];
        if(p.life>0||k>=n) continue;
        float ang=k*0.5236f; // π/6 per particle
        float sp=2.5f+(k&3)*0.7f;
        p={px,py,cosf(ang)*sp,sinf(ang)*sp-0.3f,35,col}; k++;
    }
}
static void parts_step(void) {
    for(int i=0;i<N_PARTS;i++){Part& p=s_parts[i];if(!p.life)continue;p.x+=p.vx;p.y+=p.vy;p.vy+=0.12f;p.life--;}
}

// ─── field: food / power-up ────────────────────────────────────────────────
// A free cell: no rock (food inside a rock could never be eaten), no snake, no other item.
static void spawn_free(int8_t& ox, int8_t& oy) {
    for(int t=0;t<600;t++) {
        int8_t cx=rng_range(1,WORLD_W-2), cy=rng_range(1,WORLD_H-2);
        if(!is_obstacle(cx,cy)&&!snake_has(s_s1,cx,cy)&&!snake_has(s_s2,cx,cy)
           &&!(s_pu_type&&cx==s_pu_x&&cy==s_pu_y)
           &&!(cx==s_fx&&cy==s_fy)
           &&!(cx==s_fx2&&cy==s_fy2)){ox=cx;oy=cy;return;}
    }
    ox=WORLD_W/2; oy=WORLD_H/2;
}
static void spawn_food(void)  { spawn_free(s_fx,s_fy); }
static void spawn_food2(void) { spawn_free(s_fx2,s_fy2); }
static void spawn_pu(void) {
    s_pu_type=(uint8_t)rng_range(PU_SPEED,PU_SHIELD);
    spawn_free(s_pu_x,s_pu_y);
    s_pu_life=20;
}

// ─── apply power-up ──────────────────────────────────────────────────────────
static void apply_pu(Snake& me, Snake& opp, uint8_t pu) {
    SFX(SX_PU);
    switch(pu) {
        case PU_SPEED:  me.pu=PU_SPEED;  me.pu_t=PU_TICKS; break;
        case PU_SLOW:   opp.pu=PU_SLOW;  opp.pu_t=PU_TICKS; break;
        case PU_SHORT:  me.len=(me.len>9)?me.len-6:3; break;
        case PU_GHOST:  me.pu=PU_GHOST;  me.pu_t=PU_TICKS; break;
        case PU_SHIELD: me.pu=PU_SHIELD; me.pu_t=PU_TICKS*3; break;   // lasts longer; absorbs one crash
    }
}
static int64_t snake_interval(const Snake& s) {
    if(s.pu==PU_SPEED) return FAST_MS*1000LL;
    if(s.pu==PU_SLOW)  return SLOW_MS*1000LL;
    // Progressive speed: -4ms per food eaten (combined), minimum 100ms
    int total = s_s1.score + s_s2.score;
    int64_t base = (MOVE_MS - total*4)*1000LL;
    if(base < 100000LL) base = 100000LL;
    return base;
}

// ─── step one snake (HOST only) ────────────────────────────────────────────
static void eat(Snake& s, int8_t nx, int8_t ny, uint16_t col) {
    snake_grow(s,2);
    s.score++; if(s.score>s_hisc)s_hisc=s.score;
    parts_spawn((float)(sx_(nx)+CELL/2), (float)(sy_(ny)+CELL/2), col, 12);
    SFX(SX_EAT);
}
static bool snake_step(Snake& s, Snake& opp) {
    if(!s.alive) return false;
    // Pop one buffered turn per step so quick double-taps (e.g. up-then-left to dodge) all register.
    if(s.inq_n>0){ int8_t nd=s.inq[0]; s.inq[0]=s.inq[1]; s.inq_n--; if(nd!=OPP(s.dir)) s.next_dir=nd; }
    if(s.next_dir!=OPP(s.dir)) s.dir=s.next_dir;
    int8_t nx=s.bx[0]+DX[s.dir], ny=s.by[0]+DY[s.dir];
    bool lethal=false;
    if(s.pu==PU_GHOST){nx=(nx+WORLD_W)%WORLD_W; ny=(ny+WORLD_H)%WORLD_H;}
    else if(is_obstacle(nx,ny)) lethal=true;
    if(!lethal) for(int i=0;i<s.len-1;i++) if(s.bx[i]==nx&&s.by[i]==ny){lethal=true;break;}
    if(lethal){
        if(s.pu==PU_SHIELD){            // shield absorbs one crash: drop it, flash, survive in place
            s.pu=PU_NONE; s.pu_t=0; SFX(SX_PU);
            parts_spawn((float)(sx_(s.bx[0])+CELL/2),(float)(sy_(s.by[0])+CELL/2),0x07FF,18);
            return true;
        }
        s.alive=false; return false;
    }
    memmove(&s.bx[1],&s.bx[0],s.len-1);
    memmove(&s.by[1],&s.by[0],s.len-1);
    s.bx[0]=nx; s.by[0]=ny;
    if(nx==s_fx&&ny==s_fy)   { eat(s,nx,ny,0xF800); spawn_food(); }
    if(nx==s_fx2&&ny==s_fy2) { eat(s,nx,ny,COL_GOLD); spawn_food2(); }
    if(s_pu_type&&nx==s_pu_x&&ny==s_pu_y){
        parts_spawn((float)(sx_(nx)+CELL/2), (float)(sy_(ny)+CELL/2), PU_COL[s_pu_type], 16);
        apply_pu(s,opp,s_pu_type);s_pu_type=PU_NONE;
    }
    return true;
}

// ─── AI ───────────────────────────────────────────────────────────────────────
// Free cells reachable from (x,y), capped at AI_REACH: a move into a pocket smaller than the snake is a
// slow death the old one-step lookahead walked straight into. Bodies are pre-marked OB_BODY; visited cells
// get OB_SEEN and are cleared again from the queue itself (no extra map). Stack: 2 x AI_REACH bytes.
static int ai_reach(int8_t x, int8_t y, bool wrap) {
    int8_t qx[AI_REACH], qy[AI_REACH]; int n=0, h=0;
    if(s_obstacles[y][x]&(OB_ROCK|OB_BODY)) return 0;
    s_obstacles[y][x]|=OB_SEEN; qx[n]=x; qy[n]=y; n++;
    while(h<n&&n<AI_REACH){
        int8_t cx=qx[h], cy=qy[h]; h++;
        for(int k=0;k<4&&n<AI_REACH;k++){
            int nx=cx+DX[k], ny=cy+DY[k];
            if(wrap){ nx=(nx+WORLD_W)%WORLD_W; ny=(ny+WORLD_H)%WORLD_H; }
            else if(nx<0||nx>=WORLD_W||ny<0||ny>=WORLD_H) continue;
            uint8_t &c=s_obstacles[ny][nx];
            if(c&(OB_ROCK|OB_BODY|OB_SEEN)) continue;
            c|=OB_SEEN; qx[n]=(int8_t)nx; qy[n]=(int8_t)ny; n++;
        }
    }
    for(int i=0;i<n;i++) s_obstacles[qy[i]][qx[i]]&=(uint8_t)~OB_SEEN;
    return n;
}
static void mark_bodies(uint8_t on) {
    for(int k=0;k<2;k++){ const Snake& s=s_snk[k]; if(!s.alive) continue;
        for(int i=0;i<s.len-1;i++){ uint8_t &c=s_obstacles[s.by[i]][s.bx[i]]; c=on?(c|OB_BODY):(c&(uint8_t)~OB_BODY); } }
}
static int8_t ai_choose(void) {
    const Snake& me=s_s2; const Snake& op=s_s1;
    int8_t hx=me.bx[0], hy=me.by[0];
    int best=me.dir, bsc=-99999;
    bool ghost=(me.pu==PU_GHOST);
    mark_bodies(1);
    for(int dd=0;dd<4;dd++) {
        if(dd==OPP(me.dir)) continue;
        int8_t nx=hx+DX[dd], ny=hy+DY[dd];
        if(ghost){nx=(int8_t)((nx+WORLD_W)%WORLD_W);ny=(int8_t)((ny+WORLD_H)%WORLD_H);}
        else if(is_obstacle(nx,ny)) continue;
        bool hit=false;
        for(int i=1;i<me.len-1;i++) if(me.bx[i]==nx&&me.by[i]==ny){hit=true;break;}
        if(hit) continue;
        bool opp_body=snake_has(op,nx,ny,1);
        bool head_clash=(abs(nx-op.bx[0])+abs(ny-op.by[0])<=1);   // the cell the rival's head can reach next
        int room=ai_reach(nx,ny,ghost);
        // Aim at whichever food is closer
        int d1=abs(nx-s_fx)+abs(ny-s_fy);
        int d2c=abs(nx-s_fx2)+abs(ny-s_fy2);
        int dist=d1<d2c?d1:d2c;
        int sc = -dist*3 - (opp_body?500:0) - (head_clash?80:0) + (room<AI_REACH?room*4-2000:0)
               + (room<me.len+4?-3000:0);
        if(sc>bsc){bsc=sc;best=dd;}
    }
    mark_bodies(0);
    return (int8_t)best;
}

// ─── game over ────────────────────────────────────────────────────────────────
static void go(int st);
static bool i_won(void){ return s_winner==((s_mode==MODE_GUEST)?2:1); }
static void on_death(void) {
    bool d1=!s_s1.alive, d2=!s_s2.alive;
    if(d1&&!d2)      s_winner=2;
    else if(d2&&!d1) s_winner=1;
    else             s_winner=(s_s1.score>=s_s2.score)?1:2;
    if(i_won()) s_wins1++; else s_wins2++;
    // Explosion in screen coordinates (camera already updated before game_step)
    if(d1) parts_spawn(sx_(s_s1.bx[0])+(float)CELL/2, sy_(s_s1.by[0])+(float)CELL/2, COL_P1, 20);
    if(d2) parts_spawn(sx_(s_s2.bx[0])+(float)CELL/2, sy_(s_s2.by[0])+(float)CELL/2, COL_P2, 20);
    SFX(i_won()?SX_WIN:SX_DIE);
    stats_save();
    s_flash=10; go(ST_OVER);
}

// ─── game logic (HOST/AI only) ────────────────────────────────────────────────
static void game_step(int64_t now) {
    if(now<s_go_us) return;                                   // 3-2-1: nobody moves yet
    if(s_mode==MODE_AI&&s_s2.alive) s_s2.next_dir=ai_choose();
    if(s_s1.pu&&--s_s1.pu_t<=0) s_s1.pu=PU_NONE;
    if(s_s2.pu&&--s_s2.pu_t<=0) s_s2.pu=PU_NONE;
    if(s_pu_type&&--s_pu_life<=0) s_pu_type=PU_NONE;
    if(!s_pu_type&&--s_pu_next<=0){ spawn_pu(); s_pu_next=rng_range(8,16); }
    // Save heads before the step to detect the swap (pass-through)
    int8_t old_h1x=s_s1.bx[0], old_h1y=s_s1.by[0];
    int8_t old_h2x=s_s2.bx[0], old_h2y=s_s2.by[0];
    if(s_s1.alive&&now>=s_s1.move_next_us){
        snake_step(s_s1,s_s2);
        s_s1.move_next_us=now+snake_interval(s_s1);
    }
    if(s_s2.alive&&now>=s_s2.move_next_us){
        snake_step(s_s2,s_s1);
        s_s2.move_next_us=now+snake_interval(s_s2);
    }
    // Cross-collision: head on opponent's body
    if(s_s1.alive&&snake_has(s_s2,s_s1.bx[0],s_s1.by[0],1)) s_s1.alive=false;
    if(s_s2.alive&&snake_has(s_s1,s_s2.bx[0],s_s2.by[0],1)) s_s2.alive=false;
    // Head-to-head: same node OR position swap
    if(s_s1.alive&&s_s2.alive) {
        bool same_cell = (s_s1.bx[0]==s_s2.bx[0]&&s_s1.by[0]==s_s2.by[0]);
        bool swap = (s_s1.bx[0]==old_h2x&&s_s1.by[0]==old_h2y
                     &&s_s2.bx[0]==old_h1x&&s_s2.by[0]==old_h1y);
        if(same_cell||swap){ s_s1.alive=false; s_s2.alive=false; }
    }
    s_tick++;
    if(!s_s1.alive||!s_s2.alive) on_death();
}

// ─── network send ─────────────────────────────────────────────────────────────
static void fill_hdr(void* buf, uint8_t type) {
    sn_hdr_t* h=(sn_hdr_t*)buf; h->m0=SN_M0; h->m1=SN_M1; h->ver=SN_VER; h->type=type;
}
static void send_hello(void) {
    sn_hello_t pk; fill_hdr(&pk,SN_HELLO);
    strncpy(pk.name,pnet_name(),11); pk.name[11]=0;
    pk.status=(s_st==ST_PLAY)?1:0;
    pnet_send(nullptr,&pk,sizeof(pk));
}
static void send_bye(void) {
    if(s_mode!=MODE_HOST&&s_mode!=MODE_GUEST) return;
    sn_bye_t bye; fill_hdr(&bye,SN_BYE); pnet_send(s_peer,&bye,sizeof(bye));
}
static sn_state_t *s_stpk;                // send_state() scratch, APP_RAM (sent only from poll_fn)
static void send_state(void) {
    sn_state_t& pk=*s_stpk;
    fill_hdr(&pk,SN_STATE); pk.tick=s_tick;
    int n1=(s_s1.len<NET_SEG)?s_s1.len:NET_SEG;
    pk.s1_len=n1; pk.s1_dir=s_s1.dir; pk.s1_alive=s_s1.alive;
    pk.s1_pu=s_s1.pu; pk.s1_put=s_s1.pu_t; pk.s1_score=s_s1.score;
    int n2=(s_s2.len<NET_SEG)?s_s2.len:NET_SEG;
    pk.s2_len=n2; pk.s2_dir=s_s2.dir; pk.s2_alive=s_s2.alive;
    pk.s2_pu=s_s2.pu; pk.s2_put=s_s2.pu_t; pk.s2_score=s_s2.score;
    pk.fx=s_fx; pk.fy=s_fy; pk.fx2=s_fx2; pk.fy2=s_fy2;
    pk.pu_type=s_pu_type; pk.pu_x=s_pu_x; pk.pu_y=s_pu_y;
    pk.phase=s_st;
    for(int i=0;i<n1;i++){pk.segs[i*2]=s_s1.bx[i];pk.segs[i*2+1]=s_s1.by[i];}
    int off=n1*2;
    for(int i=0;i<n2;i++){pk.segs[off+i*2]=s_s2.bx[i];pk.segs[off+i*2+1]=s_s2.by[i];}
    int plen=(int)(offsetof(sn_state_t,segs)+(n1+n2)*2);
    pnet_send(s_peer,&pk,plen);
}
static void send_input(int8_t dir) {
    sn_input_t pk; fill_hdr(&pk,SN_INPUT);
    pk.seq=s_seq++; pk.dir=(uint8_t)dir;
    pnet_send(s_peer,&pk,sizeof(pk));
}

// ─── start match ─────────────────────────────────────────────────────────────
static void start_game(uint32_t seed) {
    s_rng=seed?seed:0xDEADBEEFu;
    gen_obstacles();
    const char* nm1=(s_mode==MODE_GUEST)?s_s1.name:pnet_name();
    const char* nm2=(s_mode==MODE_GUEST)?pnet_name():(s_mode==MODE_AI?"CPU":s_s2.name);
    snake_init(s_s1, 12, WORLD_H/2, DRT, nm1);
    snake_init(s_s2, WORLD_W-13, WORLD_H/2, DLT, nm2);
    memset(s_parts,0,sizeof(Part)*N_PARTS);
    s_pu_type=PU_NONE; s_pu_next=rng_range(6,12);
    s_fx=s_fy=s_fx2=s_fy2=0;
    s_winner=0; s_flash=0; s_tick=0; s_peerleft=false;
    spawn_food(); spawn_food2();
    int64_t now=esp_timer_get_time();
    s_go_us=now+READY_MS*1000LL;
    s_s1.move_next_us=s_go_us;
    s_s2.move_next_us=s_go_us;
    s_last_rx_us=now; s_last_tx_us=now;
    go(ST_PLAY);
    cam_update();
    SFX(SX_START);
}

// ─── apply state (guest) ─────────────────────────────────────────────────────
static void apply_state(const sn_state_t* st, int plen) {
    int n1=st->s1_len, n2=st->s2_len;
    if(n1>NET_SEG||n2>NET_SEG) return;   // wire lengths are peer-controlled; a legit host caps at NET_SEG(38<MAX_SEG). Reject before the copy loops overrun bx/by[MAX_SEG] and clobber the len field.
    int need=(int)(offsetof(sn_state_t,segs)+(n1+n2)*2);
    if(plen<need) return;
    s_s1.len=n1; s_s1.dir=st->s1_dir&3; s_s1.alive=st->s1_alive;
    s_s1.pu=(st->s1_pu<PU_COUNT)?st->s1_pu:0; s_s1.pu_t=st->s1_put; s_s1.score=st->s1_score;   // pu indexes PU_COL[PU_COUNT]
    for(int i=0;i<n1;i++){s_s1.bx[i]=st->segs[i*2];s_s1.by[i]=st->segs[i*2+1];}
    int off=n1*2;
    s_s2.len=n2; s_s2.dir=st->s2_dir&3; s_s2.alive=st->s2_alive;
    s_s2.pu=(st->s2_pu<PU_COUNT)?st->s2_pu:0; s_s2.pu_t=st->s2_put; s_s2.score=st->s2_score;
    for(int i=0;i<n2;i++){s_s2.bx[i]=st->segs[off+i*2];s_s2.by[i]=st->segs[off+i*2+1];}
    s_fx=st->fx; s_fy=st->fy; s_fx2=st->fx2; s_fy2=st->fy2;
    s_pu_type=(st->pu_type<PU_COUNT)?st->pu_type:0; s_pu_x=st->pu_x; s_pu_y=st->pu_y;   // indexes PU_COL/PU_SYM[PU_COUNT]
    if(st->phase==ST_OVER&&s_st==ST_PLAY) {
        s_winner=(st->s1_alive)?1:2;
        if(i_won()) s_wins1++; else s_wins2++;
        stats_save();
        SFX(i_won()?SX_WIN:SX_DIE);
        // Explosion on the guest side
        cam_update();
        Snake& dead=(s_winner==1)?s_s2:s_s1;
        uint16_t dc=(s_winner==1)?COL_P2:COL_P1;
        parts_spawn(sx_(dead.bx[0])+(float)CELL/2, sy_(dead.by[0])+(float)CELL/2, dc, 16);
        s_flash=10; go(ST_OVER);
    }
    nucleo_app_request_draw();
}

// ─── packet handler ──────────────────────────────────────────────────────────
static void net_handle(const pnet_pkt_t* pkt) {
    const sn_hdr_t* hdr=(const sn_hdr_t*)pkt->buf;
    if(pkt->len<(int)sizeof(sn_hdr_t)) return;
    if(hdr->m0!=SN_M0||hdr->m1!=SN_M1||hdr->ver!=SN_VER) return;
    int64_t now=esp_timer_get_time();

    switch(hdr->type) {
        case SN_HELLO:
            if(s_st==ST_BROWSE&&pkt->len>=(int)sizeof(sn_hello_t)) {
                const sn_hello_t* h=(const sn_hello_t*)pkt->buf;
                if(h->status==1) return;
                int slot=-1;
                for(int i=0;i<s_n_hosts;i++) if(memcmp(s_hosts[i].mac,pkt->mac,6)==0){slot=i;break;}
                if(slot<0&&s_n_hosts<MAX_HOSTS) slot=s_n_hosts++;
                if(slot>=0) {
                    memcpy(s_hosts[slot].mac,pkt->mac,6);
                    strncpy(s_hosts[slot].name,h->name,12); s_hosts[slot].name[12]=0;
                    s_hosts[slot].ts=now; s_hosts[slot].valid=true;
                }
                nucleo_app_request_draw();
            }
            break;

        case SN_JOIN:
            if(s_st==ST_HOST&&pkt->len>=(int)sizeof(sn_join_t)) {
                const sn_join_t* jn=(const sn_join_t*)pkt->buf;
                memcpy(s_peer,pkt->mac,6);
                strncpy(s_s2.name,jn->name,12); s_s2.name[12]=0;
                sn_accept_t ac; fill_hdr(&ac,SN_ACCEPT);
                ac.seed=s_rng; strncpy(ac.host,pnet_name(),11); ac.host[11]=0;
                pnet_send(s_peer,&ac,sizeof(ac));
                s_mode=MODE_HOST;
                start_game(s_rng);
            } else if(s_st==ST_PLAY&&s_mode==MODE_HOST&&memcmp(pkt->mac,s_peer,6)==0) {
                sn_accept_t ac; fill_hdr(&ac,SN_ACCEPT);
                ac.seed=s_rng; strncpy(ac.host,pnet_name(),11); ac.host[11]=0;
                pnet_send(s_peer,&ac,sizeof(ac));
            }
            break;

        case SN_ACCEPT:
            if(s_st==ST_BROWSE&&s_join_pending&&pkt->len>=(int)sizeof(sn_accept_t)) {
                const sn_accept_t* ac=(const sn_accept_t*)pkt->buf;
                memcpy(s_peer,pkt->mac,6);
                strncpy(s_s1.name,ac->host,12); s_s1.name[12]=0;
                s_join_pending=false;
                s_mode=MODE_GUEST;
                start_game(ac->seed);
            }
            break;

        case SN_INPUT:
            if(s_mode==MODE_HOST&&pkt->len>=(int)sizeof(sn_input_t)
               &&memcmp(pkt->mac,s_peer,6)==0) {
                const sn_input_t* in=(const sn_input_t*)pkt->buf;
                if(in->dir<4) s_s2.next_dir=in->dir;
                s_last_rx_us=now;
            }
            break;

        case SN_STATE:
            if(s_mode==MODE_GUEST&&memcmp(pkt->mac,s_peer,6)==0
               &&pkt->len>=(int)offsetof(sn_state_t,segs)) {
                apply_state((const sn_state_t*)pkt->buf,pkt->len);
                s_last_rx_us=now;
            }
            break;

        case SN_BYE:
            if(memcmp(pkt->mac,s_peer,6)==0&&s_st==ST_PLAY) {
                s_winner=(s_mode==MODE_HOST)?1:2; s_peerleft=true;
                s_wins1++; stats_save();
                go(ST_OVER);
            }
            break;
    }
}

// ─── poll handler ─────────────────────────────────────────────────────────────
static bool poll_fn(void) {
    int64_t now=esp_timer_get_time();
    if(now-s_last_us<FRAME_US) return false;
    s_last_us=now;

    pnet_pkt_t pkt;
    while(pnet_recv(&pkt)) net_handle(&pkt);
    s_anim++;

    if(s_st==ST_HOST) {
        if(now-s_hello_us>HELLO_US){ send_hello(); s_hello_us=now; }
        return (s_anim&3)==0;                       // ~8 Hz is plenty for the waiting dots
    } else if(s_st==ST_MENU) {
        return gui::menu_tick(s_menu,(int)(FRAME_US/1000));   // redraw only while the cursor glides
    } else if(s_st==ST_BROWSE) {
        for(int i=0;i<s_n_hosts;) {
            if(now-s_hosts[i].ts>4000000LL){
                s_hosts[i]=s_hosts[--s_n_hosts]; nucleo_app_request_draw();
            } else i++;
        }
        if(s_browse_sel>=s_n_hosts) s_browse_sel=s_n_hosts>0?s_n_hosts-1:0;
        if(s_join_pending) {
            if(now-s_join_first_us>JOIN_TIMEOUT){ s_join_pending=false; nucleo_app_request_draw(); }
            else if(now-s_join_retry_us>JOIN_RETRY) {
                s_join_retry_us=now;
                if(s_browse_sel<s_n_hosts) {
                    sn_join_t jn; fill_hdr(&jn,SN_JOIN);
                    strncpy(jn.name,pnet_name(),11); jn.name[11]=0;
                    pnet_send(s_hosts[s_browse_sel].mac,&jn,sizeof(jn));
                }
            }
        }
        return gui::menu_tick(s_bmenu,(int)(FRAME_US/1000))||(s_anim&3)==0;
    } else if(s_st==ST_PLAY) {
        if(s_paused&&s_mode==MODE_AI) return false;   // frozen under a static pause card
        // Camera updated BEFORE game_step → explosions land at the correct position
        cam_update();
        if(s_mode==MODE_HOST||s_mode==MODE_AI) {
            game_step(now);
            if(s_mode==MODE_HOST&&now-s_last_tx_us>TX_US){
                s_last_tx_us=now; send_state();
            }
            if(s_st==ST_PLAY&&s_mode==MODE_HOST&&now-s_last_rx_us>RX_TIMEOUT){
                s_winner=1; s_peerleft=true; s_wins1++; stats_save(); go(ST_OVER);
            }
        } else {
            if(now-s_last_tx_us>TX_US){
                s_last_tx_us=now; send_input(s_s2.next_dir);
            }
            if(now-s_last_rx_us>RX_TIMEOUT){
                s_winner=2; s_peerleft=true; s_wins1++; stats_save(); go(ST_OVER);
            }
        }
        parts_step();
        if(s_flash>0) s_flash--;
        return true;
    } else if(s_st==ST_OVER) {
        if(s_mode==MODE_HOST&&now-s_last_tx_us>TX_US){ s_last_tx_us=now; send_state(); }
        bool fx=s_flash>0; parts_step(); if(s_flash>0) s_flash--;
        return fx;                                   // settle the death burst, then stay static
    }
    return false;                                    // menus / help / scores: redraw on input only
}

// ─── HUD with minimap ────────────────────────────────────────────────────────
static void hud_side(const Snake& s, uint16_t col, bool right) {
    char sc[8]; snprintf(sc,sizeof sc,"%d",s.score);
    char nm[9]; snprintf(nm,sizeof nm,"%.8s",s.name);
    int x0=right?MM_X-4:3;                      // outer edge of this player's block
    if(right){ txr(x0,4,2,COL_TXT,sc); int sw=(int)strlen(sc)*12; txr(x0-sw-4,3,1,col,nm); }
    else     { txt(x0,4,2,COL_TXT,sc); int sw=(int)strlen(sc)*12; txt(x0+sw+4,3,1,col,nm); }
    if(s.pu) {                                  // active power-up: letter chip + draining bar
        int full=(s.pu==PU_SHIELD)?PU_TICKS*3:PU_TICKS, bw=(s.pu_t*36)/full; if(bw<0) bw=0;
        int sw=(int)strlen(sc)*12, bx0=right?x0-sw-4-46:x0+sw+4;
        d.fillRoundRect(bx0,12,8,8,2,PU_COL[s.pu]);
        char sy[2]={PU_SYM[s.pu],0}; txt(bx0+1,12,1,COL_BGK,sy);
        d.fillRect(bx0+10,15,36,3,0x2104);
        d.fillRect(right?bx0+10+36-bw:bx0+10,15,bw,3,PU_COL[s.pu]);
    }
}
static void draw_hud(void) {
    d.fillRect(0,0,W,HUD_H,COL_BGK);
    d.drawFastHLine(0,HUD_H-1,W,COL_EDGE);
    hud_side(s_s1,COL_P1,false);
    hud_side(s_s2,COL_P2,true);
    // Minimap
    int mx=MM_X;
    d.fillRect(mx, MM_Y, MM_W, MM_H, COL_PANEL);
    d.drawRect(mx-1, MM_Y-1, MM_W+2, MM_H+2, COL_EDGE);
    d.drawRect(mx+s_cam_x/2, MM_Y+s_cam_y/2, VIEW_W/2, VIEW_H/2, 0x6B6D);   // viewport
    d.drawPixel(mx+s_fx/2,  MM_Y+s_fy/2,  0xF800);
    d.drawPixel(mx+s_fx2/2, MM_Y+s_fy2/2, COL_GOLD);
    if(s_pu_type) d.drawPixel(mx+s_pu_x/2, MM_Y+s_pu_y/2, PU_COL[s_pu_type]);
    for(int i=2;i<s_s1.len;i+=3) d.drawPixel(mx+s_s1.bx[i]/2, MM_Y+s_s1.by[i]/2, COL_P1D);
    for(int i=2;i<s_s2.len;i+=3) d.drawPixel(mx+s_s2.bx[i]/2, MM_Y+s_s2.by[i]/2, COL_P2D);
    d.fillRect(mx+s_s1.bx[0]/2, MM_Y+s_s1.by[0]/2, 2, 2, COL_P1);
    d.fillRect(mx+s_s2.bx[0]/2, MM_Y+s_s2.by[0]/2, 2, 2, COL_P2);
}

// ─── draw snake with camera ──────────────────────────────────────────────────
// A continuous body: each segment fills the union of its cell and the next one toward the head (inset 1 px,
// the last two taper to 2 px), in two alternating shades; the head is a rounded block with eyes + tongue.
static void draw_snake(const Snake& s, uint16_t col, uint16_t dark) {
    bool ghost=(s.pu==PU_GHOST);
    for(int i=s.len-1;i>=1;i--) {
        int8_t cx=s.bx[i], cy=s.by[i];
        if(!inv_(cx,cy)) continue;
        int in=(i>=s.len-2)?2:1;
        uint16_t c=((i>>1)&1)?dark:col;
        if(ghost&&(i&1)) continue;                       // see-through while ghosting
        int px=sx_(cx), py=sy_(cy);
        int dx=s.bx[i-1]-cx, dy=s.by[i-1]-cy;
        int x0=px+in, y0=py+in, x1=px+CELL-in, y1=py+CELL-in;
        if(abs(dx)+abs(dy)==1){ if(dx>0)x1+=CELL; if(dx<0)x0-=CELL; if(dy>0)y1+=CELL; if(dy<0)y0-=CELL; }
        if(y0<PLAY_Y) y0=PLAY_Y;
        d.fillRect(x0,y0,x1-x0,y1-y0,c);
        if(!(i&1)) d.fillRect(px+3,py+3,2,2,col|0x8410);                 // scales (a lighter tint of the body)
    }
    if(!inv_(s.bx[0],s.by[0])) return;
    int px=sx_(s.bx[0]), py=sy_(s.by[0]);
    int hcx=px+CELL/2, hcy=py+CELL/2;
    // Active power-up aura around the head (visual feedback).
    if(s.pu==PU_SHIELD){ d.drawCircle(hcx,hcy,CELL/2+3,0x07FF); d.drawCircle(hcx,hcy,CELL/2+2,0x0410); }
    else if(s.pu==PU_SPEED){ d.drawFastHLine(px-4,hcy-2,3,0xFFE0); d.drawFastHLine(px-5,hcy+1,3,0xFFE0); d.drawFastHLine(px+CELL+1,hcy-2,3,0xFFE0); }
    d.fillRoundRect(px-1,py-1,CELL+2,CELL+2,3,col);
    d.drawRoundRect(px-1,py-1,CELL+2,CELL+2,3,dark);
    // eyes (white, black pupil looking ahead) on the leading side
    int fx=DX[s.dir], fy=DY[s.dir];
    int ex1,ey1,ex2,ey2;
    if(fx){ ex1=ex2=hcx+fx*2-1; ey1=py+1; ey2=py+5; } else { ey1=ey2=hcy+fy*2-1; ex1=px+1; ex2=px+5; }
    d.fillRect(ex1,ey1,2,2,COL_TXT); d.fillRect(ex2,ey2,2,2,COL_TXT);
    d.drawPixel(ex1+(fx>0),ey1+(fy>0),COL_BGK); d.drawPixel(ex2+(fx>0),ey2+(fy>0),COL_BGK);
    if((s_anim>>2)&1){                                    // flicking tongue
        int tx=hcx+fx*(CELL/2+1), ty=hcy+fy*(CELL/2+1);
        if(fx) d.drawFastHLine(fx>0?tx:tx-2,hcy,3,0xF800); else d.drawFastVLine(hcx,fy>0?ty:ty-2,3,0xF800);
    }
}

// ─── draw field ───────────────────────────────────────────────────────────────
static void draw_play(void) {
    // floor: black with a navy dot on every cell corner, so speed and position read at a glance
    d.fillRect(0,PLAY_Y,W,H-PLAY_Y,COL_BGK);
    for(int gx_=0;gx_<VIEW_W;gx_++) for(int gy_=0;gy_<VIEW_H;gy_++) d.drawPixel(gx_*CELL,PLAY_Y+gy_*CELL,0x0010);
    // rocks (round boulders) and the border wall (bricks)
    for(int gx_=s_cam_x;gx_<s_cam_x+VIEW_W&&gx_<WORLD_W;gx_++) {
        for(int gy_=s_cam_y;gy_<s_cam_y+VIEW_H&&gy_<WORLD_H;gy_++) {
            if(!s_obstacles||!(s_obstacles[gy_][gx_]&OB_ROCK)) continue;
            int sx=sx_((int8_t)gx_), sy=sy_((int8_t)gy_);
            if(gx_==0||gy_==0||gx_==WORLD_W-1||gy_==WORLD_H-1){
                d.fillRect(sx,sy,CELL,CELL,COL_WALL);
                d.drawFastHLine(sx,sy+3,CELL,COL_WALLH); d.drawFastHLine(sx,sy+7,CELL,COL_WALLH);
                d.drawFastVLine(sx+((gy_&1)?2:6),sy,3,COL_WALLH); d.drawFastVLine(sx+((gy_&1)?6:2),sy+4,3,COL_WALLH);
            } else {
                d.fillRoundRect(sx,sy,CELL,CELL,3,COL_ROCK);
                d.drawFastHLine(sx+2,sy+1,3,COL_ROCKH); d.drawPixel(sx+1,sy+2,COL_ROCKH);
                d.drawFastHLine(sx+2,sy+CELL-1,CELL-4,0x4100);
            }
        }
    }
    int pulse=(s_anim>>3)&1;
    // Food 1 — an apple: red body, white glint, green leaf
    if(inv_(s_fx,s_fy)) {
        int cx=sx_(s_fx)+CELL/2, cy=sy_(s_fy)+CELL/2;
        if(pulse) d.drawCircle(cx,cy,6,0x6000);
        d.fillCircle(cx,cy+1,3,0xF800);
        d.drawPixel(cx-1,cy,COL_TXT);
        d.drawFastHLine(cx,cy-3,3,0x07E0); d.drawPixel(cx,cy-2,0x8200);
    }
    // Food 2 — a golden coin with a rim
    if(inv_(s_fx2,s_fy2)) {
        int cx=sx_(s_fx2)+CELL/2, cy=sy_(s_fy2)+CELL/2;
        if(!pulse) d.drawCircle(cx,cy,6,0x6300);
        d.fillCircle(cx,cy,4,0xFC00);
        d.fillCircle(cx,cy,3,COL_GOLD);
        d.drawFastVLine(cx,cy-2,4,0xFFF0);
    }
    // Power-up on the field: a lettered chip with a blinking ring
    if(s_pu_type&&inv_(s_pu_x,s_pu_y)) {
        int cx=sx_(s_pu_x)+CELL/2, cy=sy_(s_pu_y)+CELL/2;
        uint16_t pc=PU_COL[s_pu_type];
        if(pulse||s_pu_life>5) d.drawRoundRect(cx-7,cy-7,14,14,4,pc);
        d.fillRoundRect(cx-5,cy-5,11,11,3,pc);
        char sym[2]={PU_SYM[s_pu_type],0}; txt(cx-2,cy-3,1,COL_BGK,sym);
    }
    // Snakes: opponent below, own above
    if(s_mode==MODE_GUEST){ draw_snake(s_s1,COL_P1,COL_P1D);  draw_snake(s_s2,COL_P2,COL_P2D); }
    else                  { draw_snake(s_s2,COL_P2,COL_P2D);  draw_snake(s_s1,COL_P1,COL_P1D); }

    // Explosion particles
    for(int i=0;i<N_PARTS;i++)
        if(s_parts[i].life) d.fillRect((int)s_parts[i].x,(int)s_parts[i].y,3,3,s_parts[i].col);

    // Screen-edge flash on death
    if(s_flash>0) {
        uint16_t fc=(s_winner==1)?COL_P1:COL_P2;
        for(int t=0;t<(s_flash>5?2:1);t++)
            d.drawRect(t,PLAY_Y+t,W-2*t,H-PLAY_Y-2*t,fc);
    }
    draw_hud();

    if(s_st!=ST_PLAY) return;
    int64_t now=esp_timer_get_time();
    if(s_paused) {
        for(int y=PLAY_Y+1;y<H;y+=2) d.drawFastHLine(0,y,W,COL_BGK);
        gui::dialog(GT("PAUSA","PAUSED"),s_mode==MODE_AI?nullptr:GT("La partita continua!","The match goes on!"),nullptr,
                    GT("INVIO riprendi   Esc esci","ENTER resume   Esc leave"),COL_P1);
    } else if(now<s_go_us) {                              // 3-2-1 over the field
        char n[2]={(char)('1'+(int)((s_go_us-now)/(READY_MS*1000LL/3))),0};
        d.fillRoundRect(W/2-22,PLAY_Y+30,44,44,8,COL_PANEL);
        d.drawRoundRect(W/2-22,PLAY_Y+30,44,44,8,COL_EDGE);
        txc(W/2,PLAY_Y+38,4,COL_GOLD,n);
        txc(W/2,PLAY_Y+80,1,COL_TXT,GT("Pronti...","Get ready..."));
    }
}

/// ─── menu screens ────────────────────────────────────────────────────────────
// Titles, lists and the pause / result cards come from the console kit (game_ui.h), like every other game.
static void wait_dots(int cy,uint16_t col) {
    for(int i=0;i<3;i++) d.fillCircle(W/2-12+i*12,cy,2,(esp_timer_get_time()/300000)%3==i?col:0x2104);
}
static const char *menu_label(int i) {
    switch(i){
        case 0: return GT("Gioca vs CPU","Play vs CPU");
        case 1: return GT("Crea partita","Host a match");
        case 2: return GT("Entra in partita","Join a match");
        case 3: return GT("Record","Scores");
        default: return GT("Come si gioca","How to play");
    }
}
// a little snake curling beside the title
static void menu_snake(int x,int y,uint16_t col,uint16_t dark,int dir) {
    for(int i=0;i<6;i++) d.fillRect(x+i*5*dir,y+(i>2?0:4),5,5,(i>>1)&1?dark:col);
    int hx=dir>0?x+30:x-32;
    d.fillRoundRect(hx,y-1,7,7,2,col); d.fillRect(hx+(dir>0?4:1),y+1,2,2,COL_TXT);
}
static void draw_menu(void) {
    const char *items[N_MENU]; for(int i=0;i<N_MENU;i++) items[i]=menu_label(i);
    int y=gui::title("Snake Duel",nullptr,COL_P1);
    menu_snake(6,10,COL_P1,COL_P1D,1);
    menu_snake(W-12,10,COL_P2,COL_P2D,-1);
    gui::menu(s_menu,items,N_MENU,y,nucleo_app_content_height(),COL_P1);
}

static void draw_host(void) {
    char sub[32]; snprintf(sub,sizeof sub,GT("%.12s - canale %d","%.12s - channel %d"),pnet_name(),pnet_channel());
    int y=gui::title(GT("Crea partita","Host a match"),sub,COL_P1);
    gui::text(GT("Attendo sfidante","Waiting for rival"),W/2,y+4,1,gui::F_BODY,COL_TXT,COL_BGK);
    wait_dots(y+34,COL_P1);
    gui::text(GT("Sull'altro: Snake > Entra","Other device: Snake > Join"),W/2,y+44,1,gui::F_SMALL,COL_DIMC,COL_BGK);
}

static void draw_browse(void) {
    int y=gui::title(GT("Entra in partita","Join a match"),nullptr,COL_P2);
    if(s_join_pending){
        char nm[16]; snprintf(nm,sizeof nm,"%.12s",s_browse_sel<s_n_hosts?s_hosts[s_browse_sel].name:"?");
        gui::text(GT("Mi collego a","Connecting to"),W/2,y+2,1,gui::F_SMALL,COL_DIMC,COL_BGK);
        gui::text(nm,W/2,y+20,1,gui::F_BODY,COL_GOLD,COL_BGK);
        wait_dots(y+50,COL_P2);
        return;
    }
    if(s_n_hosts==0) {
        gui::text(GT("Cerco partite...","Looking for matches..."),W/2,y+4,1,gui::F_SMALL,COL_TXT,COL_BGK);
        wait_dots(y+30,COL_P2);
        gui::text(GT("Sull'altro: Snake > Crea","Other device: Snake > Host"),W/2,y+44,1,gui::F_SMALL,COL_DIMC,COL_BGK);
        return;
    }
    const char *items[MAX_HOSTS]; for(int i=0;i<s_n_hosts;i++) items[i]=s_hosts[i].name;
    s_bmenu.sel=(int8_t)s_browse_sel;
    gui::menu(s_bmenu,items,s_n_hosts,y,nucleo_app_content_height(),COL_P2);
}

static void draw_over(void) {
    d.setClipRect(0,0,W,H-HINT);                          // the field behind the card stays above the footer
    draw_play();
    for(int y=PLAY_Y;y<H-HINT;y+=2) d.drawFastHLine(0,y,W,COL_BGK);
    d.clearClipRect();
    bool won=i_won();
    const char* t=s_peerleft?GT("Fine partita","Match over"):won?GT("Hai vinto!","You win!"):
                  s_mode==MODE_AI?GT("Vince la CPU","CPU wins"):GT("Hai perso","You lose");
    char sc[40]; snprintf(sc,sizeof sc,"%.8s  %d - %d  %.8s",s_s1.name,s_s1.score,s_s2.score,s_s2.name);
    char r[40]; if(s_peerleft) snprintf(r,sizeof r,"%s",GT("Avversario disconnesso","Opponent disconnected"));
                else snprintf(r,sizeof r,GT("Record cibo: %d","Food record: %d"),s_hisc);
    gui::dialog(t,sc,r,nullptr,s_peerleft?COL_MUTE:won?COL_GOLD:0xF8A2);
}

static void help_row(int y,char sym,uint16_t col,const char *t) {
    d.fillRoundRect(8,y-1,10,10,3,col); char s[2]={sym,0}; txt(10,y,1,COL_BGK,s);
    txt(24,y,1,COL_TXT,t);
}
static void draw_help(void) {
    int y=gui::title(s_help_pg==0?GT("Come si gioca","How to play"):GT("POWER-UP","POWER-UPS"),nullptr,0xFFE0);
    if(s_help_pg==0) {
        txt(6,y,1,COL_TXT,GT("Frecce o W/A/S/D: gira","Arrows or W/A/S/D: turn"));
        txt(6,y+11,1,COL_TXT,GT("Mangia per crescere e fare punti","Eat to grow and score"));
        txt(6,y+22,1,COL_TXT,GT("Muri, rocce e corpi uccidono","Walls, rocks and bodies kill"));
        txt(6,y+33,1,COL_TXT,GT("Vince chi resta vivo","Last snake alive wins"));
        txt(6,y+50,1,COL_GOLD,GT("TAB: power-up","TAB: power-ups"));
    } else {
        help_row(y+1,'F',PU_COL[PU_SPEED], GT("veloce per un po'","faster for a while"));
        help_row(y+14,'L',PU_COL[PU_SLOW],  GT("rallenta l'avversario","slows your rival"));
        help_row(y+27,'X',PU_COL[PU_SHORT], GT("perdi 6 segmenti","drop 6 segments"));
        help_row(y+40,'G',PU_COL[PU_GHOST], GT("attraversi i muri","pass through walls"));
        help_row(y+53,'S',PU_COL[PU_SHIELD],GT("salva da 1 schianto","survive one crash"));
        txt(6,y+70,1,COL_GOLD,GT("TAB: regole","TAB: rules"));
    }
}

static void draw_scores(void) {
    int y=gui::title(GT("Record","Scores"),nullptr,COL_GOLD);
    char buf[16];
    gui::text(GT("Vinte da te","Your wins"),W/4+4,y,1,gui::F_SMALL,COL_P1,COL_BGK);
    gui::text(GT("Vinte dai rivali","Rival wins"),W*3/4-4,y,1,gui::F_SMALL,COL_P2,COL_BGK);
    snprintf(buf,sizeof buf,"%d",s_wins1); gui::text(buf,W/4+4,y+16,1,gui::F_BIG,COL_TXT,COL_BGK);
    snprintf(buf,sizeof buf,"%d",s_wins2); gui::text(buf,W*3/4-4,y+16,1,gui::F_BIG,COL_TXT,COL_BGK);
    snprintf(buf,sizeof buf,"%d",s_hisc);
    char r[48]; snprintf(r,sizeof r,"%s: %s",GT("Record cibo","Food record"),buf);
    gui::text(r,W/2,y+48,1,gui::F_SMALL,COL_GOLD,COL_BGK);
}

static void on_draw(void) {
    switch(s_st) {
        case ST_MENU:   draw_menu();   break;
        case ST_HOST:   draw_host();   break;
        case ST_BROWSE: draw_browse(); break;
        case ST_PLAY:   draw_play();   break;
        case ST_OVER:   draw_over();   break;
        case ST_HELP:   draw_help();   break;
        case ST_SCORES: draw_scores(); break;
    }
}

// ─── state / hint ────────────────────────────────────────────────────────────
static void set_hint(void) {
    switch(s_st){
        case ST_MENU:   nucleo_app_set_hint(GT("SU/GIU  INVIO scegli  Esc esci","UP/DN  ENTER pick  Esc quit")); break;
        case ST_HOST:   nucleo_app_set_hint(GT("In attesa...  Esc annulla","Waiting...  Esc cancel")); break;
        case ST_BROWSE: nucleo_app_set_hint(GT("SU/GIU  INVIO entra  Esc indietro","UP/DN  ENTER join  Esc back")); break;
        case ST_OVER:   if(s_mode==MODE_AI) nucleo_app_set_hint(GT("INVIO rivincita  Esc menu","ENTER rematch  Esc menu"));
                        else nucleo_app_set_hint(GT("INVIO o Esc: menu","ENTER or Esc: menu"));
                        break;
        case ST_HELP:   nucleo_app_set_hint(GT("TAB pagina  Esc indietro","TAB page  Esc back")); break;
        default:        nucleo_app_set_hint(GT("Esc indietro","Esc back")); break;
    }
}
static void go(int st) {
    s_st=st; s_paused=false;
    nucleo_app_set_fullscreen(st==ST_PLAY);
    set_hint();
    nucleo_app_request_draw();
}
static void leave_to_menu(void) {
    if(s_st==ST_PLAY) send_bye();
    s_join_pending=false; s_mode=MODE_AI;
    go(ST_MENU);
}

// ─── input ────────────────────────────────────────────────────────────────────
static void set_dir(int8_t dir) {
    if(s_st!=ST_PLAY||s_paused) return;
    if(s_mode==MODE_GUEST){ if(dir!=OPP(s_s2.dir)) s_s2.next_dir=dir; return; }
    // Local snake: queue up to 2 turns vs the LAST intended heading (so rapid up-then-left both land),
    // dropping reversals and repeats. snake_step pops one per move.
    Snake& s=s_s1;
    int8_t ref = s.inq_n>0 ? s.inq[s.inq_n-1] : s.next_dir;
    if(dir==ref || dir==OPP(ref)) return;
    if(s.inq_n<2) s.inq[s.inq_n++]=dir;
}

static void on_key(int key, char ch) {
    switch(s_st) {
        case ST_MENU:
            if(gui::menu_key(s_menu,key,N_MENU)){ SFX(SX_NAV); nucleo_app_request_draw(); }
            if(key==NK_ENTER) {
                switch(s_menu.sel) {
                    case 0: s_mode=MODE_AI; start_game((uint32_t)esp_timer_get_time()); break;
                    case 1: s_rng=(uint32_t)esp_timer_get_time(); s_n_hosts=0; s_hello_us=0; go(ST_HOST); break;
                    case 2: s_n_hosts=0; s_browse_sel=0; s_bmenu={0,0}; s_join_pending=false; go(ST_BROWSE); break;
                    case 3: go(ST_SCORES); break;
                    case 4: s_help_pg=0; go(ST_HELP); break;
                }
            }
            break;
        case ST_BROWSE:
            if(s_n_hosts>0&&!s_join_pending&&gui::menu_key(s_bmenu,key,s_n_hosts)){ s_browse_sel=s_bmenu.sel; SFX(SX_NAV); nucleo_app_request_draw(); }
            if(key==NK_ENTER&&s_n_hosts>0&&!s_join_pending) {
                s_join_pending=true;
                s_join_first_us=esp_timer_get_time(); s_join_retry_us=s_join_first_us;
                sn_join_t jn; fill_hdr(&jn,SN_JOIN);
                strncpy(jn.name,pnet_name(),11); jn.name[11]=0;
                pnet_send(s_hosts[s_browse_sel].mac,&jn,sizeof(jn));
                nucleo_app_request_draw();
            }
            break;
        case ST_PLAY:
            if(s_paused){
                if(key==NK_ENTER){ s_paused=false; int64_t gap=esp_timer_get_time()-s_pause_us;   // the clock stood still
                    s_go_us+=gap; s_s1.move_next_us+=gap; s_s2.move_next_us+=gap; nucleo_app_request_draw(); }
                break;
            }
            // NK_LEFT → on_back intercepts it and calls set_dir(DLT)
            if(key==NK_UP  ||ch=='w'||ch=='W') set_dir(DUP);
            if(key==NK_DOWN||ch=='s'||ch=='S') set_dir(DDN);
            if(key==NK_RIGHT||ch=='d'||ch=='D') set_dir(DRT);
            if(ch=='a'||ch=='A')               set_dir(DLT);
            break;
        case ST_OVER:
            if(key==NK_ENTER) {
                if(s_mode==MODE_AI) start_game((uint32_t)esp_timer_get_time());
                else leave_to_menu();
            }
            break;
        case ST_HELP:
            if(key==NK_ENTER||key==NK_RIGHT||key==NK_UP||key==NK_DOWN){ s_help_pg^=1; nucleo_app_request_draw(); }
            break;
        default: break;
    }
}

static bool on_back(int key) {
    if(s_st==ST_PLAY&&key==NK_LEFT){ set_dir(DLT); return true; }
    if(key==NK_LEFT){ if(s_st==ST_HELP){ s_help_pg^=1; nucleo_app_request_draw(); } return true; }   // never closes the app
    if(s_st==ST_MENU) return false;
    SFX(SX_NAV);
    if(s_st==ST_PLAY&&!s_paused){ s_paused=true; s_pause_us=esp_timer_get_time(); nucleo_app_request_draw(); return true; }
    leave_to_menu();
    return true;
}

static void on_tab(void) {
    if(s_st==ST_HELP){ s_help_pg^=1; nucleo_app_request_draw(); }
}

// ─── app lifecycle ───────────────────────────────────────────────────────────
static void on_enter(void) {
    game_text_open("snake");
    if(!s_obstacles) s_obstacles=(uint8_t(*)[WORLD_W])calloc(WORLD_H,WORLD_W);  // ~3.1 KB only while playing
    stats_load();
    s_st=ST_MENU; s_menu={0,0}; s_bmenu={0,0}; s_mode=MODE_AI; s_paused=false; s_anim=0;
    memset(s_parts,0,sizeof(Part)*N_PARTS);
    s_cam_x=0; s_cam_y=0;
    s_last_us=esp_timer_get_time();
    nucleo_app_set_poll_handler(poll_fn);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_tab_handler(on_tab);
    go(ST_MENU);
    if (!pnet_start()) nucleo_app_set_hint(GT("ESP-NOW non avviato  Esc","ESP-NOW not started  Esc"));
}
static void on_exit(void) {
    if(s_st==ST_PLAY) send_bye();
    pnet_stop();
    nucleo_app_set_fullscreen(false);
    free(s_obstacles); s_obstacles=nullptr;   // back to zero .bss until relaunched
    game_text_close();
}

// ─── registration ────────────────────────────────────────────────────────────
static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_snk, sizeof(Snake)*2 }, { (void **)&s_hosts, sizeof(HostEntry)*MAX_HOSTS },
    { (void **)&s_parts, sizeof(Part)*N_PARTS }, { (void **)&s_stpk, sizeof(sn_state_t) }, { nullptr, 0 } };
extern "C" void nucleo_register_snake(void) {
    static const nucleo_app_def_t app = {
        "snake", "Snake", "Games", "Serpente 1v1 in rete (ESP-NOW) o vs AI",
        'S', C_GREEN, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_SOLO, APP_RAM
    };
    nucleo_app_register(&app);
}
