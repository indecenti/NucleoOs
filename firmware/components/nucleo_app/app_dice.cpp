// Dadi 3D — Games. Polyhedral dice (d4/d6/d8/d20) on the fx3d engine, full-screen, no overlap.
//
// INTERACTION — a "charge / shuffle" model (no twitchy auto-roll): SHAKE the device, HOLD the GO button,
// or SCROLL (up/down) to MIX — the dice whirl in place and build energy; STOP / release and they are
// THROWN, the harder you charged the more violently they tumble and the longer they settle. ENTER does a
// quick standard roll. TAB opens settings (number of dice, die type). Honest randomness from the ESP32
// hardware TRNG. Works without the IMU too (GO / keys). Engine: draw_model_ex (3-axis) + project (pips).
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_imu.h"
#include "nucleo_fx3d.h"
#include "nucleo_audio.h"   // procedural SFX (dice clack)
#include "nucleo_exclusive.h"  // NX_NET_APP: dedicate RAM like every other native game
#include <math.h>
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

struct DieType { const fx3d::Model *m; int faces; float rscale; const char *name; };
static const DieType TYPES[4] = {
    { &CUBE,  6,  1.00f, "d6"  },
    { &TETRA, 4,  1.15f, "d4"  },
    { &OCTA,  8,  1.12f, "d8"  },
    { &ICOSA, 20, 0.64f, "d20" },
};
#define NTYPES 4

// d6 settle poses (turn the value's face toward the camera) — pips read the value
static const float POSE_Y[6] = { (float)M_PI, (float)(M_PI/2), 0, 0, (float)(-M_PI/2), 0 };
static const float POSE_P[6] = { 0, 0, (float)(-M_PI/2), (float)(M_PI/2), 0, 0 };
struct FaceFrame { float n[3], u[3], v[3]; };
static const FaceFrame FACE[6] = {
    { { 0, 0, 1}, {1,0,0}, {0,1,0} }, { { 1, 0, 0}, {0,0,-1},{0,1,0} }, { { 0, 1, 0}, {1,0,0}, {0,0,-1} },
    { { 0,-1, 0}, {1,0,0}, {0,0,1} }, { {-1, 0, 0}, {0,0,1}, {0,1,0} }, { { 0, 0,-1}, {1,0,0}, {0,-1,0} },
};
static const int   PIPN[7] = { 0,1,2,3,4,5,6 };
static const float PIPS[7][6][2] = {
    {{0,0}}, {{0,0}}, {{-0.5f,0.5f},{0.5f,-0.5f}}, {{-0.55f,0.55f},{0,0},{0.55f,-0.55f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.5f},{-0.5f,0.5f},{0,0},{0.5f,-0.5f},{0.5f,0.5f}},
    {{-0.5f,-0.62f},{-0.5f,0},{-0.5f,0.62f},{0.5f,-0.62f},{0.5f,0},{0.5f,0.62f}},
};

#define MAXDICE 6
enum { ST_IDLE, ST_CHARGE, ST_ROLL };
struct Die { float yaw, pitch, bank;  float y0, p0, ye, pe, wob;  int value; };
static Die   s_d[MAXDICE];
static int   s_state = ST_IDLE;
static int   s_n = 2, s_type = 0;
static float s_charge;                 // 0..1 (mixing energy)
static int64_t s_roll_us, s_dur_us, s_last_us, s_keymix_us;
static bool  s_go_held;
static bool  s_settings;               // settings overlay open
static int   s_set_sel;
static uint16_t COL_IVORY, COL_PIP, COL_PIP1, COL_FELT, COL_FELTG;

static float frand(void) { return (float)(esp_random() & 0xFFFFFF) / 16777215.0f; }
static int   randn(int n) { return (int)(esp_random() % (uint32_t)n); }   // uniform 0..n-1 (TRNG)

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
static void face_centroid(const fx3d::Model &m, int t, float *o)
{
    const fx3d::Tri &tr = m.t[t];
    o[0] = (m.v[tr.a].x + m.v[tr.b].x + m.v[tr.c].x) / 3.0f;
    o[1] = (m.v[tr.a].y + m.v[tr.b].y + m.v[tr.c].y) / 3.0f;
    o[2] = (m.v[tr.a].z + m.v[tr.b].z + m.v[tr.c].z) / 3.0f;
}
// yaw,pitch that turn face t's outward normal toward the camera (-z): the settle pose for that value
static void pose_for_face(const fx3d::Model &m, int t, float *yaw, float *pitch)
{
    float n[3]; face_normal(m, t, n);
    float r = sqrtf(n[0] * n[0] + n[2] * n[2]);
    *yaw = atan2f(-n[0], n[2]);
    *pitch = atan2f(n[1], r) + (float)M_PI;
}

// ---- throw: decide results + set up the eased tumble; vigor/duration scale with `charge` ----
static void throw_dice(float charge)
{
    if (charge < 0.05f) charge = 0.05f;
    if (charge > 1.0f)  charge = 1.0f;
    nucleo_audio_blip(0, (int)(charge * 85) + 15);              // throw rumble (louder the harder you charged)
    s_dur_us = (int64_t)((0.9f + 1.6f * charge) * 1000000.0f);   // 0.9–2.5 s
    s_roll_us = esp_timer_get_time();
    s_state = ST_ROLL;
    int faces = TYPES[s_type].faces;
    for (int i = 0; i < s_n; i++) {
        Die &dd = s_d[i];
        dd.y0 = dd.yaw; dd.p0 = dd.pitch;
        dd.value = 1 + randn(faces);
        int spins = 2 + (int)(charge * 5.0f) + randn(2);         // more charge -> more turns
        if (faces == 6) {
            dd.ye = POSE_Y[dd.value - 1] + TWO_PI * spins;       // d6 lands on the value face (pips)
            dd.pe = POSE_P[dd.value - 1] + TWO_PI * (spins - 1);
        } else {                                                  // d4/d8/d20: land the RESULT face toward the camera
            float ty, tp; pose_for_face(*TYPES[s_type].m, dd.value - 1, &ty, &tp);
            dd.ye = ty + TWO_PI * spins;
            dd.pe = tp + TWO_PI * (spins - 1);
        }
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
        for (int i = 0; i < s_n; i++) {
            Die &dd = s_d[i];
            dd.yaw = dd.y0 + (dd.ye - dd.y0) * e;
            dd.pitch = dd.p0 + (dd.pe - dd.p0) * e;
            dd.bank = dd.wob * (1.0f - t) * sinf(t * 16.0f);
        }
        if (t >= 1.0f) {
            s_state = ST_IDLE;
            for (int i = 0; i < s_n; i++) s_d[i].bank = 0;
            nucleo_audio_blip(1, 65);                  // sharp settle clack
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
        s_charge += in * dt * 0.9f; if (s_charge > 1.0f) s_charge = 1.0f;
        float spd = (2.5f + 14.0f * s_charge);                    // whirl faster as it charges
        for (int i = 0; i < s_n; i++) {
            s_d[i].yaw += spd * dt * (0.8f + 0.4f * i * 0.1f);
            s_d[i].pitch += spd * 0.7f * dt;
            s_d[i].bank = 0.15f * sinf((float)now * 1e-6f * 9.0f + i);
        }
    } else if (s_state == ST_CHARGE) {
        if (s_charge >= 0.12f) { throw_dice(s_charge); s_charge = 0; }   // input ended with enough -> THROW
        else { s_charge *= 0.85f; if (s_charge < 0.01f) { s_charge = 0; s_state = ST_IDLE; } }
    }
}

// ---- pips (d6 only) ----
static bool face_visible(int v, float yaw, float pitch, float bank)
{
    fx3d::V3 nrm = { FACE[v - 1].n[0], FACE[v - 1].n[1], FACE[v - 1].n[2] };
    int dx, dy; float dz;
    fx3d::project(nrm, 0, 0, 1.0f, yaw, pitch, bank, &dx, &dy, &dz);
    return dz < -0.12f;
}
static void draw_pips(int v, float cx, float cy, float sc, float yaw, float pitch, float bank)
{
    const FaceFrame &f = FACE[v - 1];
    int pr = (int)(sc * 0.12f); if (pr < 2) pr = 2;
    for (int k = 0; k < PIPN[v]; k++) {
        float pu = PIPS[v][k][0] * 1.5f, pv = PIPS[v][k][1] * 1.5f;
        fx3d::V3 p = {
            f.n[0]*1.03f + f.u[0]*pu*0.42f + f.v[0]*pv*0.42f,
            f.n[1]*1.03f + f.u[1]*pu*0.42f + f.v[1]*pv*0.42f,
            f.n[2]*1.03f + f.u[2]*pu*0.42f + f.v[2]*pv*0.42f,
        };
        int px, py; float pz;
        fx3d::project(p, cx, cy, sc, yaw, pitch, bank, &px, &py, &pz);
        d.fillCircle(px, py, pr, (v == 1) ? COL_PIP1 : COL_PIP);
    }
}

// a digit centred at (px,py), size ts, with a 1px drop shadow so it pops off the face
static void draw_digit(int px, int py, int val, int ts, uint16_t col)
{
    char b[6]; snprintf(b, sizeof(b), "%d", val);
    int x = px - (int)strlen(b) * 3 * ts, y = py - 4 * ts;
    d.setTextSize(ts);
    d.setTextColor(fx3d::rgb(8, 8, 12)); d.setCursor(x + 1, y + 1); d.print(b);   // shadow
    d.setTextColor(col);                 d.setCursor(x, y);         d.print(b);
}

// d4/d8/d20: number every camera-facing face (its fixed value = face index+1); the front-most (the
// settled result) is highlighted. Numbers ride the real faces, so they tumble and settle correctly.
static void draw_face_numbers(const fx3d::Model &m, float cx, float cy, float sc, float yaw, float pitch, float bank)
{
    float fnz[24];                                        // cache each face's rotated normal-z (avoid 2x work)
    int frontT = -1; float frontZ = 1e9f;
    for (int t = 0; t < m.nt; t++) {
        float n[3]; face_normal(m, t, n);
        fx3d::V3 nv = { n[0], n[1], n[2] };
        int dx, dy; fx3d::project(nv, 0, 0, 1.0f, yaw, pitch, bank, &dx, &dy, &fnz[t]);
        if (fnz[t] < frontZ && fnz[t] < -0.30f) { frontZ = fnz[t]; frontT = t; }
    }
    for (int t = 0; t < m.nt; t++) {
        float nz = fnz[t];
        if (nz > -0.32f) continue;                       // skip back / grazing faces
        float ce[3]; face_centroid(m, t, ce);
        fx3d::V3 cv = { ce[0] * 1.02f, ce[1] * 1.02f, ce[2] * 1.02f };
        int px, py; float pz; fx3d::project(cv, cx, cy, sc, yaw, pitch, bank, &px, &py, &pz);
        bool front = (t == frontT);
        float headon = -nz;                              // 1 = facing us straight on
        int ts = (int)(sc * (front ? 0.11f : 0.075f) * (0.55f + 0.45f * headon));
        if (ts < 1) ts = 1;
        if (ts > 3) ts = 3;
        draw_digit(px, py, t + 1, ts, front ? COL_PIP1 : COL_PIP);
    }
}

// grid layout: rows/cols chosen to fill the screen and keep dice apart
static void layout(int n, int idx, int top, int bottom, int *cx, int *cy, float *sc)
{
    int rows = (n <= 3) ? 1 : 2;
    int cols = (n + rows - 1) / rows;
    int r = idx / cols, c = idx % cols;
    int inrow = (r == rows - 1) ? (n - cols * (rows - 1)) : cols;   // last row may have fewer
    int cellw = W / cols, cellh = (bottom - top) / rows;
    *cx = (W - inrow * cellw) / 2 + c * cellw + cellw / 2;
    *cy = top + r * cellh + cellh / 2;
    float s = (cellw < cellh ? cellw : cellh) * 0.40f;
    if (s > 36) s = 36;
    if (s < 13) s = 13;
    *sc = s * TYPES[s_type].rscale;
}

static void draw_settings(int top, int h)
{
    int bw = 196, bx = (W - bw) / 2, by = top + 6, bh = h - 12;
    d.fillRoundRect(bx, by, bw, bh, 8, INK);
    d.drawRoundRect(bx, by, bw, bh, 8, C_GREEN);
    d.setTextSize(2); d.setTextColor(C_GREEN, INK);
    d.setCursor(bx + 12, by + 8); d.print("Impostazioni");
    char rows[2][32];
    snprintf(rows[0], 32, "Dadi:  %d", s_n);
    snprintf(rows[1], 32, "Tipo:  %s", TYPES[s_type].name);
    for (int i = 0; i < 2; i++) {
        bool sel = (i == s_set_sel);
        int ry = by + 36 + i * 24;
        if (sel) d.fillRoundRect(bx + 6, ry - 2, bw - 12, 22, 5, fx3d::rgb(40, 70, 50));
        d.setTextSize(2); d.setTextColor(sel ? C_GREEN : FG, sel ? fx3d::rgb(40,70,50) : INK);
        d.setCursor(bx + 14, ry); d.print(rows[i]);
    }
    d.setTextSize(1); d.setTextColor(MUTED, INK);
    d.setCursor(bx + 12, by + bh - 12); d.print("su/giu  <-/->  TAB chiude");
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height(), bottom = top + h;
    d.fillRect(0, top, W, h, fx3d::rgb(8, 22, 14));
    fx3d::Grid g = { top + 12, bottom - 1, (float)(W/2), 0.0f, (float)(W*0.8f), 7, 9, COL_FELT, COL_FELTG, 150 };
    fx3d::grid(g);

    int total = 0;
    for (int i = 0; i < s_n; i++) {
        Die &dd = s_d[i];
        if (s_state == ST_IDLE) total += dd.value;
        const fx3d::Model &mdl = *TYPES[s_type].m;
        int cx, cy; float sc; layout(s_n, i, top + 2, bottom, &cx, &cy, &sc);
        fx3d::dither_disc(cx, cy + (int)(sc * 1.02f), (int)(sc * 0.78f), fx3d::rgb(0, 0, 0));   // soft contact shadow
        // outline trick: a slightly bigger DARK solid behind reads as a clean rim around the silhouette
        fx3d::draw_model_ex(mdl, (float)cx, (float)cy, sc + 1.6f, dd.yaw, dd.pitch, dd.bank, fx3d::rgb(10, 12, 18), false);
        fx3d::draw_model_ex(mdl, (float)cx, (float)cy, sc, dd.yaw, dd.pitch, dd.bank, COL_IVORY, false);
        if (TYPES[s_type].faces == 6) {
            for (int v = 1; v <= 6; v++)
                if (face_visible(v, dd.yaw, dd.pitch, dd.bank)) draw_pips(v, (float)cx, (float)cy, sc, dd.yaw, dd.pitch, dd.bank);
        } else {
            draw_face_numbers(mdl, (float)cx, (float)cy, sc, dd.yaw, dd.pitch, dd.bank);   // numbers ON the faces
        }
    }

    char rt[24];
    if (s_state == ST_CHARGE) snprintf(rt, sizeof(rt), "mescolo %d%%", (int)(s_charge * 100));
    else if (s_state == ST_ROLL) snprintf(rt, sizeof(rt), "...");
    else snprintf(rt, sizeof(rt), "= %d", total);
    app_ui_title("Dadi", s_state == ST_IDLE ? C_GREEN : C_YELLOW, rt);

    // charge meter while mixing
    if (s_state == ST_CHARGE) {
        int mw = (int)((W - 20) * s_charge);
        d.fillRoundRect(10, bottom - 8, W - 20, 5, 2, fx3d::rgb(30, 40, 34));
        d.fillRoundRect(10, bottom - 8, mw, 5, 2, fx3d::mix(C_YELLOW, C_GREEN, (int)(s_charge * 255)));
    } else if (s_state == ST_IDLE) {
        d.setTextSize(1); d.setTextColor(MUTED, fx3d::rgb(8, 22, 14));
        d.setCursor(6, bottom - 9);
        d.print(nucleo_imu_present() ? "scuoti/GO/scroll per mescolare  INVIO lancia  TAB opz"
                                     : "tieni GO o scrolla per mescolare  INVIO lancia  TAB opz");
    }
    if (s_settings) draw_settings(top, h);
}

static bool poll(void)
{
    int64_t now = esp_timer_get_time();
    float dt = s_last_us ? (float)(now - s_last_us) / 1000000.0f : 0.02f;
    if (dt > 0.05f) dt = 0.05f;
    s_last_us = now;
    if (s_settings) { return false; }                        // frozen while settings open
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

static void on_key(int key, char ch)
{
    if (s_settings) {
        if (key == NK_UP || key == NK_DOWN) { s_set_sel ^= 1; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT) { if (s_set_sel == 0) { if (s_n < MAXDICE) s_n++; } else s_type = (s_type + 1) % NTYPES; nucleo_app_request_draw(); }
        return;
    }
    if (key == NK_ENTER || ch == ' ') { if (s_state != ST_ROLL) throw_dice(0.5f); return; }   // quick standard roll
    if (key == NK_UP || key == NK_DOWN) {
        s_charge += 0.10f;
        if (s_charge > 1) s_charge = 1;
        s_state = ST_CHARGE;
        s_keymix_us = esp_timer_get_time() + 350000;
        nucleo_app_request_draw();
    }
    if (ch >= '1' && ch <= '6') { s_n = ch - '0'; nucleo_app_request_draw(); }
}

// LEFT/BACK route here. In settings: LEFT = decrease; BACK closes settings (not the app). Else: BACK exits.
static bool back(int key)
{
    if (s_settings) {
        if (key == NK_LEFT) { if (s_set_sel == 0) { if (s_n > 1) s_n--; } else s_type = (s_type + NTYPES - 1) % NTYPES; nucleo_app_request_draw(); return true; }
        s_settings = false; nucleo_app_request_draw(); return true;   // BACK closes the panel
    }
    return false;
}

static void tab(void) { s_settings = !s_settings; s_set_sel = 0; nucleo_app_request_draw(); }

static void enter(void)
{
    COL_IVORY = fx3d::rgb(233, 227, 208);
    COL_PIP   = fx3d::rgb(30, 26, 38);
    COL_PIP1  = fx3d::rgb(196, 44, 44);
    COL_FELT  = fx3d::rgb(28, 96, 56);
    COL_FELTG = fx3d::rgb(46, 150, 88);
    for (int i = 0; i < MAXDICE; i++) {
        s_d[i].value = 1 + randn(TYPES[s_type].faces);
        s_d[i].yaw = frand() * TWO_PI; s_d[i].pitch = frand() * TWO_PI; s_d[i].bank = 0;
        s_d[i].ye = s_d[i].yaw; s_d[i].pe = s_d[i].pitch;
    }
    s_state = ST_IDLE; s_charge = 0; s_go_held = false; s_settings = false; s_last_us = 0; s_keymix_us = 0;
    nucleo_app_set_fullscreen(true);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_set_ptt_handler(ptt);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_dice(void)
{
    static const nucleo_app_def_t app = {
        "dice", "Dadi", "Games", "Dadi 3D d4/d6/d8/d20 (scuoti o tieni GO)",
        'D', C_RED, enter, on_key, nullptr, draw, nullptr,
        NX_NET_APP   // dedicate RAM + free the shared I2S line, consistent with the other games
    };
    nucleo_app_register(&app);
}
