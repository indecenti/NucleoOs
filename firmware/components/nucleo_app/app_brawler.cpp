// app_brawler.cpp — SCORRIBANDA: THE SHELL. Ties the noir belt-scroll beat'em-up together.
//
// This is the central module: it OWNS the single game state `g` and the br_* helpers every other
// brawler_*.cpp leans on, runs the app lifecycle (on_enter/on_exit), routes input (taps -> attacks,
// held W/A/S/D -> movement, Esc/TAB -> pause, Left -> menu nav), drives the ~50 Hz main loop
// (poll: movement, AI, combat, fx, camera follow, KO/lives, level progression), composites every
// frame (night street -> shadows -> blood pools -> depth-sorted outlined fighters + attack tells -> blood
// drops + sparks -> HUD -> cards), and registers the app. It calls the other sections ONLY through brawler.h.
//
// Constraints (house rules): exclusive_flags = NX_NET_APP; state in `g` + fixed locals (the fx particles are
// one heap block while open); buffered d.* drawing; controls FIXED (E/S/A/D move, J punch, K kick, L/Space
// jump, Esc/TAB pause, repeated J = combo). Text in the OS language (GT).

#include "brawler.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "game_text.h"
#include "app_large.h"
#include "nucleo_touch.h"
#include "game_ui.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

extern "C" {
#include "nucleo_audio.h"
#include "esp_timer.h"
}

// ============================ the one shared state =============================
BrCtx g;
BrView br_v = { 1.0f, 0.0f, 0.0f };

// Panel (on_draw_large) touch: play input is read from the HELD-key matrix (nucleo_kbd_char_down), which a
// touch cannot set — so the field's gestures record a short intent here that poll() folds into the movement
// read and the attack edges (P1, solo/host): a drag on the street = a step in the finger's direction, a tap on
// a foe = walk up to it and punch (tap again: the combo), a tap on the street = walk there, on the hero = jump.
static int8_t s_tmvx, s_tmvz;      // drag move intent (x along street, z depth)
static float  s_tmv_t;             // seconds the move intent stays live
static bool   s_tpunch, s_tjump;   // one-shot taps
static int8_t s_gi;                // the foe a tap picked (g.f index), -1 = walk to (s_gx, s_gz)
static float  s_gx, s_gz, s_gt;    // tap goal + seconds it stays live

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
// One entry point for both heroes (P1 keyboard, P2 co-op stream). A punch chains if we're already mid-punch
// in the active window (reads as 1-2-3), else opens a fresh strike when free; airborne J/K = jump-kick.
void br_hero_act(Fighter *h, int act)
{
    if (!h || h->hp <= 0) return;
    if (act == 2) {                                   // jump: grounded + free only
        if (h->yoff > 0.0f || fighter_busy(h)) return;
        h->vy = 380.0f; h->st = BS_JUMP; h->anim = 0.0f; h->aspd = 1.0f / 0.55f;
        bsfx(BSFX_JUMP);
        return;
    }
    BrState mv = act ? BS_KICK : BS_PUNCH;
    if (h->yoff > 0.0f) { if (h->st != BS_JKICK) combat_begin_attack(h, BS_JKICK); return; }
    if (h->st == mv && h->anim > 0.45f) { combat_begin_attack(h, mv); return; }   // combo link
    if (!fighter_busy(h)) combat_begin_attack(h, mv);
}

// ============================ lifecycle: input =================================
// TAB toggles pause. The framework routes TAB to a registered tab handler (NOT on_key) before it would
// open the global Control Center, so pausing must live here.
static void tab_handler(void)
{
    if (g.screen == SC_PLAY)       { g.paused = true;  menu_goto(SC_PAUSE); }
    else if (g.screen == SC_PAUSE) { g.paused = false; g.screen = SC_PLAY;  }
    nucleo_app_request_draw();
}

static bool lg_key(int k, char ch);
static void drag_step(int k)
{
    s_tmvx = k == NK_RIGHT ? 1 : k == NK_LEFT ? -1 : 0;
    s_tmvz = k == NK_DOWN ? 1 : k == NK_UP ? -1 : 0;
    s_tmv_t = 0.18f; s_gt = 0.0f;
}
static void on_key(int k, char ch)
{
    // In play, ATTACKS are polled from the live keyboard matrix in poll() (edge-detected) so you can
    // move AND strike at once — on_key wouldn't deliver a J/K tap reliably while a WASD key is held.
    // Here on_key drives the menus, the panel's taps (lg_key) and its street drag (arrows: a short step).
    if (lg_key(k, ch)) return;
    if (g.screen == SC_PLAY && !g.paused) {
        if (k == NK_RIGHT || k == NK_UP || k == NK_DOWN) drag_step(k);
        return;
    }
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
    nucleo_app_request_draw();          // menus are drawn on demand (poll returns false on a still screen)
}

// Back/Left routing. Left is menu navigation (and so never closes the app). Esc/back: pause toggles in
// play, resumes from pause, closes the app only from the title screen, otherwise returns to the title.
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        if (g.screen == SC_PLAY) drag_step(NK_LEFT); else { menu_key(NK_LEFT, 0); nucleo_app_request_draw(); }
        return true;
    }
    // In any co-op state (lobby or in-game) Esc tears the ESP-NOW session down and returns to the menu —
    // no networked pause to keep in sync. Solo play is unaffected (g.net is false).
    if (g.net) { bnet_stop(); g.net = false; g.paused = false; menu_goto(SC_MENU); return true; }
    switch (g.screen) {
        case SC_PLAY:  g.paused = true;  menu_goto(SC_PAUSE); return true;   // the first Esc pauses...
        case SC_PAUSE: g.paused = false; menu_goto(SC_MENU);  return true;   // ...the second leaves the fight
        case SC_MENU:  return false;   // let the framework close the app
        case SC_HELP:  menu_goto(SC_OPT); return true;
        default:       menu_goto(SC_MENU); return true;
    }
}

// ============================ lifecycle: main loop =============================
static uint32_t s_last_poll = 0;   // reset on enter so the first frame after re-open isn't a huge dt
static const char *s_hint;         // footer text last handed to the OS (reset on enter)
static bool poll(void)
{
    g.now = br_now_ms();
    float dt = s_last_poll ? (g.now - s_last_poll) / 1000.0f : 0.0f;
    s_last_poll = g.now;
    if (dt < 0.0f) dt = 0.0f; else if (dt > 0.06f) dt = 0.06f;
    if (s_tmv_t > 0.0f) s_tmv_t -= dt;       // the panel's drag intent decays (a drag step = a short walk)

    // Fullscreen only while the action is on screen (pause still shows the field under the overlay);
    // every other screen keeps the OS footer with its hint (refreshed when the text changes).
    nucleo_app_set_fullscreen(g.screen == SC_PLAY);
    const char *hint = menu_hint();
    if (hint != s_hint) { s_hint = hint; nucleo_app_set_hint(hint); }

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
            if (g.banner_t > 0.0f) g.banner_t -= dt;
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
                if (mvx == 0.0f && mvz == 0.0f && s_tmv_t > 0.0f) { mvx = (float)s_tmvx; mvz = (float)s_tmvz; }   // panel drag
                else if (mvx == 0.0f && mvz == 0.0f && s_gt > 0.0f) {      // panel tap: walk to the goal (a foe: then punch)
                    s_gt -= dt;
                    float gx = s_gx, gz = s_gz;
                    const Fighter *e = s_gi >= 0 ? &g.f[s_gi] : nullptr;
                    if (e && (!e->on || e->is_hero || e->hp <= 0)) s_gt = 0.0f;
                    else if (e) {                                          // stand at 70% of the reach, on our side
                        float r = brawler_hero(h->kind)->reach * (0.7f + 0.5f * scene_scale(h->z) / 34.0f);
                        gx = e->x + (h->x < e->x ? -0.7f : 0.7f) * r; gz = e->z;
                    }
                    float dx = gx - h->x, dz = gz - h->z;
                    if (s_gt > 0.0f && fabsf(dx) < 5.0f && fabsf(dz) < 0.06f) {
                        if (e) { h->dir = e->x > h->x ? 1 : -1; s_tpunch = true; }
                        s_gt = 0.0f;
                    } else if (s_gt > 0.0f) {
                        mvx = fabsf(dx) < 5.0f ? 0.0f : dx > 0 ? 1.0f : -1.0f;
                        mvz = fabsf(dz) < 0.06f ? 0.0f : dz > 0 ? 1.0f : -1.0f;
                    }
                }
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
            // held). Rising edge = exactly one strike per press. br_hero_act gates on busy/airborne.
            {
                static bool pj = false, pk = false, pl = false;
                bool dj = nucleo_kbd_char_down('j');
                bool dk = nucleo_kbd_char_down('k');
                bool dl = nucleo_kbd_char_down('l') || nucleo_kbd_char_down(' ');
                Fighter *ha = br_hero(0);
                if (dj && !pj) br_hero_act(ha, 0);
                if (dk && !pk) br_hero_act(ha, 1);
                if (dl && !pl) br_hero_act(ha, 2);
                pj = dj; pk = dk; pl = dl;
                // the panel's taps (one-shot): fire once, then clear
                if (s_tpunch) { br_hero_act(ha, 0); s_tpunch = false; }
                if (s_tjump)  { br_hero_act(ha, 2); s_tjump  = false; }
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
            if (g.banner_t > 0.0f) g.banner_t -= dt;

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
                if (!hp_->on || !hp_->is_hero) continue;
                if (hp_->hp > 0) { if (hp_->cool > 0.0f) hp_->cool -= dt; continue; }   // respawn shield
                if (hp_->st != BS_DOWN) continue;
                hp_->cool -= dt;
                if (hp_->cool > 0.0f) continue;
                if (g.lives > 0) {
                    g.lives--;
                    const HeroDef *hd = brawler_hero(hp_->kind);
                    hp_->maxhp = hd ? hd->maxhp : 100; hp_->hp = hp_->maxhp;
                    hp_->x = clampf_(g.camx + BR_SW * 0.28f + p * 22.0f, g.camx + 12.0f, g.gatex - 12.0f);
                    hp_->z = 0.62f; hp_->vx = hp_->vz = hp_->vy = hp_->yoff = 0.0f;
                    hp_->dir = 1; hp_->st = BS_IDLE; hp_->anim = 0.0f; hp_->flash = 0.0f;
                    hp_->var = 0; hp_->hit_done = false;
                    hp_->cool = 2.0f;                  // 2 s untouchable (blinks): no instant re-KO in the crowd
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
            if (g.screen == SC_PLAY && levels_is_clear()) { menu_goto(SC_CLEAR); bsfx(BSFX_CLEAR); }
        }
    }

    g.shake.step(dt);
    // the fight always composites a fresh frame; a still menu / card returns false (the run loop then
    // pushes nothing) and is redrawn on input or when a screen changes
    return g.net || g.screen == SC_PLAY || g.screen == SC_LOBBY || menu_animating((int)(dt * 1000.0f));
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

// The fight's stage through the view br_v (compact 1:1, the panel 4/3): street, shadows, pools, fighters, blood.
static void draw_stage(void)
{
        const float K = br_v.k;
        scene_draw();

        // dark contact shadows under each fighter (before the bodies), flattened and scaled with depth;
        // a jumping fighter's shadow stays on the ground and shrinks with the height
        for (int i = 0; i < BR_MAXF; i++) {
            Fighter *fr = &g.f[i];
            if (!fr->on) continue;
            float sc = scene_scale(fr->z) * fighter_scale(fr);
            int rx = (int)(0.38f * sc - fr->yoff * 0.08f); if (rx < 3) rx = 3;
            fxfig::puddle(brX(br_screen_x(fr->x)), brY(scene_floor_y(fr->z) + g.shake.oy()), (int)(rx * K), (int)(rx * K) / 3 + 1, BR_INK);
        }

        brfx_draw_pools();   // ground blood decals (behind fighters)

        int order[BR_MAXF];
        int n = sort_by_depth(order);
        for (int idx = 0; idx < n; idx++) {
            Fighter fr = g.f[order[idx]];
            if (br_invuln(&fr) && ((g.now / 70) & 1)) continue;   // mercy / respawn frames: the hero blinks
            if (!fr.is_hero && fr.hp <= 0 && fr.st == BS_DOWN && fr.anim >= 1.0f && ((g.now / 80) & 1)) continue;   // a body blinks out
            float sc = scene_scale(fr.z) * fighter_scale(&fr) * K;
            float feetY = br_v.oy + (scene_floor_y(fr.z) - fr.yoff + g.shake.oy()) * K;
            float sx = br_v.ox + br_screen_x(fr.x) * K;
            // nearer lanes are bigger AND brighter: the far lane is graded into the stage's haze
            br_figure(&fr, sx, feetY, sc, (int)((1.0f - fr.z) * 90.0f), scene_haze(), scene_rim());
            float head = feetY - 2.05f * sc;
            if (!fr.is_hero && (fr.st == BS_PUNCH || fr.st == BS_KICK) && fr.anim < 0.28f) {
                // the TELL: a red-hot glint over a foe winding up, growing until the blow lands
                int r = (int)((2 + fr.anim * 12.0f) * K);
                d.fillTriangle((int)sx, (int)head - 4 - r, (int)sx - r, (int)head - 4, (int)sx + r, (int)head - 4, BR_BLOOD);
                d.drawPixel((int)sx, (int)head - 5, BR_WHITE);
            } else if (fr.is_hero && g.nplayers > 1) {
                uint16_t pc = fr.player ? br_rgb(109, 255, 255) : BR_GOLD;       // P1 / P2 marker
                d.fillTriangle((int)sx - 3, (int)head - 7, (int)sx + 3, (int)head - 7, (int)sx, (int)head - 3, pc);
            }
        }

        brfx_draw_drops();   // airborne blood (in front of fighters)
}
static void draw_game(void)
{
    if (g.screen == SC_PLAY || g.screen == SC_PAUSE || g.screen == SC_OVER || g.screen == SC_CLEAR) {
        draw_stage();
        hud_draw();
        menu_draw();         // the pause / game-over / cleared card over the frozen fight; a no-op on SC_PLAY
        return;
    }

    // Title / select / options / co-op / help — fully owned by the menu module.
    menu_draw();
}

// ===== two screens ===========================================================
// Panel (on_draw_large, lg::frame): the same 240x135 stage DRAWN natively at 4/3 (320x180, view br_v) under a
// 60 px HUD band, every screen laid out for 320x240: the fight (tap a foe = go punch it, tap the street = walk
// there, tap the hero = jump, drag = step), its cards (pause / game over / cleared: tap the rows), the title /
// options / co-op lists (tap = pick, again = open), the hero cards. Cardputer (on_draw while two screens): the
// live HP / lives / level / combo and the key map, never the fight.
#define ACC  BR_GOLD
#define RED  BR_BLOOD
#define LTOP 60                                   // the stage's top on the panel (under the HUD band)
static void lt(int x, int y, int f, uint16_t c, const char *s, uint8_t dt = top_left)
{
    lg::font(f); d.setTextDatum(dt); d.setTextColor(c); d.drawString(s, x, y); d.setTextDatum(top_left);
}
static void lrow(int i, int x, int y, int w, const char *label, bool sel)
{
    d.fillRoundRect(x, y, w, 32, 6, sel ? gui::mix(BR_INK, RED, 150) : gui::mix(BR_INK, BR_MUTED, 50));
    if (sel) d.drawRoundRect(x, y, w, 32, 6, ACC);
    lt(x + w / 2, y + 16, 1, sel ? BR_WHITE : BR_MUTED, label, middle_center);
    nucleo_touch_add(x, y, w, 32, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
}
static void lbar(int x, int y, int w, int h, float f, uint16_t col)
{
    d.fillRect(x - 1, y - 1, w + 2, h + 2, BR_INK); d.fillRect(x, y, w, h, br_rgb(36, 36, 85));
    if (f > 0) d.fillRect(x, y, (int)(w * f), h, col ? col : f < 0.3f ? RED : ACC);
}
static void lband(const char *title)              // the HUD band: back chevron + a title (or the fight's HUD)
{
    for (int b = 0; b < 4; b++) d.fillRect(0, b * 15, 320, 15, gui::mix(BR_INK, RED, 12 + b * 8));
    d.drawFastHLine(0, LTOP - 1, 320, gui::mix(BR_INK, RED, 160));
    for (int i = 0; i < 2; i++) { d.drawLine(8 + i, 15, 14 + i, 9, BR_WHITE); d.drawLine(8 + i, 15, 14 + i, 21, BR_WHITE); }
    nucleo_touch_add(0, 0, 40, 30, NK_BACK, 0);
    if (title) lt(26, 6, 2, ACC, title);
}
static void lhud(void)
{
    lband(nullptr);
    char b[40];
    for (int p = 0; p < g.nplayers && p < 2; p++) {
        Fighter *fr = br_hero(p);
        if (!fr) continue;
        const int x = p ? 196 : 34, w = p ? 116 : 120;
        lt(p ? x + w : x, 4, 1, brawler_hero_color(fr->kind), brawler_hero(fr->kind)->name, p ? top_right : top_left);
        lbar(x, 24, w, 8, fr->maxhp > 0 ? (float)(fr->hp < 0 ? 0 : fr->hp) / fr->maxhp : 0, 0);
    }
    for (int p = 0; p < g.lives && p < 5; p++) d.fillCircle(40 + p * 14, 46, 5, RED);
    snprintf(b, sizeof b, "%ld", g.score);
    const int sx = g.nplayers > 1 ? 160 : 312;
    lt(sx, 4, 2, BR_WHITE, b, g.nplayers > 1 ? top_center : top_right);
    const LevelDef *L = brawler_level(g.level);
    for (int w = 0; w < L->nwaves; w++) {
        bool done = w < g.wave || (w == g.wave && g.gatex >= g.level_len);
        d.fillCircle(sx - (g.nplayers > 1 ? (L->nwaves - 1) * 6 - w * 12 : (L->nwaves - 1 - w) * 12 + 4), 42, 4, done ? ACC : br_rgb(73, 73, 85));
    }
    const bool blink = (g.now >> 6) & 1;
    if (g.combo > 1) { snprintf(b, sizeof b, "COMBO x%d", g.combo); lt(160, LTOP + 8, 2, blink ? RED : ACC, b, top_center); }
    if (g.gatex >= g.level_len && brawler_live_enemies() == 0 && ((g.now / 300) & 1)) {
        lt(290, 124, 2, ACC, GT("VAI", "GO"), middle_right);
        d.fillTriangle(294, 110, 294, 138, 312, 124, ACC);
    }
    for (int i = 2; i < BR_MAXF; i++) {                 // the boss's bar along the bottom while it stands
        Fighter *e = &g.f[i];
        if (!e->on || e->kind != 3 || e->hp <= 0) continue;
        lt(8, 206, 0, br_rgb(182, 73, 255), GT("CAPOBANDA", "GANG BOSS"));
        lbar(8, 226, 304, 6, (float)e->hp / e->maxhp, br_rgb(182, 73, 255));
        break;
    }
    if (g.banner_t > 0.0f) {                             // the stage / boss banner slides in over the street
        float t = g.banner_t, in = (g.banner ? 2.0f : 2.2f) - t;
        float k = in < 0.25f ? in / 0.25f : t < 0.25f ? t / 0.25f : 1.0f;
        int ox = (int)(-(1.0f - k) * (1.0f - k) * 320), y = 100;
        d.fillRect(ox, y, 320, 46, BR_INK);
        d.drawFastHLine(ox, y, 320, g.banner ? RED : ACC); d.drawFastHLine(ox, y + 45, 320, g.banner ? RED : ACC);
        if (g.banner) snprintf(b, sizeof b, "%s", GT("ATTENZIONE", "WARNING"));
        else          snprintf(b, sizeof b, GT("STRADA %d", "STREET %d"), g.level + 1);
        lt(ox + 160, y + 4, 0, g.banner ? RED : ACC, b, top_center);
        lt(ox + 160, y + 22, 1, BR_WHITE, g.banner ? GT("Arriva il capobanda!", "Here comes the boss!") : brawler_level_name(g.level), top_center);
    }
}
static void lcard(const char *head, const char *l1, const char *l2, const char *r0, const char *r1)
{
    d.fillRoundRect(40, 72, 240, 158, 8, BR_INK);
    d.drawRoundRect(40, 72, 240, 158, 8, RED);
    lt(160, 82, 2, ACC, head, top_center);
    lt(160, 110, 0, BR_WHITE, l1, top_center);
    lt(160, 128, 0, BR_MUTED, l2, top_center);
    lrow(0, 56, 150, 208, r0, true);
    lrow(1, 56, 188, 208, r1, false);
}
static void lp_paint(void *)
{
    char l1[48], l2[48];
    br_v = { 4.0f / 3.0f, 0.0f, (float)LTOP };
    const bool fight = g.screen == SC_PLAY || g.screen == SC_PAUSE || g.screen == SC_OVER || g.screen == SC_CLEAR;
    if (fight) draw_stage(); else scene_draw();          // the menus stand on the night street too
    br_v = { 1.0f, 0.0f, 0.0f };
    switch (g.screen) {
    case SC_PLAY:
        lhud();
        lg::field(0, LTOP, 320, 240 - LTOP);
        nucleo_touch_add_pad(0, LTOP, 320, 240 - LTOP, 8);
        break;
    case SC_PAUSE:
        lhud();
        snprintf(l1, sizeof l1, GT("Punti %ld", "Score %ld"), g.score);
        lcard(GT("PAUSA", "PAUSED"), brawler_level_name(g.level), l1, GT("Riprendi", "Resume"), GT("Esci", "Quit"));
        break;
    case SC_OVER:
        lhud();
        snprintf(l1, sizeof l1, GT("Punti %ld", "Score %ld"), g.score);
        snprintf(l2, sizeof l2, "%s %d: %s", GT("Livello", "Level"), g.level + 1, brawler_level_name(g.level));
        lcard(GT("FINE PARTITA", "GAME OVER"), l1, l2, GT("Riprova", "Retry"), "Menu");
        break;
    case SC_CLEAR: {
        lhud();
        const bool last = g.level >= brawler_level_count() - 1;
        if (last) snprintf(l1, sizeof l1, "%s", GT("Hai ripulito la citta!", "You cleared the city!"));
        else      snprintf(l1, sizeof l1, "%s: %s", GT("Prossima", "Next"), brawler_level_name(g.level + 1));
        snprintf(l2, sizeof l2, GT("Punti %ld", "Score %ld"), g.score);
        lcard(last ? GT("VITTORIA!", "VICTORY!") : GT("ZONA RIPULITA", "AREA CLEARED"), l1, l2,
              last ? "Menu" : GT("Avanti", "Continue"), "Menu");
        break;
    }
    case SC_SEL: {
        lband(nullptr);
        lt(26, 8, 1, ACC, menu_sel_head());
        for (int i = 0; i < 3; i++) {
            const bool sel = g.sel == i;
            const int x = 8 + i * 104;
            d.fillRoundRect(x, 66, 96, 128, 6, sel ? gui::mix(BR_INK, RED, 120) : BR_INK);
            d.drawRoundRect(x, 66, 96, 128, 6, sel ? ACC : br_rgb(73, 73, 85));
            lt(x + 48, 70, 1, sel ? BR_WHITE : BR_MUTED, brawler_hero(i)->name, top_center);
            menu_hero(i, sel, x + 48, 170, 36);
            lt(x + 48, 174, 0, sel ? BR_WHITE : BR_MUTED, brawler_hero_style(i), top_center);
            nucleo_touch_add(x, 66, 96, 128, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
        }
        const char *tag[4] = { GT("VIT", "HP"), GT("VEL", "SPD"), GT("FOR", "POW"), GT("POR", "RCH") };
        d.fillRect(0, 198, 320, 42, BR_INK);
        for (int k = 0; k < 4; k++) {
            const int x = 8 + (k & 1) * 158, y = 202 + (k >> 1) * 18;
            lt(x, y, 0, BR_MUTED, tag[k]);
            d.fillRect(x + 40, y + 4, 104, 8, br_rgb(36, 36, 85));
            d.fillRect(x + 40, y + 4, (int)(104 * menu_stat(k, g.sel)), 8, k == 2 ? RED : BR_WHITE);
        }
        break;
    }
    case SC_HELP: {
        lband(GT("Comandi", "Controls"));
        const char *key[5], *act[5];
        menu_help(key, act);
        d.fillRoundRect(8, 68, 304, 164, 8, BR_INK);
        for (int i = 0; i < 5; i++) {
            lt(20, 78 + i * 30, 0, i == 1 ? RED : ACC, key[i]);
            lt(120, 78 + i * 30, 0, BR_WHITE, act[i]);
        }
        break;
    }
    case SC_LOBBY: {
        lband(GT("Co-op 2 giocatori", "Co-op 2 players"));
        d.fillRoundRect(30, 84, 260, 120, 8, BR_INK);
        d.drawRoundRect(30, 84, 260, 120, 8, RED);
        lt(160, 100, 2, ACC, g.is_host ? GT("Ospito...", "Hosting...") : GT("Cerco...", "Searching..."), top_center);
        lt(160, 134, 0, BR_WHITE, g.is_host ? GT("Attendo il 2o giocatore", "Waiting for player 2") : GT("Cerco una partita vicina", "Looking for a nearby game"), top_center);
        const int dn = (int)((g.now / 300) % 4);
        for (int k = 0; k < 3; k++) d.fillCircle(136 + k * 24, 180, k < dn ? 6 : 3, k < dn ? RED : BR_MUTED);
        break;
    }
    default: {                                            // title / options / co-op: the list
        const char *it[3]; char b[2][40];
        const int n = menu_rows(it, b), sel = menu_cursor();
        if (g.screen == SC_MENU) {
            lband("SCORRIBANDA");
            lt(312, 40, 0, BR_MUTED, GT("Picchiaduro a scorrimento", "Belt-scroll beat'em up"), top_right);
            menu_hero(0, true, 40, 226, 40);
            menu_hero(1, false, 280, 226, 40);
        } else lband(g.screen == SC_OPT ? GT("Opzioni", "Options") : GT("Co-op 2 giocatori", "Co-op 2 players"));
        for (int i = 0; i < n; i++) lrow(i, 76, 84 + i * 44, 168, it[i], sel == i);
        break;
    }
    }
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lp_paint, nullptr);
    nucleo_touch_commit();
}
// A tap on the fight: a foe (the front-most under the finger) = walk up and punch it, the hero = jump, the
// street = walk there.
static void lplay_tap(int tx, int ty)
{
    Fighter *h = br_hero(0);
    if (!h || h->hp <= 0) return;
    const float K = 4.0f / 3.0f;
    int pick = -1;
    for (int i = 0; i < BR_MAXF; i++) {
        const Fighter *e = &g.f[i];
        if (!e->on || e->hp <= 0) continue;
        float sc = scene_scale(e->z) * fighter_scale(e) * K, sx = br_screen_x(e->x) * K, fy = LTOP + (scene_floor_y(e->z) - e->yoff) * K;
        if (fabsf(tx - sx) > 0.55f * sc + 6 || ty < fy - 2.2f * sc - 4 || ty > fy + 8) continue;
        if (pick < 0 || e->z > g.f[pick].z || (g.f[pick].is_hero && !e->is_hero)) pick = i;
    }
    s_tmv_t = 0.0f;
    if (pick >= 0 && g.f[pick].is_hero) { s_tjump = true; return; }
    s_gi = (int8_t)pick; s_gt = 2.5f;
    s_gx = g.camx + tx / K;
    s_gz = clampf_(((ty - LTOP) / K - BR_BELT.horizonY) / (BR_BELT.frontY - BR_BELT.horizonY), 0.18f, 1.0f);
}
static bool lg_key(int k, char ch)
{
    int tx = 0, ty = 0;
    const bool fld = lg::tap(k, ch, &tx, &ty);
    const int ri = nucleo_touch_is_row(k, ch) ? ch - NUCLEO_TOUCH_ROW : -1;
    if (!fld && ri < 0) return false;
    switch (g.screen) {
    case SC_PLAY: if (fld && !g.paused) lplay_tap(tx, ty); return true;
    case SC_PAUSE: case SC_OVER: case SC_CLEAR:
        if (ri == 0) menu_key(NK_ENTER, 0);
        else if (ri == 1) on_back(NK_BACK);
        break;
    case SC_SEL:
        if (ri < 0 || ri > 2) return true;
        if (g.sel == ri) menu_key(NK_ENTER, 0); else { g.sel = ri; bsfx(BSFX_NAV); }
        break;
    case SC_MENU: case SC_OPT: case SC_COOP:
        if (ri >= 0) menu_pick(ri);
        break;
    default: return true;
    }
    nucleo_app_request_draw();
    return true;
}
static void draw_companion(void)
{
    int y = lg::comp_head("Scorribanda", ACC);
    if (g.screen == SC_PLAY || g.screen == SC_PAUSE || g.screen == SC_OVER || g.screen == SC_CLEAR) {
        Fighter *hero = br_hero(0);
        int hp = hero ? hero->hp : 0, mhp = hero ? hero->maxhp : 100;
        char l[40];
        snprintf(l, sizeof l, "HP %d/%d", hp < 0 ? 0 : hp, mhp);
        lg::font(2); d.setTextDatum(top_left); d.setTextColor(hp * 3 <= mhp ? BR_BLOOD : BR_GOLD, BG); d.drawString(l, 8, y + 4);
        snprintf(l, sizeof l, GT("Vite x%d", "Lives x%d"), g.lives);
        d.setTextDatum(top_right); d.setTextColor(BR_GOLD, BG); d.drawString(l, 232, y + 6); d.setTextDatum(top_left);
        lg::font(0); d.setTextColor(0xFFFF, BG);
        snprintf(l, sizeof l, "%s", brawler_level_name(g.level)); d.drawString(l, 8, y + 32);
        snprintf(l, sizeof l, GT("Punti %ld   Combo %d", "Score %ld   Combo %d"), g.score, g.combo);
        d.setTextColor(BR_MUTED, BG); d.drawString(l, 8, y + 48);
        if (g.screen == SC_OVER)       { lg::font(1); d.setTextColor(BR_BLOOD, BG); d.drawString(GT("FINE PARTITA", "GAME OVER"), 8, y + 66); }
        else if (g.screen == SC_CLEAR) { lg::font(1); d.setTextColor(BR_GOLD, BG);  d.drawString(GT("ZONA RIPULITA", "AREA CLEARED"), 8, y + 66); }
        else if (g.screen == SC_PAUSE) { lg::font(1); d.setTextColor(BR_GOLD, BG);  d.drawString(GT("PAUSA", "PAUSED"), 8, y + 66); }
        lg::comp_keys(g.screen == SC_PLAY ? GT("ESAD muovi  J pugno  K calcio  L salto", "ESAD move  J punch  K kick  L jump")
                                          : GT("INVIO / Esc", "ENTER / Esc"));
        return;
    }
    lg::font(1); d.setTextDatum(top_left); d.setTextColor(ACC, BG);
    const char *t = g.screen == SC_MENU ? "Scorribanda" : g.screen == SC_SEL ? GT("Scegli eroe", "Pick hero")
                   : g.screen == SC_OPT ? GT("Impostazioni", "Settings") : g.screen == SC_COOP ? GT("Co-op", "Co-op")
                   : g.screen == SC_LOBBY ? GT("In attesa", "Lobby") : GT("Guida", "Help");
    d.drawString(t, 8, y + 6);
    lg::font(0); d.setTextColor(0xFFFF, BG);
    if (g.screen == SC_SEL) { d.setTextColor(brawler_hero_color(g.sel), BG); d.drawString(brawler_hero(g.sel)->name, 8, y + 30); }
    else if (g.screen == SC_LOBBY) { d.drawString(GT("altro Cardputer...", "other Cardputer..."), 8, y + 30); }
    lg::comp_keys(GT("INVIO scegli   Esc indietro", "ENTER select   Esc back"));
}
static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    draw_game();
}

// ============================ lifecycle: enter / exit =========================
static void on_enter(void)
{
    game_text_open("brawler");
    memset(&g, 0, sizeof g);
    g.audio  = true;
    g.diff   = 1;            // normale
    g.screen = SC_MENU;
    g.paused = false;
    g.net    = false;
    g.nplayers = 1;
    g.lives  = 3;
    g.shake.reset();

    if (nucleo_audio_volume() < 40) nucleo_audio_set_volume(80);
    brfx_reset();

    s_last_poll = 0; s_hint = nullptr;
    s_tmvx = s_tmvz = 0; s_tmv_t = s_gt = 0.0f; s_tpunch = s_tjump = false;
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
    game_text_close();
}

// ============================ registration ====================================
extern "C" void nucleo_register_brawler(void)
{
    static const nucleo_app_def_t app = {
        "brawler", "Scorribanda", "Games",
        "Picchiaduro noir a scorrimento: silhouette, sangue, vs CPU e co-op",
        'S', C_RED,
        on_enter, on_key, nullptr, on_draw, on_exit,
        NX_NET_APP, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
