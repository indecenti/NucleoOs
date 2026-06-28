// nucleo_fxfx.h — REUSABLE action-game juice: particle fields, ground decals, screen-shake.
//
// Generic and game-agnostic: a brawler's blood spray + pools, a shooter's sparks + scorch, a racer's
// dust all map onto the same Field/PoolRing/Shake. Pure-inline, HEAP-FREE (fixed-size pools, no alloc),
// drawn through the shared `d` target. Particle positions are WORLD-x (camera-independent) + screen-y;
// draw() takes an x offset so the caller folds in camera scroll + shake. Built on fx3d (rgb/scl/mix).
#pragma once
#include "nucleo_fx3d.h"
#include <math.h>

namespace fxfx {

struct Particle { float x, y, vx, vy, life, max, landY; uint16_t col; bool on; };

// A fixed pool of ballistic particles that fall under gravity and settle on a per-particle floor.
template <int N>
struct Field {
    Particle p[N];
    int      cur;

    void clear() { for (int i = 0; i < N; i++) p[i].on = false; cur = 0; }

    Particle* spawn(float x, float y, float vx, float vy, float life, float landY, uint16_t col)
    {
        Particle* q = &p[cur]; cur = (cur + 1) % N;
        q->x = x; q->y = y; q->vx = vx; q->vy = vy; q->life = q->max = life; q->landY = landY; q->col = col; q->on = true;
        return q;
    }

    // A directional burst (blood/sparks). `dir` ±1 biases the cone; `rng`/`rng2` are caller PRNGs
    // (0..1 and -1..1) so this stays deterministic-friendly and free of its own RNG state.
    template <class RNG01, class RNG11>
    void spray(float x, float y, int dir, int n, float speed, float landY, uint16_t col, RNG01 rng, RNG11 rng2)
    {
        for (int i = 0; i < n; i++) {
            float vx = dir * speed * (0.3f + 0.9f * rng()) + speed * 0.5f * rng2();
            float vy = -speed * (0.4f + 0.8f * rng());
            spawn(x, y, vx, vy, 0.35f + 0.5f * rng(), landY, col);
        }
    }

    // Integrate; `onLand(x,y)` fires when a particle settles (caller turns it into a decal). NULL = none.
    void step(float dt, float grav, void (*onLand)(float, float))
    {
        for (int i = 0; i < N; i++) {
            Particle& q = p[i]; if (!q.on) continue;
            q.vy += grav * dt; q.x += q.vx * dt; q.y += q.vy * dt; q.life -= dt;
            if (q.life <= 0.0f) { q.on = false; continue; }
            if (q.vy > 0.0f && q.y >= q.landY) { q.on = false; if (onLand) onLand(q.x, q.landY); }
        }
    }

    // `xoff` = -camx (+shake). Particles dim/shrink as life runs out.
    void draw(float xoff)
    {
        for (int i = 0; i < N; i++) {
            const Particle& q = p[i]; if (!q.on) continue;
            int sx = (int)(q.x + xoff), sy = (int)q.y;
            float k = q.max > 0 ? q.life / q.max : 0.0f;
            uint16_t c = fx3d::scl(q.col, (int)(170 + 85 * k), 255);   // stays VIVID (only a mild fade, never grey)
            int r = (k > 0.55f) ? 2 : 1;                                // chunky droplets so blood reads on white
            d.fillCircle(sx, sy, r, c);
        }
    }
};

// A ring of ground decals (blood pools, scorch marks) — oldest overwritten, so they persist a while.
template <int M>
struct PoolRing {
    struct Dot { float x, y; int rx, ry; uint16_t col; bool on; } d_[M];
    int head;

    void clear() { for (int i = 0; i < M; i++) d_[i].on = false; head = 0; }
    void add(float x, float y, int rx, int ry, uint16_t col)
    {
        Dot& s = d_[head]; head = (head + 1) % M;
        s.x = x; s.y = y; s.rx = rx; s.ry = ry; s.col = col; s.on = true;
    }
    void draw(float xoff) { for (int i = 0; i < M; i++) if (d_[i].on) fxfig::puddle((int)(d_[i].x + xoff), (int)d_[i].y, d_[i].rx, d_[i].ry, d_[i].col); }
};

// Decaying screen-shake. add() bumps magnitude on impact; step() bleeds it off; ox()/oy() give the
// per-frame pixel offset. Self-seeding xorshift (guards the zero-seed trap).
struct Shake {
    float mag; uint32_t seed; int cox, coy;
    void  reset()        { mag = 0; seed = 0x9e3779b9u; cox = coy = 0; }
    void  add(float m)   { if (m > mag) mag = m; }
    void  step(float dt) { mag -= 64.0f * dt; if (mag < 0) mag = 0; cox = axis(); coy = axis(); }  // resample ONCE per frame
    int   axis()         { if (!seed) seed = 0x9e3779b9u; seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (int)(mag * (((seed & 0xffff) / 32767.5f) - 1.0f)); }
    int   ox() const     { return cox; }   // stable within a frame: shadow / body / blood shake together
    int   oy() const     { return coy; }
};

} // namespace fxfx
