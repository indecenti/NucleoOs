// app_vs.cpp — "Orde": mini vampire-survivors (category Games, Solo boot).
// Step 2 = sprites + world: the portable sim core (vs_sim.c) drives a scrolling, culled, single-blit
// canvas at 30 fps, rendered with the curated Kenney CC0 atlas (tiled grass world + sprite entities)
// and readable Font2 HUD. Falls back to coloured primitives if the atlas isn't on SD yet, so the game
// still runs before deploy. See docs/game-mini-vs.md, assets/coop-rpg/SPRITES.md.
//
// RAM: the VS world (~7 KB SoA pools) and the T_N tiles (8bpp RGB332, 256 B each — matches the
// device canvas's own depth, half the RAM of RGB565 and a straight memcpy blit_op, no per-pixel
// convert) are HEAP-ON-ENTER (alloc in on_enter, free on_exit) — never .bss — matching the boot-RAM
// discipline. NX_SOLO opens the game in a fresh, unfragmented heap that holds world + tiles + the
// 32 KB canvas. Atlas built by assets/coop-rpg/repack_orde_atlas8.mjs; blitters shared with Cardler
// via tile_blit.h (same atlas format, same magenta key).
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"      // W H HINT BG FG C_*
#include "app_gfx.h"             // 'd' = canvas
#include "tile_blit.h"           // shared blit_op/blit_key/blit_sz (8bpp RGB332), also used by Cardler
#include "nucleo_exclusive.h"    // NX_SOLO
#include "esp_timer.h"
#include <M5GFX.h>               // M5Canvas (atlas decode)
#include "game_sfx.h"            // shared SFX engine (pack WAV -> play, degrades to tone)
#include "nucleo_imu.h"          // ADV tilt sensor (complementary control)
#include "nucleo_i18n.h"         // TR(it,en): hints follow the system language
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
extern "C" {
#include "vs_sim.h"
}

// ---- sound cues (WAV pack at /sd/data/Orde/pack/<name>.wav, else synth tone) ----
enum { SFX_START = 1, SFX_SHOT, SFX_DIE, SFX_PICKUP, SFX_LEVELUP, SFX_HURT, SFX_WAVE, SFX_OVER, SFX_SELECT, SFX_N };
static int s_audio_on = 1;
static const char *sfx_name(int id)
{
    switch (id) {
        case SFX_START: return "start"; case SFX_SHOT: return "shot"; case SFX_DIE: return "die";
        case SFX_PICKUP: return "pickup"; case SFX_LEVELUP: return "levelup"; case SFX_HURT: return "hurt";
        case SFX_WAVE: return "wave"; case SFX_OVER: return "over"; case SFX_SELECT: return "select";
    }
    return "x";
}
static int sfx_recipe(int id, notify_voice_t *v)   // tone fallback if a pack WAV is missing
{
    float hz = 440;
    switch (id) {
        case SFX_START: hz = 660; break; case SFX_SHOT: hz = 880; break; case SFX_DIE: hz = 180; break;
        case SFX_PICKUP: hz = 990; break; case SFX_LEVELUP: hz = 1180; break; case SFX_HURT: hz = 150; break;
        case SFX_WAVE: hz = 120; break; case SFX_OVER: hz = 90; break; case SFX_SELECT: hz = 720; break;
    }
    notify__voice(&v[0], hz, 0.0f, 0.08f);
    return 1;
}
static bool sfx_important(int id) { return id == SFX_LEVELUP || id == SFX_HURT || id == SFX_OVER || id == SFX_START || id == SFX_WAVE; }
static const game_sfx_t s_sfx = { "/sd/data/Orde", sfx_name, sfx_recipe, SFX_N - 1, 1, 16000, sfx_important, &s_audio_on };
static inline void sfx(int id) { game_sfx_play(&s_sfx, id); }

// ---- game state machine ----
enum { GS_MENU, GS_START, GS_SETTINGS, GS_PLAY, GS_LVLANIM, GS_LEVELUP, GS_OVER };
static int s_gs = GS_MENU;
static int s_anim_t = 0;         // level-up flourish timer (frames)
static int s_menu_sel = 0;       // 0 = Gioca, 1 = Impostazioni
// tilt controller settings (persisted to SD so calibration sticks across Solo reboots)
static int s_tilt_on = 1;        // sensor enabled
static int s_sens    = 10;       // sensitivity 1..20
static int s_inv_x   = 1;        // invert X (our ADV reads it mirrored -> default on)
static int s_inv_y   = 0;        // invert Y
enum { SET_SENSOR, SET_SENS, SET_INVX, SET_INVY, SET_COUNT };
static int s_set_sel = 0;
// tilt neutral, angle-based (atan2) so it stays symmetric at any holding angle (e.g. 45 deg)
static bool  s_tilt_recenter = true;
static int   s_rc_settle = 0;    // frames of neutral re-baselining after a recenter (absorb LPF settling)
static float s_roll0 = 0.0f, s_pitch0 = 0.0f;
static int s_up_sel = 0;   // which of the 3 level-up offers (s_off[]) is highlighted

#define ORDE_CFG "/sd/data/Orde/settings.json"
static void save_settings(void)
{
    FILE *f = fopen(ORDE_CFG, "wb"); if (!f) return;
    fprintf(f, "{\"tilt\":%d,\"sens\":%d,\"ix\":%d,\"iy\":%d}\n", s_tilt_on, s_sens, s_inv_x, s_inv_y);
    fclose(f);
}
static void load_settings(void)
{
    FILE *f = fopen(ORDE_CFG, "rb"); if (!f) return;
    char b[128]; int n = (int)fread(b, 1, sizeof b - 1, f); fclose(f); if (n < 0) n = 0; b[n] = 0;
    int t = 1, se = 5, ix = 1, iy = 0;
    sscanf(b, "{\"tilt\":%d,\"sens\":%d,\"ix\":%d,\"iy\":%d}", &t, &se, &ix, &iy);
    s_tilt_on = t ? 1 : 0; s_sens = se < 1 ? 1 : (se > 20 ? 20 : se);
    s_inv_x = ix ? 1 : 0; s_inv_y = iy ? 1 : 0;
}
static int s_prev_kills = 0, s_prev_hp = 0;   // for event-driven sfx
static int s_wave_flash = 0;                  // frames left of the "ambush!" screen-edge flash
static int s_hit_flash = 0;                   // frames left of the short "you got hit" screen-edge flash

// best-run record (survival time + kills), persisted like settings so it survives across Solo reboots
#define ORDE_BEST "/sd/data/Orde/best.json"
static int s_best_secs = 0, s_best_kills = 0;
static bool s_new_record = false;             // set on GAME OVER when this run beat a stored best
static void save_best(void)
{
    FILE *f = fopen(ORDE_BEST, "wb"); if (!f) return;
    fprintf(f, "{\"secs\":%d,\"kills\":%d}\n", s_best_secs, s_best_kills);
    fclose(f);
}
static void load_best(void)
{
    FILE *f = fopen(ORDE_BEST, "rb"); if (!f) return;
    char b[64]; int n = (int)fread(b, 1, sizeof b - 1, f); fclose(f); if (n < 0) n = 0; b[n] = 0;
    int se = 0, k = 0;
    sscanf(b, "{\"secs\":%d,\"kills\":%d}", &se, &k);
    s_best_secs = se > 0 ? se : 0; s_best_kills = k > 0 ? k : 0;
}

#define FPS30_US 33333
#define MOVE     VS_TOFIX(4)     // target speed ~4 px/frame (read_move eases toward it; snappy but weighty)
#define ATLAS_PATH "/sd/data/Orde/atlas.bin"   // raw 8bpp RGB332, T_N x (16x16), magenta key (matches the 8bpp canvas)

// atlas tile indices (see assets/coop-rpg/SPRITES.md). T_MONK..T_RANGER heroes; T_SWORD..T_POTG items.
// T_GRASSCLOVER..T_CHEST are appended past the original 23 (assets/coop-rpg/append_orde_tiles.mjs) —
// never re-crop the first 23, only ever append new tiles after them (see that script's header comment).
enum { T_GRASS, T_GRASS2, T_FLOWER, T_DIRT, T_BUSH, T_MUSH, T_MONK, T_GHOST, T_DEMON, T_SPIDER, T_WOLF,
       T_GEM, T_KNIGHT, T_WIZARD, T_RANGER, T_SWORD, T_AXE, T_DAGGER, T_SHIELD, T_POTR, T_POTG,
       T_TREETOP, T_TREEBOT,
       T_GRASSCLOVER, T_GRASSPEBBLE, T_SAND, T_SIGN, T_BARREL, T_CRATE, T_CHEST, T_N };

// ---- weapon presentation (the sim owns behaviour; this is just name + card icon) ----
static const char *WEP_NAME[WEP_COUNT] = { "Mago", "Frusta", "Guardiano", "Piromane", "Cecchino" };
static const int   WEP_ICO [WEP_COUNT] = { T_DAGGER, T_SWORD, T_SHIELD, T_POTR, T_AXE };

// ---- passives (global buffs; one card improves the whole arsenal, VS-style) ----
enum { PSV_MIGHT, PSV_HASTE, PSV_AREA, PSV_SPEED, PSV_MAGNET, PSV_REGEN, PSV_MAXHP, PSV_GARLIC, PSV_N };
static const char *PSV_NAME[PSV_N] = { "Potenza", "Cadenza", "Ampiezza", "Velocita", "Magnete", "Rigenera", "Vita+", "Aglio" };
static const int   PSV_ICO [PSV_N] = { T_AXE, T_POTG, T_POTR, T_POTG, T_GEM, T_POTG, T_POTR, T_SHIELD };
static const int   PSV_CAP [PSV_N] = { 5, 5, 5, 5, 6, 3, 99, 5 };

// a single level-up offer: level/learn a weapon, or take a passive
enum { OFF_WEP_NEW, OFF_WEP_LVL, OFF_PSV };
typedef struct { uint8_t kind; uint8_t id; } Offer;
static Offer s_off[3];

// ---- heroes: sprite + distinct starting power (applied on run start) ----
typedef struct { const char *name; int tile; const char *power; } Hero;
enum { HERO_MONK, HERO_MAGO, HERO_CAV, HERO_RANGER, HERO_COUNT };
static const Hero HEROES[HERO_COUNT] = {
    { "MONACO",    T_MONK,   "Orbite + aura" },
    { "MAGO",      T_WIZARD, "Dardi magici" },
    { "CAVALIERE", T_KNIGHT, "Frusta, robusto" },
    { "RANGER",    T_RANGER, "Cecchino perfora" },
};
static int s_hero = 0;

// Entities are drawn x2 (32x32) with L/R flip by scaling the 16x16 tile ON THE FLY into a small stack
// buffer — NO pre-scaled RAM copies (that 24 KB tipped the Solo heap over the 32 KB canvas and the
// atlas failed to load -> no sprites). Big + flippable, ~zero extra RAM.
#define ENTZ 32

static VS      *s_w = nullptr;
static uint8_t (*s_tiles)[256] = nullptr;     // [T_N][16*16] tiles (8bpp RGB332): ground + entity source
static bool     s_atlas_ok = false;
static int      s_face = 0;                    // player facing: 0 = right, 1 = left
static int64_t  s_last_us = 0;
static bool     s_over = false;

// enemy accent per type (fallback primitives)
static const uint16_t EN_COL[4] = { 0xF9A6 /*red*/, 0xFD20 /*orange*/, 0xC61F /*violet*/, 0x07FF /*cyan*/ };
// 8-direction unit vectors (x100) for the level-up ray burst
static const int s_cos8[8] = { 100, 71, 0, -71, -100, -71, 0, 71 };
static const int s_sin8[8] = { 0, 71, 100, 71, 0, -71, -100, -71 };
// 32-direction unit circle (x256) — MUST match vs_sim.c's VS_COS/VS_SIN so the Guardiano orbs the app
// draws land exactly where the sim ticks their damage.
static const int16_t VS_COS_A[32] = {
    256, 251, 237, 213, 181, 142, 98, 50, 0, -50, -98, -142, -181, -213, -237, -251,
   -256,-251,-237,-213,-181,-142, -98,-50, 0,  50,  98,  142,  181,  213,  237,  251 };
static const int16_t VS_SIN_A[32] = {
      0,  50,  98, 142, 181, 213, 237, 251, 256, 251, 237, 213, 181, 142, 98, 50,
      0, -50, -98,-142,-181,-213,-237,-251,-256,-251,-237,-213,-181,-142, -98,-50 };

static bool poll_fn(void);
static bool on_back(int key);

// ---- atlas blitters (thin wrappers over the shared tile_blit.h, bound to this app's s_tiles) ----
static inline void blit_op(int idx, int x, int y) { tile_blit_op(s_tiles, idx, x, y); }
static inline void blit(int idx, int x, int y) { tile_blit_key(s_tiles, idx, x, y); }
static inline void blit_sz(int idx, int f, int cx, int cy, int D) { tile_blit_sz(s_tiles, idx, f, cx, cy, D); }
static inline void blit_scaled(int idx, int f, int cx, int cy, int s) { blit_sz(idx, f, cx, cy, s * 16); }
static inline void blit_ent(int idx, int f, int cx, int cy) { blit_sz(idx, f, cx, cy, 32); }
#define ENZ 28   // enemies a touch smaller than the 32 px player

// Load the raw 8bpp RGB332 atlas straight into the tile buffers. No on-device PNG decode (robust +
// fast); alpha was hard-keyed to magenta on the PC. Entities are scaled x2 at draw time (blit_ent).
static void load_atlas(void)
{
    s_atlas_ok = false;
    if (!s_tiles) return;
    FILE *f = fopen(ATLAS_PATH, "rb");
    if (!f) return;
    size_t want = (size_t)T_N * 256;
    size_t got = fread(s_tiles, 1, want, f);
    fclose(f);
    s_atlas_ok = (got == want);
}

static int psv_val(VS *w, int p)
{
    switch (p) {
        case PSV_MIGHT:  return w->up_might;   case PSV_HASTE: return w->up_haste;
        case PSV_AREA:   return w->up_area;    case PSV_SPEED: return w->up_speed;
        case PSV_MAGNET: return w->up_magnet;  case PSV_REGEN: return w->up_regen;
        case PSV_GARLIC: return w->up_garlic;  default:        return 0;   // MAXHP: no meaningful cap
    }
}
static void apply_psv(VS *w, int p)
{
    switch (p) {
        case PSV_MIGHT:  w->up_might++;  break;   case PSV_HASTE: w->up_haste++;  break;
        case PSV_AREA:   w->up_area++;   break;   case PSV_SPEED: w->up_speed++;  break;
        case PSV_MAGNET: w->up_magnet++; break;   case PSV_REGEN: w->up_regen++;  break;
        case PSV_GARLIC: w->up_garlic++; break;
        case PSV_MAXHP:  w->phpmax += 25; w->php += 25; break;
    }
}
static void apply_offer(VS *w, Offer o)
{
    if (o.kind == OFF_PSV) apply_psv(w, o.id);
    else                   vs_give_weapon(w, o.id);
}
// Build the candidate pool (learn/level weapons + non-maxed passives), then pick 3 distinct via the
// sim RNG. Weapons the player already owns offer a level-up; unowned ones offer to learn (if a slot's
// free). This is the whole VS meta-loop: every level you widen or deepen the arsenal.
static void roll_ups(VS *w)
{
    Offer pool[(int)WEP_COUNT + (int)PSV_N]; int n = 0;
    for (int wt = 0; wt < WEP_COUNT; wt++) {
        int lv = vs_has_weapon(w, wt);
        if (lv < 0) { if (w->wcount < VS_MAX_WEP) pool[n++] = (Offer){ OFF_WEP_NEW, (uint8_t)wt }; }
        else if (!vs_weapon_maxed(w, wt))          pool[n++] = (Offer){ OFF_WEP_LVL, (uint8_t)wt };
    }
    for (int p = 0; p < PSV_N; p++)
        if (psv_val(w, p) < PSV_CAP[p]) pool[n++] = (Offer){ OFF_PSV, (uint8_t)p };

    // Fisher-Yates the first 3 slots (MAXHP is uncapped so the pool is never smaller than 1).
    for (int i = 0; i < 3 && i < n; i++) {
        w->rng = w->rng * 1664525u + 1013904223u;
        int j = i + (int)((w->rng >> 16) % (uint32_t)(n - i));
        Offer t = pool[i]; pool[i] = pool[j]; pool[j] = t;
        s_off[i] = pool[i];
    }
    for (int i = n; i < 3; i++) s_off[i] = (Offer){ OFF_PSV, PSV_MAXHP };   // pad (only if pool < 3)
    s_up_sel = 0;
}
static void adjust_setting(int dir)
{
    switch (s_set_sel) {
        case SET_SENSOR: s_tilt_on = !s_tilt_on; break;
        case SET_SENS:   s_sens += dir; if (s_sens < 1) s_sens = 1; if (s_sens > 20) s_sens = 20; break;
        case SET_INVX:   s_inv_x = !s_inv_x; break;
        case SET_INVY:   s_inv_y = !s_inv_y; break;
    }
    save_settings(); sfx(SFX_SELECT);
}
// Each hero starts with ONE signature weapon + a flavour buff. New weapons/passives are then earned
// through level-ups, so two runs of the same hero can diverge into very different builds.
static void apply_hero(VS *w, int h)
{
    w->wcount = 0;                                          // drop vs_init's fallback Mago; set the real kit
    switch (h) {
        case HERO_MONK:   w->up_garlic = 2; vs_give_weapon(w, WEP_GUARDIANO); break;  // orbiting orbs + aura
        case HERO_MAGO:                     vs_give_weapon(w, WEP_MAGO);      break;  // homing bolts
        case HERO_CAV:    w->phpmax = 150; w->php = 150; w->up_might = 1;
                                            vs_give_weapon(w, WEP_FRUSTA);    break;  // whip, tanky
        case HERO_RANGER: w->up_haste = 1;  vs_give_weapon(w, WEP_CECCHINO);  break;  // piercing sniper
    }
}
static void start_run(void)
{
    if (s_w) { vs_init(s_w, (uint32_t)esp_timer_get_time()); apply_hero(s_w, s_hero); }
    s_tilt_recenter = true;                            // neutral = pose at start ("hold it, then play")
    s_face = 0; s_over = false; s_gs = GS_PLAY;
    s_prev_kills = 0; s_prev_hp = s_w ? s_w->php : 100; s_wave_flash = 0; s_hit_flash = 0; s_new_record = false;
    nucleo_app_set_hint(nucleo_imu_present() ? TR("W/E su  A/S giu  I sx  O dx  o inclina   spazio azzera", "W/E up  A/S dn  I lf  O rt  or tilt   space recenter")
                                             : TR("W/E su  A/S giu  I sx  O dx  (diagonali)   esc esci", "W/E up  A/S dn  I lf  O rt  (diagonals)   esc back"));
    sfx(SFX_START);
}

static void on_enter(void)
{
    nucleo_app_set_fullscreen(true);
    if (!s_w)     s_w     = (VS *)calloc(1, sizeof *s_w);          // ~7 KB, only while playing
    if (!s_tiles) s_tiles = (uint8_t (*)[256])calloc(T_N, 256);       // 8bpp: 256 B/tile
    load_atlas();
    load_settings();
    load_best();
    if (s_w) vs_init(s_w, 1);                          // seed the world behind the title screen
    s_gs = GS_MENU; s_face = 0; s_over = false; s_last_us = 0;
    nucleo_app_set_poll_handler(poll_fn);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR(";/. scegli   invio ok   esc indietro", ";/. pick   enter ok   esc back"));
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    nucleo_app_set_fullscreen(false);
    free(s_w);     s_w = nullptr;                    // back to zero RAM until relaunched
    free(s_tiles); s_tiles = nullptr;
    s_atlas_ok = false;
}

// LEFT and BACK both arrive here (framework routing). LEFT = move/navigate; BACK = close.
static bool on_back(int key)
{
    if (!s_w) return false;
    if (key == NK_BACK) {                             // Esc: back out to the main menu (close only from it)
        if (s_gs == GS_MENU) return false;            // top level -> let the app close
        s_gs = GS_MENU; sfx(SFX_SELECT); nucleo_app_request_draw();
        return true;
    }
    if (key == NK_LEFT) {
        if (s_gs == GS_START)    { s_hero = (s_hero + HERO_COUNT - 1) % HERO_COUNT; sfx(SFX_SELECT); nucleo_app_request_draw(); return true; }
        if (s_gs == GS_SETTINGS) { adjust_setting(-1); nucleo_app_request_draw(); return true; }
        if (s_gs == GS_LEVELUP)  { if (s_up_sel > 0) s_up_sel--; sfx(SFX_SELECT); nucleo_app_request_draw(); return true; }
        return true;                                  // swallow LEFT in play/menu
    }
    return false;
}

static void on_key(int key, char ch)
{
    if (!s_w) return;
    switch (s_gs) {
        case GS_MENU:
            if (key == NK_UP || key == NK_DOWN || ch == 'e' || ch == 's') { s_menu_sel ^= 1; sfx(SFX_SELECT); }
            else if (key == NK_ENTER) { s_gs = (s_menu_sel == 0) ? GS_START : GS_SETTINGS; sfx(SFX_SELECT); }
            break;
        case GS_SETTINGS:
            if (key == NK_UP || ch == 'e')        { s_set_sel = (s_set_sel + SET_COUNT - 1) % SET_COUNT; sfx(SFX_SELECT); }
            else if (key == NK_DOWN || ch == 's') { s_set_sel = (s_set_sel + 1) % SET_COUNT; sfx(SFX_SELECT); }
            else if (key == NK_RIGHT || key == NK_ENTER || ch == 'd') adjust_setting(+1);
            break;
        case GS_START:
            if (key == NK_ENTER) start_run();
            else if (key == NK_RIGHT || key == NK_DOWN || ch == 'd' || ch == 's') { s_hero = (s_hero + 1) % HERO_COUNT; sfx(SFX_SELECT); }
            else if (key == NK_UP || ch == 'a' || ch == 'e') { s_hero = (s_hero + HERO_COUNT - 1) % HERO_COUNT; sfx(SFX_SELECT); }
            break;
        case GS_OVER:
            if (key == NK_ENTER) start_run();          // play again
            break;
        case GS_LEVELUP:
            if (key == NK_UP || key == NK_LEFT)   { if (s_up_sel > 0) s_up_sel--; sfx(SFX_SELECT); }
            else if (key == NK_DOWN || key == NK_RIGHT) { if (s_up_sel < 2) s_up_sel++; sfx(SFX_SELECT); }
            else if (key == NK_ENTER) {
                apply_offer(s_w, s_off[s_up_sel]);
                s_w->pending_up--;
                if (s_w->pending_up > 0) roll_ups(s_w); else s_gs = GS_PLAY;
                sfx(SFX_LEVELUP);
            }
            break;
        case GS_PLAY:
            break;                                    // movement is read from held keys each frame (read_move)
    }
    nucleo_app_request_draw();
}

static bool poll_fn(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < FPS30_US) return false;
    s_last_us = now;
    return true;                                     // 30 fps: step + render + blit
}

// ---- render helpers (camera-relative, culled) ----
static inline int SX(VS *w, vfix wx, int cam_x) { return VS_TOINT(wx) - cam_x; }
static inline int SY(VS *w, vfix wy, int cam_y) { return VS_TOINT(wy) - cam_y; }

static void draw_hud(VS *w)
{
    // Bordered, high-contrast HP + XP bars. HP is a true php/phpmax ratio (was a flat php-clamped-to-100,
    // so HP-ups and the 150-max Cavaliere under-read their real cushion). Shifted a few px off the literal
    // top edge and thickened — a hairline glued to y=0 was easy to miss on real hardware (bezel crop /
    // just too thin), so give it room and heft. A numeric HP/HPmax readout rides the text row below (that
    // row is already proven visible — timer/level were never reported missing), as a zero-risk backup to
    // the graphic bar, colour-matched to the same tri-color danger read.
    int hp_pct = w->phpmax > 0 ? (w->php * 100 / w->phpmax) : 0;
    hp_pct = hp_pct < 0 ? 0 : (hp_pct > 100 ? 100 : hp_pct);
    bool low = hp_pct <= 25 && (w->frame & 8);
    uint16_t hp_col = low ? C_YELLOW : (hp_pct > 60 ? C_GREEN : (hp_pct > 25 ? C_YELLOW : C_RED));

    const int TOP = 3;                                           // margin off the literal screen edge
    d.fillRect(0, TOP, W, 10, 0x0000);                           // solid black band, max contrast
    d.drawRect(1, TOP + 1, W - 2, 6, 0xFFFF);                    // HP track outline (was 4px, now 6px)
    d.fillRect(2, TOP + 2, W - 4, 4, 0x2965);                    // empty-track fill (dim red-grey)
    int hp_w = (W - 4) * hp_pct / 100;
    if (hp_w > 0) d.fillRect(2, TOP + 2, hp_w, 4, hp_col);       // fill thickened 2px -> 4px

    int req = 8 + w->plevel * 2;                                 // matches vs_sim's per-level xp requirement
    int xp_pct = req > 0 ? (w->pxp * 100 / req) : 0; xp_pct = xp_pct > 100 ? 100 : xp_pct;
    int xp_w = (W - 4) * xp_pct / 100;
    if (xp_w > 0) d.fillRect(2, TOP + 8, xp_w, 2, C_BLUE);       // bright blue accent under the HP bar

    // Readable Font2 (16 px) HUD row: timer left, HP number centered (bar backup), level+kills right —
    // all with a dark shadow for contrast against the world.
    char b[28];
    int secs = (int)(w->frame / 30);
    d.setFont(&fonts::Font2); d.setTextSize(1);
    const int TY = TOP + 11;                                     // text row starts clear of the bars
    snprintf(b, sizeof b, "%02d:%02d", secs / 60, secs % 60);
    d.setTextColor(0x0000); d.setCursor(4, TY + 1); d.print(b);
    d.setTextColor(0xFFFF); d.setCursor(3, TY);     d.print(b);

    snprintf(b, sizeof b, "%d/%d", w->php < 0 ? 0 : w->php, w->phpmax);
    int hw = (int)d.textWidth(b);
    d.setTextColor(0x0000); d.setCursor(W / 2 - hw / 2 + 1, TY + 1); d.print(b);
    d.setTextColor(hp_col); d.setCursor(W / 2 - hw / 2, TY);         d.print(b);

    snprintf(b, sizeof b, "Lv%d  x%d", w->plevel, w->kills);
    int tw = (int)d.textWidth(b);
    d.setTextColor(0x0000); d.setCursor(W - tw - 3, TY + 1); d.print(b);
    d.setTextColor(0xFFE0); d.setCursor(W - tw - 4, TY);     d.print(b);
    d.setFont(&fonts::Font0);

    // BOSS bar: a wide red bar across the bottom while a minute-boss lives (bullet-hell threat readout)
    if (w->boss_alive) {
        int bhp = 0;
        for (int i = 0; i < VS_MAX_EN; i++) if (w->ealive[i] && w->eboss[i]) { bhp = w->ehp[i]; break; }
        int by = H - 8, den = w->boss_hpmax > 0 ? w->boss_hpmax : 1;
        d.fillRect(0, by, W, 8, 0x0000);
        d.drawRect(6, by + 1, W - 12, 5, 0xF81F);
        int bw = bhp > 0 ? (W - 14) * bhp / den : 0; if (bw > W - 14) bw = W - 14;
        if (bw > 0) d.fillRect(7, by + 2, bw, 3, (w->frame & 4) ? 0xF800 : 0xFC10);
        d.setFont(&fonts::Font0); d.setTextColor(0xFFFF);
        d.setCursor(W / 2 - 15, by - 8); d.print("- BOSS -");
        d.setFont(&fonts::Font0);
    }
}

static void render_world(VS *w)
{
    // screen shake: jitter the camera a couple of px while w->shake is hot (set by the sim on big hits)
    int shx = 0, shy = 0;
    if (w->shake > 0) { int m = w->shake > 6 ? 3 : 2; shx = (int)((w->frame * 7) % (2 * m + 1)) - m; shy = (int)((w->frame * 13) % (2 * m + 1)) - m; }
    int cam_x = VS_TOINT(w->ppx) - W / 2 + shx;
    int cam_y = VS_TOINT(w->ppy) - H / 2 + shy;

    int bob = (int)((w->frame >> 3) & 1);                              // shared 1px hop cadence

    // ---- curated tiled grass world (atlas) or a scrolling dot lattice (fallback) ----
    if (s_atlas_ok) {
        int ox = cam_x & 15, oy = cam_y & 15;
        int bcx = cam_x >> 4, bcy = cam_y >> 4;
        for (int ty = 0; ty * 16 - oy < H; ty++)
            for (int tx = 0; tx * 16 - ox < W; tx++) {
                int wcx = bcx + tx, wcy = bcy + ty;
                uint32_t hsh = (uint32_t)(wcx * 73856093) ^ (uint32_t)(wcy * 19349663);
                // Dirt in coarse 2x2 patches; lusher grass tufts clustered in other patches; scattered
                // single tufts and flowers -> a more varied, less tiled-looking field.
                uint32_t patch = (uint32_t)((wcx >> 1) * 2654435761u) ^ (uint32_t)((wcy >> 1) * 40503u);
                int t = T_GRASS;
                if ((patch & 7) == 0)                t = T_DIRT;            // bare-earth patch
                else if (((patch >> 3) & 31) == 7)   t = T_SAND;            // rare sandy clearing
                else if ((patch & 15) == 3)          t = T_GRASS2;          // lush zone
                else if ((hsh & 7) == 0)             t = (hsh & 64) ? T_GRASSCLOVER : T_GRASS2;  // tuft, 2 variants
                else if ((hsh & 63) == 9)            t = T_GRASSPEBBLE;     // rarer pebble variant
                else if ((hsh & 31) == 5)            t = T_FLOWER;
                int px = tx * 16 - ox, py = ty * 16 - oy;
                blit_op(t, px, py);
                // 2-tall tree props (sparse) — foliage tile above the trunk tile. The canopy lands on the
                // CELL ABOVE's screen slot, which that cell already drew for itself one row earlier — so
                // check its own hash first (Cardler's tallTree() "is the cell above free" guard) and skip
                // planting a tree here if the neighbor already claimed that spot with its own tree/bush/
                // mushroom, instead of silently overdrawing it.
                if (((hsh >> 5) & 63) == 0) {
                    uint32_t hsh_up = (uint32_t)(wcx * 73856093) ^ (uint32_t)((wcy - 1) * 19349663);
                    bool above_clear = ((hsh_up >> 5) & 63) != 0 && ((hsh_up >> 9) & 15) != 0 && ((hsh_up >> 13) & 31) != 0;
                    if (above_clear) { blit(T_TREEBOT, px, py); blit(T_TREETOP, px, py - 16); }
                }
                else if (((hsh >> 9) & 15) == 0)   blit(T_BUSH, px, py);
                else if (((hsh >> 13) & 31) == 0)  blit(T_MUSH, px, py);
                // sparse camp-debris landmarks (new, Cardler-style props) — rarer than bush/mushroom, so
                // the wilderness gets an occasional "someone was here" point of interest, not clutter.
                else if (((hsh >> 18) & 255) == 3)  blit(T_SIGN, px, py);
                else if (((hsh >> 18) & 255) == 9)  blit(T_CHEST, px, py);
                else if (((hsh >> 26) & 63) == 5)   blit(T_BARREL, px, py);
                else if (((hsh >> 20) & 127) == 11) blit(T_CRATE, px, py);
            }
    } else {
        d.fillScreen(0x0841);
        for (int y = -(cam_y & 15); y < H; y += 16)
            for (int x = -(cam_x & 15); x < W; x += 16)
                d.drawPixel(x, y, 0x18E3);
    }

    // garlic aura (monk power): a soft pulsing green ring on the ground around the player
    if (w->aura_r > 4) {
        int pr = w->aura_r + (int)((w->frame >> 2) & 3);
        d.drawCircle(W / 2, H / 2, pr,     0x3FE6);
        d.drawCircle(W / 2, H / 2, pr - 1, 0x2F44);
        d.drawCircle(W / 2, H / 2, pr / 2, 0x1F63);
    }

    // xp gems on the ground: bounce + spin-glint. Fat gems (elite drops, gval>1) are bigger + blue-white
    // so a valuable pickup stands out from the gold trickle.
    for (int i = 0; i < VS_MAX_GEM; i++) {
        if (!w->galive[i]) continue;
        int sx = SX(w, w->gx[i], cam_x), sy = SY(w, w->gy[i], cam_y);
        if (sx < -8 || sx > W + 8 || sy < -8 || sy > H + 8) continue;
        int ph  = (int)(((w->frame >> 2) + (uint32_t)i * 3) & 7);
        int yb  = (ph < 4 ? ph : 8 - ph) >> 1;         // 0..2 px bounce (triangle wave)
        int coy = sy - yb;
        bool fat = w->gval[i] > 1;
        d.drawCircle(sx, coy, fat ? 4 : 3, fat ? 0x2C9F : 0x8400);
        d.fillCircle(sx, coy, fat ? 3 : 2, fat ? 0x5DFF : 0xFEA0);
        int sh = (int)(((w->frame >> 1) + (uint32_t)i) & 7);
        if (sh < 4) d.drawFastVLine(sx - 2 + sh, coy - 1, 3, 0xFFF2);   // shine sweeps -> spin glint
    }

    // pickups (heal chicken / bomb) — stationary, pulse to draw the eye
    for (int k = 0; k < 8; k++) {
        if (!w->kalive[k]) continue;
        int sx = SX(w, w->kx[k], cam_x), sy = SY(w, w->ky[k], cam_y);
        if (sx < -8 || sx > W + 8 || sy < -8 || sy > H + 8) continue;
        int pl = (int)((w->frame >> 3) & 1);
        if (w->ktype[k] == 0) {                        // heal: red cross in a white pill
            d.fillRoundRect(sx - 5, sy - 5, 10, 10, 3, 0xFFFF);
            d.fillRect(sx - 1, sy - 3, 2, 6, 0xF800); d.fillRect(sx - 3, sy - 1, 6, 2, 0xF800);
        } else {                                       // bomb: dark orb with a lit fuse
            d.fillCircle(sx, sy + 1, 5, 0x2104); d.drawCircle(sx, sy + 1, 5, 0x8410);
            d.drawPixel(sx + 2, sy - 4, pl ? 0xFFE0 : 0xFD20);
        }
    }

    // Guardiano orbs — orbiting shields, drawn from the SAME formula the sim damages with (vs_sim.c
    // guardiano_tick), so the visual and the hitbox always agree. Player is screen-centre, so orbs
    // circle W/2,H/2.
    for (int wi = 0; wi < w->wcount; wi++) {
        if (w->wtype[wi] != WEP_GUARDIANO) continue;
        int cnt = 2 + w->wlevel[wi], R = 26 + w->up_area * 4;
        for (int o = 0; o < cnt; o++) {
            int idx = (int)(((w->frame >> 1) + (uint32_t)(o * 32 / cnt)) & 31);
            int ox = W / 2 + VS_COS_A[idx] * R / 256;
            int oy = H / 2 + VS_SIN_A[idx] * R / 256;
            d.fillCircle(ox, oy, 4, 0x2C9F);            // deep-blue orb
            d.fillCircle(ox, oy, 2, 0xBEFF);            // bright core
        }
    }

    // projectiles: distinct look per weapon kind so the arsenal reads at a glance.
    for (int p = 0; p < VS_MAX_PROJ; p++) {
        if (!w->palive[p]) continue;
        int sx = SX(w, w->px[p], cam_x), sy = SY(w, w->py[p], cam_y);
        if (sx < -24 || sx > W + 24 || sy < -24 || sy > H + 24) continue;
        int tx = sx - VS_TOINT(w->pvx[p]) / 2, ty = sy - VS_TOINT(w->pvy[p]) / 2;
        switch (w->pkind[p]) {
            case PK_WHIP: {                             // a bright arc slash on the facing side
                int dir = w->pdmg[p] >= 0 ? 1 : -1;     // pdmg carries the side for whip markers
                int a0 = (int)(w->plife[p]);            // 7..1: sweep the arc as it fades
                for (int a = -2; a <= 2; a++) {
                    int yy = sy + a * 4;
                    d.drawFastHLine(sx - (dir < 0 ? 14 : 0), yy, 14, a0 > 3 ? 0xFFFF : 0xC618);
                }
                break;
            }
            case PK_NOVA: {                             // an expanding ring, radius from pdmg, fading
                int r = (int)(w->pdmg[p]) * (12 - w->plife[p]) / 12;
                if (r > 1) { d.drawCircle(sx, sy, r, 0xFD20); d.drawCircle(sx, sy, r - 1, 0xFCC0); }
                break;
            }
            case PK_SNIPE:                              // fast cyan-white lance with a long trail
                d.drawLine(tx, ty, sx, sy, 0x5DFF);
                d.fillCircle(sx, sy, 2, 0xFFFF);
                break;
            default:                                    // PK_BOLT — green player bolt / red enemy bolt
                if (w->powner[p]) {
                    d.drawLine(tx, ty, sx, sy, 0x6800);
                    d.fillCircle(sx, sy, 3, 0xFAE6); d.fillCircle(sx, sy, 1, 0xFFE0);
                } else {
                    d.drawLine(tx, ty, sx, sy, 0x9E63);
                    d.fillCircle(sx, sy, 3, 0xAFE8); d.fillCircle(sx, sy, 1, 0xFFFF);
                }
                break;
        }
    }

    // enemies (big sprite, face the player, 1px bob, strong camera cull). Elites are drawn larger, with
    // a purple threat ring + an HP bar so the miniboss reads as "kill this".
    for (int i = 0; i < VS_MAX_EN; i++) {
        if (!w->ealive[i]) continue;
        int sx = SX(w, w->ex[i], cam_x), sy = SY(w, w->ey[i], cam_y);
        if (sx < -24 || sx > W + 24 || sy < -24 || sy > H + 24) continue;
        int sz = w->eboss[i] ? ENZ + 30 : (w->eelite[i] ? ENZ + 14 : (w->eranged[i] ? ENZ - 6 : ENZ));
        if (w->eboss[i]) {                              // boss: big pulsing red-purple aura
            int pr = (sz / 2) + 4 + (int)((w->frame >> 1) & 5);
            d.drawCircle(sx, sy, pr, 0xF81F); d.drawCircle(sx, sy, pr - 2, 0x901F);
        } else if (w->eelite[i]) {
            int pr = (sz / 2) + 3 + (int)((w->frame >> 2) & 3);
            d.drawCircle(sx, sy, pr, 0xC01F);
        }
        if (s_atlas_ok) {
            int f = (w->ppx < w->ex[i]) ? 1 : 0;
            int eb = (int)(((w->frame >> 2) + (uint32_t)i) & 1);
            blit_sz(T_GHOST + (w->etype[i] & 3), f, sx, sy - eb, sz);
            if (w->ehflash[i]) {                        // hit flash: a bright ring pop over the sprite
                d.drawCircle(sx, sy, sz / 2, 0xFFFF); d.drawCircle(sx, sy, sz / 2 - 1, 0xFFFF);
            }
        } else { d.fillCircle(sx, sy, w->eboss[i] ? 10 : (w->eelite[i] ? 6 : (w->eranged[i] ? 2 : 3)), w->ehflash[i] ? 0xFFFF : EN_COL[w->etype[i] & 3]); d.drawPixel(sx, sy - 1, FG); }
        if (w->eranged[i] && !w->eboss[i]) {
            int bl = (int)((w->frame >> 3) & 1);
            if (bl) d.fillCircle(sx, sy - sz / 2 - 3, 2, C_RED);
        }
        if (w->eelite[i] && !w->eboss[i]) {             // slim HP bar over the elite (nominal ~130 max)
            d.fillRect(sx - 12, sy - sz / 2 - 6, 24, 3, 0x3186);
            int hw = w->ehp[i] > 0 ? (w->ehp[i] >= 130 ? 24 : w->ehp[i] * 24 / 130) : 0;
            d.fillRect(sx - 12, sy - sz / 2 - 6, hw, 3, 0xF800);
        }
    }

    // player (screen-centre, faces last move dir, gentle bob) — uses the chosen hero's sprite
    if (s_atlas_ok) blit_ent(HEROES[s_hero].tile, s_face, W / 2, H / 2 - bob);
    else { d.fillCircle(W / 2, H / 2, 4, C_GREEN); d.drawCircle(W / 2, H / 2, 4, FG); }
}

static void draw_menu(VS *w)
{
    (void)w;
    d.fillScreen(0x0000);
    d.drawRoundRect(6, 4, W - 12, H - 8, 8, 0x2124);
    d.setFont(&fonts::Font4);
    d.setTextColor(0x0000); d.setCursor(W / 2 - (int)d.textWidth("ORDE") / 2 + 1, 15); d.print("ORDE");
    d.setTextColor(0xFD20); d.setCursor(W / 2 - (int)d.textWidth("ORDE") / 2, 14); d.print("ORDE");
    if (s_atlas_ok) blit_scaled(T_MONK, 0, W / 2, 54, 2);            // mascot (shrunk 3x->2x to fit the record line)
    if (s_best_secs > 0 || s_best_kills > 0) {                       // best-run record, once a run has ended
        char rb[40];
        snprintf(rb, sizeof rb, "Record: %d:%02d  .  %d uccisioni", s_best_secs / 60, s_best_secs % 60, s_best_kills);
        d.setFont(&fonts::Font0); d.setTextColor(0xFEA0);
        d.setCursor(W / 2 - (int)d.textWidth(rb) / 2, 76); d.print(rb);
    }
    const char *items[2] = { "GIOCA", "IMPOSTAZIONI" };
    for (int i = 0; i < 2; i++) {
        int y = 92 + i * 20, sel = (i == s_menu_sel);
        if (sel) { d.fillRoundRect(36, y - 2, W - 72, 17, 4, 0x2A6A); d.drawRoundRect(36, y - 2, W - 72, 17, 4, 0xFEA0); }
        d.setFont(&fonts::Font2); d.setTextColor(sel ? 0xFFFF : 0x8C71);
        d.setCursor(W / 2 - (int)d.textWidth(items[i]) / 2, y); d.print(items[i]);
    }
    d.setFont(&fonts::Font0);
}

static void draw_settings(VS *w)
{
    (void)w;
    d.fillScreen(0x0000);
    d.drawRoundRect(6, 4, W - 12, H - 8, 8, 0x2124);
    d.setFont(&fonts::Font2); d.setTextColor(0xFD20);
    const char *t = "IMPOSTAZIONI";
    d.setCursor(W / 2 - (int)d.textWidth(t) / 2, 8); d.print(t);

    bool imu = nucleo_imu_present();
    const char *lbl[SET_COUNT] = { "Sensore", "Sensibilita", "Inverti X", "Inverti Y" };
    char vbuf[8];
    for (int i = 0; i < SET_COUNT; i++) {
        int y = 30 + i * 20, sel = (i == s_set_sel);
        if (sel) { d.fillRoundRect(16, y, W - 32, 18, 4, 0x2A6A); d.drawRoundRect(16, y, W - 32, 18, 4, 0xFEA0); }
        d.setFont(&fonts::Font2); d.setTextColor(sel ? 0xFFFF : 0x9CD3);
        d.setCursor(22, y + 2); d.print(lbl[i]);
        const char *v = vbuf; uint16_t vc = 0x3FE6;
        if (i == SET_SENSOR)   { v = !imu ? "N/D" : (s_tilt_on ? "ON" : "OFF"); vc = !imu ? 0x8C71 : (s_tilt_on ? 0x3FE6 : 0xF9A6); }
        else if (i == SET_SENS){ snprintf(vbuf, sizeof vbuf, "%d", s_sens); vc = 0xFEA0; }
        else if (i == SET_INVX){ v = s_inv_x ? "SI" : "NO"; }
        else                   { v = s_inv_y ? "SI" : "NO"; }
        d.setTextColor(vc); d.setCursor(W - 24 - (int)d.textWidth(v), y + 2); d.print(v);
    }
    d.setFont(&fonts::Font0); d.setTextColor(0x8C71);
    const char *dsc = imu ? "inclina per muovere - Spazio/C azzera lo zero" : "IMU assente (Cardputer non-ADV)";
    d.setCursor(W / 2 - (int)strlen(dsc) * 3, 116); d.print(dsc);
    d.setTextColor(0x6B4D); const char *h = ";/. voce   ,/. cambia   esc indietro";
    d.setCursor(W / 2 - (int)strlen(h) * 3, 127); d.print(h);
    d.setFont(&fonts::Font0);
}

// Dedicated hero-select carousel on a dark background.
static void draw_carousel(VS *w)
{
    (void)w;
    d.fillScreen(0x0000);                                        // dark backdrop
    d.drawRoundRect(6, 4, W - 12, H - 8, 8, 0x2124);
    d.setFont(&fonts::Font2); d.setTextColor(0xFD20);
    const char *k = "SCEGLI EROE";
    d.setCursor(W / 2 - (int)d.textWidth(k) / 2, 8); d.print(k);

    const Hero *h = &HEROES[s_hero];
    d.fillCircle(W / 2, 60, 30, 0x10A2);                         // spotlight disc
    d.fillCircle(W / 2, 60, 28, 0x0841);
    if (s_atlas_ok) blit_scaled(h->tile, 0, W / 2, 60, 4);       // big hero (4x)

    d.fillTriangle(14, 60, 24, 52, 24, 68, 0xFEA0);             // chevrons
    d.fillTriangle(W - 14, 60, W - 24, 52, W - 24, 68, 0xFEA0);

    d.setFont(&fonts::Font4); d.setTextColor(0xFFFF);
    d.setCursor(W / 2 - (int)d.textWidth(h->name) / 2, 90); d.print(h->name);
    d.setFont(&fonts::Font0); d.setTextColor(0x8FF3);
    d.setCursor(W / 2 - (int)strlen(h->power) * 3, 110); d.print(h->power);

    for (int i = 0; i < HERO_COUNT; i++) {                       // position dots
        int dx = W / 2 - (HERO_COUNT * 8) / 2 + i * 8 + 3;
        d.fillCircle(dx, 120, 2, i == s_hero ? 0xFEA0 : 0x4208);
    }
    d.setTextColor(0x8C71); const char *hint = ",/. scegli   Invio gioca";
    d.setCursor(W / 2 - (int)strlen(hint) * 3, 128); d.print(hint);
    d.setFont(&fonts::Font0);
}

// VS-style flourish before the chooser (no rings): a golden light-ray burst from the hero + rising
// sparkles + a dropping "LIVELLO SU!" banner, over the frozen game.
static void draw_lvlanim(VS *w)
{
    (void)w;
    int t = s_anim_t;
    int cx = W / 2, cy = H / 2 - 6;
    int rlen = 14 + t * 8;                               // rays shoot outward
    for (int k = 0; k < 8; k++) {
        int a = (k + (t & 1)) & 7;                       // 1-step shimmer
        int ex = cx + s_cos8[a] * rlen / 100, ey = cy + s_sin8[a] * rlen / 100;
        int ix = cx + s_cos8[a] * 9 / 100,   iy = cy + s_sin8[a] * 9 / 100;
        d.drawLine(ix, iy, ex, ey, (k & 1) ? 0xFEA0 : 0xFFF2);
    }
    for (int k = 0; k < 8; k++) {                        // sparkles rising and fanning up
        int px = cx + ((k * 53) % 60) - 30;
        int py = cy - t * 6 - k * 3;
        if (py > 8 && py < H) { d.fillRect(px, py, 2, 2, 0xFFE0); d.drawPixel(px, py - 1, 0xFFF6); }
    }
    const char *s = "LIVELLO SU!";
    d.setFont(&fonts::Font4);
    int tw = (int)d.textWidth(s), tx = cx - tw / 2;
    int by = 36 - (t < 5 ? (5 - t) * 3 : 0);            // banner drops in
    d.fillRoundRect(tx - 8, by - 5, tw + 16, 24, 5, 0x0000);
    d.drawRoundRect(tx - 8, by - 5, tw + 16, 24, 5, 0xFEA0);
    d.setTextColor(0xFFF2); d.setCursor(tx, by); d.print(s);
    d.setFont(&fonts::Font0);
}

static void draw_levelup(VS *w)
{
    (void)w;
    // All 3 cards stay simultaneously visible (compare at a glance) — legibility comes from dropping the
    // per-card description line (the label alone already says what the upgrade is) and spending that space
    // on a bigger icon + label instead, plus reclaiming a few px from the header so cards grow 28px -> 30px.
    d.fillRoundRect(6, 8, W - 12, H - 16, 8, 0x0841);
    d.drawRoundRect(6, 8, W - 12, H - 16, 8, 0xFFE0);
    d.setFont(&fonts::Font2); d.setTextColor(0xFFE0); d.setTextSize(1);
    const char *t = "LIVELLO SU!";
    d.setCursor(W / 2 - (int)d.textWidth(t) / 2, 10); d.print(t);
    for (int i = 0; i < 3; i++) {
        int cy = 28 + i * 33, sel = (i == s_up_sel);
        Offer o = s_off[i];
        int icon; char lbl[24]; const char *tag = 0; uint16_t tagc = 0x8C71;
        if (o.kind == OFF_PSV) {
            icon = PSV_ICO[o.id]; snprintf(lbl, sizeof lbl, "%s", PSV_NAME[o.id]);
        } else {
            icon = WEP_ICO[o.id]; snprintf(lbl, sizeof lbl, "%s", WEP_NAME[o.id]);
            if (o.kind == OFF_WEP_NEW) { tag = "NUOVA ARMA"; tagc = 0x8FF3; }
            else { static char lv[8]; snprintf(lv, sizeof lv, "Lv%d", vs_has_weapon(w, o.id) + 1); tag = lv; tagc = 0xFEA0; }
        }
        d.fillRoundRect(12, cy, W - 24, 30, 5, sel ? 0x2A6A : 0x18E3);
        if (sel) d.drawRoundRect(12, cy, W - 24, 30, 5, 0xFFE0);
        d.fillCircle(30, cy + 15, 14, 0x0000);                      // icon disc
        d.drawCircle(30, cy + 15, 14, sel ? 0xFEA0 : 0x4208);
        if (s_atlas_ok) blit_sz(icon, 0, 30, cy + 15, 22);
        d.setFont(&fonts::Font2); d.setTextColor(sel ? 0xFFFF : 0xC618); d.setTextSize(1.2f);
        d.setCursor(52, cy + 4); d.print(lbl);
        d.setTextSize(1);
        if (tag) { d.setFont(&fonts::Font0); d.setTextColor(tagc); d.setCursor(52, cy + 21); d.print(tag); }
    }
    d.setFont(&fonts::Font0);
}

// Full run-summary screen (was a small floating panel with 1 cramped line) — survival time, kills,
// level and hero, plus the persisted best-run record so a death always shows how it compares.
static void draw_over(VS *w)
{
    d.fillScreen(0x0000);
    d.drawRoundRect(6, 4, W - 12, H - 8, 8, 0x2124);

    d.setFont(&fonts::Font4); d.setTextColor(C_RED);
    int tw = (int)d.textWidth("GAME OVER");
    d.setCursor((W - tw) / 2, 12); d.print("GAME OVER");

    int secs = (int)(w->frame / 30);
    char b[40];
    snprintf(b, sizeof b, "%s  -  %d:%02d", HEROES[s_hero].name, secs / 60, secs % 60);
    d.setFont(&fonts::Font2); d.setTextColor(0xFFFF);
    tw = (int)d.textWidth(b); d.setCursor((W - tw) / 2, 40); d.print(b);

    snprintf(b, sizeof b, "Lv%d  .  %d uccisioni", w->plevel, w->kills);
    tw = (int)d.textWidth(b); d.setCursor((W - tw) / 2, 60); d.print(b);

    d.setFont(&fonts::Font0); d.setTextColor(0x8C71);
    snprintf(b, sizeof b, "Record: %d:%02d  .  %d uccisioni", s_best_secs / 60, s_best_secs % 60, s_best_kills);
    d.setCursor(W / 2 - (int)d.textWidth(b) / 2, 80); d.print(b);

    if (s_new_record) {
        const char *nr = "NUOVO RECORD!";
        d.setFont(&fonts::Font2); d.setTextColor(0xFEA0);
        d.setCursor(W / 2 - (int)d.textWidth(nr) / 2, 96); d.print(nr);
    }

    d.setFont(&fonts::Font0); d.setTextColor(0x6B4D);
    const char *hint = "Invio: rigioca   esc: menu";
    d.setCursor(W / 2 - (int)strlen(hint) * 3, 122); d.print(hint);
    d.setFont(&fonts::Font0);
}

// Player velocity from held keys — WASD-style (E up, S down, A left, D right) + arrow chars (;/./,//),
// multiple at once = diagonals. COMPLEMENTARY: on the ADV, if no key is held, the tilt sensor drives
// movement (analog, dead-zoned). Keys always take priority so the two never fight.
static void read_move(VS *w)
{
    vfix vx = 0, vy = 0;
    if (nucleo_kbd_char_down('w') || nucleo_kbd_char_down('e') || nucleo_kbd_char_down(';')) vy -= MOVE;   // up
    if (nucleo_kbd_char_down('a') || nucleo_kbd_char_down('s') || nucleo_kbd_char_down('.')) vy += MOVE;   // down
    if (nucleo_kbd_char_down('i') || nucleo_kbd_char_down(','))  { vx -= MOVE; s_face = 1; }               // left
    if (nucleo_kbd_char_down('o') || nucleo_kbd_char_down('/'))  { vx += MOVE; s_face = 0; }               // right

    // Re-centre the tilt neutral on the fly (rising edge). Ctrl (modifier) is BEST-EFFORT and often
    // dead on the ADV, so also accept Space and C via the reliable held-key table.
    static bool s_rc_prev = false;
    bool rc = ((nucleo_kbd_mods() & NK_MOD_CTRL) != 0) || nucleo_kbd_char_down(' ') || nucleo_kbd_char_down('c');
    if (rc && !s_rc_prev) { s_tilt_recenter = true; sfx(SFX_SELECT); }
    s_rc_prev = rc;

    if (vx || vy) {
        if (vx && vy) { vx = (vx * 181) >> 8; vy = (vy * 181) >> 8; }   // normalise diagonal (~x0.707)
    } else if (s_tilt_on) {
        float gx, gy, gz;                                              // no keys -> tilt (ADV only)
        if (nucleo_imu_present() && nucleo_imu_gravity_screen(&gx, &gy, &gz)) {
            // Proper ANGLE math: roll (left/right) + pitch (up/down) from the full gravity vector.
            // atan2 is correct + symmetric at ANY base angle (fixes the "held at 45 deg, one way slow").
            float roll = atan2f(gx, gz), pitch = atan2f(gy, gz);
            if (s_tilt_recenter) { s_roll0 = roll; s_pitch0 = pitch; s_rc_settle = 8; s_tilt_recenter = false; }
            float dr = roll - s_roll0, dp = pitch - s_pitch0;
            if (dr > (float)M_PI) dr -= 2 * (float)M_PI; else if (dr < -(float)M_PI) dr += 2 * (float)M_PI;
            if (dp > (float)M_PI) dp -= 2 * (float)M_PI; else if (dp < -(float)M_PI) dp += 2 * (float)M_PI;
            const float SPAN = 0.60f;                                  // ~34 deg deviation = full speed
            float DZ   = fmaxf(0.015f, 0.16f - (float)s_sens * 0.007f);      // rad; high sens = tiny dead-zone
            float maxv = (float)VS_TOFIX(1) * (1.0f + (float)s_sens * 0.6f); // px/frame at full deviation
            // Just after a recenter, re-baseline for a few frames so the settling low-pass can't bake a bias.
            if (s_rc_settle > 0) { s_roll0 += dr * 0.6f; s_pitch0 += dp * 0.6f; s_rc_settle--; dr = 0; dp = 0; }
            // WITHIN the dead-zone (i.e. no movement), slowly pull neutral to current -> any residual bias
            // (the "tends right" drift) decays to zero without ever fighting an intentional tilt.
            else if (fabsf(dr) < DZ && fabsf(dp) < DZ) { s_roll0 += dr * 0.05f; s_pitch0 += dp * 0.05f; }
            if (s_inv_x) dr = -dr;
            if (s_inv_y) dp = -dp;
            float mx = fabsf(dr) > DZ ? fminf(fmaxf(dr, -SPAN), SPAN) / SPAN : 0.0f;
            float my = fabsf(dp) > DZ ? fminf(fmaxf(dp, -SPAN), SPAN) / SPAN : 0.0f;
            vx = (vfix)(mx * maxv); vy = (vfix)(my * maxv);
            if (vx < 0) s_face = 1; else if (vx > 0) s_face = 0;
        }
    }
    // Movement-speed passive: scale the target by (6 + up_speed)/6 (~+17%/rank). The whip weapon needs a
    // facing, and the sim is authoritative for it, so publish the current facing every frame.
    if (w->up_speed) { vx = vx * (6 + w->up_speed) / 6; vy = vy * (6 + w->up_speed) / 6; }
    w->pface = (uint8_t)s_face;

    // Ease the velocity toward the target (vx,vy) instead of snapping: a professional weighty-but-snappy
    // feel — accelerates over ~2-3 frames and coasts briefly to a stop on release. read_move fully owns
    // the velocity now (the sim no longer applies friction).
    w->ppvx += (vx - w->ppvx) >> 1;
    w->ppvy += (vy - w->ppvy) >> 1;
}

static void on_draw(void)
{
    VS *w = s_w;
    if (!w) { d.fillScreen(0x0000); return; }

    // full-screen menus (dark) — no world behind
    if (s_gs == GS_MENU)     { draw_menu(w);     return; }
    if (s_gs == GS_SETTINGS) { draw_settings(w); return; }
    if (s_gs == GS_START)    { draw_carousel(w); return; }

    if (s_gs == GS_PLAY) {
        read_move(w);
        vs_step(w);
        if (w->kills > s_prev_kills) { sfx(SFX_DIE);  s_prev_kills = w->kills; }
        if (w->php   < s_prev_hp)    { sfx(SFX_HURT); s_hit_flash = 4; }   // short, sharp "you got hit" stinger
        s_prev_hp = w->php;
        if (w->wave_cd == 540)     { sfx(SFX_WAVE); s_wave_flash = 10; }   // the encircle wave just fired
        if (w->pending_up > 0)     { s_gs = GS_LVLANIM; s_anim_t = 0; sfx(SFX_LEVELUP); }
        else if (w->php <= 0) {
            s_gs = GS_OVER; s_over = true; sfx(SFX_OVER);
            int secs = (int)(w->frame / 30);
            s_new_record = false;
            if (secs > s_best_secs)      { s_best_secs = secs;   s_new_record = true; }
            if (w->kills > s_best_kills) { s_best_kills = w->kills; s_new_record = true; }
            if (s_new_record) save_best();
        }
    }

    render_world(w);
    // Bomb pickup: the sim wiped the screen (want_bomb counts down) — sell it with a bright full flash
    // that fades over the same window, drawn UNDER the HUD so gauges stay readable.
    if (w->want_bomb > 0) {
        uint16_t c = w->want_bomb > 5 ? 0xFFFF : (w->want_bomb > 2 ? 0xFFE0 : 0xC618);
        d.fillRect(0, 0, W, H, c);
    }
    draw_hud(w);
    // Two distinct screen-edge flashes, deliberately different rhythm so they read as different events:
    // a quick solid pulse for "you got hit" vs. the slower alternating "ambush incoming" telegraph.
    if (s_hit_flash > 0)  { d.drawRect(0, 0, W, H, 0xF800); s_hit_flash--; }
    if (s_wave_flash > 0) {
        uint16_t col = (s_wave_flash & 1) ? 0xF800 : 0x7800;
        d.drawRect(0, 0, W, H, col);
        d.drawRect(1, 1, W - 2, H - 2, col);
        s_wave_flash--;
    }
    if (s_gs == GS_LVLANIM) {
        draw_lvlanim(w);
        if (++s_anim_t >= 16) { roll_ups(w); s_gs = GS_LEVELUP; }   // flourish done -> chooser
    }
    if (s_gs == GS_LEVELUP) draw_levelup(w);
    if (s_gs == GS_OVER)    draw_over(w);
}

extern "C" void nucleo_register_vs(void)
{
    static const nucleo_app_def_t app = {
        "orde", "Orde", "Games", "Mini vampire-survivors: sopravvivi alle orde",
        'O', 0xF9A6,
        on_enter, on_key, nullptr, on_draw, on_exit,
        NX_SOLO
    };
    nucleo_app_register(&app);
}
