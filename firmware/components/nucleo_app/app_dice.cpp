// Dadi 3D — Games. Polyhedral dice (d4/d6/d8/d20) on the fx3d engine, no overlap.
//
// INTERACTION — a "charge / shuffle" model (no twitchy auto-roll): SHAKE the device, HOLD the GO button,
// or SCROLL (up/down) to MIX — the dice whirl in place and build energy; STOP / release and they are
// THROWN, the harder you charged the more violently they tumble and the longer they settle. ENTER does a
// quick standard roll. TAB opens settings (number of dice, die type). Honest randomness from the ESP32
// hardware TRNG. Works without the IMU too (GO / keys). Engine: fx3d::rot3, one rotation per die per frame.
// Every die always shows its value: the settle pose turns the value's face to the camera in a 3/4 view,
// and the header total is the sum of exactly those faces.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "nucleo_imu.h"
#include "nucleo_fx3d.h"
#include "nucleo_audio.h"
#include "nucleo_exclusive.h"  // NX_NET_APP: dedicate RAM like every other native game
#include "game_text.h"
#include "game_ui.h"
#include <math.h>
#include <sys/stat.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "app_gfx.h"

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif
#define TWO_PI 6.2831853f
#define PHI    1.6180340f

// ---- polyhedra (RAM 0, flash). All within the engine's 24 vert / 24 tri budget. ----
static const fx3d::V3 TETRAV[4] = { {1,1,1},{1,-1,-1},{-1,1,-1},{-1,-1,1} };
static const fx3d::Tri TETRAT[4] = { {0,1,2},{0,2,3},{0,3,1},{1,3,2} };
static const fx3d::Model TETRA = { TETRAV, 4, TETRAT, 4 };

static const fx3d::V3 CUBEV[8] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
static const fx3d::Tri CUBET[12] = { {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},{1,5,6},{1,6,2},{2,6,7},{2,7,3},{3,7,4},{3,4,0} };
static const fx3d::Model CUBE = { CUBEV, 8, CUBET, 12 };

static const fx3d::V3 OCTAV[6] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
static const fx3d::Tri OCTAT[8] = { {0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5} };
static const fx3d::Model OCTA = { OCTAV, 6, OCTAT, 8 };

static const fx3d::V3 ICOV[12] = {
    {-1,PHI,0},{1,PHI,0},{-1,-PHI,0},{1,-PHI,0},{0,-1,PHI},{0,1,PHI},
    {0,-1,-PHI},{0,1,-PHI},{PHI,0,-1},{PHI,0,1},{-PHI,0,-1},{-PHI,0,1},
};
static const fx3d::Tri ICOT[20] = {
    {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
    {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
};
static const fx3d::Model ICOSA = { ICOV, 12, ICOT, 20 };

// rscale = 1 / the shape's on-screen radius in model units (a tumbling cube reaches ~1.45, the tetra ~1.75),
// so every type fills its cell the same and never spills into a neighbour or the header. tilt = how far the
// settled die turns into a 3/4 view: it must stay under half the angle between two faces' normals, or a
// neighbour face would become the front one (d20 faces are only 42 deg apart: no tilt, it reads 3D anyway).
struct DieType { const fx3d::Model *m; int faces; float rscale, tilt; const char *name; };
static const DieType TYPES[4] = {
    { &CUBE,  6,  0.69f, 1.0f, "d6"  },
    { &TETRA, 4,  0.57f, 1.0f, "d4"  },
    { &OCTA,  8,  0.89f, 0.6f, "d8"  },
    { &ICOSA, 20, 0.53f, 0.0f, "d20" },
};
#define NTYPES 4

// d6 face-on poses (turn the value's face toward the camera) — pips read the value
static const float POSE_Y[6] = { (float)M_PI, (float)(M_PI/2), 0, 0, (float)(-M_PI/2), 0 };
static const float POSE_P[6] = { 0, 0, (float)(-M_PI/2), (float)(M_PI/2), 0, 0 };
struct FaceFrame { float n[3], u[3], v[3]; };
static const FaceFrame FACE[6] = {
    { { 0, 0, 1}, {1,0,0}, {0,1,0} }, { { 1, 0, 0}, {0,0,-1},{0,1,0} }, { { 0, 1, 0}, {1,0,0}, {0,0,-1} },
    { { 0,-1, 0}, {1,0,0}, {0,0,1} }, { {-1, 0, 0}, {0,0,1}, {0,1,0} }, { { 0, 0,-1}, {1,0,0}, {0,-1,0} },
};
static const float PIPS[7][6][2] = {
    {{0,0}}, {{0,0}}, {{-0.5f,0.5f},{0.5f,-0.5f}}, {{-0.55f,0.55f},{0,0},{0.55f,-0.55f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0,0},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.62f},{-0.5f,0},{-0.5f,0.62f},{0.5f,-0.62f},{0.5f,0},{0.5f,0.62f}},
};

#define MAXDICE 6
#define HEAD 22                        // header band (title + total); the felt is below it
enum { ST_IDLE, ST_CHARGE, ST_ROLL };
struct Die { float yaw, pitch, bank;  float y0, p0, b0, ye, pe, be, wob;  int value; };
static Die  *s_d;                      // MAXDICE entries, APP_RAM (posed by enter())
static int   s_state = ST_IDLE;
static int   s_n = 2, s_type = 0;      // kept across opens (static), like a real dice cup
static float s_charge;                 // 0..1 (mixing energy)
static int64_t s_roll_us, s_dur_us, s_last_us, s_keymix_us;
static bool  s_go_held;
static bool  s_settings;               // settings sheet open
static gui::Menu s_setm;              // settings rows: dice count, die type
static const char *s_hint;             // footer text on screen (set only when it changes)

static int   randn(int n) { return (int)(esp_random() % (uint32_t)n); }   // uniform 0..n-1 (TRNG)
// Sound: the PC-rendered pack (tools/sfx-gen/games/dice.py -> /sd/data/dice/pack): the cup rattle while
// mixing, the throw, the landing clacks. A missing WAV plays a short tone; nothing is synthesized here.
static void sfx(const char *name, int hz)
{
    char p[40]; snprintf(p, sizeof p, "/sd/data/dice/pack/%s.wav", name);
    struct stat st;
    if (stat(p, &st) != 0 || st.st_size <= 44 || nucleo_audio_play(p) != ESP_OK) nucleo_audio_tone(hz, 40, 55);
}
static int64_t s_rattle_us;                // next cup rattle while mixing

// ---- rotation with the trig done ONCE per die per frame (fx3d::project recomputes 6 sin/cos per point) ----
struct Rot { float cy, sy, cp, sp, cb, sb; };
static Rot rot_of(float yaw, float pitch, float bank) { return { cosf(yaw), sinf(yaw), cosf(pitch), sinf(pitch), cosf(bank), sinf(bank) }; }
static float rot_pt(const Rot &r, float x, float y, float z, float *ox, float *oy)
{
    float z2; fx3d::rot3(x, y, z, r.cy, r.sy, r.cp, r.sp, r.cb, r.sb, ox, oy, &z2);
    return z2;
}

// ---- polyhedron face geometry (numbers ON the real faces for d4/d8/d20) ----
static void face_normal(const fx3d::Model &m, int t, float *o)
{
    const fx3d::Tri &tr = m.t[t];
    const fx3d::V3 &a = m.v[tr.a], &b = m.v[tr.b], &c = m.v[tr.c];
    float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
    float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
    float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
    float gx = (a.x + b.x + c.x) / 3, gy = (a.y + b.y + c.y) / 3, gz = (a.z + b.z + c.z) / 3;
    if (nx * gx + ny * gy + nz * gz < 0) { nx = -nx; ny = -ny; nz = -nz; }   // make it point outward
    float inv = 1.0f / (sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f);
    o[0] = nx * inv; o[1] = ny * inv; o[2] = nz * inv;
}

// The settle pose of `value` for the current type: the value's face turned to the camera (d6: POSE table;
// d4/d8/d20: from the face normal), then tilted into a 3/4 view so the die reads as a solid. The combined
// rotation is decomposed back into the engine's yaw (Y) / pitch (X) / bank (Z) angles.
static void settle_pose(int value, float *yaw, float *pitch, float *bank)
{
    float py, pp;
    if (TYPES[s_type].faces == 6) { py = POSE_Y[value - 1]; pp = POSE_P[value - 1]; }
    else {
        float n[3]; face_normal(*TYPES[s_type].m, value - 1, n);
        py = atan2f(-n[0], n[2]);
        pp = atan2f(n[1], sqrtf(n[0] * n[0] + n[2] * n[2])) + (float)M_PI;
    }
    float k = TYPES[s_type].tilt;
    Rot p = rot_of(py, pp, 0), t = rot_of(0.40f * k, 0.36f * k, 0);   // pose, then the 3/4 tilt
    float m[3][3];                                                   // m[row][col] = (T*P) e_col
    for (int c = 0; c < 3; c++) {
        float x, y, z = rot_pt(p, c == 0, c == 1, c == 2, &x, &y), x2, y2, z2 = rot_pt(t, x, y, z, &x2, &y2);
        m[0][c] = x2; m[1][c] = y2; m[2][c] = z2;
    }
    *pitch = asinf(m[2][1] > 1 ? 1 : (m[2][1] < -1 ? -1 : m[2][1]));
    *yaw = atan2f(-m[2][0], m[2][2]);
    *bank = atan2f(-m[0][1], m[1][1]);
}
// Give every die a fresh value resting in its pose (launch, type change): the faces always match the total.
static void pose_all(void)
{
    for (int i = 0; i < MAXDICE; i++) {
        Die &dd = s_d[i];
        dd.value = 1 + randn(TYPES[s_type].faces);
        settle_pose(dd.value, &dd.yaw, &dd.pitch, &dd.bank);
        dd.ye = dd.yaw; dd.pe = dd.pitch; dd.be = dd.bank;
    }
}

// ---- throw: decide results + set up the eased tumble; vigor/duration scale with `charge` ----
static void throw_dice(float charge)
{
    if (charge < 0.05f) charge = 0.05f;
    if (charge > 1.0f)  charge = 1.0f;
    nucleo_audio_stop(); sfx("throw", 900);
    s_dur_us = (int64_t)((0.9f + 1.6f * charge) * 1000000.0f);   // 0.9–2.5 s
    s_roll_us = esp_timer_get_time();
    s_state = ST_ROLL;
    for (int i = 0; i < MAXDICE; i++) {                         // all of them: a die added later is posed too
        Die &dd = s_d[i];
        dd.y0 = dd.yaw; dd.p0 = dd.pitch; dd.b0 = dd.bank;
        dd.value = 1 + randn(TYPES[s_type].faces);
        int spins = 2 + (int)(charge * 5.0f) + randn(2);         // more charge -> more turns
        settle_pose(dd.value, &dd.ye, &dd.pe, &dd.be);
        dd.ye += TWO_PI * spins;
        dd.pe += TWO_PI * (spins - 1);
        dd.wob = 0.2f + 0.5f * charge;
    }
}

// ---- per-frame simulation ----
static void sim(float dt)
{
    int64_t now = esp_timer_get_time();
    if (s_state == ST_ROLL) {
        float t = (float)(now - s_roll_us) / (float)s_dur_us; if (t > 1) t = 1;
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   // ease-out cubic
        float wob = (1.0f - t) * sinf(t * 16.0f);
        for (int i = 0; i < MAXDICE; i++) {
            Die &dd = s_d[i];
            dd.yaw = dd.y0 + (dd.ye - dd.y0) * e;
            dd.pitch = dd.p0 + (dd.pe - dd.p0) * e;
            dd.bank = dd.b0 + (dd.be - dd.b0) * e + dd.wob * wob;
        }
        if (t >= 1.0f) {
            s_state = ST_IDLE;
            for (int i = 0; i < MAXDICE; i++) { s_d[i].yaw = fmodf(s_d[i].ye, TWO_PI); s_d[i].pitch = fmodf(s_d[i].pe, TWO_PI); s_d[i].bank = s_d[i].be; }
            nucleo_audio_stop(); sfx("land", 180);     // the dice hit the felt
        }
        return;
    }
    // IDLE / CHARGE: gather mixing input
    float in = 0.0f;
    if (s_go_held) in += 1.0f;
    if (now < s_keymix_us) in += 0.9f;                            // recent scroll keeps mixing
    if (nucleo_imu_present()) {
        nucleo_imu_sample();
        float en = nucleo_imu_energy();
        if (en > 0.45f) in += (en - 0.45f) * 1.8f;                // only a real shake counts (kills twitch)
    }
    if (in > 0.0f) {
        s_state = ST_CHARGE;
        if (now >= s_rattle_us && !nucleo_audio_is_playing()) { sfx("shake", 1500); s_rattle_us = now + 260000; }
        s_charge += in * dt * 0.9f; if (s_charge > 1.0f) s_charge = 1.0f;
        float spd = (2.5f + 14.0f * s_charge) * dt;               // whirl faster as it charges
        float bank = 0.15f * sinf((float)now * 9e-6f);
        for (int i = 0; i < MAXDICE; i++) {
            s_d[i].yaw += spd * (0.8f + 0.04f * i);
            s_d[i].pitch += spd * 0.7f;
            s_d[i].bank = bank;
        }
    } else if (s_state == ST_CHARGE) {
        if (s_charge >= 0.12f) { throw_dice(s_charge); s_charge = 0; }   // input ended with enough -> THROW
        else { s_charge *= 0.85f; if (s_charge < 0.01f) { s_charge = 0; s_state = ST_IDLE; } }
    }
}

// ---- drawing ----
// RGB332-exact palette (the canvas is 8bpp, and it has no neutral grey but black and white): white faces
// shading cool, like white plastic in shadow; ink / red pips; faint marks on the side faces.
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t SHADE[4] = { RGB(255, 255, 255), RGB(219, 219, 255), RGB(182, 182, 170), RGB(109, 109, 170) };
static const uint16_t COL_PIP = 0, COL_PIP1 = RGB(219, 0, 0), COL_SOFT = RGB(146, 146, 170);

// A convex die drawn the crisp way: back faces culled, every camera-facing face ONE flat shade from a key
// light just above the camera (a generic mesh shader lights each triangle apart, which on 8bpp leaves a
// seam and a tint across every cube face), over a dark rim made of the same faces 1.6 px bigger — the
// silhouette of a convex solid is the union of its front faces.
static void draw_solid(const fx3d::Model &m, const Rot &r, float cx, float cy, float sc)
{
    float px[12], py[12];
    int8_t shade[24];
    for (int i = 0; i < m.nv; i++) rot_pt(r, m.v[i].x, m.v[i].y, m.v[i].z, &px[i], &py[i]);
    for (int t = 0; t < m.nt; t++) {
        float n[3], nx, ny; face_normal(m, t, n);
        float nz = rot_pt(r, n[0], n[1], n[2], &nx, &ny);
        float k = -0.2f * nx - 0.35f * ny - 0.92f * nz;           // Lambert toward the light
        shade[t] = nz >= 0 ? -1 : k > 0.72f ? 0 : k > 0.5f ? 1 : k > 0.25f ? 2 : 3;
    }
    for (int pass = 0; pass < 2; pass++) {
        float s = pass ? sc : sc + 1.6f;
        for (int t = 0; t < m.nt; t++) {
            if (shade[t] < 0) continue;
            const fx3d::Tri &tr = m.t[t];
            d.fillTriangle((int)(cx + px[tr.a] * s), (int)(cy + py[tr.a] * s), (int)(cx + px[tr.b] * s), (int)(cy + py[tr.b] * s),
                           (int)(cx + px[tr.c] * s), (int)(cy + py[tr.c] * s), pass ? SHADE[shade[t]] : 0x0000);
        }
    }
}
// d6: pips on every camera-facing face; the result face in full ink, the side faces faint.
static void draw_pips(const Rot &r, float cx, float cy, float sc)
{
    float pr = sc * 0.15f; if (pr < 2) pr = 2;
    int front = 0; float fz = 1e9f, nz[6];
    for (int v = 1; v <= 6; v++) { float x, y; nz[v - 1] = rot_pt(r, FACE[v - 1].n[0], FACE[v - 1].n[1], FACE[v - 1].n[2], &x, &y); if (nz[v - 1] < fz) { fz = nz[v - 1]; front = v; } }
    for (int v = 1; v <= 6; v++) {
        if (nz[v - 1] >= -0.12f) continue;                        // back / edge-on face
        const FaceFrame &f = FACE[v - 1];
        uint16_t col = v != front ? COL_SOFT : (v == 1 ? COL_PIP1 : COL_PIP);
        for (int k = 0; k < v; k++) {
            float pu = PIPS[v][k][0] * 0.63f, pv = PIPS[v][k][1] * 0.63f, x, y;
            rot_pt(r, f.n[0] * 1.03f + f.u[0] * pu + f.v[0] * pv, f.n[1] * 1.03f + f.u[1] * pu + f.v[1] * pv,
                   f.n[2] * 1.03f + f.u[2] * pu + f.v[2] * pv, &x, &y);
            d.fillSmoothCircle((int)(cx + x * sc), (int)(cy + y * sc), v != front ? pr * 0.75f : v == 1 ? pr * 1.5f : pr, col);   // anti-aliased
        }
    }
}

// d4/d8/d20: number every camera-facing face (its fixed value = face index+1); the front-most (the
// settled result) is big and red, the others small and faint. Numbers ride the real faces.
static void draw_face_numbers(const fx3d::Model &m, const Rot &r, float cx, float cy, float sc)
{
    float fnz[24];
    int frontT = 0; float frontZ = 1e9f;
    for (int t = 0; t < m.nt; t++) {
        float n[3], x, y; face_normal(m, t, n);
        fnz[t] = rot_pt(r, n[0], n[1], n[2], &x, &y);
        if (fnz[t] < frontZ) { frontZ = fnz[t]; frontT = t; }
    }
    for (int t = 0; t < m.nt; t++) {
        if (fnz[t] > -0.32f || (t != frontT && sc < 16)) continue;   // back / grazing faces; small dice: the result only
        const fx3d::Tri &tr = m.t[t];
        float x, y;
        rot_pt(r, (m.v[tr.a].x + m.v[tr.b].x + m.v[tr.c].x) * 0.34f, (m.v[tr.a].y + m.v[tr.b].y + m.v[tr.c].y) * 0.34f,
               (m.v[tr.a].z + m.v[tr.b].z + m.v[tr.c].z) * 0.34f, &x, &y);
        bool front = t == frontT;
        char b[4]; snprintf(b, sizeof b, "%d", t + 1);
        int px = (int)(cx + x * sc), py = (int)(cy + y * sc);
        d.setTextDatum(textdatum_t::middle_center);
        if (front) {                                              // the result: bold face, red, soft shadow
            d.setFont(sc >= 20 ? &fonts::FreeSansBold12pt7b : &fonts::FreeSansBold9pt7b);
            d.setTextColor(SHADE[2]); d.drawString(b, px + 1, py + 1);
            d.setTextColor(COL_PIP1); d.drawString(b, px, py);
        } else { d.setFont(&fonts::Font0); d.setTextColor(COL_SOFT); d.drawString(b, px, py); }
    }
    d.setFont(&fonts::Font0); d.setTextDatum(textdatum_t::top_left);
}

// grid layout: rows/cols chosen to fill the felt and keep dice apart
static void layout(int idx, int top, int bottom, int *cx, int *cy, float *sc)
{
    int rows = (s_n <= 3) ? 1 : 2;
    int cols = (s_n + rows - 1) / rows;
    int r = idx / cols, c = idx % cols;
    int inrow = (r == rows - 1) ? (s_n - cols * (rows - 1)) : cols;   // last row may have fewer
    int cellw = W / cols, cellh = (bottom - top) / rows;
    *cx = (W - inrow * cellw) / 2 + c * cellw + cellw / 2;
    *cy = top + r * cellh + cellh / 2;
    float s = (cellw < cellh ? cellw : cellh) * 0.46f;               // the die's radius: ~92% of the cell
    if (s > 46) s = 46;
    *sc = s * TYPES[s_type].rscale;
}

static void draw_settings(void)
{
    char r0[24], r1[24];
    snprintf(r0, sizeof r0, "%s: %d", GT("Dadi", "Dice"), s_n);
    snprintf(r1, sizeof r1, "%s: %s", GT("Tipo", "Type"), TYPES[s_type].name);
    const char *rows[2] = { r0, r1 };
    int y = gui::title(GT("Impostazioni", "Settings"), nullptr, C_GREEN);
    gui::menu(s_setm, rows, 2, y + 4, nucleo_app_content_height(), C_GREEN);
}

static void set_hint(void)
{
    const char *h = s_settings ? GT("su/giu  sx/dx cambia  INVIO ok", "UP/DN  L/R change  ENTER ok")
                  : s_state == ST_CHARGE ? GT("rilascia per lanciare", "release to throw")
                  : GT("INVIO lancia  su/giu mescola  TAB", "ENTER roll  UP/DN mix  TAB setup");
    if (h != s_hint) { s_hint = h; nucleo_app_set_hint(h); }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    set_hint();
    if (s_settings) { draw_settings(); return; }
    // header: title + "NdX" on the left, the big total on the right
    gui::vgradient(0, top, W, HEAD, gui::rgb(0, 72, 36), gui::rgb(0, 18, 0));
    d.drawFastHLine(0, top + HEAD - 1, W, gui::rgb(72, 182, 85));
    int tw = gui::text(GT("Dadi", "Dice"), 6, top + 3, 0, gui::F_BODY, C_GREEN, 0);
    char rt[16]; snprintf(rt, sizeof rt, "%d%s", s_n, TYPES[s_type].name);
    gui::text(rt, 14 + tw, top + 4, 0, gui::F_SMALL, gui::rgb(182, 182, 170), 0);
    int total = 0; for (int i = 0; i < s_n; i++) total += s_d[i].value;
    if (s_state == ST_IDLE) snprintf(rt, sizeof rt, "%d", total);
    else if (s_state == ST_CHARGE) snprintf(rt, sizeof rt, "%d%%", (int)(s_charge * 100));
    else snprintf(rt, sizeof rt, "...");
    gui::text(rt, W - 6, top + 3, 2, gui::F_BODY, s_state == ST_IDLE ? 0xFFFF : C_YELLOW, 0);

    // felt: a dithered pool of light + a perspective grid, the dice clipped to it (a big tumbling die
    // never paints the header or the footer)
    int ft = top + HEAD;
    gui::vgradient(0, ft, W, bottom - ft, gui::rgb(0, 36, 0), gui::rgb(0, 109, 36));
    fx3d::Grid g = { ft + 10, bottom - 1, (float)(W/2), 0.0f, (float)(W*0.8f), 7, 9, gui::rgb(36, 109, 36), gui::rgb(36, 146, 85), 150 };
    fx3d::grid(g);
    d.setClipRect(0, ft, W, bottom - ft);
    const fx3d::Model &mdl = *TYPES[s_type].m;
    for (int i = 0; i < s_n; i++) {
        Die &dd = s_d[i];
        int cx, cy; float sc; layout(i, ft, bottom, &cx, &cy, &sc);
        fx3d::dither_disc(cx, cy + (int)(sc * 0.95f), (int)(sc * 0.85f), 0);                    // soft contact shadow
        Rot r = rot_of(dd.yaw, dd.pitch, dd.bank);
        draw_solid(mdl, r, (float)cx, (float)cy, sc);
        if (TYPES[s_type].faces == 6) draw_pips(r, (float)cx, (float)cy, sc);
        else draw_face_numbers(mdl, r, (float)cx, (float)cy, sc);
    }
    d.clearClipRect();
    if (s_state == ST_CHARGE) {                                   // charge meter while mixing
        d.fillSmoothRoundRect(10, bottom - 8, W - 20, 6, 3, gui::rgb(0, 36, 0));
        d.fillSmoothRoundRect(10, bottom - 8, 6 + (int)((W - 26) * s_charge), 6, 3, gui::mix(C_YELLOW, C_GREEN, (int)(s_charge * 256)));
    }
}

static bool poll(void)
{
    int64_t now = esp_timer_get_time();
    float dt = s_last_us ? (float)(now - s_last_us) / 1000000.0f : 0.02f;
    if (dt > 0.05f) dt = 0.05f;
    s_last_us = now;
    if (s_settings) return gui::menu_tick(s_setm, (int)(dt * 1000));   // dice frozen; only the cursor glides
    int prev = s_state;
    sim(dt);
    return (s_state != ST_IDLE) || (prev != ST_IDLE);        // redraw while anything is moving
}

static void ptt(bool on)
{
    s_go_held = on;
    if (!on && s_state == ST_CHARGE && s_charge >= 0.12f) { throw_dice(s_charge); s_charge = 0; }
    nucleo_app_request_draw();
}

// settings row change: dice count 1..6 or die type (a new type re-poses the dice so faces match the total)
static void set_change(int dir)
{
    if (s_setm.sel == 0) { s_n += dir; if (s_n < 1) s_n = 1; if (s_n > MAXDICE) s_n = MAXDICE; }
    else { s_type = (s_type + NTYPES + dir) % NTYPES; pose_all(); }
    nucleo_audio_tone(880, 25, 40);
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_settings) {
        if (gui::menu_key(s_setm, key, 2)) nucleo_app_request_draw();
        else if (key == NK_RIGHT) set_change(+1);
        else if (key == NK_ENTER) { s_settings = false; nucleo_app_request_draw(); }
        return;
    }
    if (key == NK_ENTER || ch == ' ') { if (s_state != ST_ROLL) throw_dice(0.5f); return; }   // quick standard roll
    if (key == NK_UP || key == NK_DOWN) {
        if (s_state == ST_ROLL) return;
        s_charge += 0.10f;
        if (s_charge > 1) s_charge = 1;
        s_state = ST_CHARGE;
        s_keymix_us = esp_timer_get_time() + 350000;
        nucleo_app_request_draw();
    }
    if (ch >= '1' && ch <= '6' && s_state == ST_IDLE) { s_n = ch - '0'; nucleo_app_request_draw(); }   // quick count
}

// LEFT/BACK route here. In settings: LEFT = decrease; BACK closes settings (not the app). Else: BACK exits.
static bool back(int key)
{
    if (s_settings) {
        if (key == NK_LEFT) set_change(-1);
        else { s_settings = false; nucleo_app_request_draw(); }
        return true;
    }
    return key == NK_LEFT;                                    // LEFT does nothing on the table; Esc leaves
}

static void tab(void) { if (s_state == ST_ROLL) return; s_settings = !s_settings; s_setm = {}; s_state = ST_IDLE; s_charge = 0; nucleo_app_request_draw(); }

static void enter(void)
{
    game_text_open("dice");
    pose_all();
    s_state = ST_IDLE; s_charge = 0; s_go_held = false; s_settings = false; s_last_us = 0; s_keymix_us = 0; s_hint = nullptr;
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_set_ptt_handler(ptt);
    nucleo_app_request_draw();
}
static void on_exit(void) { game_text_close(); }

// Working RAM: allocated (zeroed) by the framework before enter(), freed after on_exit(). Foreground-only.
static const nucleo_app_ram_t APP_RAM[] = {
    { (void **)&s_d, sizeof(Die) * MAXDICE },
    { nullptr, 0 }
};

extern "C" void nucleo_register_dice(void)
{
    static const nucleo_app_def_t app = {
        "dice", "Dadi", "Games", "Dadi 3D d4/d6/d8/d20 (scuoti o tieni GO)",
        'D', C_RED, enter, on_key, nullptr, draw, on_exit,
        NX_NET_APP,  // dedicate RAM + free the shared I2S line, consistent with the other games
        APP_RAM
    };
    nucleo_app_register(&app);
}
