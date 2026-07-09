// app_brawler.cpp — SCORRIBANDA: THE SHELL. Ties the noir belt-scroll beat'em-up together.
//
// This is the central module: it OWNS the single game state `g` and the br_* helpers every other
// brawler_*.cpp leans on, runs the app lifecycle (on_enter/on_exit), routes input (taps -> attacks,
// held W/A/S/D -> movement, Esc/TAB -> pause, Left -> menu nav), drives the ~50 Hz main loop
// (poll: movement, AI, combat, fx, camera follow, KO/lives, level progression), composites every
// frame (scene -> shadows -> blood pools -> depth-sorted silhouettes -> blood drops -> HUD -> menus),
// and registers the app. It calls the other sections ONLY through brawler.h.
//
// Constraints (house rules): exclusive_flags = NX_NET_APP; ALL state in `g` + fixed locals (NO heap);
// buffered d.* drawing; controls FIXED (W/A/S/D move, J punch, K kick, L/Space jump, Esc/TAB pause,
// repeated J = combo). Silhouette look, blood is the only bright colour. Bilingual IT/EN via g.lang.

#include "brawler.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include <math.h>
#include <string.h>

extern "C" {
#include "nucleo_audio.h"
#include "esp_timer.h"
}

// ============================ the one shared state =============================
BrCtx g;

// ============================ br_* helpers (shared across all modules) ==========
uint32_t br_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

uint16_t br_rgb(int r, int g_, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g_ < 0) g_ = 0; else if (g_ > 255) g_ = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g_ & 0xFC) << 3) | (b >> 3));
}

// xorshift32 — static seed, never zero (the trap that freezes the sequence).
uint32_t br_rnd(void)
{
    static uint32_t s = 0x1234abcdu;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
float br_frnd(void)  { return (br_rnd() & 0xFFFFFF) / (float)0x1000000; }   // 0..1
float br_frnd2(void) { return br_frnd() * 2.0f - 1.0f; }                    // -1..1

// World x -> screen x: subtract the camera, fold in screen-shake.
float br_screen_x(float worldx) { return worldx - g.camx + g.shake.ox(); }

// The hero in co-op slot `player` (0 or 1), or NULL if absent.
Fighter *br_hero(int player)
{
    if (player < 0 || player > 1) return NULL;
    Fighter *fr = &g.f[player];
    return (fr->on && fr->is_hero) ? fr : NULL;
}

void br_reset_fighters(void)
{
    for (int i = 0; i < BR_MAXF; i++) g.f[i].on = false;
}

// ============================ local helpers ===================================
static inline float clampf_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Per-fighter overall size multiplier (distinct builds: boss big, lama small...). Guards an unset 0 -> 1.0.
static inline float fighter_scale(const Fighter *fr)
{
    float s = fr->is_hero ? brawler_hero(fr->kind)->scale : brawler_enemy(fr->kind)->scale;
    return (s < 0.1f) ? 1.0f : s;
}

// ============================ input: attacks (taps) ===========================
// A hero "punch": chain it if we're already mid-punch in the active window (reads as 1-2-3), else open a
// fresh strike when free. combat_begin_attack walks the combo `var` itself for hero punches.
static void try_punch(Fighter *h)
{
    if (!h || h->hp <= 0) return;
    if (h->yoff > 0.0f) { combat_begin_attack(h, BS_JKICK); return; }   // airborne jab -> jump-kick
    if (h->st == BS_PUNCH && h->anim > 0.45f) { combat_begin_attack(h, BS_PUNCH); return; }  // combo link
    if (!fighter_busy(h)) combat_begin_attack(h, BS_PUNCH);
}
static void try_kick(Fighter *h)
{
    if (!h || h->hp <= 0) return;
    if (h->yoff > 0.0f) { combat_begin_attack(h, BS_JKICK); return; }   // airborne -> jump-kick
    if (h->st == BS_KICK && h->anim > 0.45f) { combat_begin_attack(h, BS_KICK); return; }
    if (!fighter_busy(h)) combat_begin_attack(h, BS_KICK);
}
static void try_jump(Fighter *h)
{
    if (!h || h->hp <= 0) return;
    if (h->yoff <= 0.0f && !fighter_busy(h)) {   // grounded + free
        h->vy = 380.0f;
        h->st = BS_JUMP;
        h->anim = 0.0f;
        h->aspd = 1.0f / 0.55f;
        bsfx(BSFX_JUMP);
    }
}

// ============================ lifecycle: input =================================
// TAB toggles pause. The framework routes TAB to a registered tab handler (NOT on_key) before it would
// open the global Control Center, so pausing must live here.
static void tab_handler(void)
{
    if (g.screen == SC_PLAY)       { g.paused = true;  menu_goto(SC_PAUSE); }
    else if (g.screen == SC_PAUSE) { g.paused = false; g.screen = SC_PLAY;  }
}

static void on_key(int k, char ch)
{
    // In play, ATTACKS are polled from the live keyboard matrix in poll() (edge-detected) so you can
    // move AND strike at once — on_key wouldn't deliver a J/K tap reliably while a WASD key is held.
    // Here on_key only drives the menus.
    if (g.screen == SC_PLAY && !g.paused) return;
    // Menus accept the SAME keys as gameplay so the controls are consistent (the ;/. arrow legends
    // still work): E/W up, S down, A left, D right, J/K/L/Space confirm. Without this the player
    // presses the movement keys in a menu and nothing scrolls.
    if (k == NK_CHAR) {
        char c = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        switch (c) {
            case 'e': case 'w': k = NK_UP;    break;
            case 's':           k = NK_DOWN;  break;
            case 'a':           k = NK_LEFT;  break;
            case 'd':           k = NK_RIGHT; break;
            case 'j': case 'k': case 'l': case ' ': k = NK_ENTER; break;
            default: break;
        }
    }
    menu_key(k, ch);
}

// Back/Left routing. Left is menu navigation (and so never closes the app). Esc/back: pause toggles in
// play, resumes from pause, closes the app only from the title screen, otherwise returns to the title.
static bool on_back(int key)
{
    if (key == NK_LEFT) { menu_key(NK_LEFT, 0); return true; }
    // In any co-op state (lobby or in-game) Esc tears the ESP-NOW session down and returns to the menu —
    // no networked pause to keep in sync. Solo play is unaffected (g.net is false).
    if (g.net) { bnet_stop(); g.net = false; g.paused = false; menu_goto(SC_MENU); return true; }
    switch (g.screen) {
        case SC_PLAY:  g.paused = true;  menu_goto(SC_PAUSE); return true;
        case SC_PAUSE: g.paused = false; g.screen = SC_PLAY;  return true;
        case SC_MENU:  return false;   // let the framework close the app
        default:       menu_goto(SC_MENU); return true;
    }
}

// ============================ lifecycle: main loop =============================
static uint32_t s_last_poll = 0;   // reset on enter so the first frame after re-open isn't a huge dt
static bool poll(void)
{
    g.now = br_now_ms();
    float dt = s_last_poll ? (g.now - s_last_poll) / 1000.0f : 0.0f;
    s_last_poll = g.now;
    if (dt < 0.0f) dt = 0.0f; else if (dt > 0.06f) dt = 0.06f;

    // Fullscreen only while the action is on screen (pause still shows the field under the overlay).
    nucleo_app_set_fullscreen(g.screen == SC_PLAY);

    // Co-op networking runs EVERY frame (lobby pairing + in-game stream). On the host bnet_poll applies
    // the guest's input and broadcasts the snapshot; on the guest it sends input and applies the host's
    // snapshot to g.f[] + camera. No-op when not in a co-op session.
    if (g.net) bnet_poll();

    // Co-op lobby: the moment the two Cardputers pair, start the match on both sides.
    if (g.screen == SC_LOBBY) {
        if (g.net && bnet_available()) menu_coop_start();
        g.shake.step(dt);
        return true;
    }

    if (g.screen == SC_PLAY && !g.paused) {
        if (g.net && !g.is_host) {
            // GUEST: the host is authoritative. The snapshot (applied in bnet_poll above) drives g.f[]
            // and the camera; we only render here. If the host drops, fall back to the menu.
            if (!bnet_available()) { bnet_stop(); g.net = false; menu_goto(SC_MENU); }
        } else if (g.hitstop > 0.0f) {
            g.hitstop -= dt;   // freeze the sim, keep drawing
        } else {
            // --- player 0 movement from physically-held W/A/S/D ---
            // Smoothed input velocity (px/s along the street, depth-units/s in z) carried frame to frame
            // so the hero has a little weight: it ramps up toward the target and coasts down when keys
            // release, instead of snapping. Lives here (shell-owned) so the Fighter struct is untouched.
            static float s_ivx = 0.0f, s_ivz = 0.0f;
            Fighter *h = br_hero(0);
            if (h && h->hp > 0 && !fighter_busy(h) && h->yoff <= 0.0f) {
                float mvx = (nucleo_kbd_char_down('d') ? 1.0f : 0.0f) - (nucleo_kbd_char_down('a') ? 1.0f : 0.0f);
                float mvz = (nucleo_kbd_char_down('s') ? 1.0f : 0.0f) - (nucleo_kbd_char_down('e') ? 1.0f : 0.0f);  // E = su (verso il fondo), S = giu'
                // Normalize the diagonal so it isn't sqrt(2) faster than a cardinal push.
                if (mvx != 0.0f && mvz != 0.0f) { mvx *= 0.7071f; mvz *= 0.7071f; }

                float spd = brawler_hero(h->kind)->speed;     // px/s street speed
                // Depth rate: cross the full z band (~0.82 units, 0.18..1.0) in ~0.85s -> ~0.97 units/s.
                const float ZSPD = 0.97f;
                float tvx = mvx * spd;
                float tvz = mvz * ZSPD;

                // Accelerate toward / decelerate from the target (weight without sluggishness).
                const float ACCEL = 12.0f, DECEL = 16.0f;
                float ax = (fabsf(tvx) > fabsf(s_ivx)) ? ACCEL : DECEL;
                float az = (fabsf(tvz) > fabsf(s_ivz)) ? ACCEL : DECEL;
                s_ivx += (tvx - s_ivx) * clampf_(dt * ax, 0.0f, 1.0f);
                s_ivz += (tvz - s_ivz) * clampf_(dt * az, 0.0f, 1.0f);
                if (fabsf(s_ivx) < 1.0f && tvx == 0.0f) s_ivx = 0.0f;
                if (fabsf(s_ivz) < 0.01f && tvz == 0.0f) s_ivz = 0.0f;

                bool moving = (s_ivx != 0.0f || s_ivz != 0.0f);
                if (moving) {
                    if (mvx > 0.0f) h->dir = 1;
                    else if (mvx < 0.0f) h->dir = -1;
                    h->x += s_ivx * dt;
                    h->z += s_ivz * dt;
                    h->z = clampf_(h->z, 0.18f, 1.0f);
                    h->x = clampf_(h->x, g.camx + 10.0f, g.gatex - 10.0f);
                    h->st = BS_WALK;
                    h->walkphase += dt * (4.0f + spd * 0.03f);
                } else if (h->st == BS_WALK || h->st == BS_IDLE) {
                    h->st = BS_IDLE;
                }
            } else {
                s_ivx = 0.0f;
                s_ivz = 0.0f;   // bleed momentum while busy / airborne / KO'd
            }

            // --- attacks polled from the LIVE matrix (edge-detected) -> move + strike at the same time ---
            // (movement reads char_down too; a tap through on_key wouldn't register while a WASD key is
            // held). Rising edge = exactly one strike per press. try_* themselves gate on busy/airborne.
            {
                static bool pj = false, pk = false, pl = false;
                bool dj = nucleo_kbd_char_down('j');
                bool dk = nucleo_kbd_char_down('k');
                bool dl = nucleo_kbd_char_down('l') || nucleo_kbd_char_down(' ');
                Fighter *ha = br_hero(0);
                if (ha) {
                    if (dj && !pj) try_punch(ha);
                    if (dk && !pk) try_kick(ha);
                    if (dl && !pl) try_jump(ha);
                }
                pj = dj; pk = dk; pl = dl;
            }

            // --- world step (host or solo; the guest never reaches here) ---
            levels_step(dt);
            enemies_ai(dt);
            combat_step(dt);
            combat_resolve(dt);
            brfx_step(dt);

            g.floorscroll += dt * 0.3f;
            if (g.floorscroll >= 1.0f) g.floorscroll -= 1.0f;

            if (g.combo_t > 0.0f) { g.combo_t -= dt; if (g.combo_t <= 0.0f) { g.combo_t = 0.0f; g.combo = 0; } }

            // --- camera follow: horizontal deadzone + look-ahead, eased, locked at the wave gate ---
            // The camera holds still while the hero stays inside a central band; only when he leaves it
            // does it ease toward a target that anchors him a little BEHIND the screen centre, with a
            // small lead in the facing direction so you see where you're heading. Clamp keeps the gate
            // and level ends honest; screen-shake stays a separate offset (g.shake), so no jitter here.
            if (h) {
                const float ANCHOR  = BR_SW * 0.42f;   // hero's resting screen-x (a touch left of centre)
                const float DEAD    = 28.0f;           // half-width of the no-scroll band (px)
                const float LOOK    = 34.0f;           // how far the camera leads in the facing dir (px)
                float hero_sx = h->x - g.camx;         // hero's current screen-x

                float hi = g.level_len - BR_SW;
                float gatehi = g.gatex - BR_SW;
                if (gatehi < hi) hi = gatehi;
                if (hi < 0.0f) hi = 0.0f;

                // Only retarget when the hero pushes past the deadzone edge; otherwise hold the camera.
                float over = 0.0f;
                if (hero_sx > ANCHOR + DEAD)      over = hero_sx - (ANCHOR + DEAD);
                else if (hero_sx < ANCHOR - DEAD) over = hero_sx - (ANCHOR - DEAD);

                if (over != 0.0f) {
                    float lead   = (float)h->dir * LOOK;
                    float target = g.camx + over + lead;
                    target = clampf_(target, 0.0f, hi);
                    g.camx += (target - g.camx) * clampf_(dt * 7.0f, 0.0f, 1.0f);
                } else {
                    g.camx = clampf_(g.camx, 0.0f, hi);   // honour gate/level shifts even while parked
                }
            }

            // --- KO / lives (per-hero, shared lives) ---
            // A KO'd hero (hp<=0) lies floored (combat holds it at BS_DOWN); after a short lie-time we
            // spend a shared life to respawn it, else take it off the field. No heroes left => game over.
            // This recovers a solo co-op death instead of soft-locking, and never leaves a walking hp<=0 hero.
            for (int p = 0; p < 2; p++) {
                Fighter *hp_ = &g.f[p];
                if (!hp_->on || !hp_->is_hero || hp_->hp > 0 || hp_->st != BS_DOWN) continue;
                hp_->cool -= dt;
                if (hp_->cool > 0.0f) continue;
                if (g.lives > 0) {
                    g.lives--;
                    const HeroDef *hd = brawler_hero(hp_->kind);
                    hp_->maxhp = hd ? hd->maxhp : 100; hp_->hp = hp_->maxhp;
                    hp_->x = clampf_(g.camx + BR_SW * 0.28f + p * 22.0f, g.camx + 12.0f, g.gatex - 12.0f);
                    hp_->z = 0.62f; hp_->vx = hp_->vz = hp_->vy = hp_->yoff = 0.0f;
                    hp_->dir = 1; hp_->st = BS_IDLE; hp_->anim = 0.0f; hp_->flash = 0.0f;
                    hp_->var = 0; hp_->hit_done = false; hp_->cool = 0.0f;
                    g.combo = 0; g.combo_t = 0.0f;
                } else {
                    hp_->on = false;   // out of lives -> this hero is gone
                }
            }
            {
                bool anyhero = false;
                for (int p = 0; p < 2; p++) if (g.f[p].on && g.f[p].is_hero) anyhero = true;
                if (!anyhero) { menu_goto(SC_OVER); bsfx(BSFX_OVER); }
            }

            // --- level cleared -> the clear screen drives next-level / victory (and resets the combo) ---
            if (g.screen == SC_PLAY && levels_is_clear()) { g.screen = SC_CLEAR; g.sel = 0; bsfx(BSFX_CLEAR); }
        }
    }

    g.shake.step(dt);
    return true;   // we drive our own clock; always composite a fresh frame
}

// ============================ lifecycle: draw =================================
// Insertion-sort the live-fighter indices by depth z ascending so far bodies paint first (painter order).
static int sort_by_depth(int *order)
{
    int n = 0;
    for (int i = 0; i < BR_MAXF; i++) {
        if (!g.f[i].on) continue;
        int j = n++;
        while (j > 0 && g.f[order[j - 1]].z > g.f[i].z) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    return n;
}

static void on_draw(void)
{
    if (g.screen == SC_PLAY || g.screen == SC_PAUSE) {
        scene_draw();

        // soft contact shadows under each fighter (drawn before the bodies). On the white "paper" floor
        // a dark blob would read as a hole, so the shadow is a LIGHT grey ellipse — still flattened and
        // scaled with depth (near = bigger), just toned to sit on the page. Greys-only, per the palette.
        for (int i = 0; i < BR_MAXF; i++) {
            Fighter *fr = &g.f[i];
            if (!fr->on) continue;
            float sc = scene_scale(fr->z) * fighter_scale(fr);
            int sx = (int)br_screen_x(fr->x);
            int sy = (int)(scene_floor_y(fr->z) + g.shake.oy());
            int rx = (int)(0.35f * sc); if (rx < 2) rx = 2;
            int ry = rx / 3; if (ry < 1) ry = 1;
            // far figures get the faintest grey, near ones a touch firmer — both light on white.
            uint16_t shc = (fr->z < 0.5f) ? BR_GREY_FAR : BR_GREY_MID;
            fxfig::puddle(sx, sy, rx, ry, shc);
        }

        brfx_draw_pools();   // ground blood decals (behind fighters)

        int order[BR_MAXF];
        int n = sort_by_depth(order);
        for (int idx = 0; idx < n; idx++) {
            Fighter fr = g.f[order[idx]];
            fxfig::Pt pose[fxfig::FX_NJ];
            fighter_pose(&fr, pose);
            float sc = scene_scale(fr.z) * fighter_scale(&fr);
            float feetY = scene_floor_y(fr.z) - fr.yoff;
            float girth = fr.is_hero ? brawler_hero(fr.kind)->girth : brawler_enemy(fr.kind)->girth;
            // White-look outline (passed as the figure's edge/rim arg): heroes get a pure-black edge so
            // the silhouette bites the paper; enemies get BR_ENEMY_DK so the steel-blue separates from
            // the white floor with a crisp darker hue-matched line. (Greys/black/blue only.)
            uint16_t body    = fighter_body(&fr);
            uint16_t outline = fr.is_hero ? 0 : fx3d::scl(body, 150, 255);  // per-type darker edge -> each foe's hue pops on paper
            fxfig::figure(pose, br_screen_x(fr.x), feetY + g.shake.oy(), sc, fr.dir,
                          body, outline, girth);
        }

        brfx_draw_drops();   // airborne blood (in front of fighters)
        hud_draw();
        menu_draw();         // pause overlay if any; a no-op on SC_PLAY
        return;
    }

    // Title / select / options / game-over / clear / help — fully owned by the menu module.
    menu_draw();
}

// ============================ lifecycle: enter / exit =========================
static void on_enter(void)
{
    memset(&g, 0, sizeof g);
    g.audio  = true;
    g.lang   = 0;            // Italiano di default
    g.diff   = 1;            // normale
    g.screen = SC_MENU;
    g.paused = false;
    g.net    = false;
    g.nplayers = 1;
    g.lives  = 3;
    g.shake.reset();

    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(80);
    bsfx_presynth();
    brfx_reset();

    s_last_poll = 0;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_handler);   // TAB pauses (else it would open the global Control Center)
    nucleo_app_set_fullscreen(false);
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    bnet_stop();
    nucleo_audio_stop();
    brfx_shutdown();   // free the heap particle Fields -> zero static RAM while the game is closed
}

// ============================ registration ====================================
extern "C" void nucleo_register_brawler(void)
{
    static const nucleo_app_def_t app = {
        "brawler", "Scorribanda", "Games",
        "Picchiaduro noir a scorrimento: silhouette, sangue, vs CPU e co-op",
        'S', C_RED,
        on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP
    };
    nucleo_app_register(&app);
}
