// app_tankduel.cpp — NucleoOS "Tank Duel"
// Top-down 1v1 arena: 40×40 proc-gen square maps (320×320 world px),
// destructible walls, contested shop zones (auto-open on enter, 20 s,
// both players race to them), 4 tank archetypes, weapon/powerup upgrades.
// Credits earned mid-match (hits/kills/passive); spend in shops.
// Tank hides from minimap while inside shop — opponent can only guess.
// ESP-NOW host-authoritative or vs CPU. NX_NET_APP exclusive.
//
// Controls: W/E/R=up  A=left  S=down  D=right  K=fire  L=powerup  ESC=pause

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "notify_synth.h"
#include "game_sfx.h"
#include <M5GFX.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
extern "C" {
#include "nucleo_pnet.h"
#include "nucleo_audio.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
}
#define TDTAG "TANKD"
static const char* s_over_why="?";   // DEBUG: reason for the last GS_OVER

// ========================== color helpers =====================================
static inline uint16_t rgb(int r,int g,int b){
    r=r<0?0:r>255?255:r; g=g<0?0:g>255?255:g; b=b<0?0:b>255?255:b;
    return(uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3));
}
static uint16_t mix(uint16_t a,uint16_t b,int t){
    t=t<0?0:t>256?256:t;
    int ar=(a>>11)&31,ag=(a>>5)&63,ab=a&31;
    int br=(b>>11)&31,bg=(b>>5)&63,bb=b&31;
    return(uint16_t)((((ar+(br-ar)*t/256)&31)<<11)|(((ag+(bg-ag)*t/256)&63)<<5)|((ab+(bb-ab)*t/256)&31));
}
#define COL_WHITE  0xFFFF
#define COL_BLACK  0x0000
#define COL_P1     rgb(96,212,236)   // cyan  — player 1
#define COL_P2     rgb(255,150,80)   // orange — player 2
#define COL_GOLD   rgb(255,200,80)
#define COL_RED    rgb(244,84,72)
#define COL_GREEN  rgb(100,220,120)
#define COL_DIM    rgb(90,100,130)
#define COL_MUT    rgb(150,160,190)


// ========================== constants =========================================
#define MAP_W   40
#define MAP_H   40
#define TILE_PX  15              // zoom: bigger tiles + sprites = elementi più visibili
#define HUD_H   14
#define PLAY_H  (H - HUD_H)      // 121 px
#define WORLD_W (MAP_W * TILE_PX) // 600
#define WORLD_H (MAP_H * TILE_PX) // 600

// tile types (damage: 1=wall3hp → 2=wall2hp → 3=wall1hp → 0=floor)
#define T_FLOOR  0
#define T_WALL3  1
#define T_WALL2  2
#define T_WALL1  3
#define T_BUNKER 4
#define T_WATER  5

// weapon types
enum { WP_CANNON=0, WP_MG, WP_RAIL, WP_SHOT, WP_FLAK, WP_ROCKET, WP_LASER, WP_SNIPER, WP_MINIGUN, WP_COUNT };
// powerup types (one active slot per tank)
enum { PU_NONE=0, PU_SHIELD, PU_BOOST, PU_BURST, PU_EMP, PU_REGEN };
// shop item types
enum { SI_ARMOR=0, SI_SPEED, SI_REPAIR, SI_SHIELD, SI_EMP,
       SI_WP_MG, SI_WP_RAIL, SI_WP_SHOT, SI_WP_FLAK, SI_WP_ROCKET, SI_WP_LASER, SI_WP_SNIPER, SI_WP_MINIGUN, SI_REGEN, SI_COUNT };
// tank archetypes
enum { TT_BULLDOG=0, TT_VIPER, TT_PHANTOM, TT_CRUSHER, TT_COUNT };
// game states
enum { GS_MENU=0, GS_SELECT, GS_HOST, GS_BROWSE, GS_PLAY, GS_SHOP, GS_PAUSE, GS_OVER, GS_HOW };
// game modes (network role)
enum { GM_CPU=0, GM_HOST, GM_GUEST };
// match type (rules / who fights whom)
enum { MT_DUEL=0,   // 1v1 humans, no bots
       MT_COOP,     // humans allied vs a respawning bot horde
       MT_BRAWL,    // 1v1 humans AND bots that hunt only humans
       MT_COUNT };
#define MAX_BOTS 4
#define BOT_OWNER0 2          // bullet owner ids: 0,1 = humans, 2.. = bots
#define BOT_KILL_CREDITS 12   // dollars dropped to the killer when a bot dies
// co-op team result codes carried in s_winner (besides 0/1=player, -1=draw)
#define WIN_HUMANS 9
#define WIN_BOTS   8

// ========================== structs ===========================================
struct Tank {
    float x, y;       // world px center
    float spd;        // px / ms
    int   dir;        // 0=up 1=right 2=down 3=left (4-way: sprite tread/net)
    float fx, fy;     // facing unit vector (8-way aim: barrel + firing)
    int   hp, hp_max;
    int   armor;      // 0-3 damage reduction
    int   credits;
    int   weapon;     // WP_*
    int   fire_cd;    // ms until next shot
    int   pu;         // PU_* active powerup
    int   pu_ms;      // ms remaining (0 = passive/none)
    int   type;       // TT_*
    bool  alive;
    bool  in_shop;
    int   shop_ms;    // ms remaining inside shop
    int   flash_ms;   // muzzle-flash timer (ms)
    int   tread;      // tread animation phase (advances while moving)
    int   hurt_ms;    // hit-flash timer (ms)
    int   respawn_ms; // bots: ms until respawn (0 = alive/none)
    int   behav;      // bots: behaviour personality (BB_*)
    int   aim_ms;     // bots: reaction/pause gate between shots (no continuous fire)
};
// bot personalities
enum { BB_RUSHER=0, BB_SNIPER, BB_FLANKER, BB_BRAWLER, BB_COUNT };
#define MAX_BULLETS 14
struct Bullet {
    float x, y, vx, vy;
    int   owner;    // 0,1=humans, 2..=bots
    int   dmg;
    int   wp;
    int   pierce;   // remaining pierces (rail/laser)
    int   life;     // ms of remaining flight (0 = unlimited); flak pellets are short-range
    bool  alive;
};
#define MAX_SHOPS 2
struct ShopZone {
    uint8_t tx, ty; // top-left tile of 3×3 zone
    int  life_ms;
    bool active;
};
#define SHOP_N SI_COUNT          // shop lists ALL items (scrollable) — everything always buyable
struct ShopItem { int type; int cost; bool sold; };
// which shop zone a tank stands in, or -1
static int tank_shop_index(int who);

// floating pickups dropped on the floor: a powerup, or a basic weapon to grab
#define MAX_PICKS 3
struct Pickup { uint8_t tx, ty; uint8_t type; uint8_t kind; int life_ms; bool active; };
enum { PK_PU=0, PK_WEAPON=1 };

#define MAX_SPARKS 32
struct Spark { float x,y,vx,vy; uint16_t col; int life,maxlife; };

#define MAX_RINGS 4
struct Ring { float x,y; int life,maxlife; uint16_t col; };  // expanding blast ring

// ========================== static state ======================================
static uint8_t   s_map[MAP_H][MAP_W];
static Tank      s_tanks[2];
static Bullet    s_bullets[MAX_BULLETS];
static ShopZone  s_shops[MAX_SHOPS];
static ShopItem  s_shop_items[2][SHOP_N];  // per-player catalog (deterministic)
static int8_t    s_shop_zone[2];           // which zone each tank is shopping (-1 none)
static int8_t    s_shop_done[2];           // zone already shopped — blocked until tank exits it
static Spark     s_sparks[MAX_SPARKS];
static Ring      s_rings[MAX_RINGS];
static Pickup    s_picks[MAX_PICKS];
static int       s_pick_spawn_cd;
static Tank      s_bots[MAX_BOTS];      // respawning AI horde (host/CPU authoritative)
static int       s_nbots;               // active bots this match (0 in MT_DUEL)
static int       s_nplayers;            // 1 (solo) or 2 (the human slots in use)
static int       s_match_type;          // MT_*
static int       s_bot_level;           // escalation tier (raises bot hp/spd/weapon)
static int       s_bot_kills;           // total bots killed this match
static int       s_bot_level_cd;        // ms until the next time-based level-up

static int  s_state, s_mode, s_local;   // local player index
static int  s_msel, s_tsel;             // menu / tank select cursor
static int  s_shop_sel, s_shop_scroll;
static int  s_shop_tab;                 // 0=BUY 1=SELL
static int  s_cam_x, s_cam_y;           // camera top-left world px
static int  s_shop_spawn_cd;            // ms until next shop spawn attempt
static int  s_passive_cd;               // ms until passive credit tick
static int  s_match_ms;                 // countdown timer
static int  s_winner;                   // 0=P1 1=P2 -1=draw
static unsigned s_anim;
static int64_t  s_now, s_last, s_frame;
static uint32_t s_rng, s_map_seed;
static float    s_shake;
static int      g_audio = 1;
static int      s_ai_shop_cd;           // ms until AI's next shop purchase
static int      s_flash;                // global screen flash (white) intensity 0-255

// ========================== SFX ==============================================
// 1=fire 2=hit 3=kill 4=shop 5=powerup 6=win 7=lose 8=buy 9=sell 10=nav 11=sel
// Real arcade WAVs live in /sd/data/tankduel/pack/<name>.wav (deployed); the synth recipes
// below are only the never-mute fallback if the SD pack is missing.
#define TD_SFX_DIR "/sd/data/tankduel"
#define SFX_FIRE 1
#define SFX_HIT  2
#define SFX_KILL 3
#define SFX_SHOP 4
#define SFX_PU   5
#define SFX_WIN  6
#define SFX_LOSE 7
#define SFX_BUY  8
#define SFX_SELL 9
#define SFX_NAV  10
#define SFX_SEL  11
static const char* sfx_td_name(int id){
    switch(id){ case 1:return"fire"; case 2:return"hit"; case 3:return"kill";
                case 4:return"shop"; case 5:return"pu";  case 6:return"win"; case 7:return"lose";
                case 8:return"buy";  case 9:return"sell";case 10:return"nav"; default:return"sel"; }
}
static int sfx_td_recipe(int id, notify_voice_t*v){
    switch(id){
        case 1: // fire — sharp bark
            notify__voice(&v[0], 320, 0,     0.025f); v[0].amp=0.9f;
            notify__voice(&v[1], 160, 0.015f,0.035f); v[1].amp=0.6f; return 2;
        case 2: // hit — thud
            notify__voice(&v[0], 220, 0,     0.04f);  v[0].amp=0.8f;
            notify__voice(&v[1], 110, 0.02f, 0.05f);  v[1].amp=0.7f; return 2;
        case 3: // kill — explosion descend
            notify__voice(&v[0], 300, 0,     0.05f);  v[0].amp=1.0f;
            notify__voice(&v[1], 180, 0.04f, 0.08f);  v[1].amp=0.9f;
            notify__voice(&v[2],  90, 0.10f, 0.14f);  v[2].amp=0.8f; return 3;
        case 4: // shop open — cash ding
            notify__voice(&v[0], 880, 0,     0.05f);  v[0].amp=0.7f;
            notify__voice(&v[1],1174.7f,0.04f,0.06f); v[1].amp=0.6f; return 2;
        case 5: // powerup — sparkle
            notify__voice(&v[0], 660, 0,     0.04f);  v[0].amp=0.6f;
            notify__voice(&v[1], 988, 0.03f, 0.05f);  v[1].amp=0.5f;
            notify__voice(&v[2],1320, 0.07f, 0.07f);  v[2].amp=0.4f; return 3;
        case 6: // win fanfare
            notify__voice(&v[0], 523.25f,0,    0.08f); v[0].amp=0.8f;
            notify__voice(&v[1], 659.25f,0.07f,0.09f); v[1].amp=0.7f;
            notify__voice(&v[2], 783.99f,0.15f,0.10f); v[2].amp=0.6f;
            notify__voice(&v[3],1046.5f, 0.24f,0.18f); v[3].amp=0.7f; return 4;
        case 7: // lose — toll
            notify__voice(&v[0], 392, 0,     0.10f); v[0].amp=0.8f;
            notify__voice(&v[1], 294, 0.09f, 0.12f); v[1].amp=0.7f;
            notify__voice(&v[2], 196, 0.20f, 0.20f); v[2].amp=0.6f; return 3;
        case 8: // buy — cha-ching up
            notify__voice(&v[0], 784, 0,     0.04f); v[0].amp=0.7f;
            notify__voice(&v[1],1046.5f,0.03f,0.06f);v[1].amp=0.6f; return 2;
        case 9: // sell — coins down
            notify__voice(&v[0],1046.5f,0,   0.04f); v[0].amp=0.6f;
            notify__voice(&v[1], 659.25f,0.03f,0.06f);v[1].amp=0.6f; return 2;
        case 10: // nav — soft tick
            notify__voice(&v[0], 520, 0, 0.02f); v[0].amp=0.4f; return 1;
        case 11: // sel — confirm blip
            notify__voice(&v[0], 660, 0, 0.03f); v[0].amp=0.6f;
            notify__voice(&v[1], 880, 0.02f, 0.04f); v[1].amp=0.5f; return 2;
    }
    return 0;
}
static bool sfx_td_important(int id){ return id==3||id==6||id==7||id==8||id==9; }
static const game_sfx_t s_sfx = {
    TD_SFX_DIR, sfx_td_name, sfx_td_recipe, 11, 2, 12000, sfx_td_important, &g_audio
};
static void sfx_play(int id){ game_sfx_play(&s_sfx, id); }

// net
static uint8_t  s_peer[6];
static bool     s_haspeer, s_join_pending, s_peerleft;
static uint32_t s_session, s_txseq, s_rxseq;
static int64_t  s_last_rx, s_last_tx, s_last_hello, s_join_t, s_join_resend;

struct Host { uint8_t mac[6]; char name[22]; int64_t seen; };
#define NHOST 6
static Host s_hosts[NHOST];
static int  s_nhost, s_bsel;

// ========================== utility ===========================================
static int64_t now_ms(void) { return esp_timer_get_time()/1000; }
static uint32_t xr(void) {
    s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; return s_rng;
}
static float frnd(float a,float b){ return a+(b-a)*((xr()&0xFFFF)/65535.0f); }

static void txt(int x,int y,int sz,uint16_t col,const char*s){
    d.setTextSize(sz); d.setTextColor(col); d.setCursor(x,y); d.print(s);
}
static void txc(int cx,int y,int sz,uint16_t col,const char*s){
    txt(cx-(int)strlen(s)*3*sz,y,sz,col,s);
}
static void txr(int rx,int y,int sz,uint16_t col,const char*s){
    txt(rx-(int)strlen(s)*6*sz,y,sz,col,s);
}

// ========================== shop catalog =====================================
static const struct { const char* name; int cost; } SHOP_CAT[SI_COUNT] = {
    {"Corazza +1", 20},
    {"Velocita +1",15},
    {"Riparazione",30},
    {"Scudo",      20},
    {"EMP",        25},
    {"Mitragl.",   25},
    {"Railgun",    35},
    {"Shotgun",    30},
    {"Flak",       30},
    {"Lanciarazzi",40},
    {"Laser",      35},
    {"Sniper",     45},
    {"Minigun",    28},
    {"Regen",      35},
};

static const char* wp_short(int w){
    switch(w){ case WP_MG: return "MG"; case WP_RAIL: return "RAIL"; case WP_SHOT: return "SHOT";
               case WP_FLAK: return "FLAK"; case WP_ROCKET: return "RAZZO"; case WP_LASER: return "LASER";
               case WP_SNIPER: return "SNIP"; case WP_MINIGUN: return "MINI"; default: return "CANN"; }
}
static const char* pu_short(int p){
    switch(p){ case PU_SHIELD: return "SCUD"; case PU_BOOST: return "BOOS"; case PU_BURST: return "BURS"; case PU_EMP: return "EMP"; case PU_REGEN: return "RGEN"; default: return "----"; }
}
static uint16_t pu_color(int p){
    switch(p){ case PU_SHIELD: return rgb(90,170,255); case PU_BOOST: return COL_GOLD;
               case PU_BURST: return COL_RED; case PU_EMP: return rgb(180,120,255); case PU_REGEN: return COL_GREEN; default: return COL_WHITE; }
}
static const char* pu_letter(int p){
    switch(p){ case PU_SHIELD: return "S"; case PU_BOOST: return "B"; case PU_BURST: return "R"; case PU_EMP: return "E"; case PU_REGEN: return "G"; default: return "?"; }
}
static const char* tank_name(int t){
    switch(t){ case TT_VIPER: return "VIPER"; case TT_PHANTOM: return "PHANTOM"; case TT_CRUSHER: return "CRUSHER"; default: return "BULLDOG"; }
}

// ========================== tile helpers =====================================
static bool tile_solid(int tx,int ty){
    if(tx<0||tx>=MAP_W||ty<0||ty>=MAP_H) return true;
    uint8_t t=s_map[ty][tx];
    return t==T_WALL3||t==T_WALL2||t==T_WALL1||t==T_BUNKER;
}
static bool tile_water(int tx,int ty){
    if(tx<0||tx>=MAP_W||ty<0||ty>=MAP_H) return false;
    return s_map[ty][tx]==T_WATER;
}
static void tile_damage(int tx,int ty){
    if(tx<0||tx>=MAP_W||ty<0||ty>=MAP_H) return;
    uint8_t &t=s_map[ty][tx];
    if(t==T_WALL3) t=T_WALL2;
    else if(t==T_WALL2) t=T_WALL1;
    else if(t==T_WALL1) t=T_FLOOR;
}

// ========================== map generation ===================================
static void fill_rect_map(int tx,int ty,int tw,int th,uint8_t v){
    for(int y=ty;y<ty+th;y++) for(int x=tx;x<tx+tw;x++)
        if(x>0&&x<MAP_W-1&&y>0&&y<MAP_H-1) s_map[y][x]=v;
}
static void map_gen(uint32_t seed){
    s_rng=seed; if(!s_rng) s_rng=0xBEEF1234u;
    memset(s_map,T_FLOOR,sizeof s_map);
    // border bunkers
    for(int x=0;x<MAP_W;x++){ s_map[0][x]=T_BUNKER; s_map[MAP_H-1][x]=T_BUNKER; }
    for(int y=0;y<MAP_H;y++){ s_map[y][0]=T_BUNKER; s_map[y][MAP_W-1]=T_BUNKER; }
    // generate top-left quadrant, then 4-way mirror
    int qw=MAP_W/2-1, qh=MAP_H/2-1;
    // random wall clusters
    for(int gy=0;gy<4;gy++) for(int gx=0;gx<3;gx++){
        if((xr()%10)>=4) continue;
        int wx=2+gx*6+(xr()%3);
        int wy=2+gy*6+(xr()%3);
        int ww=2+(xr()%3);
        int wh=2+(xr()%4);
        if(wx+ww>qw) ww=qw-wx;
        if(wy+wh>qh) wh=qh-wy;
        fill_rect_map(wx,wy,ww,wh,T_WALL3);
    }
    // water patches in quadrant
    for(int i=0;i<2;i++){
        int wx=2+(xr()%(qw-4));
        int wy=2+(xr()%(qh-4));
        int ws=2+(xr()%2);
        fill_rect_map(wx,wy,ws,ws,T_WATER);
    }
    // 4-way mirror: copy quadrant to other 3
    for(int y=1;y<MAP_H/2;y++) for(int x=1;x<MAP_W/2;x++){
        uint8_t v=s_map[y][x];
        s_map[y][MAP_W-1-x]=v;            // L-R
        s_map[MAP_H-1-y][x]=v;            // T-B
        s_map[MAP_H-1-y][MAP_W-1-x]=v;   // diagonal
    }
    // central bunker cross
    int cx=MAP_W/2, cy=MAP_H/2;
    for(int i=-1;i<=1;i++) for(int j=-3;j<=3;j++){
        if(i==0&&(j<-1||j>1)) continue;   // thin cross
        s_map[cy+i][cx+j]=T_BUNKER;
        s_map[cy+j][cx+i]=T_BUNKER;
    }
    // clear spawn areas (5×5 corners)
    fill_rect_map(1,1,5,5,T_FLOOR);
    fill_rect_map(MAP_W-6,MAP_H-6,5,5,T_FLOOR);
    // secondary spawns also clear (for 4-way mirror symmetry: 2 corners)
    fill_rect_map(MAP_W-6,1,5,5,T_FLOOR);
    fill_rect_map(1,MAP_H-6,5,5,T_FLOOR);
}

// ========================== tank init ========================================
static void tank_init(Tank &t, int type, bool p1){
    t.type=type; t.dir=p1?2:0; t.fx=0; t.fy=p1?1.0f:-1.0f; t.alive=true; t.in_shop=false; t.shop_ms=0;
    t.pu=PU_NONE; t.pu_ms=0; t.weapon=WP_CANNON; t.fire_cd=0; t.credits=0;
    t.flash_ms=0; t.tread=0; t.hurt_ms=0;
    // spawn positions
    t.x = p1 ? (3*TILE_PX+TILE_PX/2) : ((MAP_W-4)*TILE_PX+TILE_PX/2);
    t.y = p1 ? (3*TILE_PX+TILE_PX/2) : ((MAP_H-4)*TILE_PX+TILE_PX/2);
    switch(type){
        case TT_BULLDOG: t.hp=t.hp_max=8; t.armor=1; t.spd=0.042f; break;
        case TT_VIPER:   t.hp=t.hp_max=4; t.armor=0; t.spd=0.080f; break;
        case TT_PHANTOM: t.hp=t.hp_max=5; t.armor=0; t.spd=0.060f; break;
        case TT_CRUSHER: t.hp=t.hp_max=10;t.armor=2; t.spd=0.025f; break;
    }
}

// ========================== camera ===========================================
static void cam_update(void){
    Tank &t=s_tanks[s_local];
    s_cam_x=(int)t.x-W/2;
    s_cam_y=(int)t.y-PLAY_H/2;
    if(s_cam_x<0) s_cam_x=0;
    if(s_cam_y<0) s_cam_y=0;
    if(s_cam_x>WORLD_W-W) s_cam_x=WORLD_W-W;
    if(s_cam_y>WORLD_H-PLAY_H) s_cam_y=WORLD_H-PLAY_H;
}

// screen-shake offset (recomputed per frame from s_shake)
static int s_shx, s_shy;
// world-to-screen helpers (shake folded in; HUD draws unshaken)
static int wx2s(float wx){ return (int)wx - s_cam_x + s_shx; }
static int wy2s(float wy){ return (int)wy - s_cam_y + HUD_H + s_shy; }
static int tx2s(int tx){ return tx*TILE_PX - s_cam_x + s_shx; }
static int ty2s(int ty){ return ty*TILE_PX - s_cam_y + HUD_H + s_shy; }

// ========================== particles =========================================
static void spark_burst(float x,float y,int n,uint16_t col){
    for(int i=0;i<MAX_SPARKS&&n>0;i++){
        if(s_sparks[i].life>0) continue;
        float a=frnd(0,6.28f), sp=frnd(0.05f,0.30f);
        int life=240+(int)frnd(0,220);
        s_sparks[i]={x,y,cosf(a)*sp,sinf(a)*sp,col,life,life};
        n--;
    }
}
static void ring_add(float x,float y,uint16_t col,int life){
    for(int i=0;i<MAX_RINGS;i++){
        if(s_rings[i].life>0) continue;
        s_rings[i]={x,y,life,life,col}; return;
    }
    // all slots full: find oldest ring and overwrite
    int oldest=0;
    for(int i=1;i<MAX_RINGS;i++) if(s_rings[i].life<s_rings[oldest].life) oldest=i;
    s_rings[oldest]={x,y,life,life,col};
}
// big explosion: ring + flash + colored sparks + embers
static void boom(float x,float y,uint16_t col){
    ring_add(x,y,COL_WHITE,380);
    ring_add(x,y,col,460);
    ring_add(x,y,mix(col,COL_BLACK,100),520);  // extra ring wave
    spark_burst(x,y,24,col);      // more sparks
    spark_burst(x,y,16,COL_GOLD); // more embers
    spark_burst(x,y,8,COL_WHITE); // white core flares
    if(s_flash<180) s_flash=180;  // brighter flash
}
static void sparks_step(int dt){
    for(int i=0;i<MAX_SPARKS;i++){
        if(s_sparks[i].life<=0) continue;
        s_sparks[i].x+=s_sparks[i].vx*dt;
        s_sparks[i].y+=s_sparks[i].vy*dt;
        s_sparks[i].vx*=0.88f; s_sparks[i].vy*=0.88f; // more drag for slower fallout
        s_sparks[i].life-=dt;
    }
    for(int i=0;i<MAX_RINGS;i++){
        if(s_rings[i].life<=0) continue;
        s_rings[i].life-=dt;
        // fade out more smoothly
    }
    if(s_flash>0){ s_flash-=dt*1.5f; if(s_flash<0) s_flash=0; }  // slower fade
}
static void shake_set(float a){ if(a>s_shake) s_shake=a; }

// ========================== credits ==========================================
static void earn(int player, int amount){
    if(player>=0&&player<2) s_tanks[player].credits+=amount;   // only the two human slots bank credits
}
// ---- combatant addressing: ids 0,1 = humans (s_tanks); 2.. = bots (s_bots) ----
static Tank& combatant(int id){ return id<BOT_OWNER0 ? s_tanks[id] : s_bots[id-BOT_OWNER0]; }
static bool  id_is_bot(int id){ return id>=BOT_OWNER0; }
static bool  id_active(int id){ return id<BOT_OWNER0 ? (id<s_nplayers) : ((id-BOT_OWNER0)<s_nbots); }
static bool  can_hit(int owner,int victim){
    bool ab=id_is_bot(owner), vb=id_is_bot(victim);
    if(ab&&vb) return false;                       // bots never damage each other
    if(!ab&&!vb) return s_match_type!=MT_COOP;      // humans damage each other except in co-op
    return true;                                   // human <-> bot always
}
static uint16_t tank_col(int id){
    if(id==0) return COL_P1;
    if(id==1) return COL_P2;
    static const uint16_t BC[4]={rgb(220,60,60),rgb(200,90,40),rgb(190,50,120),rgb(150,40,40)};
    return BC[(id-BOT_OWNER0)&3];                    // distinct menacing shades per bot
}

// ========================== bullet system ====================================
static int fire_cd_for(int wp){
    switch(wp){ case WP_MG: return 150; case WP_RAIL: return 2200; case WP_SHOT: return 800;
                case WP_FLAK: return 900; case WP_ROCKET: return 1500; case WP_LASER: return 240;
                case WP_SNIPER: return 1800; case WP_MINIGUN: return 80; default: return 1100; }
}
static float bullet_spd(int wp){
    switch(wp){ case WP_MG: return 0.200f; case WP_RAIL: return 0.320f; case WP_SHOT: return 0.110f;
                case WP_FLAK: return 0.150f; case WP_ROCKET: return 0.105f; case WP_LASER: return 0.430f;
                case WP_SNIPER: return 0.350f; case WP_MINIGUN: return 0.180f; default: return 0.130f; }
}
static int bullet_dmg(int wp){
    switch(wp){ case WP_MG: return 1; case WP_RAIL: return 4; case WP_SHOT: return 1;
                case WP_FLAK: return 1; case WP_ROCKET: return 5; case WP_LASER: return 2;
                case WP_SNIPER: return 6; case WP_MINIGUN: return 1; default: return 2; }
}
static int   wp_pellets(int wp){ return wp==WP_SHOT?3 : wp==WP_FLAK?6 : 1; }
static float wp_spread(int wp){ return wp==WP_SHOT?0.06f : wp==WP_FLAK?0.13f : 0.0f; }
static int   wp_pierce(int wp){ return wp==WP_RAIL?1 : wp==WP_LASER?3 : wp==WP_SNIPER?2 : 0; }
static int   wp_life(int wp){ return wp==WP_FLAK?360 : 0; }           // 0 = unlimited range
static float wp_recoil(int wp){ return wp==WP_ROCKET?9.0f : wp==WP_SNIPER?2.0f : 0.0f; }    // kickback px

static bool cell_free(float cx,float cy);   // fwd (recoil needs it)
// core fire: any combatant Tank with its owner-id (0,1 humans, 2.. bots)
static void fire_owner(Tank &t, int owner){
    if(!t.alive||t.in_shop||t.fire_cd>0) return;
    int burst=(t.pu==PU_BURST)?3:1;
    int wp=t.weapon;
    int pellets=wp_pellets(wp);
    float fx=t.fx, fy=t.fy;
    if(fx==0&&fy==0) fy=-1;
    float px=-fy, py=fx;
    float spd=bullet_spd(wp);
    int dmg=bullet_dmg(wp);
    int pierce=wp_pierce(wp), life=wp_life(wp);
    for(int b=0;b<pellets;b++){
        float vx=fx*spd, vy=fy*spd;
        if(pellets>1){ float spread=(float)(b-(pellets-1)/2.0f)*wp_spread(wp); vx+=px*spread; vy+=py*spread; }
        for(int i=0;i<MAX_BULLETS;i++){
            if(s_bullets[i].alive) continue;
            s_bullets[i]={t.x+fx*8,t.y+fy*8,vx,vy,owner,dmg,wp,pierce,life,true};
            break;
        }
    }
    t.fire_cd=fire_cd_for(wp)*burst;
    t.flash_ms=100;  // longer flash
    // rocket kickback — shove the tank backwards if there's room
    float rc=wp_recoil(wp);
    if(rc>0){ float nx=t.x-fx*rc, ny=t.y-fy*rc; if(cell_free(nx,ny)){ t.x=nx; t.y=ny; } shake_set(7.0f); }
    int muzzle_sparks=(wp==WP_FLAK)?12:(wp==WP_ROCKET)?8:(wp==WP_MINIGUN)?4:5;
    spark_burst(t.x+fx*11,t.y+fy*11,muzzle_sparks,COL_GOLD);
    if(wp==WP_ROCKET||wp==WP_SNIPER) spark_burst(t.x+fx*13,t.y+fy*13,4,COL_WHITE);
    if(owner==s_local) sfx_play(1);
}
static void tank_fire(int who){ fire_owner(s_tanks[who], who); }

// apply damage to combatant `id` (handles shield, armor, death, bot respawn + credits)
static void damage_tank(int id, int dmg, int owner, float hx, float hy){
    Tank &t=combatant(id);
    if(!t.alive) return;
    if(t.pu==PU_SHIELD){ t.pu=PU_NONE; ring_add(t.x,t.y,rgb(150,225,255),200); sfx_play(2); return; }
    // arcade fairness: bot hits on humans deal reduced damage
    if(id_is_bot(owner)&&!id_is_bot(id)) dmg=(dmg+1)/2;
    int dd=dmg-t.armor; if(dd<1) dd=1;
    t.hp-=dd; t.hurt_ms=200; earn(owner,2);
    shake_set(6.0f); spark_burst(hx,hy,14,tank_col(id)); spark_burst(hx,hy,6,COL_WHITE);
    if(t.hp<=0){
        t.hp=0; t.alive=false; shake_set(14.0f); boom(t.x,t.y,tank_col(id)); sfx_play(3);
        if(id_is_bot(id)){
            t.respawn_ms=2600; s_bot_kills++;
            if(owner<2) earn(owner,BOT_KILL_CREDITS);
            if(s_bot_kills>=(s_bot_level+1)*5 && s_bot_level<10) s_bot_level++;
        } else earn(owner,15);
    } else { ring_add(hx,hy,COL_WHITE,200); ring_add(hx,hy,mix(tank_col(id),COL_WHITE,100),180); sfx_play(2); }
}
// rocket splash: hurt everyone in range (not the shooter, respecting team rules)
static void explode(float x,float y,int owner,int dmg,float radius){
    boom(x,y,COL_GOLD); shake_set(12.0f); ring_add(x,y,COL_RED,460); spark_burst(x,y,20,COL_RED);
    int nc=BOT_OWNER0+s_nbots;
    for(int id=0;id<nc;id++){
        if(!id_active(id)||id==owner||!can_hit(owner,id)) continue;
        Tank &t=combatant(id); if(!t.alive) continue;
        float dx=t.x-x, dy=t.y-y; if(dx*dx+dy*dy>radius*radius) continue;
        damage_tank(id,dmg,owner,t.x,t.y);
    }
}
static void bullets_step(int dt){
    for(int i=0;i<MAX_BULLETS;i++){
        Bullet &b=s_bullets[i];
        if(!b.alive) continue;
        // emit trail sparks for rockets and sniper shots
        if((b.wp==WP_ROCKET||b.wp==WP_SNIPER)&&(xr()&3)==0)
            spark_burst(b.x,b.y,2,mix(tank_col(b.owner),COL_BLACK,150));
        if(b.wp==WP_LASER&&(xr()&1)==0)
            spark_burst(b.x,b.y,1,rgb(120,255,210));
        b.x+=b.vx*dt; b.y+=b.vy*dt;
        if(b.life>0){ b.life-=dt; if(b.life<=0){ b.alive=false; spark_burst(b.x,b.y,2,COL_GOLD); continue; } }
        int tx=(int)b.x/TILE_PX, ty=(int)b.y/TILE_PX;
        if(b.x<0||b.x>=WORLD_W||b.y<0||b.y>=WORLD_H){
            if(b.wp==WP_ROCKET) explode(b.x,b.y,b.owner,b.dmg,26);
            b.alive=false; continue;
        }
        // wall collision
        if(s_map[ty][tx]==T_BUNKER){
            if(b.wp==WP_ROCKET) explode(b.x,b.y,b.owner,b.dmg,26);
            b.alive=false; spark_burst(b.x,b.y,4,COL_WHITE); continue;
        }
        if(s_map[ty][tx]>=T_WALL3&&s_map[ty][tx]<=T_WALL1){
            tile_damage(tx,ty);
            if(b.wp==WP_ROCKET){ explode(b.x,b.y,b.owner,b.dmg,26); b.alive=false; continue; }
            if(b.pierce>0){ b.pierce--; }
            else{ b.alive=false; spark_burst(b.x,b.y,5,COL_GOLD); continue; }
        }
        // tank collision (humans 0,1 + bots 2..), honouring the match damage rules
        int nc=BOT_OWNER0+s_nbots;
        for(int id=0;id<nc;id++){
            if(!id_active(id)) continue;
            if(b.owner==id || !can_hit(b.owner,id)) continue;
            Tank &t=combatant(id);
            if(!t.alive) continue;
            float dx=b.x-t.x, dy=b.y-t.y;
            if(dx*dx+dy*dy>110) continue; // r≈10
            if(b.wp==WP_ROCKET){ explode(b.x,b.y,b.owner,b.dmg,28); b.alive=false; break; }
            damage_tank(id,b.dmg,b.owner,b.x,b.y);
            if(b.pierce>0){ b.pierce--; }          // laser drills through
            else { b.alive=false; break; }
        }
    }
}

// ========================== tank movement =====================================
// AABB free? (tank half-size=5, scaled with zoom)
static bool cell_free(float cx,float cy){
    int l=(int)(cx-5)/TILE_PX, r=(int)(cx+4)/TILE_PX;
    int u=(int)(cy-5)/TILE_PX, dn=(int)(cy+4)/TILE_PX;
    return !tile_solid(l,u)&&!tile_solid(r,u)&&!tile_solid(l,dn)&&!tile_solid(r,dn);
}
// 8-way drive from a movement intent (mvx,mvy ∈ {-1,0,1}). Per-axis collision so the
// tank slides along walls; sets the facing vector (barrel/aim) + the 4-way dir (sprite/net).
static void drive_tank(Tank &t, int mvx, int mvy, float dt){
    if(!t.alive||t.in_shop||(mvx==0&&mvy==0)) return;
    float spd=t.spd;
    if(t.pu==PU_BOOST) spd*=1.8f;
    if(tile_water((int)(t.x/TILE_PX),(int)(t.y/TILE_PX))) spd*=0.5f;
    float vx=(float)mvx, vy=(float)mvy;
    if(mvx&&mvy){ vx*=0.70711f; vy*=0.70711f; }   // normalize diagonal speed
    // facing (8-way unit vector) for the barrel + firing
    float fl=sqrtf((float)(mvx*mvx+mvy*mvy));
    t.fx=mvx/fl; t.fy=mvy/fl;
    // 4-way dir kept for sprite treads + net
    if(abs(mvx)>=abs(mvy)) t.dir=(mvx>0)?1:3; else t.dir=(mvy>0)?2:0;
    // per-axis move (slide along walls instead of sticking)
    float nx=t.x+vx*spd*dt;
    if(cell_free(nx,t.y)) t.x=nx;
    float ny=t.y+vy*spd*dt;
    if(cell_free(t.x,ny)) t.y=ny;
    if(t.x<10) t.x=10;
    if(t.x>WORLD_W-11) t.x=WORLD_W-11;
    if(t.y<10) t.y=10;
    if(t.y>WORLD_H-11) t.y=WORLD_H-11;
    t.tread++;
}
static void tank_drive(int who, int mvx, int mvy, float dt){ drive_tank(s_tanks[who], mvx, mvy, dt); }
// cardinal wrapper (used by the AI)
static void tank_move(int who, int dir, float dt){
    static const int MX[4]={0,1,0,-1}, MY[4]={-1,0,1,0};
    tank_drive(who, MX[dir], MY[dir], dt);
}

// ========================== shop zones ========================================
// Deterministic catalog from (map seed, shop tile coords): host and guest generate
// the IDENTICAL list without transmitting it. Uses a LOCAL prng so the shared sim
// rng (s_rng) is never perturbed — host/guest stay in lockstep.
static void shop_gen_items(int pidx, int tx, int ty){
    (void)tx; (void)ty;
    // every shop sells the full catalogue (deterministic, host==guest) — all weapons always available
    for(int i=0;i<SHOP_N;i++){ s_shop_items[pidx][i]={i,SHOP_CAT[i].cost,false}; }
}
static void shop_try_spawn(void){
    for(int si=0;si<MAX_SHOPS;si++){
        if(s_shops[si].active) continue;
        // pick random floor 3×3
        for(int att=0;att<20;att++){
            int tx=2+(xr()%(MAP_W-6));
            int ty=2+(xr()%(MAP_H-6));
            bool ok=true;
            // 3×3 must be ALL floor, plus a 1-tile margin clear of walls so the
            // shop never sits jammed against (or visually "on") a wall.
            for(int dy=-1;dy<4&&ok;dy++) for(int dx=-1;dx<4&&ok;dx++)
                if(tile_solid(tx+dx,ty+dy)) ok=false;
            for(int dy=0;dy<3&&ok;dy++) for(int dx=0;dx<3&&ok;dx++)
                if(s_map[ty+dy][tx+dx]!=T_FLOOR) ok=false;
            // never overlap (or hug) another active shop
            for(int o=0;o<MAX_SHOPS&&ok;o++){
                if(o==si||!s_shops[o].active) continue;
                if(abs(tx-s_shops[o].tx)<5&&abs(ty-s_shops[o].ty)<5) ok=false;
            }
            // avoid spawning on tanks
            for(int p=0;p<2&&ok;p++){
                int ptx=(int)s_tanks[p].x/TILE_PX, pty=(int)s_tanks[p].y/TILE_PX;
                if(ptx>=tx-1&&ptx<tx+4&&pty>=ty-1&&pty<ty+4) ok=false;
            }
            if(!ok) continue;
            s_shops[si]={(uint8_t)tx,(uint8_t)ty,25000,true};
            break;
        }
        break; // only try one slot per tick
    }
}
static void shop_apply(Tank &t, int item_type){
    switch(item_type){
        case SI_ARMOR:  if(t.armor<3) t.armor++; break;
        case SI_SPEED:  t.spd*=1.20f; break;
        case SI_REPAIR: t.hp=t.hp_max; break;
        case SI_SHIELD: t.pu=PU_SHIELD; t.pu_ms=0; break;
        case SI_EMP:    t.pu=PU_EMP;    t.pu_ms=3000; break;
        case SI_REGEN:  t.pu=PU_REGEN;  t.pu_ms=0; break;
        case SI_WP_MG:  t.weapon=WP_MG;     break;
        case SI_WP_RAIL:t.weapon=WP_RAIL;   break;
        case SI_WP_SHOT:t.weapon=WP_SHOT;   break;
        case SI_WP_FLAK:t.weapon=WP_FLAK;   break;
        case SI_WP_ROCKET:t.weapon=WP_ROCKET; break;
        case SI_WP_LASER:t.weapon=WP_LASER; break;
        case SI_WP_SNIPER:t.weapon=WP_SNIPER; break;
        case SI_WP_MINIGUN:t.weapon=WP_MINIGUN; break;
    }
}
// ---- selling (refund 60% — balanced: upgrades stay worth keeping) -------------
enum { SELL_WP=0, SELL_ARMOR, SELL_SHIELD, SELL_EMP };
struct SellRow { int type; char label[16]; int refund; };
static int weapon_cost(int wp){
    switch(wp){ case WP_MG:return 25; case WP_RAIL:return 35; case WP_SHOT:return 30;
                case WP_FLAK:return 30; case WP_ROCKET:return 40; case WP_LASER:return 35; default:return 0; }
}
// build the tank's current sellable assets; returns count (0..4)
static int build_sell_list(int who, SellRow out[4]){
    Tank &t=s_tanks[who]; int n=0;
    if(t.weapon!=WP_CANNON){
        out[n].type=SELL_WP; out[n].refund=weapon_cost(t.weapon)*3/5;
        snprintf(out[n].label,sizeof out[n].label,"Vendi %s",wp_short(t.weapon)); n++;
    }
    if(t.armor>0){ out[n].type=SELL_ARMOR; out[n].refund=12; snprintf(out[n].label,16,"Vendi Corazza"); n++; }
    if(t.pu==PU_SHIELD){ out[n].type=SELL_SHIELD; out[n].refund=12; snprintf(out[n].label,16,"Vendi Scudo"); n++; }
    if(t.pu==PU_EMP){ out[n].type=SELL_EMP; out[n].refund=15; snprintf(out[n].label,16,"Vendi EMP"); n++; }
    return n;
}
static void shop_sell(Tank &t, int type){
    switch(type){
        case SELL_WP:    if(t.weapon!=WP_CANNON){ t.credits+=weapon_cost(t.weapon)*3/5; t.weapon=WP_CANNON; } break;
        case SELL_ARMOR: if(t.armor>0){ t.armor--; t.credits+=12; } break;
        case SELL_SHIELD:if(t.pu==PU_SHIELD){ t.pu=PU_NONE; t.credits+=12; } break;
        case SELL_EMP:   if(t.pu==PU_EMP){ t.pu=PU_NONE; t.pu_ms=0; t.credits+=15; } break;
    }
}
// ---- powerup pickups (host/CPU authoritative) -------------------------------
static const int PICK_POOL[4]={PU_SHIELD,PU_BOOST,PU_BURST,PU_EMP};
static const int PICK_WPOOL[3]={WP_MG,WP_SHOT,WP_FLAK};   // basic weapons droppable
static void pick_try_spawn(void){
    for(int i=0;i<MAX_PICKS;i++){
        if(s_picks[i].active) continue;
        for(int att=0;att<24;att++){
            int tx=2+(int)(xr()%(MAP_W-4)), ty=2+(int)(xr()%(MAP_H-4));
            if(tile_solid(tx,ty)||s_map[ty][tx]!=T_FLOOR) continue;
            bool ok=true;
            for(int s=0;s<MAX_SHOPS&&ok;s++){ if(!s_shops[s].active) continue;
                if(tx>=s_shops[s].tx&&tx<s_shops[s].tx+3&&ty>=s_shops[s].ty&&ty<s_shops[s].ty+3) ok=false; }
            for(int p=0;p<2&&ok;p++) if((int)s_tanks[p].x/TILE_PX==tx&&(int)s_tanks[p].y/TILE_PX==ty) ok=false;
            for(int o=0;o<MAX_PICKS&&ok;o++) if(o!=i&&s_picks[o].active&&s_picks[o].tx==tx&&s_picks[o].ty==ty) ok=false;
            if(!ok) continue;
            s_picks[i].tx=(uint8_t)tx; s_picks[i].ty=(uint8_t)ty;
            if(xr()%5<2){ s_picks[i].kind=PK_WEAPON; s_picks[i].type=(uint8_t)PICK_WPOOL[xr()%3]; }   // ~40% weapon
            else        { s_picks[i].kind=PK_PU;     s_picks[i].type=(uint8_t)PICK_POOL[xr()%4]; }
            s_picks[i].life_ms=22000; s_picks[i].active=true;
            break;
        }
        break; // one per tick
    }
}
static void pick_check(void){
    for(int i=0;i<MAX_PICKS;i++){
        if(!s_picks[i].active) continue;
        float cx=(s_picks[i].tx+0.5f)*TILE_PX, cy=(s_picks[i].ty+0.5f)*TILE_PX;
        for(int p=0;p<2;p++){
            Tank &t=s_tanks[p];
            if(!t.alive||t.in_shop) continue;
            float dx=t.x-cx, dy=t.y-cy;
            if(dx*dx+dy*dy>110) continue;
            if(s_picks[i].kind==PK_WEAPON){           // grab a weapon → equip immediately
                t.weapon=s_picks[i].type; t.fire_cd=0;
                s_picks[i].active=false;
                if(p==s_local) sfx_play(SFX_PU);
                spark_burst(cx,cy,12,COL_GOLD); ring_add(cx,cy,COL_GOLD,260);
                break;
            }
            if(t.pu==PU_NONE){                        // powerup: only grab if the slot is free
                t.pu=s_picks[i].type; t.pu_ms=0;
                s_picks[i].active=false;
                if(p==s_local) sfx_play(SFX_PU);
                spark_burst(cx,cy,12,pu_color(t.pu));
                ring_add(cx,cy,pu_color(t.pu),260);
            }
            break;
        }
    }
}
static int tank_shop_index(int who){
    Tank &t=s_tanks[who];
    int tx=(int)t.x/TILE_PX, ty=(int)t.y/TILE_PX;
    for(int si=0;si<MAX_SHOPS;si++){
        ShopZone &z=s_shops[si];
        if(!z.active) continue;
        if(tx>=z.tx&&tx<z.tx+3&&ty>=z.ty&&ty<z.ty+3) return si;
    }
    return -1;
}
// Mark `who` as shopping; build its catalog. openUI only for the local human.
static void shop_enter(int who, bool openUI){
    Tank &t=s_tanks[who];
    if(!t.alive||t.in_shop) return;
    int si=tank_shop_index(who);
    if(si<0) return;
    t.in_shop=true; t.shop_ms=20000;
    s_shop_zone[who]=(int8_t)si;
    shop_gen_items(who,s_shops[si].tx,s_shops[si].ty);
    if(who==1&&s_mode==GM_CPU) s_ai_shop_cd=500;  // AI starts buying soon
    if(openUI&&s_state==GS_PLAY){
        s_shop_sel=0; s_shop_scroll=0; s_shop_tab=0;
        sfx_play(4);
        s_state=GS_SHOP;
    }
}
// leave shop: free the tank, block re-entry into the same zone until it exits
static void shop_leave(int who){
    s_tanks[who].in_shop=false; s_tanks[who].shop_ms=0;
    if(s_shop_zone[who]>=0) s_shop_done[who]=s_shop_zone[who];
    s_shop_zone[who]=-1;
}
// host/CPU sim: detect both tanks; only local opens UI
static void shop_check_enter(int who){
    if(!s_tanks[who].alive||s_tanks[who].in_shop) return;
    int si=tank_shop_index(who);
    if(si<0){ s_shop_done[who]=-1; return; }   // exited all zones → clear block
    if(si==s_shop_done[who]) return;            // already shopped this zone
    shop_enter(who, who==s_local);
}

// ========================== powerup use ======================================
static void pu_use(int who){
    Tank &t=s_tanks[who];
    if(t.pu!=PU_NONE) sfx_play(5);
    switch(t.pu){
        case PU_BOOST: t.pu_ms=4000; break;
        case PU_BURST: t.pu_ms=3000; break;
        case PU_REGEN: t.pu_ms=5000; break;  // heal for 5 seconds
        case PU_EMP: {
            int enemy=1-who;
            Tank &e=s_tanks[enemy];
            if(e.alive){ e.fire_cd=2500; e.spd*=0.5f; e.pu_ms+=2500; }
            t.pu=PU_NONE; t.pu_ms=0;
            spark_burst(e.x,e.y,14,COL_GOLD);
            shake_set(5.0f);
            break;
        }
        default: break;
    }
}
// handle passive powerup effects (REGEN heals over time)
static void powerup_step(int who, int dt){
    Tank &t=s_tanks[who];
    if(t.pu==PU_REGEN && t.pu_ms>0 && t.hp<t.hp_max){
        static int regen_cd[2]={0,0};
        regen_cd[who]-=dt;
        if(regen_cd[who]<=0){ if(t.hp<t.hp_max) t.hp++; regen_cd[who]=500; }
    }
}

// ========================== network protocol =================================
#define TD_M0 'T'
#define TD_M1 'D'
#define TD_VER 1
enum { TD_HELLO=1, TD_JOIN, TD_ACCEPT, TD_STATE, TD_INPUT, TD_BYE, TD_BUY };
enum { HS_IDLE=0, HS_HOSTING };

typedef struct __attribute__((packed)){
    uint8_t m0,m1,ver,type; uint8_t status; char name[22];
} td_hello_t;
typedef struct __attribute__((packed)){
    uint8_t m0,m1,ver,type; char name[22]; uint8_t ttype;
} td_join_t;
typedef struct __attribute__((packed)){
    uint8_t m0,m1,ver,type; uint32_t session; uint32_t seed;
    uint8_t t1type, t2type; uint8_t mtype, nbot;
} td_accept_t;
typedef struct __attribute__((packed)){
    uint8_t m0,m1,ver,type; uint32_t seq;
    float p1x,p1y,p2x,p2y;
    int8_t p1dir,p2dir; int8_t p1hp,p2hp;
    int16_t p1cr,p2cr;
    uint8_t p1wp,p2wp,p1pu,p2pu;
    uint8_t nbul;
    struct{ int16_t x,y; int8_t vx,vy; uint8_t ow; uint8_t wp; } bul[12];
    uint8_t nshop;
    struct{ uint8_t tx,ty; } shop[2];
    uint8_t npick;
    struct{ uint8_t tx,ty,type; } pick[3];
    uint8_t mtype, nbot;
    struct{ int16_t x,y; int8_t dir; int8_t hp; int8_t hpx; uint8_t alive; uint8_t type; } bot[4];
    uint8_t inshop;     // bit0=p1 in_shop, bit1=p2 in_shop
    uint16_t gsold;     // guest(p2) sold-item bitmask (up to 16 items), authoritative
    uint8_t p1pums,p2pums; // powerup ms /64 (for shield/boost ring fx)
    uint8_t phase; uint8_t winner;
} td_state_t;
typedef struct __attribute__((packed)){
    uint8_t m0,m1,ver,type; uint32_t seq;
    uint8_t dirs;   // bit4=moving, bit5=fire, bit6=pu_use
    int8_t  mvx,mvy; // 8-way movement intent (-1,0,1)
} td_input_t;
typedef struct __attribute__((packed)){
    uint8_t m0,m1,ver,type; uint32_t seq; uint8_t item; // guest buy request
} td_buy_t;
static_assert(sizeof(td_state_t)<=PNET_MAXMSG, "td_state_t exceeds ESP-NOW payload budget");

static void send_hello(int status){
    td_hello_t h={TD_M0,TD_M1,TD_VER,TD_HELLO,(uint8_t)status,{}};
    snprintf(h.name,sizeof h.name,"%s",pnet_name());
    pnet_send(NULL,&h,sizeof h);
}
static void send_bye(void){ if(s_haspeer){ uint8_t b[4]={TD_M0,TD_M1,TD_VER,TD_BYE}; pnet_send(s_peer,b,4); } }
static void send_join(void){
    td_join_t j={TD_M0,TD_M1,TD_VER,TD_JOIN,{},(uint8_t)s_tanks[0].type};
    snprintf(j.name,sizeof j.name,"%s",pnet_name());
    pnet_send(s_peer,&j,sizeof j);
}
static void send_accept(void){
    td_accept_t a={TD_M0,TD_M1,TD_VER,TD_ACCEPT,s_session,s_map_seed,
                   (uint8_t)s_tanks[0].type,(uint8_t)s_tanks[1].type,
                   (uint8_t)s_match_type,(uint8_t)s_nbots};
    pnet_send(s_peer,&a,sizeof a);
}

static void send_state(void){
    td_state_t s={}; s.m0=TD_M0; s.m1=TD_M1; s.ver=TD_VER; s.type=TD_STATE;
    s.seq=++s_txseq;
    s.p1x=s_tanks[0].x; s.p1y=s_tanks[0].y;
    s.p2x=s_tanks[1].x; s.p2y=s_tanks[1].y;
    s.p1dir=(int8_t)s_tanks[0].dir; s.p2dir=(int8_t)s_tanks[1].dir;
    s.p1hp=(int8_t)s_tanks[0].hp;   s.p2hp=(int8_t)s_tanks[1].hp;
    s.p1cr=(int16_t)s_tanks[0].credits; s.p2cr=(int16_t)s_tanks[1].credits;
    s.p1wp=(uint8_t)s_tanks[0].weapon;  s.p2wp=(uint8_t)s_tanks[1].weapon;
    s.p1pu=(uint8_t)s_tanks[0].pu;      s.p2pu=(uint8_t)s_tanks[1].pu;
    int nb=0;
    for(int i=0;i<MAX_BULLETS&&nb<12;i++){
        if(!s_bullets[i].alive) continue;
        s.bul[nb]={(int16_t)s_bullets[i].x,(int16_t)s_bullets[i].y,
                   (int8_t)(s_bullets[i].vx*100),(int8_t)(s_bullets[i].vy*100),
                   (uint8_t)s_bullets[i].owner,(uint8_t)s_bullets[i].wp};
        nb++;
    }
    s.nbul=(uint8_t)nb;
    int nsh=0;
    for(int i=0;i<MAX_SHOPS&&nsh<2;i++){
        if(!s_shops[i].active) continue;
        s.shop[nsh++]={(uint8_t)s_shops[i].tx,(uint8_t)s_shops[i].ty};
    }
    s.nshop=(uint8_t)nsh;
    int npk=0;
    for(int i=0;i<MAX_PICKS&&npk<3;i++){
        if(!s_picks[i].active) continue;
        s.pick[npk++]={s_picks[i].tx,s_picks[i].ty,(uint8_t)(s_picks[i].kind?(0x80|s_picks[i].type):s_picks[i].type)};
    }
    s.npick=(uint8_t)npk;
    s.mtype=(uint8_t)s_match_type;
    s.nbot=(uint8_t)s_nbots;
    for(int i=0;i<s_nbots&&i<4;i++){
        s.bot[i].x=(int16_t)s_bots[i].x; s.bot[i].y=(int16_t)s_bots[i].y;
        s.bot[i].dir=(int8_t)s_bots[i].dir; s.bot[i].hp=(int8_t)s_bots[i].hp;
        s.bot[i].hpx=(int8_t)s_bots[i].hp_max;
        s.bot[i].alive=s_bots[i].alive?1:0; s.bot[i].type=(uint8_t)s_bots[i].type;
    }
    s.inshop=(uint8_t)((s_tanks[0].in_shop?1:0)|(s_tanks[1].in_shop?2:0));
    // guest(p2) authoritative sold mask (16-bit: catalogue has >8 items)
    uint16_t gm=0; for(int i=0;i<SHOP_N;i++) if(s_shop_items[1][i].sold) gm|=(1u<<i);
    s.gsold=gm;
    s.p1pums=(uint8_t)(s_tanks[0].pu_ms>16320?255:s_tanks[0].pu_ms/64);
    s.p2pums=(uint8_t)(s_tanks[1].pu_ms>16320?255:s_tanks[1].pu_ms/64);
    // phase 0 = match OVER (winner decided). Tie it to the game state, NOT the match timer:
    // a kill ends the game with time still on the clock, and the guest keys its result off phase.
    s.phase=(s_state==GS_OVER||s_match_ms<=0)?0:1;
    s.winner=(uint8_t)(s_winner+1);
    pnet_send(s_peer,&s,sizeof s);
    s_last_tx=s_now;
}
static void send_buy(int item){
    td_buy_t b={TD_M0,TD_M1,TD_VER,TD_BUY,++s_txseq,(uint8_t)item};
    pnet_send(s_peer,&b,sizeof b);
}
static void send_input(bool fire, bool pu_use, int mvx, int mvy){
    td_input_t in={}; in.m0=TD_M0; in.m1=TD_M1; in.ver=TD_VER; in.type=TD_INPUT;
    in.seq=++s_txseq;
    bool moving=(mvx||mvy);
    in.dirs=(uint8_t)((moving?0x10:0)|(fire?0x20:0)|(pu_use?0x40:0));
    in.mvx=(int8_t)mvx; in.mvy=(int8_t)mvy;
    pnet_send(s_peer,&in,sizeof in);
    s_last_tx=s_now;
}

static void host_add(const uint8_t*mac,const char*name){
    for(int i=0;i<s_nhost;i++)
        if(!memcmp(s_hosts[i].mac,mac,6)){ s_hosts[i].seen=s_now; return; }
    if(s_nhost>=NHOST) return;
    memcpy(s_hosts[s_nhost].mac,mac,6);
    snprintf(s_hosts[s_nhost].name,22,"%s",(name&&name[0])?name:"?");
    s_hosts[s_nhost].seen=s_now; s_nhost++;
}
static void hosts_prune(void){
    for(int i=0;i<s_nhost;){
        if(s_now-s_hosts[i].seen>5000){ for(int k=i;k<s_nhost-1;k++) s_hosts[k]=s_hosts[k+1]; s_nhost--; }
        else i++;
    }
    if(s_bsel>=s_nhost) s_bsel=s_nhost>0?s_nhost-1:0;
}

static void go(int state);
static void setup_bots(void);   // defined in the BOTS section below

static void net_handle(const pnet_pkt_t *p){
    if(p->len<4||p->buf[0]!=TD_M0||p->buf[1]!=TD_M1||p->buf[2]!=TD_VER) return;
    int type=p->buf[3];
    if(s_state==GS_BROWSE){
        if(type==TD_HELLO&&p->len>=(int)sizeof(td_hello_t)){
            const td_hello_t*h=(const td_hello_t*)p->buf;
            if(h->status==HS_HOSTING) host_add(p->mac,h->name);
        } else if(type==TD_ACCEPT&&s_join_pending&&p->len>=(int)sizeof(td_accept_t)){
            const td_accept_t*a=(const td_accept_t*)p->buf;
            memcpy(s_peer,p->mac,6); s_haspeer=true; s_session=a->session;
            s_map_seed=a->seed; s_join_pending=false;
            map_gen(s_map_seed);
            s_local=1; s_mode=GM_GUEST;
            tank_init(s_tanks[0],(int)a->t1type,true);
            tank_init(s_tanks[1],(int)a->t2type,false);
            s_nplayers=2; s_match_type=a->mtype; s_nbots=a->nbot>MAX_BOTS?MAX_BOTS:a->nbot;
            memset(s_bots,0,sizeof s_bots);        // render-only; host streams positions
            memset(s_bullets,0,sizeof s_bullets);
            memset(s_shops,0,sizeof s_shops);
            memset(s_sparks,0,sizeof s_sparks);
            memset(s_rings,0,sizeof s_rings);
            memset(s_picks,0,sizeof s_picks);
            s_shop_zone[0]=s_shop_zone[1]=-1; s_shop_done[0]=s_shop_done[1]=-1;
            s_flash=0; s_shake=0;
            s_match_ms=180000; s_winner=-1;
            s_rxseq=0; s_txseq=0; s_last_rx=s_last_tx=s_now;
            cam_update();
            go(GS_PLAY);
        }
        return;
    }
    if(s_state==GS_HOST){
        if(type==TD_JOIN&&p->len>=(int)sizeof(td_join_t)){
            const td_join_t*j=(const td_join_t*)p->buf;
            memcpy(s_peer,p->mac,6); s_haspeer=true;
            s_session=(esp_random()|1);
            s_mode=GM_HOST; s_local=0;
            s_map_seed=esp_random(); if(!s_map_seed) s_map_seed=0xDEADBEEFu;
            map_gen(s_map_seed);
            int htype=s_tanks[0].type;             // host's pick (set at GS_SELECT)
            tank_init(s_tanks[0],htype,true);
            tank_init(s_tanks[1],(int)j->ttype,false);
            s_nplayers=2; setup_bots();            // s_match_type chosen on the host screen
            memset(s_bullets,0,sizeof s_bullets);
            memset(s_shops,0,sizeof s_shops);
            memset(s_sparks,0,sizeof s_sparks);
            memset(s_rings,0,sizeof s_rings);
            memset(s_picks,0,sizeof s_picks);
            s_shop_zone[0]=s_shop_zone[1]=-1; s_shop_done[0]=s_shop_done[1]=-1;
            s_flash=0; s_shake=0;
            s_match_ms=180000; s_winner=-1;
            s_shop_spawn_cd=8000; s_passive_cd=10000; s_pick_spawn_cd=6000; s_peerleft=false;
            s_rxseq=0; s_txseq=0;
            send_accept();
            s_last_rx=s_last_tx=s_now;
            cam_update();
            go(GS_PLAY);
        }
        return;
    }
    if((s_state==GS_PLAY||s_state==GS_SHOP||s_state==GS_PAUSE||s_state==GS_OVER)&&s_haspeer){
        if(!memcmp(p->mac,s_peer,6)&&type==TD_BYE){ s_peerleft=true; return; }
        // guest missed our ACCEPT and is still retrying JOIN → re-ACCEPT (don't let it time out)
        if(s_mode==GM_HOST&&type==TD_JOIN&&!memcmp(p->mac,s_peer,6)){ send_accept(); s_last_rx=s_now; return; }
        if(s_mode==GM_HOST&&type==TD_INPUT&&p->len>=(int)sizeof(td_input_t)){
            const td_input_t*in=(const td_input_t*)p->buf;
            if(in->seq<s_rxseq) return;
            s_rxseq=in->seq;
            float idt=(float)(s_now-s_last_rx);   // elapsed since last guest packet (BEFORE updating it!)
            if(idt<0) idt=0;
            if(idt>60) idt=60;
            s_last_rx=s_now;
            if(s_state==GS_OVER) return;          // game's done — ignore phantom input
            int g=1;
            bool moving=(in->dirs&0x10)!=0;
            bool fire=(in->dirs&0x20)!=0;
            bool pu=(in->dirs&0x40)!=0;
            if(moving&&!s_tanks[g].in_shop) tank_drive(g,(int)in->mvx,(int)in->mvy,idt);
            if(fire) tank_fire(g);
            if(pu) pu_use(g);
        } else if(s_mode==GM_HOST&&type==TD_BUY&&p->len>=(int)sizeof(td_buy_t)){
            // guest purchase request — host is authoritative
            const td_buy_t*bq=(const td_buy_t*)p->buf;
            int idx=bq->item;
            Tank &gt=s_tanks[1];
            if(idx==0xFF){ if(gt.in_shop) shop_leave(1); s_last_rx=s_now; return; }
            if(gt.alive&&gt.in_shop&&(idx&0x80)){          // sell request
                shop_sell(gt,idx&0x7F);
                spark_burst(gt.x,gt.y,6,COL_GOLD);
            } else if(gt.alive&&gt.in_shop&&idx>=0&&idx<SHOP_N){  // buy request
                ShopItem &it=s_shop_items[1][idx];
                if(!it.sold&&gt.credits>=it.cost){
                    gt.credits-=it.cost; shop_apply(gt,it.type); it.sold=true;
                    spark_burst(gt.x,gt.y,6,COL_GOLD);
                }
            }
            s_last_rx=s_now;
        } else if(s_mode==GM_GUEST&&type==TD_STATE&&p->len>=(int)sizeof(td_state_t)){
            const td_state_t*st=(const td_state_t*)p->buf;
            if(st->seq<s_rxseq) return;
            s_rxseq=st->seq;
            s_tanks[0].x=st->p1x; s_tanks[0].y=st->p1y;
            s_tanks[1].x=st->p2x; s_tanks[1].y=st->p2y;
            s_tanks[0].dir=st->p1dir; s_tanks[1].dir=st->p2dir;
            // remote (host) tank facing from its 4-way dir; own tank keeps local prediction
            { static const float FX[4]={0,1,0,-1}, FY[4]={-1,0,1,0};
              int d0=st->p1dir&3; s_tanks[0].fx=FX[d0]; s_tanks[0].fy=FY[d0]; }
            if(st->p1hp<s_tanks[0].hp) s_tanks[0].hurt_ms=160;
            if(st->p2hp<s_tanks[1].hp) s_tanks[1].hurt_ms=160;
            s_tanks[0].hp=st->p1hp;  s_tanks[1].hp=st->p2hp;
            s_tanks[0].credits=st->p1cr; s_tanks[1].credits=st->p2cr;
            s_tanks[0].weapon=st->p1wp;  s_tanks[1].weapon=st->p2wp;
            s_tanks[0].pu=st->p1pu;      s_tanks[1].pu=st->p2pu;
            s_tanks[0].pu_ms=st->p1pums*64; s_tanks[1].pu_ms=st->p2pums*64;
            memset(s_bullets,0,sizeof s_bullets);
            for(int i=0;i<(int)st->nbul&&i<MAX_BULLETS;i++){
                s_bullets[i].x=(float)st->bul[i].x; s_bullets[i].y=(float)st->bul[i].y;
                s_bullets[i].vx=st->bul[i].vx/100.0f; s_bullets[i].vy=st->bul[i].vy/100.0f;
                s_bullets[i].owner=st->bul[i].ow; s_bullets[i].wp=st->bul[i].wp;
                s_bullets[i].alive=true;
            }
            memset(s_shops,0,sizeof s_shops);
            for(int i=0;i<(int)st->nshop;i++){ s_shops[i].active=true; s_shops[i].tx=st->shop[i].tx; s_shops[i].ty=st->shop[i].ty; s_shops[i].life_ms=10000; }
            memset(s_picks,0,sizeof s_picks);
            for(int i=0;i<(int)st->npick&&i<MAX_PICKS;i++){ s_picks[i].active=true; s_picks[i].tx=st->pick[i].tx; s_picks[i].ty=st->pick[i].ty; s_picks[i].kind=(st->pick[i].type&0x80)?PK_WEAPON:PK_PU; s_picks[i].type=st->pick[i].type&0x7F; s_picks[i].life_ms=10000; }
            // bots (render only on guest; host is authoritative)
            s_match_type=st->mtype; s_nbots=st->nbot>MAX_BOTS?MAX_BOTS:st->nbot;
            { static const float FX[4]={0,1,0,-1}, FY[4]={-1,0,1,0};
              for(int i=0;i<s_nbots;i++){
                Tank &b=s_bots[i];
                b.x=st->bot[i].x; b.y=st->bot[i].y; b.dir=st->bot[i].dir&3;
                if(st->bot[i].hp<b.hp) b.hurt_ms=160;
                b.hp=st->bot[i].hp; b.hp_max=st->bot[i].hpx>0?st->bot[i].hpx:4;
                b.alive=st->bot[i].alive!=0; b.type=st->bot[i].type;
                b.fx=FX[b.dir]; b.fy=FY[b.dir]; b.in_shop=false;
              } }
            // shop sync (host authoritative)
            bool wasShop=s_tanks[1].in_shop;
            s_tanks[0].in_shop=(st->inshop&1)!=0;
            s_tanks[1].in_shop=(st->inshop&2)!=0;
            if(s_tanks[1].in_shop&&!wasShop){
                int si=tank_shop_index(1);
                if(si>=0){ s_shop_zone[1]=(int8_t)si; shop_gen_items(1,s_shops[si].tx,s_shops[si].ty); }
                s_shop_sel=0; s_shop_scroll=0; s_shop_tab=0; s_tanks[1].shop_ms=20000;
                if(s_state==GS_PLAY){ sfx_play(4); go(GS_SHOP); }
            }
            if(!s_tanks[1].in_shop&&wasShop&&s_state==GS_SHOP) go(GS_PLAY);
            for(int i=0;i<SHOP_N;i++) s_shop_items[1][i].sold=((st->gsold>>i)&1)!=0;
            if(st->phase==0){ s_winner=(int)st->winner-1;
                if(s_state!=GS_OVER){ s_over_why="guest:phase0"; go(GS_OVER); } }   // update result once
            s_last_rx=s_now;
        }
    }
}

// ========================== AI (vs CPU) ======================================
static int64_t s_ai_fire_last;
static int      s_ai_strafe=1;       // current strafe sign (flips on block/timer)
static int      s_ai_strafe_cd;      // ms until strafe flip
static const int AI_DVX[4]={0,1,0,-1};
static const int AI_DVY[4]={-1,0,1,0};
// would tank `who` collide if it nudged ~6px in `dir`? (mirrors tank_move AABB)
static bool ai_dir_blocked(int who,int dir){
    Tank &t=s_tanks[who];
    float nx=t.x+AI_DVX[dir]*6, ny=t.y+AI_DVY[dir]*6;
    int l=(int)(nx-5)/TILE_PX, r=(int)(nx+4)/TILE_PX;
    int u=(int)(ny-5)/TILE_PX, dn=(int)(ny+4)/TILE_PX;
    return tile_solid(l,u)||tile_solid(r,u)||tile_solid(l,dn)||tile_solid(r,dn);
}
// clear line of fire between two world points? (tile raycast, half-tile steps)
static bool ai_los(float ax,float ay,float bx,float by){
    float dx=bx-ax, dy=by-ay; float dist=sqrtf(dx*dx+dy*dy);
    int steps=(int)(dist/(TILE_PX*0.5f)); if(steps<1) steps=1;
    for(int i=1;i<steps;i++){
        float fx=ax+dx*i/steps, fy=ay+dy*i/steps;
        if(tile_solid((int)fx/TILE_PX,(int)fy/TILE_PX)) return false;
    }
    return true;
}
static int ai_opp(int dir){ return (dir+2)&3; }
// move toward (tgx,tgy) with wall avoidance; returns the dir actually taken
static int ai_seek(float tgx,float tgy,int dt){
    Tank &ai=s_tanks[1];
    float dx=tgx-ai.x, dy=tgy-ai.y;
    int wantH=(dx>0)?1:3, wantV=(dy>0)?2:0;
    int order[4];
    if(fabsf(dx)>fabsf(dy)){ order[0]=wantH; order[1]=wantV; order[2]=ai_opp(wantV); order[3]=ai_opp(wantH); }
    else                   { order[0]=wantV; order[1]=wantH; order[2]=ai_opp(wantH); order[3]=ai_opp(wantV); }
    int chosen=order[0];
    for(int i=0;i<4;i++){ if(!ai_dir_blocked(1,order[i])){ chosen=order[i]; break; } }
    tank_move(1,chosen,(float)dt);
    return chosen;
}
static void ai_step(int dt){
    if(s_mode!=GM_CPU||s_match_type!=MT_DUEL) return;   // the CPU opponent exists only in 1v1 vs CPU
    Tank &ai=s_tanks[1];
    Tank &pl=s_tanks[0];
    if(!ai.alive||!pl.alive||ai.in_shop) return;
    float dx=pl.x-ai.x, dy=pl.y-ai.y;
    float adist=fabsf(dx)+fabsf(dy);

    // ---- threat dodge: is a player bullet bearing down on us?
    int dodgeDir=-1;
    for(int i=0;i<MAX_BULLETS;i++){
        Bullet &b=s_bullets[i];
        if(!b.alive||b.owner!=0) continue;
        float bdx=ai.x-b.x, bdy=ai.y-b.y;
        if(bdx*bdx+bdy*bdy>120*120) continue;          // only near bullets
        if(b.vx*bdx+b.vy*bdy<=0) continue;             // moving away
        // bullet roughly on our row/col → sidestep perpendicular
        if(fabsf(b.vx)>fabsf(b.vy)){ dodgeDir=(ai.y<b.y)?0:2; }   // horizontal shot → dodge vert
        else                       { dodgeDir=(ai.x<b.x)?3:1; }   // vertical shot → dodge horiz
        break;
    }

    // ---- pick target: rush a shop when flush with credits, else hunt the player
    float tgx=pl.x, tgy=pl.y; bool toShop=false;
    if(ai.credits>=25){
        float bd=1e9f; int best=-1;
        for(int si=0;si<MAX_SHOPS;si++){
            if(!s_shops[si].active) continue;
            float cx=(s_shops[si].tx+1.5f)*TILE_PX, cy=(s_shops[si].ty+1.5f)*TILE_PX;
            float dd=(cx-ai.x)*(cx-ai.x)+(cy-ai.y)*(cy-ai.y);
            if(dd<bd){ bd=dd; best=si; tgx=cx; tgy=cy; }
        }
        if(best>=0) toShop=true;
    }

    // ---- aim & fire: only with a clear shot on a shared row/column
    bool canFire=false; int fireDir=ai.dir;
    if(!toShop){
        if(fabsf(dx)<11 && ai_los(ai.x,ai.y,pl.x,pl.y)){ fireDir=(dy>0)?2:0; canFire=true; }
        else if(fabsf(dy)<11 && ai_los(ai.x,ai.y,pl.x,pl.y)){ fireDir=(dx>0)?1:3; canFire=true; }
    }

    // ---- movement
    s_ai_strafe_cd-=dt;
    if(s_ai_strafe_cd<=0){ s_ai_strafe=-s_ai_strafe; s_ai_strafe_cd=700+(int)(adist); }
    if(dodgeDir>=0 && !ai_dir_blocked(1,dodgeDir)){
        tank_move(1,dodgeDir,(float)dt);              // evade incoming fire
        ai.dir = canFire?fireDir:dodgeDir;
    } else if(canFire){
        // in the kill lane: strafe to stay slippery, but keep the barrel on target
        int strafeDir = (fireDir==0||fireDir==2) ? (s_ai_strafe>0?1:3) : (s_ai_strafe>0?2:0);
        if(adist>70 && !ai_dir_blocked(1,fireDir)) tank_move(1,fireDir,(float)dt); // close distance
        else if(!ai_dir_blocked(1,strafeDir))      tank_move(1,strafeDir,(float)dt);
        ai.dir=fireDir;
    } else {
        ai_seek(tgx,tgy,dt);                          // approach target with avoidance
    }

    // ---- aim: lock the facing vector onto the firing lane (movement above may have
    // set facing to a strafe/dodge dir; firing uses fx/fy, so override it here)
    if(canFire){
        static const float FX[4]={0,1,0,-1}, FY[4]={-1,0,1,0};
        ai.fx=FX[fireDir]; ai.fy=FY[fireDir]; ai.dir=fireDir;
    }
    // ---- fire (weapon fire_cd self-limits cadence)
    if(canFire && s_now-s_ai_fire_last>120){ tank_fire(1); s_ai_fire_last=s_now; }

    // ---- tactical powerups
    if(ai.pu==PU_BOOST && (adist>140||toShop)) pu_use(1);   // sprint to engage / shop
    else if(ai.pu==PU_EMP && adist<70)         pu_use(1);   // stun when point-blank
    else if(ai.pu==PU_BURST && canFire)        pu_use(1);   // unload a burst on target
    // SHIELD stays passive (auto-blocks next hit)
}
// AI in shop: auto-buy sensible upgrades then leave (no more 20 s freeze).
static void ai_shop_step(int dt){
    if(s_mode!=GM_CPU||s_match_type!=MT_DUEL) return;
    Tank &ai=s_tanks[1];
    if(!ai.alive||!ai.in_shop) return;
    s_ai_shop_cd-=dt;
    if(s_ai_shop_cd>0) return;
    s_ai_shop_cd=600;
    // priority: repair if hurt, else best affordable unsold item
    int pick=-1, pick_cost=1<<30;
    bool hurt=(ai.hp<=ai.hp_max/2);
    for(int i=0;i<SHOP_N;i++){
        ShopItem &it=s_shop_items[1][i];
        if(it.sold||ai.credits<it.cost) continue;
        if(hurt&&it.type==SI_REPAIR){ pick=i; break; }
        if(it.cost<pick_cost){ pick=i; pick_cost=it.cost; }
    }
    if(pick>=0){
        ShopItem &it=s_shop_items[1][pick];
        ai.credits-=it.cost; shop_apply(ai,it.type); it.sold=true;
        spark_burst(ai.x,ai.y,6,COL_GOLD);
    } else {
        // nothing left to buy — leave immediately
        shop_leave(1);
    }
}

// ========================== BOTS (respawning AI horde) =======================
static bool dir_blocked_t(Tank &t,int dir){
    float nx=t.x+AI_DVX[dir]*6, ny=t.y+AI_DVY[dir]*6;
    int l=(int)(nx-5)/TILE_PX, r=(int)(nx+4)/TILE_PX;
    int u=(int)(ny-5)/TILE_PX, dn=(int)(ny+4)/TILE_PX;
    return tile_solid(l,u)||tile_solid(r,u)||tile_solid(l,dn)||tile_solid(r,dn);
}
static int nearest_human(float x,float y){
    int best=-1; float bd=1e18f;
    for(int p=0;p<s_nplayers;p++){
        if(!s_tanks[p].alive||s_tanks[p].in_shop) continue;
        float dx=s_tanks[p].x-x, dy=s_tanks[p].y-y, dd=dx*dx+dy*dy;
        if(dd<bd){ bd=dd; best=p; }
    }
    return best;
}
static void bot_spawn(int i){
    Tank &b=s_bots[i];
    int tx=MAP_W/2, ty=MAP_H/2;
    for(int att=0;att<24;att++){
        int cx=2+(int)(xr()%(MAP_W-4)), cy=2+(int)(xr()%(MAP_H-4));
        if(tile_solid(cx,cy)||s_map[cy][cx]!=T_FLOOR) continue;
        bool ok=true;
        for(int p=0;p<s_nplayers&&ok;p++){
            float ddx=(cx+0.5f)*TILE_PX-s_tanks[p].x, ddy=(cy+0.5f)*TILE_PX-s_tanks[p].y;
            if(ddx*ddx+ddy*ddy < 90.0f*90.0f) ok=false;       // don't pop next to a human
        }
        if(!ok) continue;
        tx=cx; ty=cy; break;
    }
    int lvl=s_bot_level;
    { int r=(int)(xr()%6);                                // mostly stand-off types, few rushers
      b.behav=(r==0)?BB_RUSHER:(r==1)?BB_SNIPER:(r<4)?BB_BRAWLER:BB_FLANKER; }
    b.aim_ms=300+(int)(xr()%500);
    b.type=(int)(xr()%TT_COUNT);                          // varied chassis silhouette
    b.dir=2; b.fx=0; b.fy=1; b.alive=true; b.in_shop=false; b.shop_ms=0;
    b.pu=PU_NONE; b.pu_ms=0; b.fire_cd=0; b.credits=0;
    b.flash_ms=0; b.tread=0; b.hurt_ms=0; b.respawn_ms=0;
    b.hp=b.hp_max=2+(lvl>8?2:lvl/3);                      // tougher each tier (reduced: bots die faster)
    b.armor=(lvl>=6)?1:0;
    b.spd=0.050f+lvl*0.0026f; if(b.spd>0.092f) b.spd=0.092f;
    // higher tiers field nastier weapons; snipers prefer the railgun
    if(b.behav==BB_SNIPER) b.weapon=(lvl>=3)?WP_RAIL:WP_CANNON;
    else if(lvl>=4 && (xr()&1)) b.weapon=WP_MG;
    else if(lvl>=6 && (xr()&1)) b.weapon=WP_SHOT;
    else b.weapon=WP_CANNON;
    b.x=(tx+0.5f)*TILE_PX; b.y=(ty+0.5f)*TILE_PX;
}
static float bot_standoff(int behav){
    switch(behav){ case BB_RUSHER: return 85; case BB_SNIPER: return 165; case BB_FLANKER: return 118; default: return 100; }
}
// spread bots over the alive humans instead of dogpiling one
static int bot_target(int i){
    if(s_nplayers>=2){
        int want=i&1;
        if(s_tanks[want].alive&&!s_tanks[want].in_shop) return want;
        int other=want^1;
        if(s_tanks[other].alive&&!s_tanks[other].in_shop) return other;
    }
    return nearest_human(s_bots[i].x,s_bots[i].y);
}
static void bot_ai(int i,int dt){
    Tank &b=s_bots[i];
    int hid=bot_target(i);
    if(hid<0) return;                                  // no target (all humans dead/shopping)
    Tank &h=s_tanks[hid];
    float dxh=h.x-b.x, dyh=h.y-b.y;

    // ---- dodge a human shell bearing down on us (rushers don't bother)
    int dodgeDir=-1;
    if(b.behav!=BB_RUSHER){
        for(int k=0;k<MAX_BULLETS;k++){
            Bullet &bl=s_bullets[k];
            if(!bl.alive||bl.owner>=BOT_OWNER0) continue;     // only human bullets
            float bdx=b.x-bl.x, bdy=b.y-bl.y;
            if(bdx*bdx+bdy*bdy>85.0f*85.0f) continue;
            if(bl.vx*bdx+bl.vy*bdy<=0) continue;              // moving away
            if(fabsf(bl.vx)>fabsf(bl.vy)) dodgeDir=(b.y<bl.y)?0:2;
            else                         dodgeDir=(b.x<bl.x)?3:1;
            break;
        }
    }

    // ---- fire only on a clear shared lane (LOS), and only when the pause gate is open
    bool canFire=false; int fireDir=b.dir;
    if(fabsf(dxh)<12 && ai_los(b.x,b.y,h.x,h.y)){ fireDir=(dyh>0)?2:0; canFire=true; }
    else if(fabsf(dyh)<12 && ai_los(b.x,b.y,h.x,h.y)){ fireDir=(dxh>0)?1:3; canFire=true; }

    // ---- target a point on a RING around the human (each bot a different angle, slowly orbiting)
    //      so they surround at distance instead of all piling onto one spot.
    float pd=bot_standoff(b.behav);
    float ang=(float)i*(6.2832f/(float)(s_nbots>0?s_nbots:1)) + (float)b.tread*0.010f;
    float tgx=h.x+cosf(ang)*pd, tgy=h.y+sinf(ang)*pd;
    if(tgx<14) tgx=14;
    if(tgx>WORLD_W-14) tgx=WORLD_W-14;
    if(tgy<14) tgy=14;
    if(tgy>WORLD_H-14) tgy=WORLD_H-14;

    int mvDir=-1;
    if(dodgeDir>=0 && !dir_blocked_t(b,dodgeDir)){
        mvDir=dodgeDir;                                       // evade first
    } else {
        float sdx=tgx-b.x, sdy=tgy-b.y;
        if(sdx*sdx+sdy*sdy < 20.0f*20.0f && canFire){
            mvDir=-1;                                         // in position with a shot → hold
        } else {
            int wantH=(sdx>0)?1:3, wantV=(sdy>0)?2:0, order[4];
            if(fabsf(sdx)>fabsf(sdy)){ order[0]=wantH; order[1]=wantV; order[2]=ai_opp(wantV); order[3]=ai_opp(wantH); }
            else                     { order[0]=wantV; order[1]=wantH; order[2]=ai_opp(wantH); order[3]=ai_opp(wantV); }
            mvDir=order[0];
            for(int k=0;k<4;k++){ if(!dir_blocked_t(b,order[k])){ mvDir=order[k]; break; } }
        }
    }
    if(mvDir>=0) drive_tank(b, AI_DVX[mvDir], AI_DVY[mvDir], (float)dt);

    if(canFire){
        static const float FX[4]={0,1,0,-1}, FY[4]={-1,0,1,0};
        b.fx=FX[fireDir]; b.fy=FY[fireDir]; b.dir=fireDir;   // lock the barrel on the lane
        if(b.fire_cd<=0 && b.aim_ms<=0){                      // pause gate → intermittent, not spammy
            fire_owner(b, BOT_OWNER0+i);
            b.aim_ms=700+(int)(xr()%800);                    // 0.7–1.5s breather
        }
    }
}
static void bots_step(int dt){
    for(int i=0;i<s_nbots;i++){
        Tank &b=s_bots[i];
        if(!b.alive){ if(b.respawn_ms>0){ b.respawn_ms-=dt; if(b.respawn_ms<=0) bot_spawn(i); } continue; }
        if(b.fire_cd>0) b.fire_cd-=dt;
        if(b.hurt_ms>0) b.hurt_ms-=dt;
        if(b.flash_ms>0) b.flash_ms-=dt;
        if(b.aim_ms>0) b.aim_ms-=dt;
        bot_ai(i,dt);
    }
    // escalation: bots get tougher over time (waves feel)
    if(s_nbots>0 && s_bot_level<10){
        s_bot_level_cd-=dt;
        if(s_bot_level_cd<=0){ s_bot_level++; s_bot_level_cd=16000; }
    }
}
// push overlapping tanks apart so they collide instead of stacking
static void resolve_collisions(void){
    int nc=BOT_OWNER0+s_nbots;
    for(int a=0;a<nc;a++){
        if(!id_active(a)) continue;
        Tank &ta=combatant(a);
        if(!ta.alive||ta.in_shop) continue;
        for(int bb=a+1;bb<nc;bb++){
            if(!id_active(bb)) continue;
            Tank &tb=combatant(bb);
            if(!tb.alive||tb.in_shop) continue;
            float dx=tb.x-ta.x, dy=tb.y-ta.y, d2=dx*dx+dy*dy;
            const float MIND=18.0f;
            if(d2>=MIND*MIND) continue;
            float ds=sqrtf(d2); if(ds<0.001f){ ds=0.001f; dx=1; dy=0; }
            float ux=dx/ds, uy=dy/ds, ov=(MIND-ds)*0.5f;
            float ax=ta.x-ux*ov, ay=ta.y-uy*ov, bx=tb.x+ux*ov, by=tb.y+uy*ov;
            if(cell_free(ax,ay)){ ta.x=ax; ta.y=ay; }
            if(cell_free(bx,by)){ tb.x=bx; tb.y=by; }
        }
    }
}
// decide if the match is over by elimination; sets s_winner. (timer-up handled separately)
static bool match_over_check(void){
    int aliveH=0, lastH=-1;
    for(int p=0;p<s_nplayers;p++) if(s_tanks[p].alive){ aliveH++; lastH=p; }
    if(s_match_type==MT_COOP){
        if(aliveH==0){ s_winner=WIN_BOTS; return true; }   // all humans down → defeat
        return false;                                      // else COOP ends only on the timer
    }
    if(s_nplayers>=2 && aliveH<=1){                         // DUEL / BRAWL: last human standing
        s_winner=(aliveH==1)?lastH:-1;
        return true;
    }
    return false;
}
// host/CPU: size + spawn the bot horde for the current match type
static void setup_bots(void){
    memset(s_bots,0,sizeof s_bots);
    s_bot_level=0; s_bot_kills=0; s_bot_level_cd=16000;
    s_nbots=(s_match_type==MT_DUEL)?0:4;
    for(int i=0;i<s_nbots;i++) bot_spawn(i);
}

// ========================== draw: map =========================================
static uint16_t tile_col(uint8_t t){
    switch(t){
        case T_FLOOR:  return rgb(32,34,40);
        case T_WALL3:  return rgb(140,128,100);
        case T_WALL2:  return rgb(100,90,70);
        case T_WALL1:  return rgb(68,58,44);
        case T_BUNKER: return rgb(60,70,80);
        case T_WATER:  return rgb(28,55,100);
        default:       return rgb(32,34,40);
    }
}
static uint16_t tile_hi(uint8_t t){
    switch(t){
        case T_WALL3:  return rgb(200,185,148);
        case T_WALL2:  return rgb(144,128,100);
        case T_WALL1:  return rgb(100,88,68);
        case T_BUNKER: return rgb(90,105,120);
        case T_WATER:  return rgb(40,80,150);
        default: return tile_col(t);
    }
}

static void draw_map(void){
    int tx0=s_cam_x/TILE_PX, ty0=s_cam_y/TILE_PX;
    int tx1=(s_cam_x+W)/TILE_PX+1;
    int ty1=(s_cam_y+PLAY_H)/TILE_PX+1;
    if(tx0>0) tx0--;
    if(ty0>0) ty0--;                            // overscan for shake
    if(tx1>MAP_W) tx1=MAP_W;
    if(ty1>MAP_H) ty1=MAP_H;
    int wphase=(s_anim>>2);                      // water animation phase
    for(int ty=ty0;ty<ty1;ty++) for(int tx=tx0;tx<tx1;tx++){
        int sx=tx2s(tx), sy=ty2s(ty);
        uint8_t t=s_map[ty][tx];
        if(t==T_FLOOR){
            // checkerboard concrete: two tones + faint inset panel + corner rivets
            bool chk=((tx^ty)&1);
            uint16_t bc=chk?rgb(30,33,40):rgb(35,39,47);
            d.fillRect(sx,sy,TILE_PX,TILE_PX,bc);
            d.drawFastHLine(sx,sy,TILE_PX,mix(bc,COL_WHITE,16));        // top sheen
            d.drawFastHLine(sx,sy+TILE_PX-1,TILE_PX,mix(bc,COL_BLACK,70)); // bottom seam
            d.drawFastVLine(sx+TILE_PX-1,sy,TILE_PX,mix(bc,COL_BLACK,70)); // right seam
            // rivet dots on the darker tiles for texture
            if(chk){ uint16_t rv=mix(bc,COL_WHITE,40); d.drawPixel(sx+3,sy+3,rv); d.drawPixel(sx+8,sy+8,rv); }
        } else if(t==T_WATER){
            // animated two-tone dithered water + scrolling ripples + glint
            uint16_t deep=rgb(20,46,92), shal=rgb(34,72,134);
            d.fillRect(sx,sy,TILE_PX,TILE_PX,deep);
            for(int yy=0;yy<TILE_PX;yy+=2){
                int off=((wphase+yy+tx)&3);
                uint16_t c=(off<2)?shal:deep;
                d.drawFastHLine(sx+(off&1),sy+yy,TILE_PX-(off&1),c);
            }
            uint16_t rip=((wphase+tx+ty)&3)?rgb(60,110,180):rgb(120,170,220);
            d.drawFastHLine(sx+2,sy+((wphase+ty)&7),TILE_PX-4,rip);
        } else {
            // walls / bunker: chunky 3D block with bevel + shadow skirt
            uint16_t bc=tile_col(t);
            uint16_t hi=tile_hi(t);
            uint16_t sh=mix(bc,COL_BLACK,150);
            d.fillRect(sx,sy,TILE_PX,TILE_PX,bc);
            d.fillRect(sx+1,sy+1,TILE_PX-2,2,mix(hi,COL_WHITE,40)); // bright top lip
            d.drawFastHLine(sx,sy,TILE_PX,hi);
            d.drawFastVLine(sx,sy,TILE_PX,hi);
            d.drawFastHLine(sx,sy+TILE_PX-1,TILE_PX,sh);
            d.drawFastVLine(sx+TILE_PX-1,sy,TILE_PX,sh);
            d.drawFastHLine(sx+1,sy+TILE_PX-2,TILE_PX-2,mix(bc,COL_BLACK,90));
            if(t==T_BUNKER){
                // riveted steel: cross brace + 4 corner bolts
                uint16_t br=mix(bc,COL_WHITE,60), dk=mix(bc,COL_BLACK,90);
                d.drawLine(sx+2,sy+2,sx+TILE_PX-3,sy+TILE_PX-3,dk);
                d.drawLine(sx+TILE_PX-3,sy+2,sx+2,sy+TILE_PX-3,dk);
                d.drawPixel(sx+2,sy+2,br); d.drawPixel(sx+TILE_PX-3,sy+2,br);
                d.drawPixel(sx+2,sy+TILE_PX-3,br); d.drawPixel(sx+TILE_PX-3,sy+TILE_PX-3,br);
            } else {
                // brick courses + damage cracks scaling with weakening
                d.drawFastHLine(sx+1,sy+4,TILE_PX-2,mix(bc,COL_BLACK,60));
                d.drawFastHLine(sx+1,sy+8,TILE_PX-2,mix(bc,COL_BLACK,60));
                d.drawFastVLine(sx+6,sy+1,4,mix(bc,COL_BLACK,60));
                d.drawFastVLine(sx+3,sy+4,4,mix(bc,COL_BLACK,60));
                if(t==T_WALL2||t==T_WALL1){
                    uint16_t ck=mix(bc,COL_BLACK,180);
                    d.drawLine(sx+3,sy+1,sx+6,sy+TILE_PX-2,ck);
                }
                if(t==T_WALL1){
                    uint16_t ck=mix(bc,COL_BLACK,200);
                    d.drawLine(sx+8,sy+2,sx+2,sy+9,ck);
                    d.drawLine(sx+9,sy+TILE_PX-2,sx+5,sy+5,ck);
                }
            }
        }
    }
    // shop zones — pulsing energy pad: gradient floor, rotating chevrons, beacon $
    for(int si=0;si<MAX_SHOPS;si++){
        ShopZone &z=s_shops[si];
        if(!z.active) continue;
        int sx=tx2s(z.tx), sy=ty2s(z.ty);
        int pw=3*TILE_PX;
        int ph=(s_anim>>1)&7;
        int pulse=(s_anim>>2)&1;
        uint16_t gc=pulse?rgb(120,255,140):rgb(70,190,90);
        uint16_t gd=mix(gc,COL_BLACK,170);
        // tinted pad with scanline glow
        d.fillRect(sx,sy,pw,pw,rgb(20,48,24));
        for(int yy=2;yy<pw;yy+=4) d.drawFastHLine(sx+2,sy+yy,pw-4,rgb(28,66,34));
        // animated dashed border
        for(int e=0;e<pw;e+=4){
            uint16_t c=((e+ph)&7)<4?gc:gd;
            d.drawPixel(sx+e,sy-1,c); d.drawPixel(sx+e,sy+pw,c);
            d.drawPixel(sx-1,sy+e,c); d.drawPixel(sx+pw,sy-1+e,c);
        }
        d.drawRect(sx-1,sy-1,pw+2,pw+2,gd);
        // corner chevrons pointing inward
        d.drawLine(sx+2,sy+6,sx+6,sy+2,gc); d.drawLine(sx+pw-3,sy+6,sx+pw-7,sy+2,gc);
        d.drawLine(sx+2,sy+pw-7,sx+6,sy+pw-3,gc); d.drawLine(sx+pw-3,sy+pw-7,sx+pw-7,sy+pw-3,gc);
        // beacon $ with shadow
        txc(sx+pw/2+1,sy+pw/2-3,2,gd,"$");
        txc(sx+pw/2,sy+pw/2-4,2,pulse?COL_WHITE:gc,"$");
    }
}

// ========================== draw: tank =======================================
static void draw_tank_sprite(const Tank &t, uint16_t col, bool bot=false){
    int sx=wx2s(t.x), sy=wy2s(t.y);
    int dir=t.dir;
    if(sx<-22||sx>W+12||sy<HUD_H-22||sy>H+12) return;
    if(t.in_shop){
        // cloaked: ghosted chassis + green $ beacon (hidden from foe, shown to self)
        uint16_t g=((s_anim>>2)&1)?COL_GREEN:rgb(40,120,60);
        d.fillRoundRect(sx-9,sy-9,19,19,4,rgb(16,46,22));
        d.drawRoundRect(sx-9,sy-9,19,19,4,g);
        txc(sx+1,sy-3,2,g,"$");
        return;
    }
    static const int BDX[4]={0,1,0,-1};
    static const int BDY[4]={-1,0,1,0};
    // ---- drop shadow
    d.fillRoundRect(sx-8,sy-3,17,15,3,rgb(8,9,14));
    // ---- treads with animated cleats
    uint16_t track=rgb(38,40,48), cleat=rgb(60,64,74);
    int ph=(t.tread>>1)&3;
    if(dir==0||dir==2){
        d.fillRect(sx-9,sy-8,5,17,track); d.fillRect(sx+5,sy-8,5,17,track);
        for(int i=-8+ph;i<9;i+=4){ d.drawFastHLine(sx-9,sy+i,5,cleat); d.drawFastHLine(sx+5,sy+i,5,cleat); }
    } else {
        d.fillRect(sx-8,sy-9,17,5,track); d.fillRect(sx-8,sy+5,17,5,track);
        for(int i=-8+ph;i<9;i+=4){ d.drawFastVLine(sx+i,sy-9,5,cleat); d.drawFastVLine(sx+i,sy+5,5,cleat); }
    }
    // ---- hull (team color) with bevel + rim light
    uint16_t hurt=(t.hurt_ms>0)?COL_WHITE:col;
    uint16_t hull=mix(hurt,COL_BLACK,70);
    uint16_t hull_hi=mix(hurt,COL_WHITE,110);
    uint16_t hull_sh=mix(hurt,COL_BLACK,130);
    d.fillRoundRect(sx-7,sy-7,16,16,3,hull);
    d.drawFastHLine(sx-4,sy-7,11,hull_hi);
    d.drawFastVLine(sx-7,sy-4,11,hull_hi);
    d.drawFastHLine(sx-4,sy+8,11,hull_sh);
    d.drawFastVLine(sx+8,sy-4,11,hull_sh);
    // ---- turret (type-specific) with sheen
    uint16_t turr=mix(hurt,COL_WHITE,40);
    uint16_t turr_hi=mix(turr,COL_WHITE,90);
    switch(t.type){
        case TT_BULLDOG: d.fillRoundRect(sx-4,sy-4,11,11,2,turr); break;     // heavy box
        case TT_VIPER:   d.fillRect(sx-3,sy-5,7,12,turr); break;             // sleek
        case TT_PHANTOM: d.fillRect(sx-4,sy-3,10,8,turr);
                         d.fillRect(sx-2,sy-5,5,12,turr); break;             // cross
        case TT_CRUSHER: d.fillCircle(sx+1,sy+1,6,turr); break;             // round drum
    }
    d.drawFastHLine(sx-3,sy-4,7,turr_hi);
    d.drawFastHLine(sx-2,sy-3,5,mix(turr_hi,COL_WHITE,60));
    // ---- barrel along the 8-way facing vector (fallback to dir if zero)
    float ax=t.fx, ay=t.fy;
    if(ax==0&&ay==0){ ax=BDX[dir]; ay=BDY[dir]; }
    int pxx=(int)(-ay), pyy=(int)(ax);     // perpendicular (rounded) for barrel width
    int bx0=sx+(int)(ax*4), by0=sy+(int)(ay*4);
    int tip_x=sx+(int)(ax*15), tip_y=sy+(int)(ay*15);
    uint16_t bar=rgb(54,58,68), barhi=rgb(90,96,110);
    d.drawLine(bx0+pxx,by0+pyy,tip_x+pxx,tip_y+pyy,bar);
    d.drawLine(bx0,by0,tip_x,tip_y,barhi);
    d.drawLine(bx0-pxx,by0-pyy,tip_x-pxx,tip_y-pyy,bar);
    d.fillRect(tip_x-1,tip_y-1,4,4,rgb(110,116,130));
    // ---- muzzle flash (bigger, brighter, more dramatic)
    if(t.flash_ms>0){
        int intensity=(t.flash_ms*255)/100;  // fade out
        int fxp=sx+(int)(ax*16), fyp=sy+(int)(ay*16);
        uint16_t flash_col=mix(COL_GOLD,COL_WHITE,intensity/2);
        d.fillCircle(fxp,fyp,6,flash_col);
        d.fillCircle(fxp,fyp,3,COL_WHITE);
        d.fillCircle(fxp,fyp,1,rgb(255,255,200));
        d.drawLine(fxp-pxx*5,fyp-pyy*5,fxp+pxx*5,fyp+pyy*5,flash_col);
        d.drawLine(fxp+(int)(ax*6),fyp+(int)(ay*6),fxp,fyp,mix(rgb(255,240,180),COL_GOLD,intensity/2));
        // flash bloom outward
        d.drawCircle(fxp,fyp,8,mix(flash_col,COL_BLACK,200));
    }
    // ---- BOT markings: angular spikes + glowing red visor → unmistakably an enemy
    if(bot){
        uint16_t spike=rgb(40,8,12);
        d.drawLine(sx-7,sy-7,sx-10,sy-10,spike); d.drawLine(sx+7,sy-7,sx+10,sy-10,spike);
        d.drawLine(sx-7,sy+7,sx-10,sy+10,spike); d.drawLine(sx+7,sy+7,sx+10,sy+10,spike);
        d.fillRect(sx-4,sy-7,9,2,rgb(10,10,12));               // dark band
        uint16_t eye=((s_anim>>1)&1)?rgb(255,80,80):rgb(255,180,60);
        d.drawFastHLine(sx-3,sy-6,7,eye);                       // hostile visor
        d.drawPixel(sx-3,sy-6,COL_WHITE); d.drawPixel(sx+3,sy-6,COL_WHITE);
    }
}

static void draw_tanks(void){
    // bots first (under the human sprites)
    for(int i=0;i<s_nbots;i++){
        Tank &b=s_bots[i];
        if(!b.alive) continue;
        draw_tank_sprite(b, tank_col(BOT_OWNER0+i), true);
        // tiny health pip over the bot so you can gauge it
        int sx=wx2s(b.x), sy=wy2s(b.y)-13;
        d.fillRect(sx-5,sy,10,2,rgb(40,12,14));
        d.fillRect(sx-5,sy,(b.hp*10)/(b.hp_max>0?b.hp_max:1),2,rgb(255,90,90));
    }
    for(int p=0;p<s_nplayers;p++){
        Tank &t=s_tanks[p];
        if(!t.alive) continue;
        draw_tank_sprite(t, p==0?COL_P1:COL_P2);
        if(t.pu==PU_SHIELD&&!t.in_shop){
            int sx=wx2s(t.x), sy=wy2s(t.y);
            uint16_t sc=((s_anim>>2)&1)?rgb(150,225,255):rgb(90,170,255);
            d.drawCircle(sx,sy,13,sc);
            d.drawCircle(sx,sy,14,mix(sc,COL_BLACK,150));
            int a=(s_anim<<3)&0xFF; float an=a*0.0245f;
            d.fillCircle(sx+(int)(cosf(an)*13),sy+(int)(sinf(an)*13),2,COL_WHITE);
        }
        if(t.pu==PU_BOOST&&!t.in_shop){
            static const int BDX[4]={0,1,0,-1}, BDY[4]={-1,0,1,0};
            int sx=wx2s(t.x)-BDX[t.dir]*10, sy=wy2s(t.y)-BDY[t.dir]*10;
            d.fillCircle(sx,sy,3,((s_anim&1)?COL_GOLD:COL_RED));
        }
    }
}

// ========================== draw: powerup pickups =============================
static void draw_picks(void){
    for(int i=0;i<MAX_PICKS;i++){
        if(!s_picks[i].active) continue;
        int sx=tx2s(s_picks[i].tx)+TILE_PX/2, sy=ty2s(s_picks[i].ty)+TILE_PX/2;
        if(sx<-10||sx>W+10||sy<HUD_H-10||sy>H+10) continue;
        sy-=((s_anim>>2)&3);                       // gentle bob
        bool weap=(s_picks[i].kind==PK_WEAPON);
        uint16_t c=weap?COL_GOLD:pu_color(s_picks[i].type);
        const char* lbl=weap?wp_short(s_picks[i].type):pu_letter(s_picks[i].type);
        bool blink=((s_anim>>2)&1);
        if(s_picks[i].life_ms<4000 && (s_anim&2)) { /* blink out near expiry */ }
        else {
            if(weap){ d.fillRoundRect(sx-9,sy-6,19,12,3,mix(c,COL_BLACK,150)); d.drawRoundRect(sx-9,sy-6,19,12,3,blink?c:mix(c,COL_WHITE,90)); txc(sx,sy-3,1,COL_WHITE,lbl); }
            else    { d.fillCircle(sx,sy,7,mix(c,COL_BLACK,150)); d.drawCircle(sx,sy,8,blink?c:mix(c,COL_WHITE,90)); d.drawCircle(sx,sy,6,mix(c,COL_WHITE,40)); txc(sx,sy-3,1,COL_WHITE,lbl); }
        }
    }
}
// ========================== draw: bullets =====================================
static void draw_bullets(void){
    for(int i=0;i<MAX_BULLETS;i++){
        Bullet &b=s_bullets[i];
        if(!b.alive) continue;
        int sx=wx2s(b.x), sy=wy2s(b.y);
        if(sx<-6||sx>W+6||sy<HUD_H-6||sy>H+6) continue;
        uint16_t col=tank_col(b.owner);
        int tx=sx-(int)(b.vx*6), ty=sy-(int)(b.vy*6);   // trail anchor
        if(b.wp==WP_RAIL){
            int tx2=sx-(int)(b.vx*16), ty2=sy-(int)(b.vy*16);
            d.drawLine(tx2,ty2,sx,sy,mix(col,COL_BLACK,90));
            d.drawLine(tx,ty,sx,sy,mix(col,COL_WHITE,60));
            d.fillCircle(sx,sy,4,COL_WHITE);
            d.fillCircle(sx,sy,2,mix(col,COL_WHITE,120));
        } else if(b.wp==WP_LASER){
            // thin bright beam, long streak
            int tx2=sx-(int)(b.vx*18), ty2=sy-(int)(b.vy*18);
            uint16_t lc=rgb(120,255,210);
            d.drawLine(tx2,ty2,sx,sy,mix(lc,COL_BLACK,70));
            d.drawLine(tx,ty,sx,sy,lc);
            d.fillCircle(sx,sy,2,COL_WHITE);
        } else if(b.wp==WP_ROCKET){
            // fat warhead + fire trail
            d.fillCircle(tx,ty,2,COL_RED);
            d.fillCircle(sx-(int)(b.vx*4),sy-(int)(b.vy*4),2,COL_GOLD);
            d.fillCircle(sx,sy,3,rgb(80,84,96));
            d.fillCircle(sx,sy,2,COL_WHITE);
            d.drawPixel(sx,sy,COL_RED);
        } else if(b.wp==WP_MG){
            d.drawLine(tx,ty,sx,sy,mix(col,COL_GOLD,120));
            d.fillCircle(sx,sy,2,COL_WHITE);
            d.fillCircle(sx,sy,1,COL_GOLD);
        } else if(b.wp==WP_FLAK){
            // tiny hot pellet
            d.fillCircle(sx,sy,1,COL_GOLD);
            d.drawPixel(sx,sy,COL_WHITE);
        } else if(b.wp==WP_SNIPER){
            // bright piercing bolt, long trail
            int tx3=sx-(int)(b.vx*24), ty3=sy-(int)(b.vy*24);
            uint16_t sc=rgb(255,220,100);
            d.drawLine(tx3,ty3,sx,sy,mix(sc,COL_BLACK,80));
            d.drawLine(tx,ty,sx,sy,mix(sc,COL_WHITE,100));
            d.fillCircle(sx,sy,3,COL_WHITE);
            d.fillCircle(sx,sy,1,sc);
        } else if(b.wp==WP_MINIGUN){
            // tiny tracer
            d.drawLine(tx,ty,sx,sy,mix(col,COL_WHITE,100));
            d.drawPixel(sx,sy,COL_WHITE);
        } else {
            // cannon/shotgun shell with smoke puff trail + hot core
            d.fillCircle(tx,ty,2,mix(col,COL_BLACK,120));
            d.fillCircle(sx,sy,3,COL_WHITE);
            d.fillCircle(sx,sy,2,col);
            d.drawPixel(sx,sy,COL_GOLD);
        }
    }
}

// ========================== draw: particles ==================================
static void draw_sparks(void){
    // expanding blast rings — bigger, brighter, more dramatic
    for(int i=0;i<MAX_RINGS;i++){
        if(s_rings[i].life<=0) continue;
        int sx=wx2s(s_rings[i].x), sy=wy2s(s_rings[i].y);
        if(sx<-60||sx>W+60||sy<HUD_H-60||sy>H+60) continue;
        int age=s_rings[i].maxlife-s_rings[i].life;
        int rad=3+age/10;  // faster expansion
        int f=s_rings[i].life*256/s_rings[i].maxlife;
        // triple ring: outer fade, mid-bright, inner core
        d.drawCircle(sx,sy,rad,mix(COL_BLACK,s_rings[i].col,f/2));
        if(f>100) d.drawCircle(sx,sy,rad-1,mix(COL_BLACK,s_rings[i].col,f));
        if(f>180) d.drawCircle(sx,sy,rad-2,s_rings[i].col);
    }
    // sparks (size fades with life)
    for(int i=0;i<MAX_SPARKS;i++){
        if(s_sparks[i].life<=0) continue;
        int sx=wx2s(s_sparks[i].x), sy=wy2s(s_sparks[i].y);
        if(sx<0||sx>=W||sy<HUD_H||sy>=H) continue;
        int f=s_sparks[i].life*256/s_sparks[i].maxlife;
        uint16_t c=mix(COL_BLACK,s_sparks[i].col,f);
        if(f>160) d.fillRect(sx,sy,2,2,c); else d.drawPixel(sx,sy,c);
    }
}
// brief white screen flash on big explosions (drawn over play field)
static void draw_flash(void){
    if(s_flash<=0) return;
    int a=s_flash>120?120:s_flash;
    for(int y=HUD_H;y<H;y+=2) d.drawFastHLine((y&2)?0:1,y,W,mix(rgb(10,12,20),COL_WHITE,a));
}

// ========================== draw: HUD =========================================
// segmented HP bar (4px pips, team color, low-HP pulse). Returns end-x of the bar.
static int hud_hp(int x,int y,int who,uint16_t col,bool rightAlign){
    Tank &t=s_tanks[who];
    int seg=t.hp_max>16?16:t.hp_max;
    int per=(t.hp_max>16)?(t.hp*16/t.hp_max):t.hp;
    bool low=(t.hp*4<=t.hp_max);
    uint16_t on=low&&((s_anim>>2)&1)?COL_RED:col;
    for(int i=0;i<seg;i++){
        int bx=rightAlign? x-(i+1)*5 : x+i*5;
        uint16_t c=(i<per)?on:rgb(30,34,46);
        d.fillRect(bx,y,4,8,c);
        if(i<per){ d.drawFastHLine(bx,y,4,mix(c,COL_WHITE,110)); d.drawFastVLine(bx,y,8,mix(c,COL_WHITE,60)); }
    }
    return rightAlign? x-seg*5 : x+seg*5;
}
static void draw_hud(void){
    // panel + bright underline
    d.fillRect(0,0,W,HUD_H,rgb(15,17,25));
    d.drawFastHLine(0,0,W,rgb(30,34,48));
    d.drawFastHLine(0,HUD_H-2,W,rgb(38,46,64));
    d.drawFastHLine(0,HUD_H-1,W,rgb(64,78,104));
    // ---- minimap pinned top-right, full HUD height (fog: foe hidden while shopping)
    int mmw=24, mmh=10, mmx=W-mmw-2, mmy=2;
    d.fillRect(mmx,mmy,mmw,mmh,rgb(16,19,27));
    d.drawRect(mmx-1,mmy-1,mmw+2,mmh+2,rgb(58,70,94));
    for(int si=0;si<MAX_SHOPS;si++){
        if(!s_shops[si].active) continue;
        int gx=mmx+(int)((s_shops[si].tx+1)*mmw/MAP_W);
        int gy=mmy+(int)((s_shops[si].ty+1)*mmh/MAP_H);
        d.fillRect(gx,gy,2,2,((s_anim>>2)&1)?COL_GREEN:rgb(40,120,60));
    }
    // bots on the minimap (red), so you can read the horde around you
    for(int i=0;i<s_nbots;i++){
        if(!s_bots[i].alive) continue;
        int dx=mmx+(int)(s_bots[i].x*mmw/WORLD_W);
        int dy=mmy+(int)(s_bots[i].y*mmh/WORLD_H);
        if(dx<mmx) dx=mmx;
        if(dx>mmx+mmw-1) dx=mmx+mmw-1;
        if(dy<mmy) dy=mmy;
        if(dy>mmy+mmh-1) dy=mmy+mmh-1;
        d.drawPixel(dx,dy,tank_col(BOT_OWNER0+i));
    }
    for(int p=0;p<s_nplayers;p++){
        if(p!=s_local&&s_tanks[p].in_shop) continue;
        int dx=mmx+(int)(s_tanks[p].x*mmw/WORLD_W);
        int dy=mmy+(int)(s_tanks[p].y*mmh/WORLD_H);
        if(dx<mmx) dx=mmx;
        if(dx>mmx+mmw-2) dx=mmx+mmw-2;
        if(dy<mmy) dy=mmy;
        if(dy>mmy+mmh-2) dy=mmy+mmh-2;
        d.fillRect(dx,dy,2,2,p==0?COL_P1:COL_P2);
    }
    // ---- P1 (left): HP pips row, credits below
    hud_hp(2,1,0,COL_P1,false);
    char b1[12]; snprintf(b1,sizeof b1,"$%d",s_tanks[0].credits);
    txt(2,HUD_H-8,1,COL_GOLD,b1);
    // ---- P2 (right of minimap): HP pips + credits — only when a 2nd human plays;
    //      in solo survival show the live bot count instead
    int p2x=mmx-4;
    if(s_nplayers>=2){
        hud_hp(p2x,1,1,COL_P2,true);
        char b2[12]; snprintf(b2,sizeof b2,"$%d",s_tanks[1].credits);
        txr(p2x,HUD_H-8,1,COL_GOLD,b2);
    } else if(s_nbots>0){
        char w1[12]; snprintf(w1,sizeof w1,"OND.%d",s_bot_level+1);
        txr(p2x,2,1,tank_col(BOT_OWNER0),w1);
        char w2[12]; snprintf(w2,sizeof w2,"x%d",s_bot_kills);
        txr(p2x,HUD_H-8,1,COL_GOLD,w2);
    }
    // ---- center: weapon/powerup (top) + cooldown bar + match timer (bottom)
    Tank &lt=s_tanks[s_local];
    char cent[16]; snprintf(cent,sizeof cent,"%s|%s",wp_short(lt.weapon),pu_short(lt.pu));
    bool puon=(lt.pu!=PU_NONE);
    txc(W/2,1,1,puon&&((s_anim>>3)&1)?COL_GOLD:COL_MUT,cent);

    // weapon cooldown bar (red when ready, dark when cooling)
    int cd_max=fire_cd_for(lt.weapon);
    int cd_left=lt.fire_cd;
    int bar_w=20, bar_x=W/2-bar_w/2;
    uint16_t cd_col=(cd_left<=0)?COL_GREEN:mix(COL_RED,COL_BLACK,cd_left*255/cd_max);
    d.fillRect(bar_x,6,bar_w,3,rgb(20,20,30));
    if(cd_left>0) d.fillRect(bar_x,6,(bar_w*cd_left)/cd_max,3,cd_col);
    else d.drawRect(bar_x,6,bar_w,3,COL_GREEN);

    int sec=s_match_ms/1000;
    char tm[8]; snprintf(tm,sizeof tm,"%d:%02d",sec/60,sec%60);
    txc(W/2,HUD_H-8,1,sec<=10?COL_RED:COL_GREEN,tm);
}

// ========================== draw: shop overlay ================================
static const char* shop_effect(int sitype){
    switch(sitype){
        case SI_ARMOR:  return "-1 danno subito";
        case SI_SPEED:  return "+20% velocita";
        case SI_REPAIR: return "HP al massimo";
        case SI_SHIELD: return "para 1 colpo";
        case SI_EMP:    return "stordisce nemico";
        case SI_WP_MG:  return "raffica rapida";
        case SI_WP_RAIL:return "perfora i muri";
        case SI_WP_SHOT:return "3 pallini a ventaglio";
        case SI_WP_FLAK:return "rosa di piombini, corto raggio";
        case SI_WP_ROCKET:return "esplosione ad area + rinculo";
        case SI_WP_LASER:return "raggio veloce che trapassa";
        case SI_WP_SNIPER:return "danno alto, trafora 2 volte";
        case SI_WP_MINIGUN:return "fuoco rapido, poco danno";
        case SI_REGEN:  return "guarisce 1 HP ogni 500ms";
    }
    return "";
}
#define SHOP_ROW   23
#define SHOP_LIST0 33
static void draw_shop(void){
    Tank &me=s_tanks[s_local];
    SellRow sell[4]; int nsell=build_sell_list(s_local,sell);
    int count=(s_shop_tab==0)?SHOP_N:nsell;
    int vis=(H-12-SHOP_LIST0)/SHOP_ROW;                 // visible rows
    if(s_shop_sel>=count) s_shop_sel=count>0?count-1:0;
    if(s_shop_sel<s_shop_scroll) s_shop_scroll=s_shop_sel;
    if(s_shop_sel>=s_shop_scroll+vis) s_shop_scroll=s_shop_sel-vis+1;
    if(s_shop_scroll<0) s_shop_scroll=0;

    // ---- backdrop
    d.fillRect(0,0,W,H,rgb(10,13,20));
    // ---- header: title + big credits + countdown
    d.fillRect(0,0,W,15,rgb(18,46,24));
    d.drawFastHLine(0,15,W,rgb(40,120,60));
    txt(4,1,2,COL_WHITE,"NEGOZIO");
    char cr[12]; snprintf(cr,sizeof cr,"$%d",me.credits);
    txr(W-30,1,2,COL_GOLD,cr);
    int sec=me.shop_ms/1000;
    char tt[6]; snprintf(tt,sizeof tt,"%ds",sec);
    txr(W-2,4,1,sec<=5?COL_RED:COL_GREEN,tt);
    // ---- tabs
    const char* tabs[2]={"COMPRA","VENDI"};
    for(int i=0;i<2;i++){
        int x0=i*(W/2), tw=W/2;
        bool on=(s_shop_tab==i);
        d.fillRect(x0,16,tw,15,on?rgb(30,60,36):rgb(16,20,28));
        if(on){ d.drawFastHLine(x0,16,tw,COL_GREEN); d.fillRect(x0,29,tw,2,COL_GREEN); }
        d.drawFastVLine(W/2,16,15,rgb(40,48,64));
        txc(x0+tw/2,18,2,on?COL_WHITE:COL_DIM,tabs[i]);
    }
    // ---- timer bar
    int tbar=(me.shop_ms*W)/20000; if(tbar>W) tbar=W;
    d.fillRect(0,31,W,2,rgb(24,30,40));
    d.fillRect(0,31,tbar,2,sec<=5?COL_RED:COL_GREEN);

    // ---- list
    if(count==0){
        txc(W/2,H/2-6,2,COL_DIM, s_shop_tab? "NIENTE DA VENDERE":"ESAURITO");
    }
    int y=SHOP_LIST0;
    for(int i=s_shop_scroll;i<count&&y<H-12;i++){
        bool sel=(i==s_shop_sel);
        const char* name; const char* eff; int price; bool can; uint16_t accent;
        if(s_shop_tab==0){
            ShopItem &it=s_shop_items[s_local][i];
            name=SHOP_CAT[it.type].name; eff=shop_effect(it.type); price=it.cost;
            can=!it.sold&&me.credits>=it.cost; accent=COL_GREEN;
            if(it.sold){ name=SHOP_CAT[it.type].name; }
        } else {
            name=sell[i].label; eff="+ crediti"; price=sell[i].refund; can=true; accent=COL_GOLD;
        }
        bool soldOut=(s_shop_tab==0&&s_shop_items[s_local][i].sold);
        uint16_t bg = soldOut?rgb(26,26,30) : sel?(s_shop_tab?rgb(48,40,18):rgb(22,46,26)) : rgb(16,19,26);
        d.fillRect(0,y,W,SHOP_ROW-1,bg);
        if(sel){ d.drawRect(0,y,W,SHOP_ROW-1,accent); d.fillRect(0,y,3,SHOP_ROW-1,accent); }
        uint16_t nc = soldOut?COL_DIM : can?COL_WHITE : mix(COL_RED,COL_WHITE,90);
        txt(8,y+2,2,nc,name);
        txt(8,y+15,1,sel?COL_MUT:COL_DIM,soldOut?"VENDUTO":eff);
        // price chip right
        char pc[8]; snprintf(pc,sizeof pc,(s_shop_tab?"+%d":"$%d"),price);
        txr(W-6,y+4,2, soldOut?COL_DIM : s_shop_tab?COL_GOLD : can?COL_GOLD:COL_RED, pc);
        if(sel&&can&&!soldOut) txr(W-6,y+16,1,accent,"K");
        y+=SHOP_ROW;
    }
    // ---- scroll arrows
    if(s_shop_scroll>0)            txc(W-12,SHOP_LIST0-1,1,COL_GREEN,"^"); // more above
    if(s_shop_scroll+vis<count)    txc(W-12,H-13,1,COL_GREEN,"v");         // more below
    // ---- footer
    d.fillRect(0,H-11,W,11,rgb(14,17,24));
    d.drawFastHLine(0,H-11,W,rgb(34,40,54));
    txt(2,H-9,1,COL_MUT,"A/D scheda  W/S scorri");
    txr(W-2,H-9,1,COL_MUT,"K ok  ESC esci");
}

// ========================== draw: felt bg =====================================
// cheap full-screen backdrop: vertical gradient (1 hline/row) + parallax stars + grid
static void felt(void){
    uint16_t top=rgb(10,14,30), bot=rgb(20,12,28);
    for(int y=0;y<H;y++) d.drawFastHLine(0,y,W,mix(top,bot,y*256/H));
    // faint perspective grid toward horizon
    for(int y=H/2;y<H;y+=6){ int sh=(y-H/2); d.drawFastHLine(0,y,W,mix(bot,rgb(40,30,60),40+sh)); }
    // drifting stars (deterministic positions, scroll with s_anim)
    for(int i=0;i<26;i++){
        int bx=(i*97)%W;
        int sx=(bx + (int)(s_anim>>2) + i*13) % W;
        int sy=(i*53)%(H/2);
        uint16_t c=(i&3)?rgb(60,70,110):rgb(120,140,200);
        d.drawPixel(sx,sy,c);
        if((i&7)==0) d.drawPixel((sx+1)%W,sy,mix(c,COL_BLACK,80));
    }
}

// ========================== draw: menu ========================================
#define NMENU 5
static const char* menu_label(int i){
    switch(i){ case 0: return "Duello vs CPU"; case 1: return "Sopravvivenza"; case 2: return "Crea partita";
               case 3: return "Entra in partita"; default: return "Come si gioca"; }
}
static const char* mtype_name(int m){
    switch(m){ case MT_COOP: return "Co-op vs Bot"; case MT_BRAWL: return "Duello + Bot"; default: return "Duello 1v1"; }
}
// little tank emblem for the menu header
static void menu_emblem(int cx,int cy,uint16_t col,int dir){
    static const int BDX[4]={0,1,0,-1}, BDY[4]={-1,0,1,0};
    d.fillRoundRect(cx-6,cy-5,12,11,2,mix(col,COL_BLACK,70));
    d.drawFastHLine(cx-4,cy-5,8,mix(col,COL_WHITE,110));
    d.fillRoundRect(cx-3,cy-3,7,7,1,mix(col,COL_WHITE,40));
    d.drawLine(cx,cy,cx+BDX[dir]*9,cy+BDY[dir]*9,rgb(90,96,110));
    d.fillRect(cx-7,cy-6,3,13,rgb(40,42,50)); d.fillRect(cx+5,cy-6,3,13,rgb(40,42,50));
}
static void draw_menu(void){
    felt();
    // title with chunky drop-shadow + chrome shimmer
    int blink=(s_anim>>3)&1;
    txc(W/2+2,6,3,rgb(8,8,16),"TANK DUEL");
    txc(W/2+1,5,3,mix(COL_P1,COL_BLACK,120),"TANK DUEL");
    txc(W/2,4,3,blink?COL_WHITE:COL_P1,"TANK DUEL");
    d.drawFastHLine(W/2-58,24,116,mix(COL_P1,COL_BLACK,80));
    d.drawFastHLine(W/2-46,26,92,rgb(40,50,80));
    // flanking tank emblems facing inward
    menu_emblem(20,14,COL_P1,1);
    menu_emblem(W-20,14,COL_P2,3);
    // fixed list (no carousel wrap → no duplicated entry)
    int cy=30, rh=19;
    for(int i=0;i<NMENU;i++){
        int y=cy+i*rh;
        bool sel=(i==s_msel);
        if(sel){
            d.fillRoundRect(10,y,W-20,rh-3,4,rgb(26,40,68));
            d.fillRoundRect(10,y,W-20,3,4,rgb(40,60,100));     // top sheen
            d.drawRoundRect(10,y,W-20,rh-3,4,blink?COL_GOLD:COL_P1);
            d.fillRect(14,y+3,3,rh-9,COL_GOLD);
            txc(W/2+4,y+1,2,COL_WHITE,menu_label(i));
            txt(W-26,y+2,1,blink?COL_GOLD:COL_MUT,">");
        } else {
            txc(W/2,y+1,2,COL_MUT,menu_label(i));
        }
    }
    txc(W/2,H-9,1,COL_DIM,"SU/GIU scegli   K conferma");
}
static void draw_how(void){
    felt();
    txc(W/2,3,2,COL_P1,"CONTROLLI");
    d.drawFastHLine(10,20,W-20,COL_DIM);
    txt(6,24,1,COL_WHITE,"W/E/R = su    A = sx    S = giu    D = dx");
    txt(6,36,1,COL_WHITE,"K = spara   L = usa powerup");
    txt(6,48,1,COL_WHITE,"ESC = pausa / menu");
    d.drawFastHLine(10,60,W-20,COL_DIM);
    txc(W/2,64,1,COL_GOLD,"SHOP");
    txt(6,74,1,COL_WHITE,"Entra nel riquadro verde per comprare.");
    txt(6,84,1,COL_WHITE,"20 secondi max. Sei nascosto dalla mappa.");
    txt(6,94,1,COL_WHITE,"Guadagni crediti: colpire +2, uccidere +15.");
    txc(W/2,H-10,1,COL_DIM,"ESC indietro");
}
static void draw_tank_select(void){
    felt();
    static const struct{ const char*name; int hp; int spd; int arm; const char*desc; } TYPES[TT_COUNT]={
        {"BULLDOG", 8,40,1,"Robusto e resistente, lento"},
        {"VIPER",   4,80,0,"Velocissimo ma fragile"},
        {"PHANTOM", 5,60,0,"Bilanciato, buon miratore"},
        {"CRUSHER",10,25,2,"Corazzato, lentissimo, duro"},
    };
    // header
    d.fillRect(0,0,W,16,rgb(20,28,46));
    d.drawFastHLine(0,16,W,rgb(60,90,150));
    txc(W/2,1,2,COL_GOLD,"SCEGLI IL TANK");
    // rows
    int cy=20, rh=22;
    for(int i=0;i<TT_COUNT;i++){
        bool sel=(i==s_tsel);
        int y=cy+i*rh;
        uint16_t bc=sel?rgb(24,42,72):rgb(13,15,22);
        d.fillRect(0,y,W,rh-1,bc);
        if(sel){ d.drawRect(0,y,W,rh-1,COL_P1); d.fillRect(0,y,3,rh-1,COL_GOLD); }
        menu_emblem(18,y+rh/2-1, sel?COL_P1:COL_DIM, 1);
        txt(32,y+3,2,sel?COL_WHITE:COL_MUT,TYPES[i].name);
        char st[24]; snprintf(st,sizeof st,"HP%d SPD%d ARM%d",TYPES[i].hp,TYPES[i].spd,TYPES[i].arm);
        txr(W-6,y+7,1,sel?COL_GREEN:COL_DIM,st);
    }
    // selected description strip
    int dy=cy+TT_COUNT*rh+2;
    d.fillRect(0,dy,W,11,rgb(16,20,30));
    txc(W/2,dy+2,1,COL_GREEN,TYPES[s_tsel].desc);
    // footer
    txc(W/2,H-9,1,COL_DIM,"W/S scegli   K conferma   ESC indietro");
}
static void draw_host_screen(void){
    felt();
    txc(W/2,4,2,COL_P1,"CREA PARTITA");
    d.drawFastHLine(10,21,W-20,COL_DIM);
    char nm[24]; snprintf(nm,sizeof nm,"%.18s",pnet_name()); txc(W/2,26,2,COL_GOLD,nm);
    char ch[24]; snprintf(ch,sizeof ch,"canale %d  -  Tank %s",pnet_channel(),tank_name(s_tanks[0].type));
    txc(W/2,44,1,COL_GREEN,ch);
    // ---- match-type selector (SU/GIU)
    d.fillRect(20,56,W-40,18,rgb(24,34,56));
    d.drawRect(20,56,W-40,18,COL_P1);
    txt(26,61,1,COL_MUT,"Modalita:");
    txr(W-26,60,2,((s_anim>>3)&1)?COL_WHITE:COL_GOLD,mtype_name(s_match_type));
    txc(W/2,78,1,COL_DIM,"SU/GIU cambia modalita");
    char dots[5]="    "; for(int i=0;i<(int)((s_anim>>2)%4);i++) dots[i]='.';
    char w[40]; snprintf(w,sizeof w,"Attendo sfidante%s",dots);
    txc(W/2,96,1,COL_WHITE,w);
    txc(W/2,H-9,1,COL_DIM,"ESC annulla");
}
static void draw_browse_screen(void){
    felt();
    txc(W/2,5,2,COL_P2,"ENTRA IN PARTITA");
    char ch[24]; snprintf(ch,sizeof ch,"canale %d",pnet_channel()); txr(W-8,8,1,COL_GREEN,ch);
    d.drawFastHLine(10,22,W-20,COL_DIM);
    if(s_join_pending){
        txc(W/2,50,1,COL_DIM,"Connessione...");
        char dots[5]="    "; for(int i=0;i<(int)((s_anim>>2)%4);i++) dots[i]='.';
        char w[30]; snprintf(w,sizeof w,"contatto%s",dots); txc(W/2,70,1,COL_WHITE,w);
        return;
    }
    if(s_nhost==0){
        txc(W/2,50,1,COL_WHITE,"Nessuna partita trovata");
        txc(W/2,66,1,COL_DIM,"Sull'altro Cardputer:");
        txc(W/2,78,1,COL_DIM,"Tank Duel > Crea partita");
        return;
    }
    txt(10,28,1,COL_DIM,"Partite disponibili:");
    int y=40;
    for(int i=0;i<s_nhost&&y<H-14;i++){
        bool sel=(i==s_bsel);
        if(sel){
            d.fillRoundRect(8,y,W-16,20,4,rgb(36,28,20));
            d.drawRoundRect(8,y,W-16,20,4,COL_P2);
            txr(W-14,y+5,1,((s_anim>>2)&1)?COL_GREEN:COL_WHITE,"INVIO>");
        }
        txt(16,y+5,1,sel?COL_WHITE:COL_MUT,s_hosts[i].name);
        y+=sel?24:18;
    }
}
static void draw_pause(void){
    // semi-transparent overlay
    for(int y=H/2-22;y<H/2+24;y+=2) d.drawFastHLine(0,y,W,rgb(14,16,24));
    d.fillRoundRect(20,H/2-24,W-40,48,6,rgb(16,20,36));
    d.drawRoundRect(20,H/2-24,W-40,48,6,COL_P1);
    txc(W/2,H/2-16,2,COL_GOLD,"PAUSA");
    txc(W/2,H/2-2,1,COL_WHITE,"K = riprendi  ESC = abbandona");
}
static void draw_over(void){
    felt();
    bool coop=(s_match_type==MT_COOP);
    const char* t; uint16_t tc;
    if(coop){
        bool win=(s_winner==WIN_HUMANS);
        t=win?"VITTORIA":"SCONFITTA"; tc=win?COL_GREEN:COL_RED;
    } else {
        bool iwon=(s_winner==s_local);
        t=s_winner<0?"PAREGGIO":iwon?"HAI VINTO":"HAI PERSO";
        tc=s_winner<0?COL_GOLD:iwon?COL_GREEN:COL_RED;
    }
    // ---- big banner with framed title
    d.fillRect(0,6,W,34,mix(tc,COL_BLACK,205));
    d.drawFastHLine(0,6,W,tc);
    d.drawFastHLine(0,7,W,mix(tc,COL_WHITE,40));
    d.drawFastHLine(0,39,W,tc);
    txc(W/2+2,13,3,rgb(6,8,14),t);            // drop shadow
    txc(W/2,11,3,((s_anim>>3)&1)?COL_WHITE:tc,t);
    // ---- result rows for the active human players (winner row highlighted)
    for(int p=0;p<s_nplayers;p++){
        int y=48+p*22;
        bool isLocal=(p==s_local);
        bool isWin=(s_winner==p)||(coop&&s_winner==WIN_HUMANS);
        uint16_t col=p==0?COL_P1:COL_P2;
        d.fillRect(16,y,W-32,19,isWin?mix(col,COL_BLACK,150):rgb(15,18,26));
        d.fillRect(16,y,4,19,col);
        if(isWin) d.drawRect(16,y,W-32,19,COL_GOLD);
        char lbl[16]; snprintf(lbl,sizeof lbl,"%s%s",p==0?"P1":"P2",isLocal?" (tu)":"");
        txt(26,y+6,1,col,lbl);
        char st[20]; snprintf(st,sizeof st,"HP %d/%d  $%d",s_tanks[p].hp,s_tanks[p].hp_max,s_tanks[p].credits);
        txr(W-22,y+6,1,COL_WHITE,st);
        if(isWin) txt(W/2-8,y+6,1,COL_GOLD,"WIN");
    }
    // ---- co-op / survival score line
    if(s_nbots>0){
        char sc[40]; snprintf(sc,sizeof sc,"Bot eliminati: %d   Ondata: %d",s_bot_kills,s_bot_level+1);
        txc(W/2,coop?94:96,1,COL_GOLD,sc);
    }
    // ---- disconnect notice (own line, readable)
    if(s_peerleft) txc(W/2,s_nbots>0?104:96,1,COL_RED,"Avversario disconnesso");
    // ---- footer prompt, clear of the bottom edge
    d.fillRect(0,H-12,W,12,rgb(14,17,24));
    d.drawFastHLine(0,H-12,W,rgb(34,40,54));
    txc(W/2,H-10,1,((s_anim>>3)&1)?COL_WHITE:COL_DIM,"INVIO rigioca    ESC menu");
}

// ========================== draw: main ========================================
static void on_draw(void){
    switch(s_state){
        case GS_MENU:   draw_menu(); break;
        case GS_SELECT: draw_tank_select(); break;
        case GS_HOST:   draw_host_screen(); break;
        case GS_BROWSE: draw_browse_screen(); break;
        case GS_SHOP:   draw_map(); draw_picks(); draw_tanks(); draw_bullets(); draw_sparks(); draw_flash(); draw_hud(); draw_shop(); break;
        case GS_PAUSE:  draw_map(); draw_picks(); draw_tanks(); draw_bullets(); draw_sparks(); draw_hud(); draw_pause(); break;
        case GS_PLAY:
            draw_map();
            draw_picks();
            draw_tanks();
            draw_bullets();
            draw_sparks();
            draw_flash();
            draw_hud();
            break;
        case GS_OVER:   draw_over(); break;
        case GS_HOW:    draw_how(); break;
        default: break;
    }
}

// ========================== state transitions ================================
static void go(int state){
    s_state=state;
    nucleo_app_set_fullscreen(state==GS_PLAY||state==GS_SHOP||state==GS_PAUSE);
    if(state==GS_OVER) sfx_play((s_winner==s_local)?6:7);
    nucleo_app_request_draw();
}
static void start_match(int mode, int t1type, int t2type){
    s_mode=mode; s_local=(mode==GM_GUEST)?1:0;
    s_map_seed=esp_random(); if(!s_map_seed) s_map_seed=0xDEADBEEFu;
    map_gen(s_map_seed);
    tank_init(s_tanks[0],t1type,true);
    tank_init(s_tanks[1],t2type,false);
    s_nplayers=(s_match_type==MT_COOP)?1:2;     // co-op vs CPU is solo survival
    if(s_nplayers<2) s_tanks[1].alive=false;
    setup_bots();
    memset(s_bullets,0,sizeof s_bullets);
    memset(s_shops,0,sizeof s_shops);
    memset(s_sparks,0,sizeof s_sparks);
    memset(s_rings,0,sizeof s_rings);
    memset(s_picks,0,sizeof s_picks);
    s_shop_zone[0]=s_shop_zone[1]=-1; s_shop_done[0]=s_shop_done[1]=-1;
    s_flash=0; s_ai_shop_cd=0; s_ai_strafe_cd=0; s_ai_strafe=1; s_ai_fire_last=0;
    s_match_ms=180000; s_winner=-1; s_shake=0;
    s_shop_spawn_cd=8000; s_passive_cd=10000; s_pick_spawn_cd=6000;
    s_peerleft=false;
    cam_update();
    go(GS_PLAY);
}
static void leave_to_menu(void){
    send_bye(); s_haspeer=false; s_join_pending=false;
    s_nhost=0; s_msel=0; go(GS_MENU);
}

// ========================== input ============================================
static bool s_was_fire=false, s_was_pu=false;

static void on_key(int k, char ch){
    (void)k;
    switch(s_state){
        case GS_MENU:
            if(k==NK_UP)    { s_msel=(s_msel+NMENU-1)%NMENU; sfx_play(SFX_NAV); nucleo_app_request_draw(); }
            else if(k==NK_DOWN){ s_msel=(s_msel+1)%NMENU;    sfx_play(SFX_NAV); nucleo_app_request_draw(); }
            else if((k==NK_ENTER)||ch=='k'){
                sfx_play(SFX_SEL);
                if(s_msel<=3){ s_tsel=0; go(GS_SELECT); }   // 0..3 pick a tank first
                else go(GS_HOW);
            }
            return;
        case GS_SELECT:
            if((k==NK_UP)||ch=='w'||ch=='e'||ch=='r')
                { s_tsel=(s_tsel+TT_COUNT-1)%TT_COUNT; sfx_play(SFX_NAV); nucleo_app_request_draw(); }
            else if((k==NK_DOWN)||ch=='s')
                { s_tsel=(s_tsel+1)%TT_COUNT; sfx_play(SFX_NAV); nucleo_app_request_draw(); }
            else if(ch=='k'||(k==NK_ENTER)){
                sfx_play(SFX_SEL);
                if(s_msel==0){                        // Duello vs CPU
                    s_match_type=MT_DUEL;
                    start_match(GM_CPU,s_tsel,(int)(esp_random()%TT_COUNT));
                } else if(s_msel==1){                  // Sopravvivenza (solo + bot)
                    s_match_type=MT_COOP;
                    start_match(GM_CPU,s_tsel,TT_BULLDOG);
                } else if(s_msel==2){                  // Crea partita (match type cycled on host screen)
                    s_match_type=MT_DUEL;
                    tank_init(s_tanks[0],s_tsel,true);
                    s_haspeer=false; s_nhost=0;
                    go(GS_HOST); send_hello(HS_HOSTING);
                } else {                               // Entra in partita
                    tank_init(s_tanks[0],s_tsel,true);
                    s_nhost=0; s_bsel=0;
                    go(GS_BROWSE);
                }
            }
            return;
        case GS_HOST:
            if((k==NK_UP)||ch=='w'||ch=='e'||ch=='r'){ s_match_type=(s_match_type+MT_COUNT-1)%MT_COUNT; sfx_play(SFX_NAV); nucleo_app_request_draw(); }
            else if((k==NK_DOWN)||ch=='s'){ s_match_type=(s_match_type+1)%MT_COUNT; sfx_play(SFX_NAV); nucleo_app_request_draw(); }
            return;
        case GS_BROWSE:
            if((k==NK_UP)||ch=='w')  { if(s_bsel>0){ s_bsel--; sfx_play(SFX_NAV);} nucleo_app_request_draw(); }
            else if((k==NK_DOWN)||ch=='s'){ if(s_bsel<s_nhost-1){ s_bsel++; sfx_play(SFX_NAV);} nucleo_app_request_draw(); }
            else if((ch=='k'||(k==NK_ENTER))&&s_nhost>0&&!s_join_pending){
                sfx_play(SFX_SEL);
                memcpy(s_peer,s_hosts[s_bsel].mac,6); s_haspeer=true;
                send_join();
                s_join_pending=true; s_join_t=s_join_resend=s_now; nucleo_app_request_draw();
            }
            return;
        case GS_PLAY:
            // movement + fire polled continuously in poll()
            // L = use powerup (tap)
            if(ch=='l'&&!s_was_pu){ pu_use(s_local); s_was_pu=true; }
            else if(ch!='l') s_was_pu=false;
            return;
        case GS_SHOP: {
            Tank &me=s_tanks[s_local];
            SellRow sell[4]; int nsell=build_sell_list(s_local,sell);
            int count=(s_shop_tab==0)?SHOP_N:nsell;
            if(ch=='a'){
                if(s_shop_tab!=0){ s_shop_tab=0; s_shop_sel=0; s_shop_scroll=0; sfx_play(SFX_NAV); }
                nucleo_app_request_draw();
            } else if(ch=='d'){
                if(s_shop_tab!=1){ s_shop_tab=1; s_shop_sel=0; s_shop_scroll=0; sfx_play(SFX_NAV); }
                nucleo_app_request_draw();
            } else if((k==NK_UP)||ch=='w'||ch=='e'||ch=='r'){
                if(s_shop_sel>0){ s_shop_sel--; sfx_play(SFX_NAV); }
                nucleo_app_request_draw();
            } else if((k==NK_DOWN)||ch=='s'){
                if(s_shop_sel<count-1){ s_shop_sel++; sfx_play(SFX_NAV); }
                nucleo_app_request_draw();
            } else if(ch=='k'){
                if(s_shop_tab==0&&s_shop_sel<SHOP_N){
                    ShopItem &it=s_shop_items[s_local][s_shop_sel];
                    if(!it.sold&&me.credits>=it.cost){
                        if(s_mode==GM_GUEST) send_buy(s_shop_sel);
                        me.credits-=it.cost; shop_apply(me,it.type); it.sold=true;
                        spark_burst(me.x,me.y,6,COL_GOLD); sfx_play(SFX_BUY);
                    }
                } else if(s_shop_tab==1&&nsell>0&&s_shop_sel<nsell){
                    int st=sell[s_shop_sel].type;
                    if(s_mode==GM_GUEST) send_buy(0x80|st);
                    shop_sell(me,st);
                    spark_burst(me.x,me.y,6,COL_GOLD); sfx_play(SFX_SELL);
                }
                nucleo_app_request_draw();
            }
            return;
        }
        case GS_PAUSE:
            if(ch=='k') go(GS_PLAY);
            return;
        case GS_OVER:
            if(ch=='k'||(k==NK_ENTER)){
                if(s_mode==GM_CPU){ start_match(GM_CPU,s_tanks[s_local].type,(int)(esp_random()%TT_COUNT)); }
                else leave_to_menu();
            }
            return;
        case GS_HOW:
            return; // ESC handled by back_handler
        default: return;
    }
}
static bool on_back(int key){
    if(key==NK_LEFT) return true; // swallow LEFT
    switch(s_state){
        case GS_MENU:   return false; // close app
        case GS_PLAY:   go(GS_PAUSE); return true;
        case GS_PAUSE:  leave_to_menu(); return true;
        case GS_SHOP: {
            if(s_mode==GM_GUEST){ send_buy(0xFF); s_tanks[s_local].in_shop=false; s_tanks[s_local].shop_ms=0; }
            else shop_leave(s_local);
            go(GS_PLAY); return true;
        }
        case GS_OVER:   leave_to_menu(); return true;
        default: s_msel=0; go(GS_MENU); return true;
    }
}

// ========================== poll =============================================
static bool poll(void){
    s_now=now_ms();
    int dt=(int)(s_now-s_last); if(dt<0) dt=0; if(dt>60) dt=60;
    s_last=s_now;

    pnet_pkt_t p;
    while(pnet_recv(&p)) net_handle(&p);

    if(s_shake>0.2f) s_shake*=0.85f; else s_shake=0;
    // deterministic shake jitter (no rng — keeps host/guest sim in lockstep)
    int sh=(int)(s_shake+0.5f);
    s_shx=(s_anim&1)? sh : -sh;
    s_shy=(s_anim&2)? sh : -sh;

    bool live=false;

    if(s_state==GS_OVER){
        live=true;   // host re-broadcasts the result (ESP-NOW lossy); also lets the prompt blink
        if(s_mode==GM_HOST&&s_haspeer&&s_now-s_last_tx>80) send_state();
    } else if(s_state==GS_MENU||s_state==GS_SELECT||s_state==GS_HOW){
        // STATIC screens — redraw only on input (request_draw). Per-frame repaint of the
        // full-screen felt()/stars caused visible flicker; Pong keeps its menu static too.
        live=false;
    } else if(s_state==GS_HOST){
        if(s_now-s_last_hello>400){ send_hello(HS_HOSTING); s_last_hello=s_now; }
        live=true;
    } else if(s_state==GS_BROWSE){
        hosts_prune();
        if(s_join_pending){
            if(s_now-s_join_resend>350){ send_join(); s_join_resend=s_now; }  // ESP-NOW is lossy: retry
            if(s_now-s_join_t>6000) s_join_pending=false;                       // give up after 6 s
        }
        live=true;
    } else if(s_state==GS_PLAY||s_state==GS_SHOP||s_state==GS_PAUSE){
        sparks_step(dt);
        live=true;

        // PAUSE freezes the sim, but keep the MP link alive with a heartbeat
        if(s_state==GS_PAUSE){
            if(s_mode==GM_HOST&&s_now-s_last_tx>200) send_state();
            else if(s_mode==GM_GUEST&&s_now-s_last_tx>200) send_input(false,false,0,0);
            if(s_mode!=GM_CPU&&s_now-s_last_rx>6000){ s_winner=(s_mode==GM_HOST)?0:-1; s_over_why="pause:rxto"; go(GS_OVER); return true; }
            goto frame_check;
        }

        // shop timers
        for(int si=0;si<MAX_SHOPS;si++){
            if(!s_shops[si].active) continue;
            s_shops[si].life_ms-=dt;
            if(s_shops[si].life_ms<=0) s_shops[si].active=false;
        }
        // shop spawn
        if(s_mode!=GM_GUEST){
            s_shop_spawn_cd-=dt;
            if(s_shop_spawn_cd<=0){ shop_try_spawn(); s_shop_spawn_cd=45000; }
        }
        // powerup pickups: spawn + expire (host/CPU authoritative)
        if(s_mode!=GM_GUEST){
            for(int i=0;i<MAX_PICKS;i++){
                if(!s_picks[i].active) continue;
                s_picks[i].life_ms-=dt;
                if(s_picks[i].life_ms<=0) s_picks[i].active=false;
            }
            s_pick_spawn_cd-=dt;
            if(s_pick_spawn_cd<=0){ pick_try_spawn(); s_pick_spawn_cd=9000; }
        }
        // match timer
        if(s_match_ms>0){
            s_match_ms-=dt;
            if(s_match_ms<=0){
                s_match_ms=0;
                // time up: co-op humans survived → win; duel/brawl → most HP wins
                if(s_match_type==MT_COOP) s_winner=WIN_HUMANS;
                else s_winner=(s_tanks[0].hp>s_tanks[1].hp)?0:(s_tanks[1].hp>s_tanks[0].hp)?1:-1;
                s_over_why="timeup"; go(GS_OVER);
                if(s_mode==GM_HOST) send_state();
                return true;
            }
        }
        // passive credits
        s_passive_cd-=dt;
        if(s_passive_cd<=0){ earn(0,1); earn(1,1); s_passive_cd=10000; }

        if(s_mode==GM_GUEST){
            // guest: predict own movement (host authoritative corrects), send input.
            // While shopping the tank is frozen; we only keep the timer bar ticking.
            Tank &lt=s_tanks[s_local];
            bool shopping=lt.in_shop;
            if(shopping){ lt.shop_ms-=dt; if(lt.shop_ms<0) lt.shop_ms=0; }
            bool up=!shopping&&(nucleo_kbd_char_down('w')||nucleo_kbd_char_down('e')||nucleo_kbd_char_down('r'));
            bool lt_=!shopping&&nucleo_kbd_char_down('a');
            bool dn=!shopping&&nucleo_kbd_char_down('s');
            bool rt=!shopping&&nucleo_kbd_char_down('d');
            bool fire=!shopping&&nucleo_kbd_char_down('k');
            int mvx=(rt?1:0)-(lt_?1:0), mvy=(dn?1:0)-(up?1:0);   // 8-way intent
            if(mvx||mvy) tank_drive(s_local,mvx,mvy,(float)dt);  // local prediction
            if(fire) tank_fire(s_local);
            for(int p=0;p<2;p++) if(s_tanks[p].flash_ms>0) s_tanks[p].flash_ms-=dt;
            for(int p=0;p<2;p++) if(s_tanks[p].hurt_ms>0) s_tanks[p].hurt_ms-=dt;
            if(s_now-s_last_tx>25) send_input(fire,false,mvx,mvy);
            cam_update();
            if(s_now-s_last_rx>3500){ s_winner=-1; s_over_why="guest:rxto"; go(GS_OVER); return true; }
            goto frame_check;
        }

        // Host or CPU: run full simulation
        // local tank input — read BOTH axes for 8-way diagonal movement
        {
            bool up=nucleo_kbd_char_down('w')||nucleo_kbd_char_down('e')||nucleo_kbd_char_down('r');
            bool lt_=nucleo_kbd_char_down('a');
            bool dn=nucleo_kbd_char_down('s');
            bool rt=nucleo_kbd_char_down('d');
            bool fire=nucleo_kbd_char_down('k');
            int mvx=(rt?1:0)-(lt_?1:0), mvy=(dn?1:0)-(up?1:0);
            if(mvx||mvy) tank_drive(s_local,mvx,mvy,(float)dt);
            if(fire) tank_fire(s_local);
        }
        // fire cooldowns
        for(int p=0;p<2;p++) if(s_tanks[p].fire_cd>0) s_tanks[p].fire_cd-=dt;
        // powerup timers
        for(int p=0;p<2;p++){
            Tank &t=s_tanks[p];
            if(t.pu!=PU_NONE&&t.pu_ms>0){ t.pu_ms-=dt; if(t.pu_ms<=0){ if(t.pu!=PU_SHIELD) t.pu=PU_NONE; } }
        }
        // passive powerup effects (regen heal)
        for(int p=0;p<2;p++) powerup_step(p,dt);
        // muzzle-flash + hit-flash timers
        for(int p=0;p<2;p++){
            if(s_tanks[p].flash_ms>0) s_tanks[p].flash_ms-=dt;
            if(s_tanks[p].hurt_ms>0)  s_tanks[p].hurt_ms-=dt;
        }
        // shop enter check
        for(int p=0;p<2;p++) shop_check_enter(p);
        // powerup pickup collection
        pick_check();
        // shop timers for both tanks
        for(int p=0;p<2;p++){
            Tank &t=s_tanks[p];
            if(t.in_shop){
                t.shop_ms-=dt;
                if(t.shop_ms<=0){ shop_leave(p);
                    if(p==s_local&&s_state==GS_SHOP) go(GS_PLAY); }
            }
        }
        // bullets
        bullets_step(dt);
        // AI (move toward player, then auto-shop if inside a zone)
        ai_step(dt);
        ai_shop_step(dt);
        bots_step(dt);          // respawning bot horde (co-op / brawl)
        resolve_collisions();   // tanks push apart — no stacking
        // local left shop early (e.g. zone expired under feet) → close UI
        if(s_state==GS_SHOP&&!s_tanks[s_local].in_shop) go(GS_PLAY);
        // check victory
        if(match_over_check()){
            s_over_why="elim"; go(GS_OVER);              // state=OVER first so send_state carries phase=0
            if(s_mode==GM_HOST) send_state();
            return true;
        }
        // camera follows local
        cam_update();
        // host: send state + check remote
        if(s_mode==GM_HOST){
            if(s_now-s_last_tx>25) send_state();
            if(s_peerleft||s_now-s_last_rx>4000){ s_winner=0; s_over_why=s_peerleft?"host:peerleft":"host:rxto"; go(GS_OVER); return true; }
        }
    }

frame_check:
    if(!live) return false;
    if(s_now-s_frame<16) return false;
    s_frame=s_now; s_anim++;
    return true;
}

// ========================== lifecycle ========================================
static void on_enter(void){
    mkdir("/sd/data",0777); mkdir("/sd/data/tankduel",0777);
    game_sfx_ensure(&s_sfx);
    s_state=GS_MENU; s_msel=0; s_tsel=0; s_anim=0;
    s_haspeer=s_join_pending=s_peerleft=false;
    s_nhost=s_bsel=0; s_txseq=s_rxseq=0;
    s_now=s_last=s_frame=now_ms();
    s_last_hello=s_last_tx=s_last_rx=0;
    s_was_fire=s_was_pu=false;
    s_ai_fire_last=0;
    s_join_pending=false; s_join_t=s_join_resend=0;
    s_match_type=MT_DUEL; s_nbots=0; s_nplayers=2; memset(s_bots,0,sizeof s_bots);
    // Start ESP-NOW immediately (like Pong) so the lobby's HELLO discovery works the instant
    // you enter Create/Join — pnet also disables WiFi power-save so broadcasts aren't dropped.
    if(!pnet_start()) nucleo_app_set_hint("ESP-NOW KO  ESC esci");
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_fullscreen(false);
    nucleo_app_set_hint("SU/GIU  K scegli  ESC esci");
    nucleo_app_request_draw();
}
static void on_exit(void){
    send_bye();
    pnet_stop();
}

extern "C" void nucleo_register_tankduel(void){
    static const nucleo_app_def_t app = {
        "tankd","Tank Duel","Games","Arena 1v1: mappa 40x40, shop contestato, 4 tank, upgrade",
        'D', rgb(255,150,80),
        on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP
    };
    nucleo_app_register(&app);
}
