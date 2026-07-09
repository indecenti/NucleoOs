// Mini Vampire-Survivors — portable simulation core. See vs_sim.h.
// All integer / fixed-point. No sqrt, no atan2, no malloc in the frame loop.
//
// Multi-weapon build: the player carries several auto-firing weapons at once (the Vampire-Survivors
// core loop), each on its own cooldown and auto-targeting. Global "passive" stats (might/haste/area)
// buff every weapon, so one upgrade card can improve the whole kit. Plus elites, pickups, fat xp gems.
#include "vs_sim.h"

// 32-direction unit circle in 8.8 (cos, sin scaled by 256). Precomputed constant table — the
// 16-bit way to place spawns / aim / orbit without trig at runtime.
static const int16_t VS_COS[32] = {
    256, 251, 237, 213, 181, 142, 98, 50, 0, -50, -98, -142, -181, -213, -237, -251,
   -256,-251,-237,-213,-181,-142, -98,-50, 0,  50,  98,  142,  181,  213,  237,  251 };
static const int16_t VS_SIN[32] = {
      0,  50,  98, 142, 181, 213, 237, 251, 256, 251, 237, 213, 181, 142, 98, 50,
      0, -50, -98,-142,-181,-213,-237,-251,-256,-251,-237,-213,-181,-142, -98,-50 };

static uint32_t rnd(VS *w) { w->rng = w->rng * 1664525u + 1013904223u; return w->rng; }

// Enemy variety unlocks over time (Vampire-Survivors style): start with one type, add another every
// ~30 s up to all four. Tougher types (higher index) get a little more HP.
static int avail_types(VS *w) { int a = 1 + (int)(w->frame / 900); return a > 4 ? 4 : a; }

// Enemy HP: base-by-type + a level-linked bonus (capped) so the horde keeps pace with the player's
// own level-scaled weapons instead of staying flat once all 4 types unlock (~90 s in).
static int enemy_hp(VS *w, uint8_t ty)
{
    int bonus = w->plevel / 4; if (bonus > 12) bonus = 12;
    return 2 + ty + bonus;
}

static void spawn_gem(VS *w, vfix x, vfix y, int val)
{
    for (int i = 0; i < VS_MAX_GEM; i++) if (!w->galive[i]) {
        w->gx[i] = x; w->gy[i] = y; w->galive[i] = 1;
        w->gval[i] = (uint8_t)(val < 1 ? 1 : (val > 250 ? 250 : val));
        w->gem_count++; return;
    }
}

static void spawn_pickup(VS *w, vfix x, vfix y, int type)
{
    for (int i = 0; i < 8; i++) if (!w->kalive[i]) {
        w->kx[i] = x; w->ky[i] = y; w->kalive[i] = 1; w->ktype[i] = (uint8_t)type; return;
    }
}

// spawn one enemy on a ring around the player (off-camera)
static void spawn_enemy(VS *w)
{
    int slot = -1;
    for (int i = 0; i < VS_MAX_EN; i++) if (!w->ealive[i]) { slot = i; break; }
    if (slot < 0) return;
    int a = rnd(w) & 31;
    int rad = 280 + (int)(rnd(w) % 80);           // just outside the 512 px window
    w->ex[slot] = w->ppx + VS_TOFIX(1) * VS_COS[a] * rad / 256;
    w->ey[slot] = w->ppy + VS_TOFIX(1) * VS_SIN[a] * rad / 256;
    uint8_t ty = (uint8_t)(rnd(w) % avail_types(w));
    w->etype[slot] = ty;
    w->ehp[slot]   = (int16_t)enemy_hp(w, ty);
    w->ealive[slot] = 1;
    w->eranged[slot] = 0; w->eelite[slot] = 0; w->eboss[slot] = 0; w->ehflash[slot] = 0;   // reused slot may be stale
    w->en_count++;
}

// an elite / miniboss: much tankier + bigger, slower, always drops a fat gem and a pickup.
static void spawn_elite(VS *w)
{
    int slot = -1;
    for (int i = 0; i < VS_MAX_EN; i++) if (!w->ealive[i]) { slot = i; break; }
    if (slot < 0) return;
    int a = rnd(w) & 31, rad = 260 + (int)(rnd(w) % 40);
    w->ex[slot] = w->ppx + VS_TOFIX(1) * VS_COS[a] * rad / 256;
    w->ey[slot] = w->ppy + VS_TOFIX(1) * VS_SIN[a] * rad / 256;
    w->etype[slot] = (uint8_t)(rnd(w) & 3);
    w->ehp[slot]   = (int16_t)(enemy_hp(w, 3) * 6 + 24);
    w->ealive[slot] = 1; w->eranged[slot] = 0; w->eelite[slot] = 1; w->eboss[slot] = 0; w->ehflash[slot] = 0;
    w->en_count++;
}

// a minute BOSS: enormous HP, radial bullet-hell fire, a fat xp jackpot on death. One at a time.
static void spawn_boss(VS *w)
{
    if (w->boss_alive) return;
    int slot = -1;
    for (int i = 0; i < VS_MAX_EN; i++) if (!w->ealive[i]) { slot = i; break; }
    if (slot < 0) return;
    int a = rnd(w) & 31, rad = 300;
    w->ex[slot] = w->ppx + VS_TOFIX(1) * VS_COS[a] * rad / 256;
    w->ey[slot] = w->ppy + VS_TOFIX(1) * VS_SIN[a] * rad / 256;
    w->etype[slot] = (uint8_t)(rnd(w) & 3);
    int mins = 1 + (int)(w->frame / 1800);
    w->ehp[slot]   = (int16_t)(enemy_hp(w, 3) * 20 * mins + 120);   // scales with each minute survived
    w->ealive[slot] = 1; w->eranged[slot] = 1; w->eelite[slot] = 1; w->eboss[slot] = 1; w->ehflash[slot] = 0;
    w->efire_cd[slot] = 60;
    w->en_count++; w->boss_alive = 1; w->boss_hpmax = w->ehp[slot];
}

// ---- weapon inventory helpers (exposed for the app's level-up chooser) ----
int vs_has_weapon(VS *w, int wt)
{
    for (int i = 0; i < w->wcount; i++) if (w->wtype[i] == wt) return w->wlevel[i];
    return -1;
}
int vs_weapon_maxed(VS *w, int wt) { int l = vs_has_weapon(w, wt); return l >= VS_WLVL_MAX; }
int vs_give_weapon(VS *w, int wt)
{
    for (int i = 0; i < w->wcount; i++) if (w->wtype[i] == wt) {
        if (w->wlevel[i] < VS_WLVL_MAX) { w->wlevel[i]++; return 1; }
        return 0;
    }
    if (w->wcount < VS_MAX_WEP) {
        int i = w->wcount++;
        w->wtype[i] = (uint8_t)wt; w->wlevel[i] = 1; w->wcd[i] = (int16_t)(6 + i * 3);
        return 1;
    }
    return 0;
}

void vs_init(VS *w, uint32_t seed)
{
    for (int i = 0; i < (int)(sizeof *w); i++) ((uint8_t *)w)[i] = 0;
    w->rng = seed ? seed : 1u;
    w->ppx = VS_TOFIX(2048); w->ppy = VS_TOFIX(2048);   // middle of a large world
    w->php = w->phpmax = 100;
    w->wave_cd = 450;                                    // first encircle wave at ~15 s
    vs_give_weapon(w, WEP_MAGO);                          // fallback kit; the app's apply_hero overrides it
    for (int i = 0; i < 16; i++) spawn_enemy(w);          // small opening horde (less clumping)
}

// spawn `n` ranged shooters evenly on a ring around the player (the "encircle" ambush wave).
static void spawn_ring(VS *w, int n, int rad)
{
    for (int k = 0; k < n; k++) {
        int slot = -1;
        for (int i = 0; i < VS_MAX_EN; i++) if (!w->ealive[i]) { slot = i; break; }
        if (slot < 0) return;
        int a = (k * 32 / n) & 31;
        w->ex[slot] = w->ppx + VS_COS[a] * rad;
        w->ey[slot] = w->ppy + VS_SIN[a] * rad;
        uint8_t ty = (uint8_t)(rnd(w) % avail_types(w));
        w->etype[slot] = ty; w->ehp[slot] = (int16_t)enemy_hp(w, ty);
        w->ealive[slot] = 1; w->en_count++;
        w->eranged[slot] = 1; w->eelite[slot] = 0; w->eboss[slot] = 0; w->ehflash[slot] = 0;
        w->efire_cd[slot] = (int16_t)(20 + (rnd(w) % 50));   // stagger first shot: no synced alpha-strike
    }
}

// classify (dx,dy) into an 8-direction step at fixed speed S (no div, no trig)
static void steer(vfix dx, vfix dy, vfix S, vfix *ox, vfix *oy)
{
    vfix adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    if (ady * 10 < adx * 4) sy = 0;                       // near-horizontal
    else if (adx * 10 < ady * 4) sx = 0;                  // near-vertical
    if (sx && sy) { S = (S * 181) >> 8; }                 // diagonal ~*0.707
    *ox = sx * S; *oy = sy * S;
}

// rebuild the spatial hash from the live enemy pool (camera-centred window)
static void grid_build(VS *w)
{
    for (int i = 0; i < VS_GW * VS_GH; i++) w->head[i] = -1;
    w->grid_ox = w->ppx - VS_TOFIX(VS_GW * VS_CELL / 2);
    w->grid_oy = w->ppy - VS_TOFIX(VS_GH * VS_CELL / 2);
    for (int i = 0; i < VS_MAX_EN; i++) {
        if (!w->ealive[i]) { w->nxt[i] = -1; continue; }
        int cx = (VS_TOINT(w->ex[i]) - VS_TOINT(w->grid_ox)) / VS_CELL;
        int cy = (VS_TOINT(w->ey[i]) - VS_TOINT(w->grid_oy)) / VS_CELL;
        if (cx < 0 || cx >= VS_GW || cy < 0 || cy >= VS_GH) { w->nxt[i] = -1; continue; }
        int c = cy * VS_GW + cx;
        w->nxt[i] = w->head[c]; w->head[c] = (int16_t)i;
    }
}

static int cell_of(VS *w, vfix x, vfix y)
{
    int cx = (VS_TOINT(x) - VS_TOINT(w->grid_ox)) / VS_CELL;
    int cy = (VS_TOINT(y) - VS_TOINT(w->grid_oy)) / VS_CELL;
    if (cx < 0 || cx >= VS_GW || cy < 0 || cy >= VS_GH) return -1;
    return cy * VS_GW + cx;
}

static long sqd(vfix ax, vfix ay, vfix bx, vfix by)
{
    long dx = VS_TOINT(ax) - VS_TOINT(bx), dy = VS_TOINT(ay) - VS_TOINT(by);
    return dx * dx + dy * dy;
}

static int nearest_enemy(VS *w)
{
    int pc = cell_of(w, w->ppx, w->ppy);
    if (pc < 0) return -1;
    int pcx = pc % VS_GW, pcy = pc / VS_GW, best = -1; long bestd = 1L << 30;
    for (int dy = -5; dy <= 5; dy++) for (int dx = -5; dx <= 5; dx++) {
        int cx = pcx + dx, cy = pcy + dy;
        if (cx < 0 || cx >= VS_GW || cy < 0 || cy >= VS_GH) continue;
        for (int i = w->head[cy * VS_GW + cx]; i >= 0; i = w->nxt[i]) {
            if (!w->ealive[i]) continue;
            w->checks++;
            long d = sqd(w->ppx, w->ppy, w->ex[i], w->ey[i]);
            if (d < bestd) { bestd = d; best = i; }
        }
    }
    return best;
}

// apply `dmg` to enemy i; on death drop a gem (fat gem for elites) and tally the kill.
static void hurt_enemy(VS *w, int i, int dmg)
{
    w->ehp[i] -= dmg;
    w->ehflash[i] = 3;                                    // flash white for a few frames (hit juice)
    if (w->ehp[i] <= 0) {
        if (w->eboss[i]) {
            for (int g = 0; g < 12; g++) spawn_gem(w, w->ex[i], w->ey[i], 8);   // boss jackpot
            spawn_pickup(w, w->ex[i], w->ey[i], 0);       // and a heal
            w->boss_alive = 0; w->shake = 12;             // a satisfying kill-shake
        } else {
            spawn_gem(w, w->ex[i], w->ey[i], w->eelite[i] ? 6 : 1);
            if (w->eelite[i]) { int en = (int)(w->frame / 900); spawn_pickup(w, w->ex[i], w->ey[i], (en % 3 == 0) ? 1 : 0); }
        }
        w->ealive[i] = 0; w->en_count--; w->kills++;
    }
}

// damage every live enemy within R px of (cx,cy) — used by the fire-nova, garlic aura and bomb.
static void hit_radius(VS *w, vfix cx, vfix cy, int R, int dmg)
{
    long r2 = (long)R * R;
    int pc = cell_of(w, cx, cy); if (pc < 0) return;
    int pcx = pc % VS_GW, pcy = pc / VS_GW, span = R / VS_CELL + 1;
    for (int dy = -span; dy <= span; dy++) for (int dx = -span; dx <= span; dx++) {
        int gx = pcx + dx, gy = pcy + dy;
        if (gx < 0 || gx >= VS_GW || gy < 0 || gy >= VS_GH) continue;
        for (int i = w->head[gy * VS_GW + gx]; i >= 0; i = w->nxt[i]) {
            if (!w->ealive[i]) continue;
            w->checks++;
            if (sqd(cx, cy, w->ex[i], w->ey[i]) <= r2) hurt_enemy(w, i, dmg);
        }
    }
}

static int free_proj(VS *w) { for (int i = 0; i < VS_MAX_PROJ; i++) if (!w->palive[i]) return i; return -1; }
static int dmg_base(VS *w, int base) { return base + w->up_might; }

// ---- per-weapon fire routines (called on each weapon's own cooldown) ----
static void fire_mago(VS *w, int lv)                       // homing fan of bolts at the nearest enemy
{
    int t = nearest_enemy(w); if (t < 0) return;
    int shots = lv + w->up_area; if (shots > 7) shots = 7;
    int pierce = 1 + lv / 2;
    vfix bx, by; steer(w->ex[t] - w->ppx, w->ey[t] - w->ppy, VS_TOFIX(5), &bx, &by);
    vfix perpx = -by, perpy = bx;
    for (int s = 0; s < shots; s++) {
        int slot = free_proj(w); if (slot < 0) break;
        int off = s - (shots - 1) / 2;
        w->px[slot] = w->ppx; w->py[slot] = w->ppy;
        w->pvx[slot] = bx + (perpx * off) / 4; w->pvy[slot] = by + (perpy * off) / 4;
        w->plife[slot] = 40; w->ppierce[slot] = (uint8_t)pierce; w->powner[slot] = 0;
        w->pkind[slot] = PK_BOLT; w->pdmg[slot] = (int8_t)dmg_base(w, 1);
        w->palive[slot] = 1; w->pr_count++;
    }
}

static void fire_cecchino(VS *w, int lv)                   // fast piercing straight shots (single-target melt)
{
    int t = nearest_enemy(w); if (t < 0) return;
    int shots = 1 + (lv - 1) / 2;                          // 1,1,2,2,3
    int pierce = 3 + lv;
    vfix bx, by; steer(w->ex[t] - w->ppx, w->ey[t] - w->ppy, VS_TOFIX(8), &bx, &by);
    vfix perpx = -by, perpy = bx;
    for (int s = 0; s < shots; s++) {
        int slot = free_proj(w); if (slot < 0) break;
        int off = s - (shots - 1) / 2;
        w->px[slot] = w->ppx; w->py[slot] = w->ppy;
        w->pvx[slot] = bx + (perpx * off) / 6; w->pvy[slot] = by + (perpy * off) / 6;
        w->plife[slot] = 34; w->ppierce[slot] = (uint8_t)pierce; w->powner[slot] = 0;
        w->pkind[slot] = PK_SNIPE; w->pdmg[slot] = (int8_t)dmg_base(w, 2);
        w->palive[slot] = 1; w->pr_count++;
    }
}

static void fire_frusta(VS *w, int lv)                     // instant melee arc in the facing direction(s)
{
    int reach = 34 + w->up_area * 6 + lv * 5;
    long r2 = (long)reach * reach;
    int dmg = dmg_base(w, 2);
    int dir = w->pface ? -1 : 1;                            // primary side = facing; lv>=3 hits both sides
    int pc = cell_of(w, w->ppx, w->ppy);
    if (pc >= 0) {
        int pcx = pc % VS_GW, pcy = pc / VS_GW, span = reach / VS_CELL + 1;
        for (int dy = -span; dy <= span; dy++) for (int dx = -span; dx <= span; dx++) {
            int gx = pcx + dx, gy = pcy + dy;
            if (gx < 0 || gx >= VS_GW || gy < 0 || gy >= VS_GH) continue;
            for (int i = w->head[gy * VS_GW + gx]; i >= 0; i = w->nxt[i]) {
                if (!w->ealive[i]) continue;
                int side = VS_TOINT(w->ex[i]) - VS_TOINT(w->ppx);
                if (lv < 3 && side * dir < 0) continue;     // single arc: only the facing side
                w->checks++;
                if (sqd(w->ppx, w->ppy, w->ex[i], w->ey[i]) <= r2) hurt_enemy(w, i, dmg);
            }
        }
    }
    // visual-only markers the app animates as a slash (no travel, no collision — damage already applied)
    for (int s = 0; s < (lv >= 3 ? 2 : 1); s++) {
        int slot = free_proj(w); if (slot < 0) break;
        int sd = (s == 0) ? dir : -dir;
        w->px[slot] = w->ppx + VS_TOFIX(reach / 2) * sd; w->py[slot] = w->ppy;
        w->pvx[slot] = 0; w->pvy[slot] = 0;
        w->plife[slot] = 7; w->ppierce[slot] = 0; w->powner[slot] = 0;
        w->pkind[slot] = PK_WHIP; w->pdmg[slot] = (int8_t)sd;   // pdmg carries the side for the renderer
        w->palive[slot] = 1; w->pr_count++;
    }
}

static void fire_piromane(VS *w, int lv)                   // periodic AoE nova burst centred on the player
{
    int R = 40 + lv * 10 + w->up_area * 6;
    hit_radius(w, w->ppx, w->ppy, R, dmg_base(w, 2));
    int slot = free_proj(w); if (slot < 0) return;         // expanding-ring visual
    w->px[slot] = w->ppx; w->py[slot] = w->ppy; w->pvx[slot] = 0; w->pvy[slot] = 0;
    w->plife[slot] = 12; w->ppierce[slot] = 0; w->powner[slot] = 0;
    w->pkind[slot] = PK_NOVA; w->pdmg[slot] = (int8_t)(R > 120 ? 120 : R);   // carry radius for the ring anim
    w->palive[slot] = 1; w->pr_count++;
}

static int wep_cd(VS *w, int wt, int lv)
{
    int base;
    switch (wt) {
        case WEP_MAGO:     base = 14 - lv * 2; break;
        case WEP_FRUSTA:   base = 44 - lv * 4; break;
        case WEP_PIROMANE: base = 104 - lv * 8; break;
        case WEP_CECCHINO: base = 34 - lv * 3; break;
        default:           base = 60;          break;
    }
    base -= w->up_haste * 2;
    return base < 6 ? 6 : base;
}

// Guardiano orbs — persistent orbiting damage (drawn by the app from this same formula). Returns
// nothing; ticks contact damage a few times a second so orbs sweep the crowd without instantly melting.
static void guardiano_tick(VS *w, int lv)
{
    if ((w->frame % 5) != 0) return;                       // ~6 ticks/s
    int cnt = 2 + lv, R = 26 + w->up_area * 4, dmg = dmg_base(w, 1);
    int ppx = VS_TOINT(w->ppx), ppy = VS_TOINT(w->ppy);
    for (int o = 0; o < cnt; o++) {
        int idx = (int)(((w->frame >> 1) + (uint32_t)(o * 32 / cnt)) & 31);
        vfix ox = VS_TOFIX(ppx + VS_COS[idx] * R / 256);
        vfix oy = VS_TOFIX(ppy + VS_SIN[idx] * R / 256);
        hit_radius(w, ox, oy, 9, dmg);
    }
}

void vs_step(VS *w)
{
    w->frame++;
    w->checks = 0;

    // 0) player: integrate velocity (the app eases + owns deceleration, so no friction here)
    w->ppx += w->ppvx; w->ppy += w->ppvy;

    grid_build(w);

    // 1) enemies steer toward the player. Far-off-camera enemies are NOT updated (RAM/CPU discipline).
    //    Ranged (ring-wave) enemies also fire a bolt at the player on their own cooldown. Elites crawl.
    const int CULL = 640;
    int ppx = VS_TOINT(w->ppx), ppy = VS_TOINT(w->ppy);
    for (int i = 0; i < VS_MAX_EN; i++) {
        if (!w->ealive[i]) continue;
        int ax = VS_TOINT(w->ex[i]) - ppx, ay = VS_TOINT(w->ey[i]) - ppy;
        if (ax < -CULL || ax > CULL || ay < -CULL || ay > CULL) continue;
        if (w->ehflash[i]) w->ehflash[i]--;                // hit-flash decay
        vfix ES = w->eboss[i] ? 84 : (w->eelite[i] ? 108 : 154);   // boss crawls, elites slower
        vfix ox, oy; steer(w->ppx - w->ex[i], w->ppy - w->ey[i], ES, &ox, &oy);
        w->ex[i] += ox; w->ey[i] += oy;

        if (w->eranged[i]) {
            if (w->efire_cd[i] > 0) w->efire_cd[i]--;
            if (w->efire_cd[i] == 0) {
                if (w->eboss[i]) {                          // BOSS: radial bullet-hell ring
                    for (int a = 0; a < 32; a += 2) {
                        int slot = free_proj(w); if (slot < 0) break;
                        w->px[slot] = w->ex[i]; w->py[slot] = w->ey[i];
                        w->pvx[slot] = VS_COS[a] * 3 / 2; w->pvy[slot] = VS_SIN[a] * 3 / 2;
                        w->plife[slot] = 80; w->ppierce[slot] = 1; w->powner[slot] = 1;
                        w->pkind[slot] = PK_BOLT; w->pdmg[slot] = 0;
                        w->palive[slot] = 1; w->pr_count++;
                    }
                    w->efire_cd[i] = 75;
                } else {
                    int slot = free_proj(w);
                    if (slot >= 0) {
                        vfix bx, by; steer(w->ppx - w->ex[i], w->ppy - w->ey[i], VS_TOFIX(3), &bx, &by);
                        w->px[slot] = w->ex[i]; w->py[slot] = w->ey[i];
                        w->pvx[slot] = bx; w->pvy[slot] = by;
                        w->plife[slot] = 90; w->ppierce[slot] = 1; w->powner[slot] = 1;
                        w->pkind[slot] = PK_BOLT; w->pdmg[slot] = 0;
                        w->palive[slot] = 1; w->pr_count++;
                    }
                    w->efire_cd[i] = (int16_t)(90 + (rnd(w) % 40));
                }
            }
        }
    }
    if (w->shake > 0) w->shake--;

    // 2) WEAPONS: every owned weapon fires independently on its own cooldown (auto-target). This is the
    //    Vampire-Survivors core — the player moves, the arsenal does the shooting.
    for (int wi = 0; wi < w->wcount; wi++) {
        if (w->wtype[wi] == WEP_GUARDIANO) { guardiano_tick(w, w->wlevel[wi]); continue; }
        if (w->wcd[wi] > 0) { w->wcd[wi]--; continue; }
        int lv = w->wlevel[wi];
        switch (w->wtype[wi]) {
            case WEP_MAGO:     fire_mago(w, lv);     break;
            case WEP_FRUSTA:   fire_frusta(w, lv);   break;
            case WEP_PIROMANE: fire_piromane(w, lv); break;
            case WEP_CECCHINO: fire_cecchino(w, lv); break;
        }
        w->wcd[wi] = (int16_t)wep_cd(w, w->wtype[wi], lv);
    }

    // 3) projectiles move + hit their target. PK_WHIP/PK_NOVA are visual-only (damage already applied
    //    at spawn) — they just age. Enemy bolts (powner=1) only ever threaten the player: O(1) check.
    const long HIT2 = 8 * 8;
    for (int p = 0; p < VS_MAX_PROJ; p++) {
        if (!w->palive[p]) continue;
        if (--w->plife[p] == 0) { w->palive[p] = 0; w->pr_count--; continue; }
        if (w->pkind[p] == PK_WHIP || w->pkind[p] == PK_NOVA) continue;   // visual only

        w->px[p] += w->pvx[p]; w->py[p] += w->pvy[p];

        if (w->powner[p]) {                            // enemy bolt: hits the player, never pierces
            if (sqd(w->px[p], w->py[p], w->ppx, w->ppy) <= HIT2) {
                w->php -= 3; w->palive[p] = 0; w->pr_count--; if (w->shake < 4) w->shake = 4;
            }
            continue;
        }

        int pc = cell_of(w, w->px[p], w->py[p]); if (pc < 0) continue;
        int pcx = pc % VS_GW, pcy = pc / VS_GW; int done = 0;
        for (int dy = -1; dy <= 1 && !done; dy++) for (int dx = -1; dx <= 1 && !done; dx++) {
            int cx = pcx + dx, cy = pcy + dy;
            if (cx < 0 || cx >= VS_GW || cy < 0 || cy >= VS_GH) continue;
            for (int i = w->head[cy * VS_GW + cx]; i >= 0; i = w->nxt[i]) {
                if (!w->ealive[i]) continue;
                w->checks++;
                if (sqd(w->px[p], w->py[p], w->ex[i], w->ey[i]) <= HIT2) {
                    hurt_enemy(w, i, w->pdmg[p]);
                    if (--w->ppierce[p] == 0) { w->palive[p] = 0; w->pr_count--; done = 1; break; }
                }
            }
        }
    }

    // 4) player takes contact damage from nearby enemies (throttled). Elites hit harder.
    if (w->hurt_cd > 0) w->hurt_cd--;
    {
        int pc = cell_of(w, w->ppx, w->ppy);
        if (pc >= 0) {
            int pcx = pc % VS_GW, pcy = pc / VS_GW; const long CON2 = 7 * 7;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int cx = pcx + dx, cy = pcy + dy;
                if (cx < 0 || cx >= VS_GW || cy < 0 || cy >= VS_GH) continue;
                for (int i = w->head[cy * VS_GW + cx]; i >= 0; i = w->nxt[i]) {
                    if (!w->ealive[i]) continue;
                    w->checks++;
                    if (w->hurt_cd == 0 && sqd(w->ppx, w->ppy, w->ex[i], w->ey[i]) <= CON2) {
                        w->php -= w->eboss[i] ? 8 : (w->eelite[i] ? 4 : 1); w->hurt_cd = 20;
                        if (w->shake < 4) w->shake = 4;
                    }
                }
            }
        }
    }

    // 5) gems magnet toward the player + collect (magnet radius grows with the upgrade)
    long mag = (long)(44 + w->up_magnet * 20); mag *= mag;
    for (int g = 0; g < VS_MAX_GEM; g++) {
        if (!w->galive[g]) continue;
        long d = sqd(w->gx[g], w->gy[g], w->ppx, w->ppy);
        if (d < mag) { vfix ox, oy; steer(w->ppx - w->gx[g], w->ppy - w->gy[g], VS_TOFIX(3), &ox, &oy); w->gx[g] += ox; w->gy[g] += oy; }
        if (d < 8 * 8) {
            w->galive[g] = 0; w->gem_count--; w->pxp += w->gval[g];
            int req = 8 + w->plevel * 3;
            while (w->pxp >= req) { w->pxp -= req; w->plevel++; w->pending_up++; req = 8 + w->plevel * 3; }
        }
    }

    // 5a) pickups (heal / bomb): stationary, collected on contact within a generous radius
    for (int k = 0; k < 8; k++) {
        if (!w->kalive[k]) continue;
        if (sqd(w->kx[k], w->ky[k], w->ppx, w->ppy) <= 12 * 12) {
            w->kalive[k] = 0;
            if (w->ktype[k] == 0) { w->php += 25; if (w->php > w->phpmax) w->php = w->phpmax; }
            else                  { w->want_bomb = 8; }     // bomb: clear the screen (below) + flash (app)
        }
    }
    if (w->want_bomb) {
        if (w->want_bomb == 8) hit_radius(w, w->ppx, w->ppy, 300, 200);   // wipe everything on screen
        w->want_bomb--;
    }

    // 5b) slow HP regen if that upgrade was taken
    if (w->up_regen && (w->frame % 45) == 0 && w->php < w->phpmax) w->php += w->up_regen;

    // 5c) GARLIC aura (Monk innate): a ring that damages enemies inside it every few frames.
    w->aura_r = w->up_garlic > 0 ? (20 + w->up_garlic * 14) : 0;
    if (w->aura_r > 0 && (w->frame & 7) == 0) hit_radius(w, w->ppx, w->ppy, w->aura_r, dmg_base(w, 1));

    // 6) director: sustained horde + periodic ENCIRCLE ambush + an elite every ~30 s
    if (--w->wave_cd <= 0) {
        int ring_rad = 150 + w->plevel * 4; if (ring_rad > 230) ring_rad = 230;
        spawn_ring(w, 10 + w->plevel, ring_rad);
        w->wave_cd = 540;
    }
    // a BOSS every minute (1800 f); an elite on the OFF half-minutes so pacing stays lively
    if (w->frame >= 1800 && (w->frame % 1800) == 0) spawn_boss(w);
    else if (w->frame >= 900 && (w->frame % 900) == 0) spawn_elite(w);
    int target = 20 + (int)(w->frame / 90) + w->plevel;
    if (target > 85) target = 85;
    for (int guard = 0; w->en_count < target && guard < VS_MAX_EN; guard++) spawn_enemy(w);

    if (w->checks > w->checks_max) w->checks_max = w->checks;
}
