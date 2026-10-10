// brawler_scene.cpp — SCORRIBANDA: the belt depth plane + the night-street backdrop.
//
// Art direction: a noir NIGHT street per level, 16-bit beat'em-up style — a banded dusk sky, a far skyline
// of hazy silhouettes with scattered lit windows, a near row of buildings with lit windows, shop fronts
// under striped awnings, fire escapes and blinking blade signs, lamp posts and parked cars on the kerb,
// puddles mirroring the neon (and rain on the docks), a sidewalk + kerb and lane dashes that scroll 1:1
// with the camera (the ground itself shows you walking). Three parallax layers + the belt. Each street has
// its own palette (one small table). Colours are RGB332-exact (gui::rgb) so the 8bpp canvas shows them as
// authored. Procedural from a hash of the tile index: zero RAM, no bitmaps. Cheap: ~150 fill calls/frame.
//
// Owns: BR_BELT (the depth plane every module uses to measure feet/scale) + scene_draw().

#include "brawler.h"
#include "game_ui.h"
#include <math.h>

// Belt depth mapping: z=0 (far, small, high) .. z=1 (near, large, low).
//   horizonY = 70  : where the sidewalk meets the buildings.
//   frontY   = 131 : near edge of the belt.
//   farS/nearS = 25/32 : figure scale per unit, far .. near: fighters stand ~44..56 px tall — chunky,
//                        readable characters in every lane (depth also reads from brightness + overlap).
const fxfig::Belt BR_BELT = { 70.0f, 131.0f, 25.0f, 32.0f };

float scene_floor_y(float z) { return fxfig::belt_y(BR_BELT, z); }
float scene_scale(float z) { return fxfig::belt_s(BR_BELT, z); }

// Per-street palette, RGB triples: sky top, sky at the horizon, far skyline, near buildings, lit window,
// neon, asphalt.
static const uint8_t PAL[6][7][3] = {
    { {0,0,85},  {109,36,170}, {36,36,85},  {0,0,85},   {255,219,85},  {255,73,170}, {36,36,85} },   // the alley: indigo night
    { {73,0,85}, {255,109,85}, {109,36,85}, {36,0,85},  {255,219,170}, {255,219,0},  {73,36,85} },   // the market: dusk
    { {0,36,0},  {36,146,85},  {0,73,85},   {0,36,0},   {182,255,170}, {0,255,170},  {36,73,85} },   // the subway: sodium green
    { {36,0,0},  {146,73,0},   {73,36,0},   {36,0,0},   {255,182,85},  {255,146,0},  {73,36,0} },    // the warehouses: rust
    { {36,0,85}, {146,73,255}, {73,36,170}, {36,0,85},  {255,255,170}, {109,219,255},{73,73,85} },   // the rooftops: moonlit
    { {0,36,85}, {36,146,170}, {0,73,85},   {0,36,85},  {255,219,85},  {255,73,85},  {36,73,85} },   // the docks: storm teal
};
static uint16_t pc(int lv, int k) { const uint8_t *c = PAL[lv][k]; return gui::rgb(c[0], c[1], c[2]); }

static int lvl(void) { return g.level < 0 ? 0 : g.level > 5 ? 5 : g.level; }
uint16_t scene_haze(void) { return pc(lvl(), 2); }
uint16_t scene_rim(void)  { return gui::mix(pc(lvl(), 5), 0xFFFF, 40); }

static inline uint32_t hsh(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

// One layer of buildings tiled in world space every `sp` px, scrolled by camx * par. Heights / widths come
// from a hash of the tile index; windows light up from the same hash (`lit` = how many in 16), and the near
// layer hangs a neon sign on some fronts.
static void buildings(int lv, int base, int sp, float par, int hmin, int hmax, uint16_t col, uint16_t win, int lit, bool near_, uint32_t t)
{
    int off = (int)(g.camx * par);
    for (int i = off / sp - 1; ; i++) {
        int bx = i * sp - off;
        if (bx > BR_SW) break;
        uint32_t h = hsh((uint32_t)i * 2654435761u + lv * 977u + (near_ ? 31u : 7u));
        int w = sp - 4 - (int)(h % (unsigned)(sp / 3)), bh = hmin + (int)((h >> 8) % (unsigned)(hmax - hmin + 1));
        d.fillRect(bx, base - bh, w, bh, col);
        if (near_) d.drawFastHLine(bx, base - bh, w, gui::mix(col, win, 60));      // a lit roof edge
        // windows: a grid, each lit from a hash bit (a few flicker); the near row keeps its ground floor
        int cw = near_ ? 7 : 5, rh = near_ ? 8 : 6;
        for (int r = 0; r < (bh - (near_ ? 20 : 8)) / rh && r < 6; r++)
            for (int c = 0; c < (w - 4) / cw && c < 8; c++) {
                uint32_t q = hsh(h + r * 131u + c * 17u);
                if ((int)(q & 15) >= lit) continue;
                bool flick = near_ && (q & 0x300) == 0x300 && ((t / 180 + (q >> 12)) & 7) == 0;
                d.fillRect(bx + 3 + c * cw, base - bh + 5 + r * rh, near_ ? 3 : 2, near_ ? 4 : 2, flick ? col : win);
            }
        if (!near_) continue;
        // the ground floor: a lit shop window under a striped awning (one front in two), always a door
        uint16_t neon = pc(lv, 5), dark = gui::mix(col, 0, 120);
        int gy = base - 13;
        d.drawFastHLine(bx, gy - 1, w, dark);
        if (h & 16) {
            uint16_t glass = gui::mix(win, col, 90);
            d.fillRect(bx + 3, gy + 2, w / 2, 10, glass);
            d.drawFastHLine(bx + 3, gy + 7, w / 2, gui::mix(glass, col, 110));          // a shelf in the window
            for (int k = 0; k < w / 2 + 2; k += 4) {
                d.fillRect(bx + 2 + k, gy - 1, 2, 3, neon);
                d.fillRect(bx + 4 + k, gy - 1, 2, 3, gui::mix(neon, 0xFFFF, 150));
            }
        }
        d.fillRect(bx + w - 11, gy + 2, 7, 11, dark);
        d.drawPixel(bx + w - 6, gy + 8, win);                                             // the door handle
        // a fire escape zig-zagging down the facade (one building in four)
        if ((h & 0x60) == 0x20) {
            uint16_t rail = gui::mix(col, 0xFFFF, 50);
            for (int r = 0, py = base - bh + 12; py < gy - 6 && r < 4; r++, py += 11) {
                d.drawFastHLine(bx + 3, py, w / 2, rail);
                d.drawLine(bx + 5 + (r & 1) * (w / 2 - 8), py, bx + 5 + ((r + 1) & 1) * (w / 2 - 8), py + 11 > gy - 2 ? gy - 2 : py + 11, rail);
            }
        }
        // a vertical blade sign in the street's neon, blinking on its own phase (one front in three)
        if ((h & 3) == 0 && bh > 30) {
            bool on = ((t / 400 + (h >> 20)) % 5) != 0;
            int sx = bx + w - 8, sy = base - bh + 6;
            d.fillRect(sx, sy, 6, 18, on ? neon : gui::mix(col, neon, 70));
            if (on) for (int k = 0; k < 3; k++) d.fillRect(sx + 2, sy + 2 + k * 6, 2, 3, gui::mix(neon, 0xFFFF, 150));
        }
    }
}

void scene_draw(void)
{
    int lv = lvl();
    int hy = (int)BR_BELT.horizonY;
    uint32_t t = br_now_ms();

    // (1) a banded dusk sky (8 bands, the retro look) + stars on the clear nights, a moon over the rooftops
    uint16_t top = pc(lv, 0), hor = pc(lv, 1);
    for (int b = 0; b < 8; b++) d.fillRect(0, b * hy / 8, BR_SW, hy / 8 + 1, gui::mix(top, hor, b * 256 / 7));
    if (lv == 0 || lv == 4) for (int s = 0; s < 14; s++) {
        uint32_t q = hsh(s * 7919u + lv);
        int sx = (int)((q % 260) - (int)(g.camx * 0.05f) % 260); if (sx < 0) sx += 260;
        d.drawPixel(sx, (q >> 9) % (hy / 2), (q >> 20) & 1 ? 0xFFFF : gui::mix(top, 0xFFFF, 140));
    }
    if (lv == 4) { d.fillCircle(186 - (int)(g.camx * 0.03f), 14, 7, gui::rgb(255, 255, 170)); d.fillCircle(183 - (int)(g.camx * 0.03f), 12, 6, top); }

    // (2) far skyline (hazy, slow) and (3) near buildings (lit windows, neon), resting on the sidewalk
    buildings(lv, hy, 26, 0.15f, 16, 44, pc(lv, 2), gui::mix(pc(lv, 2), pc(lv, 4), 110), 3, false, t);
    buildings(lv, hy, 64, 0.45f, 32, 58, pc(lv, 3), pc(lv, 4), 5, true, t);

    // (4) the street: sidewalk + kerb, then asphalt that lightens toward the camera, lane dashes and lamp
    // posts — all at 1:1 with the camera (the belt's own scroll)
    uint16_t road = pc(lv, 6);
    int cam = (int)g.camx;
    d.fillRect(0, hy, BR_SW, 8, gui::mix(road, 0xFFFF, 50));
    for (int x = -(cam % 24); x < BR_SW; x += 24) d.drawFastVLine(x, hy, 8, road);   // paving joints
    d.drawFastHLine(0, hy + 8, BR_SW, gui::mix(road, 0xFFFF, 110));                // the kerb's lit edge
    for (int b = 0; b < 4; b++) {
        int y0 = hy + 9 + b * (BR_SH - hy - 9) / 4;
        d.fillRect(0, y0, BR_SW, (BR_SH - hy - 9) / 4 + 1, gui::mix(road, gui::mix(road, 0xFFFF, 40), b * 85));
    }
    for (int x = -(cam % 160); x < BR_SW + 40; x += 160) {                         // lamp posts on the kerb
        d.fillRect(x - 1, hy - 34, 2, 42, gui::rgb(73, 73, 85));
        d.fillRect(x - 5, hy - 37, 11, 3, gui::rgb(73, 73, 85));
        d.fillRect(x - 4, hy - 34, 9, 2, pc(lv, 4));
    }
    // parked cars along the far kerb, one per stretch of street, each its own colour
    for (int x = -(cam % 420) + 140, n = (cam / 420); x < BR_SW + 40; x += 420, n++) {
        if (x < -40) continue;
        uint32_t q = hsh((uint32_t)n * 911u + lv);
        uint16_t body = gui::rgb(73 + (q & 3) * 60, 36 + ((q >> 2) & 3) * 36, 85 * ((q >> 4) & 3));
        int y = hy + 18;
        d.fillRoundRect(x - 18, y - 8, 36, 8, 3, body);
        d.fillRoundRect(x - 10, y - 14, 20, 7, 3, gui::mix(body, 0, 60));
        d.fillRect(x - 8, y - 12, 7, 4, gui::mix(top, 0xFFFF, 60)); d.fillRect(x + 1, y - 12, 7, 4, gui::mix(top, 0xFFFF, 60));   // windows
        d.drawFastHLine(x - 17, y - 7, 34, gui::mix(body, 0xFFFF, 110));                // the lit roofline of the body
        d.fillCircle(x - 10, y, 3, 0); d.fillCircle(x + 10, y, 3, 0);
        d.drawPixel(x - 10, y, gui::rgb(182, 182, 170)); d.drawPixel(x + 10, y, gui::rgb(182, 182, 170));
        d.fillRect(x + 16, y - 6, 2, 2, gui::rgb(255, 255, 170)); d.fillRect(x - 18, y - 6, 2, 2, gui::rgb(255, 0, 0));   // lights
    }
    // the alley and the docks are wet: puddles mirror the neon; on the docks it rains
    if (lv == 0 || lv == 5) for (int x = -(cam % 190) + 60; x < BR_SW + 30; x += 190) {
        int y = (int)scene_floor_y(0.48f);
        d.fillEllipse(x, y, 18, 3, gui::mix(road, pc(lv, 0), 120));
        d.drawFastHLine(x - 9, y - 1, 12, gui::mix(pc(lv, 5), road, 90));
        d.drawFastHLine(x - 3, y + 1, 9, gui::mix(pc(lv, 4), road, 120));
    }
    if (lv == 5) for (int i = 0; i < 28; i++) {
        uint32_t q = hsh(i * 2246822519u);
        int x = (int)((q % 260 + t / 6 * (1 + (q >> 30))) % 260) - 10, y = (int)(((q >> 9) % 140 + t / 3) % 140);
        d.drawLine(x, y, x - 2, y + 5, gui::mix(top, 0xFFFF, 90));
    }
    int ly = (int)scene_floor_y(0.62f);
    for (int x = -(cam % 48); x < BR_SW; x += 48) d.fillRect(x, ly, 22, 2, gui::mix(road, gui::rgb(255, 219, 85), 120));   // lane dashes
}
