// app_vs.cpp — "Orde": mini vampire-survivors (category Games, Solo boot).
// The portable sim core (vs_sim.c) runs at 30 Hz from the poll handler and drives a scrolling, culled
// world rendered with the curated Kenney CC0 atlas (tiled grass + sprite entities, tile_blit.h writes
// straight into the 8bpp canvas). Falls back to coloured primitives if the atlas isn't on SD yet, so the
// game still runs. Menus, settings and modal cards are the shared console kit (game_ui.h); text follows
// the OS language (game_text.h); sound is the PC-rendered pack (game_sfx.h, tools/sfx-gen/games/orde.py).
// See docs/game-mini-vs.md, assets/coop-rpg/SPRITES.md.
//
// RAM: the VS world (~7 KB SoA pools) and the T_N tiles (8bpp RGB332, 256 B each, 4 KB chunks) are
// HEAP-ON-ENTER (alloc in on_enter, free on_exit) — never .bss. NX_SOLO opens the game in a fresh,
// unfragmented heap. Atlas built by assets/coop-rpg/repack_orde_atlas8.mjs.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"      // W H HINT BG FG C_*
#include "app_gfx.h"             // 'd' = canvas
#include "tile_blit.h"           // shared 8bpp atlas + blitters, also used by Cardler
#include "nucleo_exclusive.h"    // NX_SOLO
#include "esp_timer.h"
#include <M5GFX.h>
#include "game_sfx.h"            // shared SFX engine (pack WAV -> play, degrades to tone)
#include "game_text.h"           // the five OS languages (it/en in flash, es/fr/de from the SD pack)
#include "game_ui.h"             // gui:: the console kit (title, menu, dialog, text)
#include "nucleo_imu.h"          // ADV tilt sensor (complementary control)
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
extern "C" {
#include "vs_sim.h"
}

#define DIRP       "/sd/data/Orde"
#define ORDE_CFG   DIRP "/settings.json"
#define ORDE_BEST  DIRP "/best.json"
#define ATLAS_PATH DIRP "/atlas.bin"    // raw 8bpp RGB332, T_N x (16x16), magenta key (matches the 8bpp canvas)
#define STEP_US    33333                // 30 Hz sim step
#define MOVE       VS_TOFIX(4)          // target speed ~4 px/frame (read_move eases toward it; snappy but weighty)
#define COL_ACC    0xFD20               // Orde orange

// ---- sound cues (the PC-rendered pack at /sd/data/Orde/pack/<name>.wav, else a tone) ----
enum { SFX_START = 1, SFX_SHOT, SFX_DIE, SFX_PICKUP, SFX_LEVELUP, SFX_HURT, SFX_WAVE, SFX_OVER, SFX_SELECT,
       SFX_BOSS, SFX_BOMB, SFX_HEAL, SFX_N };
static TileCfg s_cfg;                         // sound + tilt settings (tile_blit.h, shared with Cardler)
static const char *sfx_name(int id)
{
    static const char *const N[SFX_N] = { "x", "start", "shot", "die", "pickup", "levelup", "hurt", "wave", "over",
                                          "select", "boss", "bomb", "heal" };
    return id > 0 && id < SFX_N ? N[id] : "x";
}
static int sfx_recipe(int id, notify_voice_t *v)   // tone fallback if a pack WAV is missing
{
    static const uint16_t HZ[SFX_N] = { 0, 660, 880, 180, 990, 1180, 150, 120, 90, 720, 100, 80, 1050 };
    notify__voice(&v[0], HZ[id > 0 && id < SFX_N ? id : 1], 0.0f, 0.08f);
    return 1;
}
static bool sfx_important(int id) { return id == SFX_LEVELUP || id == SFX_HURT || id == SFX_OVER || id == SFX_START || id == SFX_WAVE || id == SFX_BOSS || id == SFX_BOMB; }
static const game_sfx_t s_sfx = { DIRP, sfx_name, sfx_recipe, SFX_N - 1, 2, 16000, sfx_important, &s_cfg.audio };
static inline void sfx(int id) { game_sfx_play(&s_sfx, id); }

// ---- game state machine ----
enum { GS_MENU, GS_SETTINGS, GS_START, GS_PLAY, GS_LVLANIM, GS_LEVELUP, GS_PAUSE, GS_OVER };
static int s_gs = GS_MENU;
static int s_anim_t = 0;                       // level-up flourish timer (frames)
static gui::Menu s_menu, s_setm;
static int s_up_sel = 0;                       // which of the 3 level-up offers (s_off[]) is highlighted

static int s_wave_flash = 0;                  // frames left of the "ambush!" screen-edge flash
static int s_hit_flash = 0;                   // frames left of the short "you got hit" screen-edge flash

// best-run record (survival time + kills), persisted like settings so it survives across Solo reboots.
// A record file that exists but does not parse is left alone: no run may overwrite it with a worse one.
static int s_best_secs = 0, s_best_kills = 0;
static bool s_best_bad = false;
static bool s_new_record = false;             // set on GAME OVER when this run beat a stored best
static void save_best(void)
{
    if (s_best_bad) return;
    FILE *f = fopen(ORDE_BEST, "wb"); if (!f) return;
    fprintf(f, "{\"secs\":%d,\"kills\":%d}\n", s_best_secs, s_best_kills);
    fclose(f);
}
static void load_best(void)
{
    s_best_secs = s_best_kills = 0; s_best_bad = false;
    FILE *f = fopen(ORDE_BEST, "rb"); if (!f) return;
    char b[64]; int n = (int)fread(b, 1, sizeof b - 1, f); fclose(f); if (n < 0) n = 0; b[n] = 0;
    int se = 0, k = 0;
    if (sscanf(b, "{\"secs\":%d,\"kills\":%d}", &se, &k) != 2 || se < 0 || k < 0) { s_best_bad = true; return; }
    s_best_secs = se; s_best_kills = k;
}

// atlas tile indices (see assets/coop-rpg/SPRITES.md). T_MONK..T_RANGER heroes; T_SWORD..T_POTG items.
// T_GRASSCLOVER..T_CHEST are appended past the original 23 (assets/coop-rpg/append_orde_tiles.mjs) —
// never re-crop the first 23, only ever append new tiles after them (see that script's header comment).
enum { T_GRASS, T_GRASS2, T_FLOWER, T_DIRT, T_BUSH, T_MUSH, T_MONK, T_GHOST, T_DEMON, T_SPIDER, T_WOLF,
       T_GEM, T_KNIGHT, T_WIZARD, T_RANGER, T_SWORD, T_AXE, T_DAGGER, T_SHIELD, T_POTR, T_POTG,
       T_TREETOP, T_TREEBOT,
       T_GRASSCLOVER, T_GRASSPEBBLE, T_SAND, T_SIGN, T_BARREL, T_CRATE, T_CHEST, T_N };

// ---- weapons + passives: presentation only (the sim owns behaviour) ----
static const int WEP_ICO[WEP_COUNT] = { T_DAGGER, T_SWORD, T_SHIELD, T_POTR, T_AXE };
static const char *wep_name(int i)
{
    switch (i) {
        case WEP_MAGO:      return GT("Dardi magici", "Magic bolts");
        case WEP_FRUSTA:    return GT("Frusta", "Whip");
        case WEP_GUARDIANO: return GT("Guardiano", "Guardian");
        case WEP_PIROMANE:  return GT("Piromane", "Pyromancer");
        default:            return GT("Cecchino", "Sniper");
    }
}
static const char *wep_desc(int i)
{
    switch (i) {
        case WEP_MAGO:      return GT("Dardi che inseguono il nemico", "Bolts that seek the nearest");
        case WEP_FRUSTA:    return GT("Sferza ai lati, molto danno", "Lashes your sides, hits hard");
        case WEP_GUARDIANO: return GT("Sfere che ti orbitano attorno", "Orbs that circle around you");
        case WEP_PIROMANE:  return GT("Onda di fuoco tutt'intorno", "A ring of fire all around");
        default:            return GT("Colpi veloci che trapassano", "Fast shots that pierce");
    }
}
// ---- passives (global buffs; one card improves the whole arsenal, VS-style) ----
enum { PSV_MIGHT, PSV_HASTE, PSV_AREA, PSV_SPEED, PSV_MAGNET, PSV_REGEN, PSV_MAXHP, PSV_GARLIC, PSV_N };
static const int PSV_ICO[PSV_N] = { T_AXE, T_POTG, T_POTR, T_POTG, T_GEM, T_POTG, T_POTR, T_SHIELD };
static const int PSV_CAP[PSV_N] = { 5, 5, 5, 5, 6, 3, 99, 5 };
static const char *psv_name(int p)
{
    switch (p) {
        case PSV_MIGHT:  return GT("Potenza", "Might");
        case PSV_HASTE:  return GT("Cadenza", "Haste");
        case PSV_AREA:   return GT("Ampiezza", "Area");
        case PSV_SPEED:  return GT("Velocita", "Speed");
        case PSV_MAGNET: return GT("Magnete", "Magnet");
        case PSV_REGEN:  return GT("Rigenera", "Regen");
        case PSV_MAXHP:  return GT("Vita+", "Max HP+");
        default:         return GT("Aglio", "Garlic");
    }
}
static const char *psv_desc(int p)
{
    switch (p) {
        case PSV_MIGHT:  return GT("+1 danno a tutte le armi", "+1 damage on every weapon");
        case PSV_HASTE:  return GT("Le armi sparano piu' spesso", "Weapons fire more often");
        case PSV_AREA:   return GT("Piu' area e piu' colpi", "More area, more projectiles");
        case PSV_SPEED:  return GT("Ti muovi piu' veloce", "You move faster");
        case PSV_MAGNET: return GT("Attira i cristalli da lontano", "Pulls gems from further away");
        case PSV_REGEN:  return GT("Recuperi vita col tempo", "Regain health over time");
        case PSV_MAXHP:  return GT("+25 vita massima", "+25 max health");
        default:         return GT("Un'aura che ferisce", "An aura that hurts on touch");
    }
}

// a single level-up offer: level/learn a weapon, or take a passive
enum { OFF_WEP_NEW, OFF_WEP_LVL, OFF_PSV };
typedef struct { uint8_t kind; uint8_t id; } Offer;
static Offer s_off[3];

// ---- heroes: sprite + distinct starting power (applied on run start) ----
enum { HERO_MONK, HERO_MAGO, HERO_CAV, HERO_RANGER, HERO_COUNT };
static const int HERO_TILE[HERO_COUNT] = { T_MONK, T_WIZARD, T_KNIGHT, T_RANGER };
static const char *hero_name(int h)
{
    switch (h) {
        case HERO_MONK: return GT("Monaco", "Monk");
        case HERO_MAGO: return GT("Mago", "Wizard");
        case HERO_CAV:  return GT("Cavaliere", "Knight");
        default:        return GT("Ranger", "Ranger");
    }
}
static const char *hero_power(int h)
{
    switch (h) {
        case HERO_MONK: return GT("Sfere orbitanti e aura", "Orbiting orbs and an aura");
        case HERO_MAGO: return GT("Dardi magici a ricerca", "Seeking magic bolts");
        case HERO_CAV:  return GT("Frusta, 150 di vita", "Whip, 150 health");
        default:        return GT("Cecchino perforante", "A piercing sniper");
    }
}
static int s_hero = 0;

static VS      *s_w = nullptr;
static TileAtlas s_atlas;                      // 4 KB heap chunks while open
static bool     s_atlas_ok = false;
static int      s_face = 0;                    // player facing: 0 = right, 1 = left
static int64_t  s_last_us = 0, s_poll_us = 0;

// enemy accent per type (fallback primitives)
static const uint16_t EN_COL[4] = { 0xF9A6 /*red*/, 0xFD20 /*orange*/, 0xC61F /*violet*/, 0x07FF /*cyan*/ };

static bool poll_fn(void);
static bool on_back(int key);

static inline const uint8_t *T(int i) { return tile_px(&s_atlas, i); }
static inline void blit_ent(int idx, int f, int cx, int cy, int D, int ol = 0x00) { tile_blit_sz(T(idx), f, cx, cy, D, ol); }

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

// ---- screen flow: one place sets fullscreen + hint for each state (menus keep the hint bar) ----
static void go(int gs)
{
    s_gs = gs;
    nucleo_app_set_fullscreen(gs >= GS_PLAY);
    switch (gs) {
        case GS_MENU:     nucleo_app_set_hint(GT("SU/GIU  INVIO scegli  Esc esci", "UP/DN  ENTER pick  Esc quit")); break;
        case GS_SETTINGS: nucleo_app_set_hint(GT("SU/GIU  SX/DX cambia  Esc ok", "UP/DN  L/R change  Esc done")); break;
        case GS_START:    nucleo_app_set_hint(GT("SX/DX eroe  INVIO gioca  Esc", "L/R hero  ENTER play  Esc")); break;
        default: break;
    }
    nucleo_app_request_draw();
}
static void start_run(void)
{
    vs_init(s_w, (uint32_t)esp_timer_get_time()); apply_hero(s_w, s_hero);
    s_cfg.recenter = true;                             // neutral = pose at start ("hold it, then play")
    s_face = 0; s_wave_flash = 0; s_hit_flash = 0; s_new_record = false;
    sfx(SFX_START);
    go(GS_PLAY);
}

static void on_enter(void)
{
    game_text_open("orde");
    if (!s_w) s_w = (VS *)calloc(1, sizeof *s_w);      // ~7 KB, only while playing
    s_atlas_ok = tile_atlas_load(&s_atlas, ATLAS_PATH, T_N);
    tile_cfg_load(&s_cfg, ORDE_CFG);
    load_best();
    if (s_w) vs_init(s_w, 1);                          // seed the world behind the title screen
    s_menu.sel = 0; s_menu.pos = 0; s_face = 0; s_last_us = s_poll_us = 0;
    nucleo_app_set_poll_handler(poll_fn);
    nucleo_app_set_back_handler(on_back);
    go(GS_MENU);
}

static void on_exit(void)
{
    tile_cfg_save(&s_cfg, ORDE_CFG);
    nucleo_app_set_fullscreen(false);
    free(s_w); s_w = nullptr;                          // back to zero RAM until relaunched
    tile_atlas_free(&s_atlas); s_atlas_ok = false;
    game_text_close();
}

// LEFT and BACK both arrive here (framework routing). LEFT = move/navigate; BACK = back / pause.
static bool on_back(int key)
{
    if (!s_w) return false;
    if (key == NK_LEFT) {
        if (s_gs == GS_START)    { s_hero = (s_hero + HERO_COUNT - 1) % HERO_COUNT; sfx(SFX_SELECT); nucleo_app_request_draw(); }
        if (s_gs == GS_SETTINGS) { tile_cfg_adjust(&s_cfg, s_setm.sel, -1); sfx(SFX_SELECT); nucleo_app_request_draw(); }
        if (s_gs == GS_LEVELUP)  { if (s_up_sel > 0) s_up_sel--; sfx(SFX_SELECT); nucleo_app_request_draw(); }
        return true;                                  // LEFT in play is a held movement key
    }
    switch (s_gs) {                                    // Esc
        case GS_MENU:     return false;                // top level -> let the app close
        case GS_SETTINGS: tile_cfg_save(&s_cfg, ORDE_CFG); break;
        case GS_PLAY: case GS_LVLANIM: case GS_LEVELUP:
            if (s_gs != GS_PLAY && s_w->pending_up <= 0) s_w->pending_up = 1;   // the chooser comes back on resume
            sfx(SFX_SELECT); go(GS_PAUSE); return true;                      // the run is paused, not lost
        default: break;                                // hero select / pause / game over -> title
    }
    sfx(SFX_SELECT); go(GS_MENU);
    return true;
}

static void on_key(int key, char ch)
{
    if (!s_w) return;
    switch (s_gs) {
        case GS_MENU:
            if (gui::menu_key(s_menu, key, 2)) sfx(SFX_SELECT);
            else if (key == NK_ENTER || key == NK_RIGHT) { sfx(SFX_SELECT); if (s_menu.sel == 0) go(GS_START); else { s_setm.sel = 0; s_setm.pos = 0; go(GS_SETTINGS); } return; }
            break;
        case GS_SETTINGS:
            if (gui::menu_key(s_setm, key, tile_cfg_rows())) sfx(SFX_SELECT);
            else if (key == NK_RIGHT || key == NK_ENTER) { tile_cfg_adjust(&s_cfg, s_setm.sel, +1); sfx(SFX_SELECT); }
            break;
        case GS_START:
            if (key == NK_ENTER) { start_run(); return; }
            if (key == NK_RIGHT || key == NK_DOWN) { s_hero = (s_hero + 1) % HERO_COUNT; sfx(SFX_SELECT); }
            else if (key == NK_UP) { s_hero = (s_hero + HERO_COUNT - 1) % HERO_COUNT; sfx(SFX_SELECT); }
            break;
        case GS_PAUSE:
            if (key == NK_ENTER) go(s_w->pending_up > 0 ? GS_LEVELUP : GS_PLAY);
            break;
        case GS_OVER:
            if (key == NK_ENTER) { start_run(); return; }   // play again with the same hero
            break;
        case GS_LEVELUP:
            if (key == NK_UP)   { if (s_up_sel > 0) s_up_sel--; sfx(SFX_SELECT); }
            else if (key == NK_DOWN || key == NK_RIGHT) { if (s_up_sel < 2) s_up_sel++; sfx(SFX_SELECT); }
            else if (key == NK_ENTER || (ch >= '1' && ch <= '3')) {
                if (ch >= '1' && ch <= '3') s_up_sel = ch - '1';
                Offer o = s_off[s_up_sel];
                if (o.kind == OFF_PSV) apply_psv(s_w, o.id); else vs_give_weapon(s_w, o.id);
                s_w->pending_up--;
                if (s_w->pending_up > 0) roll_ups(s_w); else go(GS_PLAY);
                sfx(SFX_LEVELUP);
            }
            break;
        default:
            break;                                    // movement is read from held keys each step (read_move)
    }
    nucleo_app_request_draw();
}

// Player velocity from held keys — W/E up, A/S down, I left, O right + the arrow chars (;/./,//), several
// at once = diagonals. COMPLEMENTARY: on the ADV, if no key is held, the tilt sensor drives movement
// (analog, dead-zoned). Keys always take priority so the two never fight.
static void read_move(VS *w)
{
    vfix vx = 0, vy = 0;
    if (nucleo_kbd_char_down('w') || nucleo_kbd_char_down('e') || nucleo_kbd_char_down(';')) vy -= MOVE;   // up
    if (nucleo_kbd_char_down('a') || nucleo_kbd_char_down('s') || nucleo_kbd_char_down('.')) vy += MOVE;   // down
    if (nucleo_kbd_char_down('i') || nucleo_kbd_char_down(','))  { vx -= MOVE; s_face = 1; }               // left
    if (nucleo_kbd_char_down('o') || nucleo_kbd_char_down('/'))  { vx += MOVE; s_face = 0; }               // right

    if (tile_cfg_recenter_key(&s_cfg)) sfx(SFX_SELECT);                 // Ctrl / Space / C: new tilt neutral
    float mx, my;
    if (vx || vy) {
        if (vx && vy) { vx = (vx * 181) >> 8; vy = (vy * 181) >> 8; }   // normalise diagonal (~x0.707)
    } else if (tile_cfg_tilt(&s_cfg, &mx, &my)) {                       // no keys -> tilt (ADV only)
        float maxv = (float)VS_TOFIX(1) * (1.0f + (float)s_cfg.sens * 0.6f);   // px/frame at full deviation
        vx = (vfix)(mx * maxv); vy = (vfix)(my * maxv);
        if (vx < 0) s_face = 1; else if (vx > 0) s_face = 0;
    }
    // Movement-speed passive: scale the target by (6 + up_speed)/6 (~+17%/rank). The whip needs a facing,
    // and the sim is authoritative for it, so publish the current facing every frame.
    if (w->up_speed) { vx = vx * (6 + w->up_speed) / 6; vy = vy * (6 + w->up_speed) / 6; }
    w->pface = (uint8_t)s_face;
    // Ease the velocity toward the target: accelerates over ~2-3 frames, coasts briefly to a stop.
    w->ppvx += (vx - w->ppvx) >> 1;
    w->ppvy += (vy - w->ppvy) >> 1;
}

// One 30 Hz step of play: input, the sim, then its events -> sound, flashes and screen changes.
static void step(void)
{
    VS *w = s_w;
    if (s_gs == GS_LVLANIM) { if (++s_anim_t >= 16) { roll_ups(w); go(GS_LEVELUP); } return; }
    read_move(w);
    vs_step(w);
    uint16_t ev = w->ev;
    if (s_hit_flash > 0) s_hit_flash--;
    if (s_wave_flash > 0) s_wave_flash--;
    if (ev & VS_EV_HURT) { sfx(SFX_HURT); s_hit_flash = 4; }
    if (ev & VS_EV_WAVE) { sfx(SFX_WAVE); s_wave_flash = 10; }
    if (ev & VS_EV_BOSS) sfx(SFX_BOSS);
    if (ev & VS_EV_BOMB) sfx(SFX_BOMB);
    if (ev & VS_EV_HEAL) sfx(SFX_HEAL);
    if (ev & VS_EV_NOVA) sfx(SFX_SHOT);
    if (ev & VS_EV_KILL) sfx(SFX_DIE);
    if (ev & VS_EV_GEM)  sfx(SFX_PICKUP);
    if (w->php <= 0) {                                   // death wins over a level-up due on the same frame
        sfx(SFX_OVER);
        int secs = (int)(w->frame / 30);
        if (secs > s_best_secs)      { s_best_secs = secs;   s_new_record = true; }
        if (w->kills > s_best_kills) { s_best_kills = w->kills; s_new_record = true; }
        if (s_new_record) save_best();
        go(GS_OVER);
    } else if (w->pending_up > 0) { s_anim_t = 0; sfx(SFX_LEVELUP); s_gs = GS_LVLANIM; }
}

static bool poll_fn(void)
{
    int64_t now = esp_timer_get_time();
    int dt = s_poll_us ? (int)((now - s_poll_us) / 1000) : 0;
    s_poll_us = now;
    if (!s_w) return false;
    if (s_gs == GS_MENU)     return gui::menu_tick(s_menu, dt);   // still menus push nothing
    if (s_gs == GS_SETTINGS) return gui::menu_tick(s_setm, dt);
    if (s_gs != GS_PLAY && s_gs != GS_LVLANIM) return false;      // cards repaint on input only
    if (now - s_last_us < STEP_US) return false;
    s_last_us = now;
    step();
    return true;
}

// ---- render helpers (camera-relative, culled) ----
static inline int SX(vfix wx, int cam_x) { return VS_TOINT(wx) - cam_x; }
static inline int SY(vfix wy, int cam_y) { return VS_TOINT(wy) - cam_y; }

static void draw_hud(VS *w)
{
    // Bordered, high-contrast HP bar (php/phpmax, tri-colour danger read) + the XP bar under it, over a
    // dark dithered band; timer / HP number / level + kills in Font2 with drop shadows.
    int hp_pct = w->phpmax > 0 ? (w->php * 100 / w->phpmax) : 0;
    hp_pct = hp_pct < 0 ? 0 : (hp_pct > 100 ? 100 : hp_pct);
    bool low = hp_pct <= 25 && (w->frame & 8);
    uint16_t hp_col = low ? C_YELLOW : (hp_pct > 60 ? C_GREEN : (hp_pct > 25 ? C_YELLOW : C_RED));

    const int TOP = 2;
    d.fillRect(0, 0, W, TOP + 11, 0x0000);
    d.drawRect(1, TOP + 1, W - 2, 6, 0xFFFF);                    // HP track outline
    d.fillRect(2, TOP + 2, W - 4, 4, 0x2965);
    int hp_w = (W - 4) * hp_pct / 100;
    if (hp_w > 0) d.fillRect(2, TOP + 2, hp_w, 4, hp_col);
    int req = vs_xp_req(w->plevel);                              // the sim's own level rule
    int xp_w = (W - 4) * (w->pxp < req ? w->pxp : req) / req;
    if (xp_w > 0) d.fillRect(2, TOP + 8, xp_w, 2, C_BLUE);

    char b[28];
    int secs = (int)(w->frame / 30);
    const int TY = TOP + 11;
    snprintf(b, sizeof b, "%02d:%02d", secs / 60, secs % 60);
    gui::text(b, 3, TY, 0, gui::F_SMALL, 0xFFFF, 0x0000);
    snprintf(b, sizeof b, "%d/%d", w->php < 0 ? 0 : w->php, w->phpmax);
    gui::text(b, W / 2, TY, 1, gui::F_SMALL, hp_col, 0x0000);
    snprintf(b, sizeof b, "Lv%d  x%d", w->plevel, w->kills);
    gui::text(b, W - 4, TY, 2, gui::F_SMALL, 0xFFE0, 0x0000);

    // BOSS bar: a wide red bar across the bottom while a minute-boss lives
    if (w->boss_alive) {
        int bhp = 0;
        for (int i = 0; i < VS_MAX_EN; i++) if (w->ealive[i] && w->eboss[i]) { bhp = w->ehp[i]; break; }
        int by = H - 8, den = w->boss_hpmax > 0 ? w->boss_hpmax : 1;
        d.fillRect(0, by, W, 8, 0x0000);
        d.drawRect(6, by + 1, W - 12, 5, 0xF81F);
        int bw = bhp > 0 ? (W - 14) * bhp / den : 0; if (bw > W - 14) bw = W - 14;
        if (bw > 0) d.fillRect(7, by + 2, bw, 3, (w->frame & 4) ? 0xF800 : 0xFC10);
        gui::text(GT("BOSS", "BOSS"), W / 2, by - 15, 1, gui::F_SMALL, 0xFFFF, 0x0000);
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
                // single tufts and flowers -> a varied, less tiled-looking field.
                uint32_t patch = (uint32_t)((wcx >> 1) * 2654435761u) ^ (uint32_t)((wcy >> 1) * 40503u);
                int t = T_GRASS;
                if ((patch & 7) == 0)                t = T_DIRT;
                else if (((patch >> 3) & 31) == 7)   t = T_SAND;
                else if ((patch & 15) == 3)          t = T_GRASS2;
                else if ((hsh & 7) == 0)             t = (hsh & 64) ? T_GRASSCLOVER : T_GRASS2;
                else if ((hsh & 63) == 9)            t = T_GRASSPEBBLE;
                else if ((hsh & 31) == 5)            t = T_FLOWER;
                int px = tx * 16 - ox, py = ty * 16 - oy;
                tile_blit_op(T(t), px, py);
                // 2-tall tree props (sparse): canopy on the cell above, only if that cell planted nothing
                if (((hsh >> 5) & 63) == 0) {
                    uint32_t hsh_up = (uint32_t)(wcx * 73856093) ^ (uint32_t)((wcy - 1) * 19349663);
                    bool above_clear = ((hsh_up >> 5) & 63) != 0 && ((hsh_up >> 9) & 15) != 0 && ((hsh_up >> 13) & 31) != 0;
                    if (above_clear) { tile_blit_key(T(T_TREEBOT), px, py); tile_blit_key(T(T_TREETOP), px, py - 16); }
                }
                else if (((hsh >> 9) & 15) == 0)   tile_blit_key(T(T_BUSH), px, py);
                else if (((hsh >> 13) & 31) == 0)  tile_blit_key(T(T_MUSH), px, py);
                else if (((hsh >> 18) & 255) == 3)  tile_blit_key(T(T_SIGN), px, py);    // rare camp debris
                else if (((hsh >> 18) & 255) == 9)  tile_blit_key(T(T_CHEST), px, py);
                else if (((hsh >> 26) & 63) == 5)   tile_blit_key(T(T_BARREL), px, py);
                else if (((hsh >> 20) & 127) == 11) tile_blit_key(T(T_CRATE), px, py);
            }
    } else {
        d.fillScreen(0x0841);
        for (int y = -(cam_y & 15); y < H; y += 16)
            for (int x = -(cam_x & 15); x < W; x += 16)
                d.drawPixel(x, y, 0x18E3);
    }

    // garlic aura (monk power): a stippled green light pool + a pulsing ring around the player
    if (w->aura_r > 4) {
        int pr = w->aura_r + (int)((w->frame >> 2) & 3);
        tile_dither_ellipse(W / 2, H / 2, pr, pr, TILE_RGB332(40, 200, 60));
        d.drawCircle(W / 2, H / 2, pr, 0x3FE6);
        d.drawCircle(W / 2, H / 2, pr - 1, 0x2F44);
    }

    // xp gems on the ground: bounce + spin-glint. Fat gems (elite drops, gval>1) are bigger + blue-white.
    for (int i = 0; i < VS_MAX_GEM; i++) {
        if (!w->galive[i]) continue;
        int sx = SX(w->gx[i], cam_x), sy = SY(w->gy[i], cam_y);
        if (sx < -8 || sx > W + 8 || sy < -8 || sy > H + 8) continue;
        int ph  = (int)(((w->frame >> 2) + (uint32_t)i * 3) & 7);
        int coy = sy - ((ph < 4 ? ph : 8 - ph) >> 1);           // 0..2 px bounce (triangle wave)
        bool fat = w->gval[i] > 1;
        d.drawCircle(sx, coy, fat ? 4 : 3, fat ? 0x2C9F : 0x8400);
        d.fillCircle(sx, coy, fat ? 3 : 2, fat ? 0x5DFF : 0xFEA0);
        int sh = (int)(((w->frame >> 1) + (uint32_t)i) & 7);
        if (sh < 4) d.drawFastVLine(sx - 2 + sh, coy - 1, 3, 0xFFF2);   // shine sweeps -> spin glint
    }

    // pickups (heal chicken / bomb) — stationary, pulse to draw the eye
    for (int k = 0; k < 8; k++) {
        if (!w->kalive[k]) continue;
        int sx = SX(w->kx[k], cam_x), sy = SY(w->ky[k], cam_y);
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

    // death puffs: a white pop ring + four sparks flying out (bigger for elites and bosses)
    for (int k = 0; k < VS_NFX; k++) {
        if (!w->fxt[k]) continue;
        int sx = w->fxx[k] - cam_x, sy = w->fxy[k] - cam_y, age = 10 - w->fxt[k];
        int r = (3 + age * 2) * (1 + w->fxk[k]);
        d.drawCircle(sx, sy, r, age < 4 ? 0xFFFF : 0xC618);
        for (int j = 0; j < 4; j++) d.fillRect(sx + vs_cos32[j * 8 + 4] * r / 200, sy + vs_sin32[j * 8 + 4] * r / 200, 2, 2, 0xFFE0);
    }

    // Guardiano orbs — drawn from the SAME table + formula the sim damages with (vs_sim.c guardiano_tick),
    // so the visual and the hitbox always agree. The player is screen-centre.
    for (int wi = 0; wi < w->wcount; wi++) {
        if (w->wtype[wi] != WEP_GUARDIANO) continue;
        int cnt = 2 + w->wlevel[wi], R = 26 + w->up_area * 4;
        for (int o = 0; o < cnt; o++) {
            int idx = (int)(((w->frame >> 1) + (uint32_t)(o * 32 / cnt)) & 31);
            int ox = W / 2 + vs_cos32[idx] * R / 256, oy = H / 2 + vs_sin32[idx] * R / 256;
            d.fillCircle(ox, oy, 4, 0x2C9F);
            d.fillCircle(ox, oy, 2, 0xBEFF);
        }
    }

    // projectiles: distinct look per weapon kind so the arsenal reads at a glance.
    for (int p = 0; p < VS_MAX_PROJ; p++) {
        if (!w->palive[p]) continue;
        int sx = SX(w->px[p], cam_x), sy = SY(w->py[p], cam_y);
        if (sx < -24 || sx > W + 24 || sy < -24 || sy > H + 24) continue;
        int tx = sx - VS_TOINT(w->pvx[p]) / 2, ty = sy - VS_TOINT(w->pvy[p]) / 2;
        switch (w->pkind[p]) {
            case PK_WHIP: {                             // a bright arc slash on the facing side
                int dir = w->pdmg[p] >= 0 ? 1 : -1;     // pdmg carries the side for whip markers
                int a0 = (int)(w->plife[p]);            // 7..1: sweep the arc as it fades
                for (int a = -2; a <= 2; a++) d.drawFastHLine(sx - (dir < 0 ? 14 : 0), sy + a * 4, 14, a0 > 3 ? 0xFFFF : 0xC618);
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

    // enemies: big outlined sprite facing the player, 1px bob, strong camera cull. Elites are larger with
    // a purple threat ring + HP bar; bosses pulse a red-purple aura.
    for (int i = 0; i < VS_MAX_EN; i++) {
        if (!w->ealive[i]) continue;
        int sx = SX(w->ex[i], cam_x), sy = SY(w->ey[i], cam_y);
        if (sx < -30 || sx > W + 30 || sy < -30 || sy > H + 30) continue;
        int sz = w->eboss[i] ? 58 : (w->eelite[i] ? 42 : (w->eranged[i] ? 22 : 28));
        if (w->eboss[i]) {
            int pr = (sz / 2) + 4 + (int)((w->frame >> 1) & 5);
            d.drawCircle(sx, sy, pr, 0xF81F); d.drawCircle(sx, sy, pr - 2, 0x901F);
        } else if (w->eelite[i]) {
            d.drawCircle(sx, sy, (sz / 2) + 3 + (int)((w->frame >> 2) & 3), 0xC01F);
        }
        if (s_atlas_ok) {
            int f = (w->ppx < w->ex[i]) ? 1 : 0;
            int eb = (int)(((w->frame >> 2) + (uint32_t)i) & 1);
            int ol = w->ehflash[i] ? 0xFF : w->eboss[i] || w->eelite[i] ? TILE_RGB332(120, 0, 160) : 0x00;   // white outline = hit flash
            blit_ent(T_GHOST + (w->etype[i] & 3), f, sx, sy - eb, sz, ol);
        } else { d.fillCircle(sx, sy, w->eboss[i] ? 10 : (w->eelite[i] ? 6 : (w->eranged[i] ? 2 : 3)), w->ehflash[i] ? 0xFFFF : EN_COL[w->etype[i] & 3]); d.drawPixel(sx, sy - 1, FG); }
        if (w->eranged[i] && !w->eboss[i] && ((w->frame >> 3) & 1)) d.fillCircle(sx, sy - sz / 2 - 3, 2, C_RED);
        if (w->eelite[i] && !w->eboss[i]) {             // slim HP bar over the elite (nominal ~130 max)
            d.fillRect(sx - 12, sy - sz / 2 - 6, 24, 3, 0x3186);
            int hw = w->ehp[i] > 0 ? (w->ehp[i] >= 130 ? 24 : w->ehp[i] * 24 / 130) : 0;
            d.fillRect(sx - 12, sy - sz / 2 - 6, hw, 3, 0xF800);
        }
    }

    // player (screen-centre, faces last move dir, gentle bob) — the chosen hero's sprite, white-outlined
    if (s_atlas_ok) {
        tile_dither_ellipse(W / 2, H / 2 + 14, 12, 4, 0x00);
        blit_ent(HERO_TILE[s_hero], s_face, W / 2, H / 2 - bob, 32, 0xFF);
    } else { d.fillCircle(W / 2, H / 2, 4, C_GREEN); d.drawCircle(W / 2, H / 2, 4, FG); }
}

// a small sprite in a disc (menu flourish, level-up card icon)
static void icon_disc(int idx, int cx, int cy, int r, uint16_t edge)
{
    d.fillCircle(cx, cy, r, 0x0000);
    d.drawCircle(cx, cy, r, edge);
    if (s_atlas_ok) tile_blit_sz(T(idx), 0, cx, cy, r * 3 / 2);
}

static void draw_menu(void)
{
    char sub[48];
    if (s_best_secs > 0 || s_best_kills > 0)
        snprintf(sub, sizeof sub, GT("Record %d:%02d  -  %d uccisi", "Best %d:%02d  -  %d kills"), s_best_secs / 60, s_best_secs % 60, s_best_kills);
    else snprintf(sub, sizeof sub, "%s", GT("Sopravvivi alle orde", "Survive the hordes"));
    int y = gui::title("Orde", sub, COL_ACC);
    if (s_atlas_ok) {                                  // a hero and a ghost face off around the title
        blit_ent(T_MONK, 0, 26, 18, 30, 0xFF);
        blit_ent(T_GHOST, 1, W - 26, 18, 30, 0x00);
    }
    const char *items[2] = { GT("Gioca", "Play"), GT("Impostazioni", "Settings") };
    gui::menu(s_menu, items, 2, y, nucleo_app_content_height(), COL_ACC);
}

// Hero select: the shared title header, the hero big in a spotlight, name + signature power.
static void draw_carousel(void)
{
    char sub[12]; snprintf(sub, sizeof sub, "%d / %d", s_hero + 1, HERO_COUNT);
    int y = gui::title(GT("Scegli l'eroe", "Choose your hero"), sub, COL_ACC);
    int cy = y + 22;
    tile_dither_ellipse(W / 2, cy + 20, 26, 5, 0x00);
    d.fillCircle(W / 2, cy, 24, gui::rgb(36, 36, 36));
    d.drawCircle(W / 2, cy, 24, COL_ACC);
    if (s_atlas_ok) blit_ent(HERO_TILE[s_hero], 0, W / 2, cy, 44, 0xFF);
    d.fillTriangle(16, cy, 28, cy - 10, 28, cy + 10, COL_ACC);                  // chevrons
    d.fillTriangle(W - 16, cy, W - 28, cy - 10, W - 28, cy + 10, COL_ACC);
    gui::text(hero_name(s_hero), 42, cy - 18, 0, gui::F_BODY, 0xFFFF, 0x0000);
    gui::text(hero_power(s_hero), W / 2, cy + 27, 1, gui::F_SMALL, 0x8FF3, 0x0000);
}

// VS-style flourish before the chooser: a golden light-ray burst from the hero + rising sparkles + a
// dropping banner, over the frozen game.
static void draw_lvlanim(void)
{
    int t = s_anim_t, cx = W / 2, cy = H / 2 - 6, rlen = 14 + t * 8;
    for (int k = 0; k < 8; k++) {
        int a = ((k + (t & 1)) & 7) * 4;                 // 8 rays from the 32-step table, 1-step shimmer
        d.drawLine(cx + vs_cos32[a] * 9 / 256, cy + vs_sin32[a] * 9 / 256, cx + vs_cos32[a] * rlen / 256, cy + vs_sin32[a] * rlen / 256, (k & 1) ? 0xFEA0 : 0xFFF2);
    }
    for (int k = 0; k < 8; k++) {                        // sparkles rising and fanning up
        int px = cx + ((k * 53) % 60) - 30, py = cy - t * 6 - k * 3;
        if (py > 8 && py < H) { d.fillRect(px, py, 2, 2, 0xFFE0); d.drawPixel(px, py - 1, 0xFFF6); }
    }
    const char *s = GT("LIVELLO SU!", "LEVEL UP!");
    int tw = gui::text_width(s, gui::F_TITLE);
    int by = 30 - (t < 5 ? (5 - t) * 3 : 0);             // banner drops in
    gui::panel(cx - tw / 2 - 10, by - 4, tw + 20, 30, 0x0000, 0xFEA0);
    gui::text(s, cx, by, 1, gui::F_TITLE, 0xFFF2, 0x8400);
}

// The chooser: all 3 cards visible at once (compare at a glance) — icon, name, NEW / next level, and a
// one-line description of what the card does.
static void draw_levelup(VS *w)
{
    gui::panel(4, 3, W - 10, H - 8, gui::rgb(0, 0, 36), 0xFFE0);
    gui::text(GT("LIVELLO SU! Scegli 1-3", "LEVEL UP! Pick 1-3"), W / 2, 5, 1, gui::F_BODY, 0xFFE0, 0x0000);
    for (int i = 0; i < 3; i++) {
        int cy = 25 + i * 35, sel = (i == s_up_sel);
        Offer o = s_off[i];
        const char *nm, *ds, *tag = nullptr; char lv[12]; uint16_t tagc = 0x8C71;
        if (o.kind == OFF_PSV) { nm = psv_name(o.id); ds = psv_desc(o.id); }
        else {
            nm = wep_name(o.id); ds = wep_desc(o.id);
            if (o.kind == OFF_WEP_NEW) { tag = GT("NUOVA", "NEW"); tagc = 0x8FF3; }
            else { snprintf(lv, sizeof lv, "Lv%d", vs_has_weapon(w, o.id) + 1); tag = lv; tagc = 0xFEA0; }
        }
        d.fillSmoothRoundRect(10, cy, W - 20, 32, 6, sel ? gui::rgb(36, 73, 109) : gui::rgb(18, 22, 40));
        if (sel) d.drawRoundRect(10, cy, W - 20, 32, 6, 0xFFE0);
        icon_disc(o.kind == OFF_PSV ? PSV_ICO[o.id] : WEP_ICO[o.id], 27, cy + 16, 13, sel ? 0xFEA0 : 0x4208);
        gui::text(nm, 46, cy + 2, 0, gui::F_BODY, sel ? 0xFFFF : 0xC618, 0x0000);
        if (tag) gui::text(tag, W - 16, cy + 3, 2, gui::F_SMALL, tagc, 0x0000);
        d.setTextColor(sel ? 0x8FF3 : 0x8C71); d.setCursor(46, cy + 21); d.print(ds);
    }
}

static void on_draw(void)
{
    VS *w = s_w;
    if (!w) { d.fillScreen(0x0000); return; }
    switch (s_gs) {                                     // full-screen menus — no world behind
        case GS_MENU:     draw_menu(); return;
        case GS_SETTINGS: tile_cfg_draw(&s_cfg, s_setm, COL_ACC); return;
        case GS_START:    draw_carousel(); return;
        default: break;
    }
    render_world(w);
    // Bomb pickup: the sim wiped the screen (want_bomb counts down) — a bright full flash under the HUD.
    if (w->want_bomb > 0) d.fillRect(0, 0, W, H, w->want_bomb > 5 ? 0xFFFF : (w->want_bomb > 2 ? 0xFFE0 : 0xC618));
    draw_hud(w);
    // Two distinct screen-edge flashes: a quick solid pulse for "you got hit" vs the slower alternating
    // "ambush incoming" telegraph.
    if (s_hit_flash > 0) d.drawRect(0, 0, W, H, 0xF800);
    if (s_wave_flash > 0) {
        uint16_t col = (s_wave_flash & 1) ? 0xF800 : 0x7800;
        d.drawRect(0, 0, W, H, col); d.drawRect(1, 1, W - 2, H - 2, col);
    }
    char l1[40], l2[40];
    int secs = (int)(w->frame / 30);
    switch (s_gs) {
        case GS_LVLANIM: draw_lvlanim(); break;
        case GS_LEVELUP: draw_levelup(w); break;
        case GS_PAUSE:
            snprintf(l1, sizeof l1, GT("%s  -  %d:%02d  -  Lv%d", "%s  -  %d:%02d  -  Lv%d"), hero_name(s_hero), secs / 60, secs % 60, w->plevel);
            gui::dialog(GT("Pausa", "Paused"), l1, nullptr, GT("INVIO riprendi   Esc esci", "ENTER resume   Esc quit"), COL_ACC);
            break;
        case GS_OVER:
            snprintf(l1, sizeof l1, GT("%s  %d:%02d  Lv%d  %d uccisi", "%s  %d:%02d  Lv%d  %d kills"), hero_name(s_hero), secs / 60, secs % 60, w->plevel, w->kills);
            if (s_new_record) snprintf(l2, sizeof l2, "%s", GT("NUOVO RECORD!", "NEW RECORD!"));
            else snprintf(l2, sizeof l2, GT("Record %d:%02d  -  %d uccisi", "Best %d:%02d  -  %d kills"), s_best_secs / 60, s_best_secs % 60, s_best_kills);
            gui::dialog(GT("Sconfitto", "Defeated"), l1, l2, GT("INVIO rigioca   Esc menu", "ENTER play again   Esc menu"), C_RED);
            break;
        default: break;
    }
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
