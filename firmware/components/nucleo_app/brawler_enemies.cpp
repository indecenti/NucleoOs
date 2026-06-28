// brawler_enemies.cpp — SCORRIBANDA: enemy roster + AI brains.
//
// Four noir thugs of distinct SIZE, SHAPE and TECHNIQUE so the fight reads like a Capcom belt-scroll, not
// a wall of identical punchers. The core promise: NOBODY hits continuously. Every foe runs a tiny
// per-slot state machine — APPROACH -> ATTACK(its signature sequence) -> RETREAT+RECOVER (back off a step
// and wait out a cooldown) -> APPROACH — so after any swing the player ALWAYS gets an opening.
//   - Teppista  (0): average street thug. Cautious: close to range, ONE jab, short retreat + pause.
//   - Bruto     (1): big slow tank. GUARDS, then a heavy 3-hit string (punch, punch, KICK), then a LONG
//                    recovery you can punish.
//   - Lama      (2): small, lean, fast, fragile, long reach. Hit-and-run: dash in, ONE quick strike,
//                    immediately retreat far, weave in depth to flank, wait, repeat.
//   - Capobanda (3): towering, strong all-round. Mixes the above — a 2-3 hit string, a lama-like rush,
//                    occasional guard. Most aggressive, but STILL with clear recovery gaps.
// Size READS as power: girth = build/width, scale = height, body = the per-type hue (none red — red is
// blood), stats curved to match. Pure logic + state: owns NO rendering (the draw loop inks fighters),
// reads/writes g.f[] enemy slots (2..BR_MAXF-1), and opens attacks ONLY through combat_begin_attack() so
// combat owns every hit/down/recovery transition. A global swing budget keeps pressure without dogpiling.
// HEAP-FREE: fixed/static arrays, no allocation.

#include "brawler.h"
#include <math.h>

// Enemy type ids (match the roster order; kept local for readable AI branches).
enum { ET_THUG = 0, ET_BRUTE = 1, ET_BLADE = 2, ET_BOSS = 3 };

// ---------------------------------------------------------------- roster
// Stats curved so the four TYPES feel different. Size reads as power:
//   THUG  — average build, average everything (the yardstick).
//   BRUTO — big & broad (girth 1.46, scale 1.18): slow, lots of hp, hits hard, short reach (it lumbers in).
//   LAMA  — small & lean (girth 0.78, scale 0.90): very fast, fragile, the LONGEST reach (a knife at arm's
//           length) so it can poke from outside and slip away.
//   BOSS  — towering & broad (girth 1.52, scale 1.34): the most hp, strong, good reach, decent speed.
// Fields: name_it name_en | hp speed(px/s) dmg reach(px) girth body score scale.
static const EnemyDef ENEMIES[] = {
    { "Teppista",   "Thug",       24, 64.0f,  6, 26.0f, 1.00f, 0,  100, 1.00f }, // 0 baseline
    { "Bruto",      "Bruiser",    72, 36.0f, 15, 24.0f, 1.46f, 0,  280, 1.18f }, // 1 big slow tank
    { "Lama",       "Blade",      14, 104.0f, 7, 40.0f, 0.78f, 0,  180, 0.90f }, // 2 small fast fragile, long reach
    { "Capobanda",  "Boss",      180, 60.0f, 16, 34.0f, 1.52f, 0, 1000, 1.34f }, // 3 towering all-round
};
#define ENEMY_N ((int)(sizeof(ENEMIES) / sizeof(ENEMIES[0])))

// br_rgb() is a runtime fn, so the per-type hue can't be const-folded into the table. Resolve once into a
// cache on first access. Palette: one DISTINCT hue per TYPE (none red — red is blood only); heroes stay
// BR_INK, backdrop stays white + greys. The draw loop derives each outline from the body shade.
static EnemyDef s_def[ENEMY_N];
static bool     s_def_ready = false;
static void enemy_defs_init(void)
{
    if (s_def_ready) return;
    for (int i = 0; i < ENEMY_N; i++) s_def[i] = ENEMIES[i];
    s_def[ET_THUG].body  = BR_EN_THUG;   // steel blue
    s_def[ET_BRUTE].body = BR_EN_BRUTE;  // ochre / khaki
    s_def[ET_BLADE].body = BR_EN_BLADE;  // teal green
    s_def[ET_BOSS].body  = BR_EN_BOSS;   // purple
    s_def_ready = true;
}

int brawler_enemy_count(void) { return ENEMY_N; }

const EnemyDef *brawler_enemy(int t)
{
    enemy_defs_init();
    if (t < 0 || t >= ENEMY_N) return NULL;
    return &s_def[t];
}

// ---------------------------------------------------------------- per-slot AI scratch (heap-free)
// State the Fighter struct doesn't carry. Indexed by g.f[] slot (only 2..BR_MAXF-1 used for enemies).
// The state machine that enforces "attack then disengage":
//   AS_APPROACH : close toward striking range (or hold spacing). The only phase that opens attacks.
//   AS_SEQUENCE : a multi-hit string is in flight — wait for each swing to finish (fighter_busy), then
//                 fire the next step. When the string is spent we drop into RETREAT.
//   AS_RETREAT  : back away from the hero for a beat, then wait out the recover cooldown -> APPROACH.
// Per-slot fields:
//   phase    : AS_* above.
//   seq_step : index of the next hit to throw in the current string (0..seq_len-1).
//   seq_len  : how many hits this string has (1 = single poke, 3 = bruto punch-punch-kick).
//   retreat_t: remaining back-pedal time (s) in AS_RETREAT before we settle and recover.
//   block_t  : remaining guard time (s) while st == BS_BLOCK; AI owns this timer (combat leaves BLOCK be).
//   guard_cd : cooldown before this fighter may raise its guard again.
enum { AS_APPROACH = 0, AS_SEQUENCE = 1, AS_RETREAT = 2 };
static uint8_t s_phase[BR_MAXF];
static uint8_t s_seq_step[BR_MAXF];
static uint8_t s_seq_len[BR_MAXF];
static float   s_retreat_t[BR_MAXF];
static float   s_block_t[BR_MAXF];
static float   s_guard_cd[BR_MAXF];
// Surround data: each enemy engages from its OWN side + depth offset around the hero, so the gang spreads
// across the WHOLE belt instead of stacking on the hero's plane. Re-picked on spawn / retreat / timer.
static float   s_side[BR_MAXF];     // +1 / -1: which side of the hero to hold
static float   s_zoff[BR_MAXF];     // depth offset from the hero's z while roaming (-0.35..+0.35)
static float   s_reshuf[BR_MAXF];   // time until we pick a fresh surround spot (keeps them circling)

// Pick a fresh "surround spot": a side + a depth offset, and when to re-pick. Varied per enemy so foes
// come at the hero from different sides and depths and keep moving rather than queuing on one line.
static void ai_pick_spot(int slot)
{
    s_side[slot]   = (br_frnd() < 0.5f) ? -1.0f : 1.0f;
    s_zoff[slot]   = br_frnd() * 0.70f - 0.35f;       // -0.35..+0.35 of belt depth
    s_reshuf[slot] = 1.6f + br_frnd() * 2.2f;
}

static void ai_scratch_clear(int slot)
{
    s_phase[slot]     = AS_APPROACH;
    s_seq_step[slot]  = 0;
    s_seq_len[slot]   = 0;
    s_retreat_t[slot] = 0.0f;
    s_block_t[slot]   = 0.0f;
    s_guard_cd[slot]  = 0.0f;
    s_side[slot]      = 1.0f;
    s_zoff[slot]      = 0.0f;
    s_reshuf[slot]    = 0.0f;
}

// Clamp helper — keep z inside the walkable belt band, etc.
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- spawn
// Nearest hero to a world point (NULL if no heroes alive) — used to face new spawns.
static Fighter *nearest_hero_to(float x, float z)
{
    Fighter *best = NULL;
    float bd = 0.0f;
    for (int p = 0; p < 2; p++) {
        Fighter *h = br_hero(p);
        if (!h || !h->on || h->hp <= 0) continue;
        float dx = h->x - x, dz = (h->z - z) * 120.0f;     // weight depth into px-ish space
        float dd = dx * dx + dz * dz;
        if (!best || dd < bd) { best = h; bd = dd; }
    }
    return best;
}

Fighter *brawler_spawn_enemy(int type, float x, float z)
{
    enemy_defs_init();
    if (type < 0 || type >= ENEMY_N) return NULL;
    const EnemyDef *e = &s_def[type];

    // Slots 0,1 are reserved for heroes; enemies live in 2..BR_MAXF-1.
    int slot = -1;
    for (int i = 2; i < BR_MAXF; i++) {
        if (!g.f[i].on) { slot = i; break; }
    }
    if (slot < 0) return NULL;
    Fighter *fr = &g.f[slot];

    if (z < 0.0f) z = 0.0f; else if (z > 1.0f) z = 1.0f;

    fr->on      = true;
    fr->is_hero = false;
    fr->kind    = type;
    fr->x       = x;
    fr->z       = z;
    fr->vx      = 0.0f;
    fr->vz      = 0.0f;
    fr->yoff    = 0.0f;
    fr->vy      = 0.0f;
    fr->hp      = e->maxhp;
    fr->maxhp   = e->maxhp;
    fr->st      = BS_WALK;
    fr->anim    = 0.0f;
    fr->aspd    = 0.0f;
    fr->var     = 0;
    fr->hit_done = false;
    fr->flash   = 0.0f;
    fr->cool    = 0.3f;        // brief grace before the first swing
    fr->walkphase = br_frnd(); // de-sync strides
    fr->player  = 0;

    ai_scratch_clear(slot);
    ai_pick_spot(slot);

    // Face the nearest hero on entry.
    Fighter *h = nearest_hero_to(x, z);
    fr->dir = (h && h->x < x) ? -1 : 1;
    return fr;
}

int brawler_live_enemies(void)
{
    int n = 0;
    for (int i = 0; i < BR_MAXF; i++) {
        Fighter *fr = &g.f[i];
        if (fr->on && !fr->is_hero && fr->hp > 0) n++;
    }
    return n;
}

// ---------------------------------------------------------------- AI helpers
static inline bool is_attack_state(BrState s)
{
    return s == BS_PUNCH || s == BS_KICK || s == BS_JKICK || s == BS_GRAB || s == BS_THROW;
}

// Count enemies currently mid-attack — caps the simultaneous-attacker budget.
static int attacking_enemies(void)
{
    int n = 0;
    for (int i = 2; i < BR_MAXF; i++) {
        Fighter *fr = &g.f[i];
        if (fr->on && !fr->is_hero && fr->hp > 0 && is_attack_state(fr->st)) n++;
    }
    return n;
}

// Is a hero close to this enemy AND winding up / mid-swing? Used to decide when guarding makes sense.
static bool hero_threatening(const Fighter *e)
{
    for (int p = 0; p < 2; p++) {
        Fighter *h = br_hero(p);
        if (!h || !h->on || h->hp <= 0) continue;
        if (fabsf(h->x - e->x) > 46.0f) continue;
        if (fabsf(h->z - e->z) > 0.16f) continue;
        if (h->st == BS_PUNCH || h->st == BS_KICK || h->st == BS_JKICK) return true;
    }
    return false;
}

// Raise the guard for `dur` seconds (combat leaves BS_BLOCK alone; the AI counts it down via s_block_t).
static void begin_block(Fighter *e, int slot, float dur)
{
    e->st           = BS_BLOCK;
    e->anim         = 0.0f;
    e->vx          *= 0.4f;        // plant the feet
    s_block_t[slot] = dur;
}

// How long this type rests after a full attack sequence before it may re-engage (the player's window).
// Bruto rests LONGEST (heavy, punishable); lama shortest (but its retreat enforces spacing); boss medium.
// Scaled by difficulty: easy = longer naps, hard = snappier (but never spammy).
static float recover_cooldown(int kind)
{
    float lo, hi;
    switch (kind) {
        case ET_BRUTE: lo = 1.0f; hi = 1.6f; break;   // telegraphed-tank punish window (but not a nap)
        case ET_BLADE: lo = 0.5f; hi = 0.9f; break;   // short — the retreat does the spacing
        case ET_BOSS:  lo = 0.7f; hi = 1.2f; break;   // medium — aggressive but with gaps
        default:       lo = 0.7f; hi = 1.2f; break;   // thug — cautious but keeps the pressure up
    }
    float dm = (g.diff == 0) ? 1.35f : (g.diff == 2 ? 0.75f : 1.0f);
    lo *= dm;
    hi *= dm;
    return lo + br_frnd() * (hi - lo);
}

// Pick a type's attack-string length (number of hits before it disengages). Boss occasionally chains.
static int sequence_length(int kind)
{
    switch (kind) {
        case ET_BRUTE: return 3;                                  // punch, punch, KICK
        case ET_BOSS:  return (br_frnd() < 0.55f) ? 3 : 2;        // mixes 2- and 3-hit strings
        default:       return 1;                                  // thug + lama: one clean strike
    }
}

// The move for step `i` of `kind`'s string. Bruto/boss cap with a KICK (heavier, knocks down) so the
// string ends on a bang; everything else is a punch. combat owns the actual hit resolution.
static BrState sequence_move(int kind, int step, int len)
{
    bool last = (step >= len - 1);
    if ((kind == ET_BRUTE || kind == ET_BOSS) && last && len >= 2) return BS_KICK;
    return BS_PUNCH;
}

// Inter-hit pacing INSIDE a string (between successive swings of the same sequence). Short enough to read
// as one combo, long enough that the player can react/block. The boss links a touch tighter on hard.
static float link_cooldown(int kind)
{
    float base = (kind == ET_BOSS) ? 0.16f : 0.22f;
    if (g.diff == 2) base *= 0.8f;
    return base;
}

// ---------------------------------------------------------------- AI
void enemies_ai(float dt)
{
    enemy_defs_init();

    // Global swing budget: never let the player be dogpiled. +1 on hard.
    const int atk_budget = (g.diff >= 2) ? 3 : 2;
    int atk_now = attacking_enemies();

    for (int i = 2; i < BR_MAXF; i++) {
        Fighter *e = &g.f[i];
        if (!e->on || e->is_hero || e->hp <= 0) {
            if (!e->on) ai_scratch_clear(i);  // freed slot: wipe scratch for the next tenant
            continue;
        }

        // Tick the timers (they keep counting through hitstun too).
        if (e->cool > 0.0f)        { e->cool -= dt;        if (e->cool < 0.0f) e->cool = 0.0f; }
        if (s_guard_cd[i] > 0.0f)  { s_guard_cd[i] -= dt;  if (s_guard_cd[i] < 0.0f) s_guard_cd[i] = 0.0f; }
        if (s_retreat_t[i] > 0.0f) { s_retreat_t[i] -= dt; if (s_retreat_t[i] < 0.0f) s_retreat_t[i] = 0.0f; }
        if (s_reshuf[i] > 0.0f)    { s_reshuf[i] -= dt;    if (s_reshuf[i] < 0.0f) s_reshuf[i] = 0.0f; }

        // --- guard upkeep: BS_BLOCK is AI-owned. Count it down, then drop the guard; hold position. ---
        if (e->st == BS_BLOCK) {
            s_block_t[i] -= dt;
            if (s_block_t[i] <= 0.0f) {
                s_block_t[i]  = 0.0f;
                e->st         = BS_IDLE;
                s_guard_cd[i] = 0.40f + br_frnd() * 0.40f;
            }
            e->vx *= expf(-9.0f * dt);
            continue;
        }

        // --- Combat owns hit/down/recovery + attack ANIMATIONS. Interrupted mid-string -> abandon it
        // and retreat so a stunned foe never resumes a stale combo. ---
        if (fighter_busy(e)) {
            if (s_phase[i] == AS_SEQUENCE && (e->st == BS_HIT || e->st == BS_DOWN || e->st == BS_RISE)) {
                s_phase[i] = AS_RETREAT; s_seq_step[i] = 0; s_seq_len[i] = 0; s_retreat_t[i] = 0.0f;
            }
            continue;
        }

        const EnemyDef *def = &s_def[e->kind];
        Fighter *h = nearest_hero_to(e->x, e->z);
        if (!h) {
            e->vx = 0.0f; e->vz = 0.0f;
            if (e->st != BS_IDLE) e->st = BS_IDLE;
            continue;
        }

        e->dir = (h->x < e->x) ? -1 : 1;

        float diffmul = (g.diff == 0) ? 0.85f : (g.diff == 2 ? 1.2f : 1.0f);
        float spd  = def->speed * diffmul;
        float zspd = (0.85f + 0.0035f * def->speed) * diffmul;   // belt-depth speed (units/s): foes cross the floor

        float dxh = h->x - e->x;
        float dzh = h->z - e->z;
        bool  in_range = (fabsf(dxh) <= def->reach) && (fabsf(dzh) < 0.14f);

        // refresh the surround spot now and then so the gang keeps circling the whole belt
        if (s_reshuf[i] <= 0.0f) ai_pick_spot(i);

        // ============================================================ SEQUENCE phase
        // A string is in flight and we're FREE again: fire the next link (cooldown + budget permitting),
        // homing onto the hero so it lands; else close the string and disengage.
        if (s_phase[i] == AS_SEQUENCE) {
            bool more  = (s_seq_step[i] < s_seq_len[i]);
            bool near_ = (fabsf(dxh) <= def->reach + 8.0f) && (fabsf(dzh) < 0.18f);
            if (more && near_) {
                if (e->cool <= 0.0f && atk_now < atk_budget) {
                    BrState mv = sequence_move(e->kind, s_seq_step[i], s_seq_len[i]);
                    combat_begin_attack(e, mv); atk_now++; s_seq_step[i]++;
                    e->cool = link_cooldown(e->kind);
                } else if (fabsf(dzh) > 0.02f) {           // line up depth while waiting on the link
                    float zs = zspd * dt; if (zs > fabsf(dzh)) zs = fabsf(dzh);
                    e->z += (dzh > 0 ? zs : -zs); e->z = clampf(e->z, 0.18f, 1.0f);
                }
                e->vx *= expf(-7.0f * dt);
                if (e->st != BS_PUNCH && e->st != BS_KICK) e->st = BS_IDLE;
                continue;
            }
            s_phase[i] = AS_RETREAT; s_seq_step[i] = 0; s_seq_len[i] = 0; s_retreat_t[i] = 0.0f;
        }

        // ============================================================ RETREAT phase
        // Back off a SHORT beat (re-picking a fresh surround angle) then sit out the recover cooldown,
        // drifting toward the new depth meanwhile so the foe keeps moving rather than freezing.
        if (s_phase[i] == AS_RETREAT) {
            if (s_retreat_t[i] <= 0.0f && e->cool <= 0.0f) {
                s_retreat_t[i] = (e->kind == ET_BLADE) ? (0.35f + br_frnd() * 0.30f)
                                                       : (0.20f + br_frnd() * 0.25f);
                e->cool        = recover_cooldown(e->kind);
                ai_pick_spot(i);                            // come back from a new side / depth
            }
            bool moving = false;
            float want_gap = def->reach * ((e->kind == ET_BLADE) ? 1.8f : 1.25f);
            if (s_retreat_t[i] > 0.0f && fabsf(dxh) < want_gap) {
                float rs = spd * ((e->kind == ET_BLADE) ? 1.2f : 0.9f) * dt;
                e->x -= (dxh > 0 ? rs : -rs); moving = true;
            }
            float tz  = clampf(h->z + s_zoff[i], 0.18f, 1.0f); // drift to the fresh surround depth
            float dzt = tz - e->z;
            if (fabsf(dzt) > 0.02f) {
                float zs = zspd * dt; if (zs > fabsf(dzt)) zs = fabsf(dzt);
                e->z += (dzt > 0 ? zs : -zs); e->z = clampf(e->z, 0.18f, 1.0f); moving = true;
            }
            if (s_retreat_t[i] <= 0.0f && e->cool <= 0.0f) s_phase[i] = AS_APPROACH;
            e->st = moving ? BS_WALK : BS_IDLE;
            if (moving) e->walkphase += dt * (spd / 26.0f);
            continue;
        }

        // ============================================================ APPROACH phase
        // Reactive GUARD (bruto/boss): shield a close, swinging hero instead of eating it. Gated so they
        // don't turtle; lama/thug rely on speed/spacing instead.
        bool can_guard = (e->kind == ET_BRUTE || e->kind == ET_BOSS);
        if (can_guard && s_guard_cd[i] <= 0.0f && hero_threatening(e)) {
            float gchance = (e->kind == ET_BRUTE) ? 0.6f : 0.4f;
            if (br_frnd() < gchance) {
                float lo = (e->kind == ET_BRUTE) ? 0.40f : 0.32f;
                float hi = (e->kind == ET_BRUTE) ? 0.62f : 0.50f;
                begin_block(e, i, lo + br_frnd() * (hi - lo));
                continue;
            }
        }

        bool ready = (e->cool <= 0.0f) && (atk_now < atk_budget);

        // Target: when READY, home onto the hero's plane just in front and strike; otherwise roam a
        // SURROUND spot (own side + depth offset) so the gang spreads across the WHOLE belt and keeps
        // moving instead of queuing on the hero's line.
        float tz, tx;
        if (ready) {
            tz = h->z;
            tx = h->x - (float)e->dir * (def->reach * 0.82f);
        } else {
            tz = clampf(h->z + s_zoff[i], 0.18f, 1.0f);
            tx = h->x + s_side[i] * (def->reach * 1.05f);
        }

        bool moving = false;
        float dxt = tx - e->x, dzt = tz - e->z;
        if (fabsf(dxt) > 2.0f) {
            float xs = spd * dt; if (xs > fabsf(dxt)) xs = fabsf(dxt);
            e->x += (dxt > 0 ? xs : -xs); moving = true;
        }
        if (fabsf(dzt) > 0.02f) {
            float zs = zspd * dt; if (zs > fabsf(dzt)) zs = fabsf(dzt);
            e->z += (dzt > 0 ? zs : -zs); moving = true;
        }
        e->z = clampf(e->z, 0.18f, 1.0f);

        // gentle separation so foes don't stack on one another (push apart in depth)
        for (int j = 2; j < BR_MAXF; j++) {
            if (j == i) continue;
            Fighter *o = &g.f[j];
            if (!o->on || o->is_hero || o->hp <= 0) continue;
            if (fabsf(o->x - e->x) < 12.0f && fabsf(o->z - e->z) < 0.07f) {
                e->z += ((e->z >= o->z) ? 0.6f : -0.6f) * dt;
                e->z  = clampf(e->z, 0.18f, 1.0f);
            }
        }

        // strike: lined up on the hero AND ready -> open this type's string.
        if (ready && in_range) {
            int len = sequence_length(e->kind); if (len < 1) len = 1;
            s_seq_len[i] = (uint8_t)len; s_seq_step[i] = 0;
            BrState mv = sequence_move(e->kind, 0, len);
            combat_begin_attack(e, mv); atk_now++; s_seq_step[i] = 1;
            if (len > 1) { s_phase[i] = AS_SEQUENCE; e->cool = link_cooldown(e->kind); }
            else         { s_phase[i] = AS_RETREAT;  e->cool = 0.0f; }
            continue;
        }

        e->st = moving ? BS_WALK : BS_IDLE;
        if (moving) e->walkphase += dt * (spd / 26.0f);
    }
}
