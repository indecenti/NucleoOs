// app_snake.cpp — Snake Duel: 1v1 in rete (ESP-NOW) o vs AI
// Mondo 80×40 celle, CELL=8px, camera segue il proprio serpente (come Tank Duel).
// Host-autoritativa. HUD con minimappa. NX_SOLO only: httpd non tocco in solo boot.
// Power-up: SPEED, SLOW, SHORT, GHOST.
#include "app_gfx.h"
#include "game_sfx.h"
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

// ─── mondo / camera ──────────────────────────────────────────────────────────
#define WORLD_W   80
#define WORLD_H   40
#define CELL       8      // px per cella
#define HUD_H     25
#define PLAY_Y    HUD_H
#define VIEW_W    30      // celle visibili X: 240/8=30
#define VIEW_H    13      // celle visibili Y: floor(110/8)=13

// Minimappa nella HUD
#define MM_X      100
#define MM_Y        2
#define MM_W       40     // 1px = 2 celle X (WORLD_W/MM_W = 2)
#define MM_H       20     // 1px = 2 celle Y (WORLD_H/MM_H = 2)

// ─── timing ───────────────────────────────────────────────────────────────────
#define MOVE_MS      200LL
#define FAST_MS      110LL
#define SLOW_MS      340LL
#define PU_TICKS       8
#define HELLO_US  400000LL
#define TX_US      40000LL
#define RX_TIMEOUT  3500000LL
#define JOIN_RETRY   350000LL
#define JOIN_TIMEOUT 4000000LL
#define FRAME_US   33333LL

// ─── limiti ───────────────────────────────────────────────────────────────────
#define MAX_SEG   60
#define NET_SEG   38
#define MAX_HOSTS  6
#define N_PARTS   24

// ─── direzioni ────────────────────────────────────────────────────────────────
enum { DUP=0, DRT=1, DDN=2, DLT=3 };
static const int8_t DX[4]={0,1,0,-1}, DY[4]={-1,0,1,0};
#define OPP(d_) ((d_)^2)

// ─── power-up ─────────────────────────────────────────────────────────────────
enum { PU_NONE=0, PU_SPEED, PU_SLOW, PU_SHORT, PU_GHOST, PU_SHIELD, PU_COUNT };
static const uint16_t PU_COL[PU_COUNT]={0, C_YELLOW, C_PURPLE, C_PINK, C_GREY, 0x07FF};
static const char     PU_SYM[PU_COUNT]={' ','F','L','X','G','S'};

// ─── stati ────────────────────────────────────────────────────────────────────
enum { ST_MENU=0, ST_HOST, ST_BROWSE, ST_PLAY, ST_OVER, ST_HELP, ST_SCORES };
enum { MODE_AI=0, MODE_HOST=1, MODE_GUEST=2 };
#define N_MENU 5
static const char* MENU_ITEMS[N_MENU]={
    "1P vs CPU",
    "Crea Partita",
    "Entra Partita",
    "Classifica",
    "Come si gioca"
};

// ─── protocollo ───────────────────────────────────────────────────────────────
#define SN_M0 'S'
#define SN_M1 'N'
#define SN_VER 1
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
    int8_t   fx, fy, fx2, fy2;   // 2 cibi sempre presenti
    uint8_t  pu_type; int8_t pu_x, pu_y;
    uint8_t  phase;
    int8_t   segs[NET_SEG*4]; // s1 × NET_SEG × (x,y) then s2
};
#pragma pack(pop)
static_assert(sizeof(sn_state_t) <= PNET_MAXMSG, "State packet too large");

// ─── snake ────────────────────────────────────────────────────────────────────
struct Snake {
    int8_t  bx[MAX_SEG], by[MAX_SEG]; // [0]=testa
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

// ─── ostacoli ─────────────────────────────────────────────────────────────────
// Heap-on-enter (was .bss ~3.1 KB): a Solo-boot game is closed during normal OS boot, so this map
// held boot RAM for nothing. calloc in on_enter(), freed on_exit; readers skip cleanly if null.
static uint8_t (*s_obstacles)[WORLD_W] = nullptr;  // 0=free, 1=wall

// ─── trail ────────────────────────────────────────────────────────────────────
struct Trail { int8_t x,y; int life; };
struct SnakeTrail { Trail pts[8]; int cnt; };
static SnakeTrail s_trail1, s_trail2;

// ─── particelle ───────────────────────────────────────────────────────────────
struct Part { float x,y,vx,vy; int life; uint16_t col; };

// ─── SFX ──────────────────────────────────────────────────────────────────────
static const int SFX_ENABLED = 1;
enum { SX_EAT=1, SX_PU, SX_DIE, SX_WIN, SX_NAV, SX_START, SX_COUNT=SX_START };

static const char* sn_sfx_name(int id) {
    switch(id) {
        case SX_EAT:   return "eat";
        case SX_PU:    return "pu";
        case SX_DIE:   return "die";
        case SX_WIN:   return "win";
        case SX_NAV:   return "nav";
        case SX_START: return "start";
        default: return "?";
    }
}
static int sn_sfx_recipe(int id, notify_voice_t* v) {
    switch(id) {
        case SX_EAT:
            notify__voice(&v[0], 880.f,  0.00f, 0.08f); v[0].amp=0.7f;
            notify__voice(&v[1], 1320.f, 0.05f, 0.08f); v[1].amp=0.6f;
            return 2;
        case SX_PU:
            notify__voice(&v[0], 660.f,  0.00f, 0.10f); v[0].amp=0.9f;
            notify__voice(&v[1], 880.f,  0.06f, 0.10f); v[1].amp=0.9f;
            notify__voice(&v[2], 1100.f, 0.12f, 0.12f); v[2].amp=0.9f;
            return 3;
        case SX_DIE:
            notify__voice(&v[0], 330.f,  0.00f, 0.12f); v[0].amp=0.9f;
            notify__voice(&v[1], 220.f,  0.10f, 0.14f); v[1].amp=0.9f;
            notify__voice(&v[2], 140.f,  0.22f, 0.20f); v[2].amp=0.9f;
            return 3;
        case SX_WIN:
            notify__voice(&v[0], 523.f,  0.00f, 0.12f); v[0].amp=0.8f;
            notify__voice(&v[1], 659.f,  0.10f, 0.12f); v[1].amp=0.8f;
            notify__voice(&v[2], 784.f,  0.20f, 0.18f); v[2].amp=0.9f;
            notify__voice(&v[3], 1047.f, 0.32f, 0.25f); v[3].amp=1.0f;
            return 4;
        case SX_NAV:
            notify__voice(&v[0], 440.f,  0.00f, 0.04f); v[0].amp=0.4f;
            return 1;
        case SX_START:
            notify__voice(&v[0], 440.f,  0.00f, 0.08f); v[0].amp=0.7f;
            notify__voice(&v[1], 880.f,  0.10f, 0.12f); v[1].amp=0.9f;
            return 2;
        default: return 0;
    }
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

// ─── stato globale ────────────────────────────────────────────────────────────
static int      s_st;
static int      s_mode;
static Snake    s_s1, s_s2;
static int8_t   s_fx, s_fy, s_fx2, s_fy2;  // 2 cibi simultanei
static uint8_t  s_pu_type;
static int8_t   s_pu_x, s_pu_y;
static int      s_pu_life;
static int      s_pu_next;
static uint32_t s_rng;
static uint8_t  s_tick;
static int64_t  s_last_us;
static int      s_winner;
static int      s_wins1, s_wins2;
static int      s_hisc;
static int      s_flash;
static int      s_menu_sel;
static int      s_menu_top;     // windowed-list scroll top (keeps the footer clear)
static int      s_browse_sel;
static int      s_help_pg;
static int      s_cam_x, s_cam_y;   // top-left del viewport in celle mondo

static uint8_t  s_peer[6];
static int64_t  s_last_tx_us;
static int64_t  s_last_rx_us;
static int64_t  s_hello_us;
static int64_t  s_join_first_us;
static int64_t  s_join_retry_us;
static bool     s_join_pending;
static uint8_t  s_seq;

struct HostEntry { uint8_t mac[6]; char name[13]; int64_t ts; bool valid; };
static HostEntry s_hosts[MAX_HOSTS];
static int       s_n_hosts;

static Part s_parts[N_PARTS];
static int  s_logo_off;

// ─── RNG ──────────────────────────────────────────────────────────────────────
static uint32_t rng_next(void) {
    s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng;
}
static int rng_range(int lo, int hi) {
    return lo+(int)(rng_next()%(uint32_t)(hi-lo+1));
}

// ─── colori ───────────────────────────────────────────────────────────────────
static uint16_t dim565(uint16_t c, int n, int d_) {
    int r=((c>>11)&0x1F)*n/d_;
    int g=((c>>5)&0x3F)*n/d_;
    int b=(c&0x1F)*n/d_;
    return (uint16_t)((r<<11)|(g<<5)|b);
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
// Coordinate schermo da cella mondo
static inline int sx_(int8_t c) { return (c-s_cam_x)*CELL; }
static inline int sy_(int8_t r) { return HUD_H+(r-s_cam_y)*CELL; }
static inline bool inv_(int8_t c, int8_t r) {
    return c>=s_cam_x&&c<s_cam_x+VIEW_W&&r>=s_cam_y&&r<s_cam_y+VIEW_H;
}

// ─── snake helpers ────────────────────────────────────────────────────────────
static void snake_init(Snake& s, int8_t hx, int8_t hy, int8_t dir, const char* nm) {
    memset(&s,0,sizeof(s));
    s.dir=s.next_dir=dir; s.alive=true; s.len=4;
    strncpy(s.name,nm,12);
    for(int i=0;i<4;i++){s.bx[i]=hx-DX[dir]*i; s.by[i]=hy-DY[dir]*i;}
}
static bool snake_has(const Snake& s, int8_t x, int8_t y, int skip=0) {
    for(int i=skip;i<s.len;i++) if(s.bx[i]==x&&s.by[i]==y) return true;
    return false;
}

// ─── mappa ostacoli ───────────────────────────────────────────────────────────
static void gen_obstacles(void) {
    if(!s_obstacles) return;
    memset(s_obstacles,0,(size_t)WORLD_H*WORLD_W);
    // Bordi
    for(int c=0;c<WORLD_W;c++) s_obstacles[0][c]=s_obstacles[WORLD_H-1][c]=1;
    for(int r=0;r<WORLD_H;r++) s_obstacles[r][0]=s_obstacles[r][WORLD_W-1]=1;
    // Rocce sparse: ~15 blocchi 2×2 random
    for(int i=0;i<15;i++) {
        int x=rng_range(4,WORLD_W-5), y=rng_range(4,WORLD_H-5);
        for(int dy=0;dy<2&&y+dy<WORLD_H;dy++)
            for(int dx=0;dx<2&&x+dx<WORLD_W;dx++)
                s_obstacles[y+dy][x+dx]=1;
    }
}
static inline bool is_obstacle(int8_t x, int8_t y) {
    if(!s_obstacles) return true;
    return (x<0||x>=WORLD_W||y<0||y>=WORLD_H) ? true : s_obstacles[y][x];
}

// ─── trail ────────────────────────────────────────────────────────────────────
static void trail_add(SnakeTrail& tr, int8_t x, int8_t y) {
    if(tr.cnt<8) { tr.pts[tr.cnt].x=x; tr.pts[tr.cnt].y=y; tr.pts[tr.cnt].life=24; tr.cnt++; }
    else { for(int i=0;i<7;i++) tr.pts[i]=tr.pts[i+1]; tr.pts[7].x=x; tr.pts[7].y=y; tr.pts[7].life=24; }
}
static void trail_step(SnakeTrail& tr) {
    for(int i=0;i<tr.cnt;i++) if(tr.pts[i].life>0) tr.pts[i].life--;
}

// ─── particelle ───────────────────────────────────────────────────────────────
static void parts_spawn(float px, float py, uint16_t col, int n) {
    int k=0;
    for(auto& p:s_parts) {
        if(p.life>0||k>=n) continue;
        float ang=k*0.5236f; // π/6 per particle
        float sp=2.5f+(k&3)*0.7f;
        p={px,py,cosf(ang)*sp,sinf(ang)*sp-0.3f,35,col}; k++;
    }
}
static void parts_step(void) {
    for(auto& p:s_parts){if(!p.life)continue;p.x+=p.vx;p.y+=p.vy;p.vy+=0.12f;p.life--;}
}

// ─── campo: cibo / power-up ───────────────────────────────────────────────────
static void spawn_free(int8_t& ox, int8_t& oy) {
    for(int t=0;t<600;t++) {
        int8_t cx=rng_range(1,WORLD_W-2), cy=rng_range(1,WORLD_H-2);
        if(!snake_has(s_s1,cx,cy)&&!snake_has(s_s2,cx,cy)
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

// ─── applica power-up ────────────────────────────────────────────────────────
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
    // Velocità progressiva: -4ms ogni cibo mangiato combinato, minimo 100ms
    int total = s_s1.score + s_s2.score;
    int64_t base = (MOVE_MS - total*4)*1000LL;
    if(base < 100000LL) base = 100000LL;
    return base;
}

// ─── step un serpente (HOST only) ────────────────────────────────────────────
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
    int cp=(s.len<MAX_SEG)?s.len:MAX_SEG-1;
    memmove(&s.bx[1],&s.bx[0],cp);
    memmove(&s.by[1],&s.by[0],cp);
    // Trail: aggiungi la vecchia posizione della testa
    SnakeTrail& tr=(&s==&s_s1)?s_trail1:s_trail2;
    trail_add(tr, s.bx[0], s.by[0]);
    s.bx[0]=nx; s.by[0]=ny;
    // Cibo 1 (cresce di 2)
    if(nx==s_fx&&ny==s_fy) {
        if(s.len<MAX_SEG-1) s.len+=2; else if(s.len<MAX_SEG) s.len++;
        s.score++; if(s.score>s_hisc)s_hisc=s.score;
        parts_spawn((float)(sx_(nx)+CELL/2), (float)(sy_(ny)+CELL/2), C_RED, 12);
        spawn_food(); SFX(SX_EAT);
    }
    // Cibo 2 (cresce di 2)
    if(nx==s_fx2&&ny==s_fy2) {
        if(s.len<MAX_SEG-1) s.len+=2; else if(s.len<MAX_SEG) s.len++;
        s.score++; if(s.score>s_hisc)s_hisc=s.score;
        parts_spawn((float)(sx_(nx)+CELL/2), (float)(sy_(ny)+CELL/2), 0xFD00, 12);
        spawn_food2(); SFX(SX_EAT);
    }
    if(s_pu_type&&nx==s_pu_x&&ny==s_pu_y){
        parts_spawn((float)(sx_(nx)+CELL/2), (float)(sy_(ny)+CELL/2), PU_COL[s_pu_type], 16);
        apply_pu(s,opp,s_pu_type);s_pu_type=PU_NONE;
    }
    return true;
}

// ─── AI ───────────────────────────────────────────────────────────────────────
static int8_t ai_choose(void) {
    const Snake& me=s_s2; const Snake& op=s_s1;
    int8_t hx=me.bx[0], hy=me.by[0];
    int best=me.dir, bsc=-99999;
    bool ghost=(me.pu==PU_GHOST);
    for(int dd=0;dd<4;dd++) {
        if(dd==OPP(me.dir)) continue;
        int8_t nx=hx+DX[dd], ny=hy+DY[dd];
        if(ghost){nx=(int8_t)((nx+WORLD_W)%WORLD_W);ny=(int8_t)((ny+WORLD_H)%WORLD_H);}
        else if(is_obstacle(nx,ny)) continue;
        bool hit=false;
        for(int i=1;i<me.len-1;i++) if(me.bx[i]==nx&&me.by[i]==ny){hit=true;break;}
        if(hit) continue;
        bool opp_body=snake_has(op,nx,ny,1);
        bool head_clash=(nx==op.bx[0]&&ny==op.by[0]);
        // Lookahead: conta vicini liberi
        int free_nb=0;
        for(int d2=0;d2<4;d2++) {
            int8_t nnx=nx+DX[d2], nny=ny+DY[d2];
            if(ghost){nnx=(int8_t)((nnx+WORLD_W)%WORLD_W);nny=(int8_t)((nny+WORLD_H)%WORLD_H);}
            else if(nnx<0||nnx>=WORLD_W||nny<0||nny>=WORLD_H) continue;
            bool blk=false;
            for(int i=0;i<me.len-2;i++) if(me.bx[i]==nnx&&me.by[i]==nny){blk=true;break;}
            if(!blk&&!snake_has(op,nnx,nny)) free_nb++;
        }
        // Punta al cibo più vicino tra i due
        int d1=abs(nx-s_fx)+abs(ny-s_fy);
        int d2c=abs(nx-s_fx2)+abs(ny-s_fy2);
        int dist=d1<d2c?d1:d2c;
        int sc = -dist*3 - (opp_body?50:0) - (head_clash?80:0) + free_nb*8;
        if(sc>bsc){bsc=sc;best=dd;}
    }
    return (int8_t)best;
}

// ─── game over ────────────────────────────────────────────────────────────────
static void on_death(void) {
    bool d1=!s_s1.alive, d2=!s_s2.alive;
    if(d1&&!d2)     { s_winner=2; s_wins2++; }
    else if(d2&&!d1){ s_winner=1; s_wins1++; }
    else            { s_winner=(s_s1.score>=s_s2.score)?1:2;
                      if(s_winner==1)s_wins1++;else s_wins2++; }
    // Esplosione in coordinate schermo (cam già aggiornata prima di game_step)
    if(d1) parts_spawn(sx_(s_s1.bx[0])+(float)CELL/2, sy_(s_s1.by[0])+(float)CELL/2, C_GREEN, 20);
    if(d2) parts_spawn(sx_(s_s2.bx[0])+(float)CELL/2, sy_(s_s2.by[0])+(float)CELL/2, 0xF81F, 20);
    SFX(s_winner==1?SX_WIN:SX_DIE);
    s_flash=10; s_st=ST_OVER;
}

// ─── game logic (HOST/AI only) ────────────────────────────────────────────────
static void game_step(int64_t now) {
    if(s_mode==MODE_AI&&s_s2.alive) s_s2.next_dir=ai_choose();
    if(s_s1.pu&&--s_s1.pu_t<=0) s_s1.pu=PU_NONE;
    if(s_s2.pu&&--s_s2.pu_t<=0) s_s2.pu=PU_NONE;
    if(s_pu_type&&--s_pu_life<=0) s_pu_type=PU_NONE;
    if(!s_pu_type&&--s_pu_next<=0){ spawn_pu(); s_pu_next=rng_range(8,16); }
    // Salvo teste prima dello step per rilevare lo swap (pass-through)
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
    // Cross-collision: testa su corpo avversario
    if(s_s1.alive&&snake_has(s_s2,s_s1.bx[0],s_s1.by[0],1)) s_s1.alive=false;
    if(s_s2.alive&&snake_has(s_s1,s_s2.bx[0],s_s2.by[0],1)) s_s2.alive=false;
    // Testa-a-testa: stesso nodo OPPURE swap di posizione
    if(s_s1.alive&&s_s2.alive) {
        bool same_cell = (s_s1.bx[0]==s_s2.bx[0]&&s_s1.by[0]==s_s2.by[0]);
        bool swap = (s_s1.bx[0]==old_h2x&&s_s1.by[0]==old_h2y
                     &&s_s2.bx[0]==old_h1x&&s_s2.by[0]==old_h1y);
        if(same_cell||swap){ s_s1.alive=false; s_s2.alive=false; }
    }
    // Update particelle e trail
    parts_step();
    trail_step(s_trail1);
    trail_step(s_trail2);
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
static void send_state(void) {
    static sn_state_t pk;
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

// ─── avvia partita ────────────────────────────────────────────────────────────
static void start_game(uint32_t seed) {
    s_rng=seed?seed:0xDEADBEEFu;
    gen_obstacles();
    const char* nm1=(s_mode==MODE_GUEST)?s_s1.name:pnet_name();
    const char* nm2=(s_mode==MODE_GUEST)?pnet_name():(s_mode==MODE_AI?"CPU":s_s2.name);
    snake_init(s_s1, 12, WORLD_H/2, DRT, nm1);   s_s1.len=5;
    snake_init(s_s2, WORLD_W-13, WORLD_H/2, DLT, nm2); s_s2.len=5;
    memset(s_parts,0,sizeof(s_parts));
    memset(&s_trail1,0,sizeof(s_trail1)); memset(&s_trail2,0,sizeof(s_trail2));
    s_pu_type=PU_NONE; s_pu_next=rng_range(6,12);
    s_winner=0; s_flash=0; s_tick=0;
    s_cam_x=0; s_cam_y=WORLD_H/2-VIEW_H/2;
    spawn_food(); spawn_food2();
    int64_t now=esp_timer_get_time();
    s_s1.move_next_us=now+MOVE_MS*1000LL;
    s_s2.move_next_us=now+MOVE_MS*1000LL;
    s_last_rx_us=now; s_last_tx_us=now;
    s_st=ST_PLAY;
    cam_update();
    nucleo_app_request_draw();
}

// ─── applica stato (guest) ───────────────────────────────────────────────────
static void apply_state(const sn_state_t* st, int plen) {
    int n1=st->s1_len, n2=st->s2_len;
    int need=(int)(offsetof(sn_state_t,segs)+(n1+n2)*2);
    if(plen<need) return;
    s_s1.len=n1; s_s1.dir=st->s1_dir; s_s1.alive=st->s1_alive;
    s_s1.pu=st->s1_pu; s_s1.pu_t=st->s1_put; s_s1.score=st->s1_score;
    for(int i=0;i<n1;i++){s_s1.bx[i]=st->segs[i*2];s_s1.by[i]=st->segs[i*2+1];}
    int off=n1*2;
    s_s2.len=n2; s_s2.dir=st->s2_dir; s_s2.alive=st->s2_alive;
    s_s2.pu=st->s2_pu; s_s2.pu_t=st->s2_put; s_s2.score=st->s2_score;
    for(int i=0;i<n2;i++){s_s2.bx[i]=st->segs[off+i*2];s_s2.by[i]=st->segs[off+i*2+1];}
    s_fx=st->fx; s_fy=st->fy; s_fx2=st->fx2; s_fy2=st->fy2;
    s_pu_type=st->pu_type; s_pu_x=st->pu_x; s_pu_y=st->pu_y;
    if(st->phase==ST_OVER&&s_st==ST_PLAY) {
        s_winner=(st->s1_alive)?1:2;
        if(s_winner==2)s_wins2++;else s_wins1++;
        SFX(s_winner==2?SX_WIN:SX_DIE);
        // Esplosione lato guest
        cam_update();
        Snake& dead=(s_winner==1)?s_s2:s_s1;
        uint16_t dc=(s_winner==1)?0xF81F:C_GREEN;
        parts_spawn(sx_(dead.bx[0])+(float)CELL/2, sy_(dead.bx[0])+(float)CELL/2, dc, 16);
        s_flash=10; s_st=ST_OVER;
    }
    nucleo_app_request_draw();
}

// ─── gestore pacchetti ────────────────────────────────────────────────────────
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
                strncpy(s_s2.name,pnet_name(),12); s_s2.name[12]=0;
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
                s_winner=(s_mode==MODE_HOST)?1:2;
                if(s_winner==1)s_wins1++;else s_wins2++;
                s_st=ST_OVER; nucleo_app_request_draw();
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

    if(s_st==ST_HOST) {
        if(now-s_hello_us>HELLO_US){ send_hello(); s_hello_us=now; }
    } else if(s_st==ST_BROWSE) {
        for(int i=0;i<s_n_hosts;) {
            if(now-s_hosts[i].ts>4000000LL){
                s_hosts[i]=s_hosts[--s_n_hosts]; nucleo_app_request_draw();
            } else i++;
        }
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
    } else if(s_st==ST_PLAY) {
        // Camera aggiornata PRIMA di game_step → esplosioni in pos. corrette
        cam_update();
        if(s_mode==MODE_HOST||s_mode==MODE_AI) {
            game_step(now);
            if(s_mode==MODE_HOST&&now-s_last_tx_us>TX_US){
                s_last_tx_us=now; send_state();
            }
            if(s_mode==MODE_HOST&&now-s_last_rx_us>RX_TIMEOUT){
                s_winner=1; s_wins1++; s_st=ST_OVER; nucleo_app_request_draw();
            }
        } else {
            if(now-s_last_tx_us>TX_US){
                s_last_tx_us=now; send_input(s_s2.next_dir);
            }
            if(now-s_last_rx_us>RX_TIMEOUT){
                s_winner=2; s_wins2++; s_st=ST_OVER; nucleo_app_request_draw();
            }
        }
        parts_step();
        if(s_flash>0) s_flash--;
    } else if(s_st==ST_OVER&&s_mode==MODE_HOST) {
        if(now-s_last_tx_us>TX_US){ s_last_tx_us=now; send_state(); }
    }

    if(s_st==ST_MENU) s_logo_off=(s_logo_off+1)%40;
    return true;
}

// ─── HUD con minimappa ────────────────────────────────────────────────────────
static void draw_hud(void) {
    d.fillRect(0,0,W,HUD_H,0x1082);
    d.drawFastHLine(0,HUD_H-1,W,0x2945);

    // P1 (verde) — lato sinistro
    d.setTextColor(C_GREEN); d.setTextSize(1);
    char buf[14]; strncpy(buf,s_s1.name,8); buf[8]=0;
    d.setCursor(3,4); d.print(buf);
    d.setTextSize(1); snprintf(buf,sizeof buf,"%d",s_s1.score);
    d.setCursor(3,14); d.setTextColor(0xFFFF); d.print(buf);
    if(s_s1.pu) {
        int bw=(s_s1.pu_t*30)/PU_TICKS;
        d.fillRect(3,HUD_H-3,bw,2,PU_COL[s_s1.pu]);
    }

    // P2 (magenta) — lato destro
    d.setTextColor(0xF81F); d.setTextSize(1);
    strncpy(buf,s_s2.name,8); buf[8]=0;
    int nw=(int)strlen(buf)*6;
    d.setCursor(W-MM_W-6-nw,4); d.print(buf);
    snprintf(buf,sizeof buf,"%d",s_s2.score);
    int sw=(int)strlen(buf)*6;
    d.setCursor(W-MM_W-6-sw,14); d.setTextColor(0xFFFF); d.print(buf);
    if(s_s2.pu) {
        int bw=(s_s2.pu_t*30)/PU_TICKS;
        d.fillRect(W-MM_W-6-bw,HUD_H-3,bw,2,PU_COL[s_s2.pu]);
    }

    // Minimappa — centro destra
    d.fillRect(W-MM_W-2, MM_Y, MM_W, MM_H, 0x0821);
    d.drawRect(W-MM_W-2, MM_Y, MM_W, MM_H, 0x4228);
    int mx=W-MM_W-2;
    // Viewport rect sulla mappa
    int vrx=mx+s_cam_x/2, vry=MM_Y+s_cam_y/2;
    d.drawRect(vrx, vry, VIEW_W/2, VIEW_H/2, 0x52AA);
    // Cibi
    d.drawPixel(mx+s_fx/2,  MM_Y+s_fy/2,  C_RED);
    d.drawPixel(mx+s_fx2/2, MM_Y+s_fy2/2, 0xFD00);
    // PU
    if(s_pu_type) d.drawPixel(mx+s_pu_x/2, MM_Y+s_pu_y/2, PU_COL[s_pu_type]);
    // Corpi serpenti (ogni 3° segmento)
    for(int i=2;i<s_s1.len;i+=3) d.drawPixel(mx+s_s1.bx[i]/2, MM_Y+s_s1.by[i]/2, dim565(C_GREEN,5,8));
    for(int i=2;i<s_s2.len;i+=3) d.drawPixel(mx+s_s2.bx[i]/2, MM_Y+s_s2.by[i]/2, dim565(0xF81F,5,8));
    // Teste
    d.fillRect(mx+s_s1.bx[0]/2, MM_Y+s_s1.by[0]/2, 2, 2, C_GREEN);
    d.fillRect(mx+s_s2.bx[0]/2, MM_Y+s_s2.by[0]/2, 2, 2, 0xF81F);
}

// ─── draw serpente con camera ─────────────────────────────────────────────────
static void draw_snake(const Snake& s, uint16_t col) {
    for(int i=s.len-1;i>=0;i--) {
        int8_t cx=s.bx[i], cy=s.by[i];
        if(!inv_(cx,cy)) continue;
        int bright=8-(i*5/s.len); if(bright<2)bright=2;
        uint16_t sc=(i==0)?col:dim565(col,bright,8);
        int px=sx_(cx), py=sy_(cy);
        if(i==0) {
            // Active power-up aura around the head (visual feedback).
            int hcx=px+CELL/2, hcy=py+CELL/2;
            if(s.pu==PU_SHIELD){ d.drawCircle(hcx,hcy,CELL/2+2,0x07FF); d.drawCircle(hcx,hcy,CELL/2+1,dim565((uint16_t)0x07FF,1,2)); }
            else if(s.pu==PU_GHOST){ d.drawRoundRect(px-2,py-2,CELL+4,CELL+4,3,dim565((uint16_t)0xC618,1,2)); }
            else if(s.pu==PU_SPEED){ d.drawFastHLine(px-3,hcy,2,C_YELLOW); d.drawFastHLine(px+CELL+1,hcy,2,C_YELLOW); }
            // Testa con occhi
            d.fillRoundRect(px,py,CELL,CELL,2,sc);
            d.drawRoundRect(px,py,CELL,CELL,2,dim565(sc,14,8));
            // Occhi in base alla direzione
            if(s.dir==DRT){ d.fillRect(px+5,py+1,2,2,0); d.fillRect(px+5,py+5,2,2,0); }
            else if(s.dir==DLT){ d.fillRect(px+1,py+1,2,2,0); d.fillRect(px+1,py+5,2,2,0); }
            else if(s.dir==DUP){ d.fillRect(px+2,py+1,2,2,0); d.fillRect(px+4,py+1,2,2,0); }
            else              { d.fillRect(px+2,py+5,2,2,0); d.fillRect(px+4,py+5,2,2,0); }
        } else {
            int inset=(i==1)?1:2;
            d.fillRect(px+inset,py+inset,CELL-inset*2,CELL-inset*2,sc);
        }
    }
}

// ─── bordi mondo visibili ─────────────────────────────────────────────────────
static void draw_borders(void) {
    uint16_t wc=0x528A; // blu-grigio scuro per muro
    if(s_cam_x==0)            d.fillRect(0,PLAY_Y,2,H-PLAY_Y,wc);
    if(s_cam_x+VIEW_W>=WORLD_W) d.fillRect(W-2,PLAY_Y,2,H-PLAY_Y,wc);
    if(s_cam_y==0)            d.fillRect(0,PLAY_Y,W,2,wc);
    if(s_cam_y+VIEW_H>=WORLD_H) {
        int yw=PLAY_Y+(WORLD_H-s_cam_y)*CELL;
        if(yw<H) d.fillRect(0,yw,W,H-yw,wc);
    }
}

// ─── draw campo ───────────────────────────────────────────────────────────────
static void draw_play(void) {
    // Background semplice
    d.fillRect(0,PLAY_Y,W,H-PLAY_Y,0x0821);

    // Ostacoli (minimale)
    for(int gx_=s_cam_x;gx_<s_cam_x+VIEW_W&&gx_<WORLD_W;gx_++) {
        for(int gy_=s_cam_y;gy_<s_cam_y+VIEW_H&&gy_<WORLD_H;gy_++) {
            if(!s_obstacles||gx_<0||gx_>=WORLD_W||gy_<0||gy_>=WORLD_H||!s_obstacles[gy_][gx_]) continue;
            int sx=sx_((int8_t)gx_), sy=sy_((int8_t)gy_);
            d.fillRect(sx+1,sy+1,CELL-2,CELL-2,0x6A48);
        }
    }

    draw_borders();

    // Cibo 1 — rosso pulsante + glow
    if(inv_(s_fx,s_fy)) {
        int fpx=sx_(s_fx)+CELL/2, fpy=sy_(s_fy)+CELL/2;
        int frad=(s_tick&8)?3:4;
        int glow=((s_tick>>1)&3);
        d.drawCircle(fpx,fpy,frad+3+glow,dim565(C_RED,2,8));
        d.drawCircle(fpx,fpy,frad+2,dim565(C_RED,4,8));
        d.fillCircle(fpx,fpy,frad,C_RED);
    }
    // Cibo 2 — arancione + glow
    if(inv_(s_fx2,s_fy2)) {
        uint16_t oc=0xFD00;
        int fpx=sx_(s_fx2)+CELL/2, fpy=sy_(s_fy2)+CELL/2;
        int frad=(s_tick&8)?4:3;
        int glow=((s_tick>>1)&3);
        d.drawCircle(fpx,fpy,frad+3+glow,dim565(oc,2,8));
        d.drawCircle(fpx,fpy,frad+2,dim565(oc,4,8));
        d.fillCircle(fpx,fpy,frad,oc);
    }

    // Power-up sul campo con aura
    if(s_pu_type&&inv_(s_pu_x,s_pu_y)) {
        int ppx=sx_(s_pu_x)+CELL/2, ppy=sy_(s_pu_y)+CELL/2;
        uint16_t pc=PU_COL[s_pu_type];
        int pr=(s_tick&8)?3:4;
        int aura=((s_tick>>1)&2);
        d.drawCircle(ppx,ppy,pr+2+aura,dim565(pc,3,8));
        d.fillCircle(ppx,ppy,pr,pc);
        // Simbolo sopra l'orb
        d.setTextColor(0xFFFF); d.setTextSize(1);
        char sym[2]={PU_SYM[s_pu_type],0};
        d.setCursor(ppx-3, ppy-CELL-1); d.print(sym);
    }

    // Trail dietro i serpenti
    for(int i=0;i<s_trail1.cnt;i++) {
        if(!s_trail1.pts[i].life) continue;
        int tx=sx_(s_trail1.pts[i].x), ty=sy_(s_trail1.pts[i].y);
        int alpha=s_trail1.pts[i].life*255/24;
        uint16_t tc=dim565(C_GREEN,alpha,255);
        d.fillRect(tx+2,ty+2,CELL-4,CELL-4,tc);
    }
    for(int i=0;i<s_trail2.cnt;i++) {
        if(!s_trail2.pts[i].life) continue;
        int tx=sx_(s_trail2.pts[i].x), ty=sy_(s_trail2.pts[i].y);
        int alpha=s_trail2.pts[i].life*255/24;
        uint16_t tc=dim565(0xF81F,alpha,255);
        d.fillRect(tx+2,ty+2,CELL-4,CELL-4,tc);
    }

    // Serpenti: avversario sotto, proprio sopra
    if(s_mode==MODE_GUEST){ draw_snake(s_s1,C_GREEN);  draw_snake(s_s2,0xF81F); }
    else                  { draw_snake(s_s2,0xF81F);   draw_snake(s_s1,C_GREEN); }

    // Particelle esplosione
    for(auto& p:s_parts)
        if(p.life) d.fillRect((int)p.x,(int)p.y,3,3,p.col);

    // Flash bordo schermo a morte
    if(s_flash>0) {
        uint16_t fc=(s_winner==1)?C_GREEN:0xF81F;
        for(int t=0;t<(s_flash>5?2:1);t++)
            d.drawRect(t,PLAY_Y+t,W-2*t,H-PLAY_Y-2*t,fc);
    }

    draw_hud();
}

// ─── schermate menu ───────────────────────────────────────────────────────────
static void draw_menu(void) {
    d.fillScreen(BG);

    // Header band: a soft dark-green gradient with the wordmark + a bright accent baseline.
    for(int y=0;y<22;y++) d.drawFastHLine(0,y,W, dim565(C_GREEN, 22-y, 64));
    d.drawFastHLine(0,21,W, dim565(C_GREEN,1,2));
    d.drawFastHLine(0,22,W, dim565(C_GREEN,1,4));
    d.setTextSize(2);
    d.setTextColor(dim565(C_GREEN,1,3)); d.setCursor(21,4); d.print("SNAKE");   // drop shadow
    d.setTextColor(C_GREEN);             d.setCursor(20,3); d.print("SNAKE");
    d.setTextColor(0xF81F);              d.setCursor(92,3); d.print("DUEL");

    // Windowed list — scrolls so the selection is always visible and the footer stays clear.
    const int ITEM_H=18, TOP=28, VIS=5, FY=H-13;
    int maxtop = N_MENU>VIS ? N_MENU-VIS : 0;
    if(s_menu_sel < s_menu_top) s_menu_top=s_menu_sel;
    if(s_menu_sel >= s_menu_top+VIS) s_menu_top=s_menu_sel-VIS+1;
    if(s_menu_top>maxtop) s_menu_top=maxtop;
    if(s_menu_top<0) s_menu_top=0;
    int shown = (N_MENU-s_menu_top<VIS) ? N_MENU-s_menu_top : VIS;

    for(int k=0;k<shown;k++){
        int i=s_menu_top+k, py=TOP+k*ITEM_H;
        bool sel=(i==s_menu_sel);
        if(sel){
            d.fillRoundRect(4,py-1,W-8,ITEM_H-2,4, dim565(C_GREEN,1,6));
            d.drawRoundRect(4,py-1,W-8,ITEM_H-2,4, C_GREEN);
            d.fillRect(4,py,3,ITEM_H-3, C_GREEN);                    // left accent bar
            d.setTextColor(FG);
        } else d.setTextColor(MUTED);
        d.setTextSize(2);
        d.setCursor(15,py+1); d.print(MENU_ITEMS[i]);
    }
    // Scroll chevrons when the list overflows the window.
    int mid=W/2;
    if(s_menu_top>0)          d.fillTriangle(mid-4,TOP-3, mid+4,TOP-3, mid,TOP-7, C_GREEN);
    if(s_menu_top+VIS<N_MENU){ int yb=TOP+VIS*ITEM_H-2; d.fillTriangle(mid-4,yb, mid+4,yb, mid,yb+4, C_GREEN); }

    // Footer stats.
    d.drawFastHLine(0,FY-3,W,LINE);
    d.setTextColor(DIM); d.setTextSize(1);
    char buf[44];
    snprintf(buf,sizeof buf,"Vinte %d-%d   Record %d   Ch.%d",s_wins1,s_wins2,s_hisc,pnet_channel());
    d.setCursor(4,FY); d.print(buf);
}

static void draw_host(void) {
    d.fillScreen(BG);
    d.setTextColor(C_GREEN); d.setTextSize(2);
    d.setCursor(6,8); d.print("Crea Partita");
    d.drawFastHLine(0,28,W,LINE);
    d.setTextColor(FG); d.setTextSize(1);
    d.setCursor(6,38); d.print("In attesa avversario...");
    static int dot_t;
    for(int i=0;i<(dot_t/10)%4;i++){d.setCursor(6+i*8,54);d.print(".");}
    dot_t++;
    d.setTextColor(MUTED);
    char buf[32];
    d.setCursor(6,70); snprintf(buf,sizeof buf,"Dispositivo: %s",pnet_name()); d.print(buf);
    d.setCursor(6,82); snprintf(buf,sizeof buf,"Canale: %d",pnet_channel()); d.print(buf);
    d.setTextColor(DIM); d.setCursor(6,H-12); d.print("ESC = indietro");
}

static void draw_browse(void) {
    d.fillScreen(BG);
    d.setTextColor(0xF81F); d.setTextSize(2);
    d.setCursor(6,8); d.print("Entra Partita");
    d.drawFastHLine(0,28,W,LINE);
    if(s_n_hosts==0) {
        d.setTextColor(MUTED); d.setTextSize(1);
        d.setCursor(6,42); d.print("Cerco partite...");
        static int dt2;
        for(int i=0;i<(dt2/10)%4;i++){d.setCursor(100+i*8,42);d.print(".");}
        dt2++;
    } else {
        for(int i=0;i<s_n_hosts;i++) {
            int py=36+i*18;
            bool sel=(i==s_browse_sel);
            if(sel){d.fillRoundRect(2,py-2,W-4,16,2,0x2104);d.setTextColor(FG);}
            else   {d.setTextColor(MUTED);}
            d.setTextSize(1);
            if(sel){d.setCursor(4,py);d.print(">");}
            d.setCursor(14,py); d.print(s_hosts[i].name);
        }
        if(s_join_pending){
            d.setTextColor(C_YELLOW); d.setTextSize(1);
            d.setCursor(6,H-22); d.print("Connessione in corso...");
        }
    }
    d.setTextColor(DIM); d.setTextSize(1);
    d.setCursor(6,H-12); d.print("INVIO=entra  ESC=indietro");
}

static void draw_over(void) {
    draw_play();
    for(int y=PLAY_Y+20;y<H-10;y+=2) d.drawFastHLine(8,y,W-16,0x0000);
    d.fillRoundRect(8,PLAY_Y+18,W-16,H-PLAY_Y-28,4,0x1082);
    d.drawRoundRect(8,PLAY_Y+18,W-16,H-PLAY_Y-28,4,LINE);
    const char* wname=(s_winner==1)?s_s1.name:s_s2.name;
    uint16_t wcol=(s_winner==1)?C_GREEN:0xF81F;
    d.setTextColor(wcol); d.setTextSize(2);
    char buf[32];
    char wn[7]; strncpy(wn,wname,6); wn[6]=0;
    snprintf(buf,sizeof buf,"%s VINCE",wn);
    int tw=(int)strlen(buf)*12;
    d.setCursor((W-tw)/2,PLAY_Y+28); d.print(buf);
    d.setTextColor(FG); d.setTextSize(1);
    snprintf(buf,sizeof buf,"%s %d   %s %d",s_s1.name,s_s1.score,s_s2.name,s_s2.score);
    buf[30]=0;
    d.setCursor(14,PLAY_Y+52); d.print(buf);
    d.setTextColor(DIM); d.setCursor(14,PLAY_Y+66);
    d.print("INVIO=rivincita  ESC=menu");
}

static void draw_help(void) {
    d.fillScreen(BG);
    d.setTextColor(C_YELLOW); d.setTextSize(1);
    if(s_help_pg==0) {
        d.setCursor(4,4); d.print("[ CONTROLLI ]");
        d.setTextColor(FG);
        d.setCursor(4,18); d.print("W/A/S/D o Frecce = direz.");
        d.setCursor(4,30); d.print("ESC              = esci");
        d.setCursor(4,48); d.print("Mondo grande: camera segue");
        d.setCursor(4,58); d.print("il tuo serpente.");
        d.setTextColor(DIM); d.setCursor(4,74);
        d.print("La minimappa in alto destra");
        d.setCursor(4,84); d.print("mostra l'intero campo.");
    } else {
        d.setCursor(4,4); d.print("[ POWER-UP ]");
        d.setTextColor(C_YELLOW);  d.setCursor(4,18); d.print("F = FAST   ");
        d.setTextColor(FG);        d.print("vai piu veloce");
        d.setTextColor(C_PURPLE);  d.setCursor(4,30); d.print("L = LENTO  ");
        d.setTextColor(FG);        d.print("avversario rallenta");
        d.setTextColor(C_PINK);    d.setCursor(4,42); d.print("X = TAGLIA ");
        d.setTextColor(FG);        d.print("perdi 6 segmenti");
        d.setTextColor(C_GREY);    d.setCursor(4,54); d.print("G = GHOST  ");
        d.setTextColor(FG);        d.print("attraversi i muri");
        d.setTextColor(0x07FF);    d.setCursor(4,66); d.print("S = SCUDO  ");
        d.setTextColor(FG);        d.print("salva da 1 schianto");
    }
    d.setTextColor(DIM); d.setCursor(4,H-12);
    d.print("TAB=pagina  ESC=indietro");
}

static void draw_scores(void) {
    d.fillScreen(BG);
    d.setTextColor(C_YELLOW); d.setTextSize(2);
    d.setCursor(4,4); d.print("Classifica");
    d.drawFastHLine(0,26,W,LINE);
    d.setTextColor(C_GREEN); d.setTextSize(1);
    char buf[32];
    d.setCursor(4,34); snprintf(buf,sizeof buf,"P1 vittorie: %d",s_wins1); d.print(buf);
    d.setTextColor(0xF81F);
    d.setCursor(4,46); snprintf(buf,sizeof buf,"P2 vittorie: %d",s_wins2); d.print(buf);
    d.setTextColor(C_YELLOW);
    d.setCursor(4,62); snprintf(buf,sizeof buf,"Record cibo: %d",s_hisc); d.print(buf);
    d.setTextColor(DIM); d.setCursor(4,H-12); d.print("ESC = menu");
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

// ─── input ────────────────────────────────────────────────────────────────────
static void set_dir(int8_t dir) {
    if(s_st!=ST_PLAY) return;
    if(s_mode==MODE_GUEST){ if(dir!=OPP(s_s2.dir)) s_s2.next_dir=dir; return; }
    // Local snake: queue up to 2 turns vs the LAST intended heading (so rapid up-then-left both land),
    // dropping reversals and repeats. snake_step pops one per move.
    Snake& s=s_s1;
    int8_t ref = s.inq_n>0 ? s.inq[s.inq_n-1] : s.next_dir;
    if(dir==ref || dir==OPP(ref)) return;
    if(s.inq_n<2) s.inq[s.inq_n++]=dir;
}

static void on_key(int key, char ch) {
    SFX(SX_NAV);
    switch(s_st) {
        case ST_MENU:
            if(key==NK_UP  &&s_menu_sel>0)       { s_menu_sel--;  nucleo_app_request_draw(); }
            if(key==NK_DOWN&&s_menu_sel<N_MENU-1){ s_menu_sel++;  nucleo_app_request_draw(); }
            if(key==NK_ENTER||ch=='\r') {
                switch(s_menu_sel) {
                    case 0: s_mode=MODE_AI; strncpy(s_s2.name,"CPU",12); start_game((uint32_t)esp_timer_get_time()); break;
                    case 1: s_rng=(uint32_t)esp_timer_get_time(); s_n_hosts=0; s_hello_us=0; s_st=ST_HOST; nucleo_app_request_draw(); break;
                    case 2: s_n_hosts=0; s_browse_sel=0; s_join_pending=false; s_st=ST_BROWSE; nucleo_app_request_draw(); break;
                    case 3: s_st=ST_SCORES; nucleo_app_request_draw(); break;
                    case 4: s_help_pg=0; s_st=ST_HELP; nucleo_app_request_draw(); break;
                }
            }
            break;
        case ST_BROWSE:
            if(key==NK_UP  &&s_browse_sel>0)           { s_browse_sel--; nucleo_app_request_draw(); }
            if(key==NK_DOWN&&s_browse_sel<s_n_hosts-1){ s_browse_sel++; nucleo_app_request_draw(); }
            if((key==NK_ENTER||ch=='\r')&&s_n_hosts>0&&!s_join_pending) {
                s_join_pending=true;
                s_join_first_us=esp_timer_get_time(); s_join_retry_us=s_join_first_us;
                sn_join_t jn; fill_hdr(&jn,SN_JOIN);
                strncpy(jn.name,pnet_name(),11); jn.name[11]=0;
                pnet_send(s_hosts[s_browse_sel].mac,&jn,sizeof(jn));
                nucleo_app_request_draw();
            }
            break;
        case ST_PLAY:
            // NK_LEFT → on_back lo intercetta e chiama set_dir(DLT)
            if(key==NK_UP  ||ch=='w'||ch=='W') set_dir(DUP);
            if(key==NK_DOWN||ch=='s'||ch=='S') set_dir(DDN);
            if(key==NK_RIGHT||ch=='d'||ch=='D') set_dir(DRT);
            if(ch=='a'||ch=='A')               set_dir(DLT);
            break;
        case ST_OVER:
            if(key==NK_ENTER||ch=='\r') {
                if(s_mode==MODE_AI) start_game((uint32_t)esp_timer_get_time());
                else { s_st=ST_MENU; nucleo_app_request_draw(); }
            }
            break;
        case ST_HELP:
            if(key==NK_TAB){ s_help_pg^=1; nucleo_app_request_draw(); }
            break;
        default: break;
    }
}

static bool on_back(int key) {
    if(s_st==ST_PLAY&&key==NK_LEFT){ set_dir(DLT); return true; }
    SFX(SX_NAV);
    if(s_st==ST_PLAY) {
        if(s_mode==MODE_HOST||s_mode==MODE_GUEST){
            sn_bye_t bye; fill_hdr(&bye,SN_BYE);
            pnet_send(s_peer,&bye,sizeof(bye));
        }
        s_st=ST_MENU; nucleo_app_request_draw(); return true;
    }
    if(s_st==ST_OVER||s_st==ST_HOST||s_st==ST_HELP||s_st==ST_SCORES){
        if(s_st==ST_BROWSE) s_join_pending=false;
        s_st=ST_MENU; nucleo_app_request_draw(); return true;
    }
    if(s_st==ST_BROWSE){ s_join_pending=false; s_st=ST_MENU; nucleo_app_request_draw(); return true; }
    return false;
}

static void on_tab(void) {
    if(s_st==ST_HELP){ s_help_pg^=1; nucleo_app_request_draw(); }
}

// ─── ciclo vita app ───────────────────────────────────────────────────────────
static void on_enter(void) {
    if(!s_obstacles) s_obstacles=(uint8_t(*)[WORLD_W])calloc(WORLD_H,WORLD_W);  // ~3.1 KB only while playing
    s_st=ST_MENU; s_menu_sel=0;
    memset(s_parts,0,sizeof(s_parts));
    s_cam_x=0; s_cam_y=0;
    s_last_us=esp_timer_get_time();
    if (!pnet_start()) nucleo_app_set_hint("ESP-NOW non avviato  Esc");
    else nucleo_app_set_hint("\x18\x19 scegli  Invio avvia  Esc esci");
    nucleo_app_set_poll_handler(poll_fn);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_fullscreen(false);
    nucleo_app_request_draw();
}
static void on_exit(void) {
    if(s_mode==MODE_HOST||s_mode==MODE_GUEST){
        sn_bye_t bye; fill_hdr(&bye,SN_BYE);
        pnet_send(s_peer,&bye,sizeof(bye));
    }
    pnet_stop();
    nucleo_app_set_fullscreen(false);
    free(s_obstacles); s_obstacles=nullptr;   // back to zero .bss until relaunched
}

// ─── registrazione ────────────────────────────────────────────────────────────
extern "C" void nucleo_register_snake(void) {
    static const nucleo_app_def_t app = {
        "snake", "Snake", "Games", "Serpente 1v1 in rete (ESP-NOW) o vs AI",
        'S', C_GREEN, on_enter, on_key, nullptr, on_draw, on_exit,
        NX_SOLO
    };
    nucleo_app_register(&app);
}
