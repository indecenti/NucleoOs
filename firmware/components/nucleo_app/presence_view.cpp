// presence_view.cpp — see presence_view.h.
#include "presence_view.h"
#include "launcher_theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
extern "C" {
#include "nucleo_i18n.h"
}

// The scope is its own dark instrument in every theme: phosphor green on near-black.
static const uint16_t SC_BG = 0x0861, SC_GRID = 0x1A85, SC_RIM = 0x2C48, SC_TICK = 0x3D0A, SC_SWEEP = 0x5FEB,
                      SC_TRAIL = 0x1BE4, ZONE = 0x2B5A, HOT = 0xF9E4, WARM = 0xFD00, ROUTER = 0x8DDF,
                      FARO = 0xFEA0, YOU = 0xFFFF, ARMED_RED = 0xC000;
static const float DEG = 57.29578f;
static const float A_ROUTER = -90, A_FARO = 35;    // where the two radio paths are drawn (bearing is unknown)

static uint16_t mix(uint16_t a, uint16_t b, float t)   // a -> b
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t));
}

// ── the two links seen as one room ────────────────────────────────────────────
struct agg_t {
    bool calib, present, moving;
    float shown;
    uint32_t last_motion, clear_since;
};

static bool live(const pd_state_t *p) { return p && p->phase == PD_RUN; }

static agg_t aggregate(const pv_model_t &m)
{
    const pd_state_t *a = m.pd, *b = m.faro;
    agg_t r = {};
    r.calib = a->phase == PD_CALIB && !live(b);
    r.present = (live(a) && a->present) || (live(b) && b->present);
    r.moving = (live(a) && a->moving && a->present) || (live(b) && b->moving && b->present);
    r.shown = b && b->shown > a->shown ? b->shown : a->shown;
    r.last_motion = a->last_motion_ms;
    if (b && b->last_motion_ms > r.last_motion) r.last_motion = b->last_motion_ms;
    r.clear_since = a->clear_since_ms;
    if (b && b->clear_since_ms > r.clear_since && !a->present) r.clear_since = b->clear_since_ms;
    return r;
}

// Which side someone is crossing: 0 none, 1 router path, 2 Faro path, 3 both.
static int sides(const pv_model_t &m)
{
    return (live(m.pd) && m.pd->present ? 1 : 0) | (live(m.faro) && m.faro->present ? 2 : 0);
}

// ── words ─────────────────────────────────────────────────────────────────────
static void dur(char *o, int n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s < 60) snprintf(o, n, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(o, n, "%lum", (unsigned long)(s / 60));
    else snprintf(o, n, "%luh", (unsigned long)(s / 3600));
}

static const char *router_word(void) { return TR5("router", "router", "router", "routeur", "Router"); }
static const char *faro_word(void)   { return TR5("Faro", "Beacon", "Faro", "Phare", "Bake"); }

static pa_state_t ast(const pv_model_t &m);
static const char *alarm_word(const pv_model_t &m);
static uint16_t alarm_col(const pv_model_t &m);

static const char *title(const pv_model_t &m)
{
    if (m.beacon) return TR5("FARO", "BEACON", "FARO", "PHARE", "BAKE");
    if (ast(m) != PA_OFF) return alarm_word(m);
    if (m.status == PV_NOWIFI) return TR5("NO WI-FI", "NO WI-FI", "SIN WI-FI", "PAS DE WI-FI", "KEIN WLAN");
    if (m.status != PV_OK)     return TR5("RADIO OFF", "RADIO OFF", "RADIO OFF", "RADIO OFF", "RADIO AUS");
    agg_t a = aggregate(m);
    if (a.calib) return TR5("CALIBRO", "CALIBRATING", "CALIBRANDO", "CALIBRAGE", "KALIBRIERE");
    return a.present ? TR5("PRESENZA", "PRESENCE", "PRESENCIA", "PRESENCE", "PRAESENZ")
                     : TR5("LIBERO", "CLEAR", "LIBRE", "LIBRE", "FREI");
}

static uint16_t title_col(const pv_model_t &m)
{
    if (m.beacon) return FARO;
    if (ast(m) != PA_OFF) return alarm_col(m);
    if (m.status != PV_OK) return C_YELLOW;
    agg_t a = aggregate(m);
    if (a.calib) return C_YELLOW;
    return a.present ? HOT : C_GREEN;
}

static void subtitle(char *o, int n, const pv_model_t &m)
{
    char t[12];
    if (m.beacon) { snprintf(o, n, TR5("trasmetto %d/s", "sending %d/s", "emito %d/s", "j'emets %d/s", "sende %d/s"), m.rate); return; }
    if (ast(m) == PA_ARMING) { snprintf(o, n, TR5("esci: armo in %ds", "leave: armed in %ds", "sal: armo en %ds", "sors: arme dans %ds", "raus: scharf in %ds"), pa_left_s(m.alarm, m.now_ms)); return; }
    if (ast(m) == PA_TRIGGERED) { snprintf(o, n, TR5("sirena ancora %ds", "siren %ds more", "sirena %ds mas", "sirene encore %ds", "Sirene noch %ds"), pa_left_s(m.alarm, m.now_ms)); return; }
    if (m.status == PV_NOWIFI) { snprintf(o, n, "%s", TR5("connetti il Wi-Fi", "join a Wi-Fi", "conecta Wi-Fi", "connecte le Wi-Fi", "WLAN verbinden")); return; }
    if (m.status == PV_BUSY)   { snprintf(o, n, "%s", TR5("radio occupata", "radio busy", "radio ocupada", "radio occupee", "Radio belegt")); return; }
    if (m.status == PV_NOCSI)  { snprintf(o, n, "%s", TR5("CSI non attivo", "CSI unavailable", "CSI no activo", "CSI inactif", "CSI inaktiv")); return; }
    agg_t a = aggregate(m);
    if (a.calib) {
        int left = (int)((1.0f - pd_calib_progress(m.pd, m.now_ms)) * PD_CALIB_MS / 1000) + 1;
        snprintf(o, n, TR5("resta fermo %ds", "stay still %ds", "quieto %ds", "immobile %ds", "still %ds"), left);
        return;
    }
    if (a.moving) { snprintf(o, n, "%s", TR5("si muove ora", "moving now", "se mueve", "bouge", "bewegt sich")); return; }
    if (a.present) { dur(t, sizeof t, m.now_ms - a.last_motion); snprintf(o, n, TR5("fermo da %s", "still %s", "quieto %s", "immobile %s", "still %s"), t); return; }
    dur(t, sizeof t, m.now_ms - a.clear_since);
    snprintf(o, n, TR5("libero da %s", "clear %s", "libre %s", "libre %s", "frei %s"), t);
}

// Where the motion is, when two paths make that knowable.
static const char *where(const pv_model_t &m)
{
    if (!m.faro) return nullptr;
    switch (sides(m)) {
        case 1: return TR5("lato router", "router side", "lado router", "cote routeur", "Router-Seite");
        case 2: return TR5("lato Faro", "beacon side", "lado Faro", "cote phare", "Baken-Seite");
        case 3: return TR5("entrambi i lati", "both sides", "ambos lados", "des deux cotes", "beide Seiten");
        default: return TR5("due zone attive", "two zones on", "dos zonas", "deux zones", "zwei Zonen");
    }
}

// ── alarm words ───────────────────────────────────────────────────────────────
static pa_state_t ast(const pv_model_t &m) { return m.alarm && !m.beacon ? m.alarm->st : PA_OFF; }
static bool flash_on(const pv_model_t &m) { return (m.now_ms / 300) & 1; }

static const char *alarm_word(const pv_model_t &m)
{
    switch (ast(m)) {
        case PA_ARMING:    return TR5("ARMO", "ARMING", "ARMANDO", "ARMEMENT", "SCHARF..");
        case PA_ARMED:     return TR5("ARMATO", "ARMED", "ARMADO", "ARME", "SCHARF");
        case PA_TRIGGERED: return TR5("INTRUSO!", "INTRUDER!", "INTRUSO!", "INTRUS!", "EINDRING.!");
        default:           return TR5("spento", "off", "apagado", "eteint", "aus");
    }
}

static uint16_t alarm_col(const pv_model_t &m)
{
    switch (ast(m)) {
        case PA_ARMING:    return C_YELLOW;
        case PA_ARMED:     return ARMED_RED;
        case PA_TRIGGERED: return flash_on(m) ? HOT : C_YELLOW;
        default:           return LINE;
    }
}

// The PIN as dots in boxes: never the digits. Red after a wrong attempt.
static void pin_field(LovyanGFX &g, int x, int y, int bw, const pv_model_t &m)
{
    int n = m.pin_len > 4 ? m.pin_len : 4;
    for (int i = 0; i < n; i++) {
        int bx = x + i * (bw + 3);
        g.drawRoundRect(bx, y, bw, bw + 2, 3, m.pin_bad ? HOT : MUTED);
        if (i < m.pin_len) g.fillCircle(bx + bw / 2, y + bw / 2 + 1, bw / 4, FG);
    }
}

// What to do right now, in one line: the most useful thing the user can act on in this state.
static const char *tip(const pv_model_t &m)
{
    if (ast(m) == PA_ARMING)    return TR5("Esci dalla zona: PIN + OK annulla", "Leave the zone: PIN + OK cancels", "Sal de la zona: PIN + OK anula", "Sors de la zone: PIN + OK annule", "Zone verlassen: PIN + OK stoppt");
    if (ast(m) == PA_ARMED)     return TR5("Armato: PIN + OK per disarmare", "Armed: PIN + OK to disarm", "Armado: PIN + OK desarma", "Arme: PIN + OK pour desarmer", "Scharf: PIN + OK entschaerft");
    if (ast(m) == PA_TRIGGERED) return TR5("PIN + OK per fermare", "PIN + OK to stop", "PIN + OK para parar", "PIN + OK pour arreter", "PIN + OK zum Stoppen");
    if (m.beacon) return TR5("Mettimi dall'altra parte", "Put me across the room", "Ponme al otro lado", "Mets-moi en face", "Stell mich gegenueber");
    if (m.status == PV_NOWIFI) return TR5("Connetti il Wi-Fi da Impostazioni", "Join a Wi-Fi in Settings", "Conecta el Wi-Fi en Ajustes", "Connecte le Wi-Fi (Reglages)", "WLAN in Einstellungen verbinden");
    if (m.status == PV_BUSY)   return TR5("Chiudi Sniffer o Airspace", "Close Sniffer or Airspace", "Cierra Sniffer o Airspace", "Ferme Sniffer ou Airspace", "Sniffer/Airspace schliessen");
    if (m.status == PV_NOCSI)  return TR5("Aggiorna il firmware", "Update the firmware", "Actualiza el firmware", "Mets a jour le firmware", "Firmware aktualisieren");
    agg_t a = aggregate(m);
    if (a.calib)               return TR5("Resta fermo: imparo la stanza", "Stay still: learning the room", "Quieto: aprendo la sala", "Immobile: j'apprends la piece", "Still: ich lerne den Raum");
    if (m.rate < 5 && !m.faro) return TR5("Pochi dati: avvicinati al router", "Little data: go nearer the router", "Pocos datos: acercate al router", "Peu de donnees: approche-toi", "Wenig Daten: naeher zum Router");
    if (a.present && m.faro)   return where(m);
    if (a.moving)              return TR5("Movimento tra te e il router", "Motion between you and router", "Movimiento entre tu y el router", "Mouvement vers le routeur", "Bewegung zwischen dir u. Router");
    if (a.present)             return TR5("Qualcuno e' fermo nella zona", "Someone is still in the zone", "Alguien quieto en la zona", "Quelqu'un immobile ici", "Jemand steht still in der Zone");
    if (!m.faro)               return TR5("Piu' preciso: 2o Cardputer Faro", "More precise: a 2nd one as Beacon", "Mas preciso: 2o como Faro", "Plus precis: un 2e en Phare", "Genauer: 2. Geraet als Bake");
    return TR5("Prova: attraversa una zona", "Try: cross one of the zones", "Prueba: cruza una zona", "Essaie: traverse une zone", "Teste: geh durch eine Zone");
}

// ── pieces ────────────────────────────────────────────────────────────────────
#ifdef PV_LAYOUT_PROBE
pv_box_t g_pv_box[256];
int g_pv_nbox;
static int s_probe_oy;      // the large panel draws shifted; boxes are kept in panel coordinates
static void probe(int x, int y, int w, int h, char kind, const char *what)
{
    if (g_pv_nbox < 0 || g_pv_nbox >= 256 || w <= 0 || h <= 0) return;
    pv_box_t &b = g_pv_box[g_pv_nbox++];
    b.x = x; b.y = y + s_probe_oy; b.w = w; b.h = h; b.kind = kind;
    snprintf(b.what, sizeof b.what, "%s", what);
}
#else
#define probe(...) do {} while (0)
#endif

static void text(LovyanGFX &g, const lgfx::IFont *f, int x, int y, const char *s, uint16_t c, textdatum_t dt = textdatum_t::top_left)
{
    g.setFont(f); g.setTextSize(1); g.setTextDatum(dt); g.setTextColor(c);
    g.drawString(s, x, y);
#ifdef PV_LAYOUT_PROBE
    if (!s[0]) return;
    int w = g.textWidth(s), hh = g.fontHeight();
    int bx = (dt & textdatum_t::top_center) ? x - w / 2 : (dt & textdatum_t::top_right) ? x - w : x;
    int by = (dt & textdatum_t::middle_left) ? y - hh / 2 : (dt & textdatum_t::bottom_left) ? y - hh : y;
    probe(bx, by, w, hh, 't', s);
#endif
}

// Text cut to fit `w` pixels in font `f` (no wrap on a 240 px screen: a clean cut beats an overflow).
static void text_fit(LovyanGFX &g, const lgfx::IFont *f, int x, int y, int w, const char *s, uint16_t c)
{
    char b[48]; snprintf(b, sizeof b, "%s", s);
    g.setFont(f);
    while (b[0] && g.textWidth(b) > w) b[strlen(b) - 1] = 0;
    text(g, f, x, y, b, c);
}

// A filled status pill, the biggest font that fits.
static void pill(LovyanGFX &g, int x, int y, int w, int h, const char *s, uint16_t col)
{
    g.fillRoundRect(x, y, w, h, h / 2 < 10 ? h / 2 : 10, col);
    g.setFont(&fonts::Font4);
    const lgfx::IFont *f = g.textWidth(s) <= w - 12 && h >= 26 ? (const lgfx::IFont *)&fonts::Font4 : (const lgfx::IFont *)&fonts::Font2;
    text(g, f, x + w / 2, y + h / 2 + 1, s, INK, textdatum_t::middle_center);
}

// Motion bar with the threshold mark. `shown` 0..1.
static void meter(LovyanGFX &g, int x, int y, int w, int h, float shown, float thr)
{
    g.fillRoundRect(x, y, w, h, h / 2, LINE);
    int fw = (int)(shown * w);
    if (fw > 0) g.fillRoundRect(x, y, fw < h ? h : fw, h, h / 2, shown >= thr ? HOT : C_GREEN);
    int tx = x + (int)(thr * w);
    g.fillRect(tx - 1, y - 3, 3, h + 6, C_YELLOW);
}

// History: the louder of the two links per bucket, threshold dashed.
static void graph(LovyanGFX &g, int x, int y, int w, int h, const pv_model_t &m, bool big)
{
    const pd_state_t *p = m.pd, *f = m.faro;
    g.fillRect(x, y, w, h, SC_BG);
    g.drawRect(x, y, w, h, LINE);
    int n = w - 2 < PD_HIST ? w - 2 : PD_HIST;
    int thr = (int)(p->thr * 255);
    for (int i = 0; i < n; i++) {
        uint8_t v = p->hist[(p->hist_head + PD_HIST - n + i) % PD_HIST];
        if (f) { uint8_t v2 = f->hist[(f->hist_head + PD_HIST - n + i) % PD_HIST]; if (v2 > v) v = v2; }
        int bh = v * (h - 2) / 255;
        if (bh) g.drawFastVLine(x + 1 + i, y + h - 1 - bh, bh, v >= thr ? HOT : 0x3D7B);
    }
    int ty = y + h - 1 - thr * (h - 2) / 255;
    for (int i = 0; i < w - 2; i += 4) g.drawFastHLine(x + 1 + i, ty, 2, C_YELLOW);
    if (big) {                         // the time axis above the frame, never over the bars
        char t[8]; snprintf(t, sizeof t, "-%ds", n * PD_BUCKET_MS / 1000);
        text(g, &fonts::Font0, x, y - 10, t, DIM);
        text(g, &fonts::Font0, x + w, y - 10, TR5("ora", "now", "ahora", "maint.", "jetzt"), DIM, textdatum_t::top_right);
    }
}

static void person(LovyanGFX &g, int x, int y, int s, uint16_t c)
{
    g.fillCircle(x, y - s * 2, s, c);
    g.fillRoundRect(x - s, y - s + 1, 2 * s + 1, s * 3, s, c);
}

static void router_icon(LovyanGFX &g, int x, int y, int s)
{
    probe(x - 2 * s, y - 3 * s, 4 * s + 1, 4 * s, 'm', "router");
    g.fillRoundRect(x - 2 * s, y - s, 4 * s + 1, 2 * s, 1, ROUTER);
    g.drawLine(x - s - 1, y - s, x - 2 * s, y - 3 * s, ROUTER);
    g.drawLine(x + s + 1, y - s, x + 2 * s, y - 3 * s, ROUTER);
}

// The Faro: a small Cardputer with a wave over it.
static void faro_icon(LovyanGFX &g, int x, int y, int s)
{
    probe(x - 3 * s, y - 4 * s, 6 * s + 1, 5 * s + 1, 'm', "faro");
    g.fillRoundRect(x - 2 * s, y - s, 4 * s + 1, 2 * s + 1, 1, FARO);
    g.drawArc(x, y - s, 3 * s, 3 * s - 1, 220, 320, FARO);
}

// Linear range scale chosen so the farthest end lands between ~1/3 and ~3/4 of the radius: the zones
// are always big enough to read, and the ring labels stay true metres.
static float scope_range(float dist)
{
    static const float R[] = { 3, 4.5f, 6, 9, 12, 18, 24, 30, 45 };
    for (float r : R) if (dist * 1.3f <= r) return r;
    return 45;
}

// A zone: the ellipse spanning the radio path from the centre to `len` px along `ang` (degrees), drawn as a
// polygon so it can point anywhere. heat 0..1 lights it; echo 0..1 = the beam has just swept over it.
static void zone(LovyanGFX &g, int cx, int cy, float ang, int len, int r, float heat, float echo, uint16_t tint)
{
    float a = ang / DEG, ca = cosf(a), sa = sinf(a);
    float hx = len / 2.0f + r / 12.0f, hy = len * 0.24f;
    if (hy < r / 7.0f) hy = r / 7.0f;
    float mx = cx + ca * len / 2.0f, my = cy + sa * len / 2.0f;
    uint16_t idle = mix(SC_BG, tint, 0.22f + 0.25f * echo);
    uint16_t fill = heat > 0.02f ? mix(SC_BG, HOT, 0.10f + 0.30f * heat + 0.20f * echo) : idle;
    uint16_t edge = heat > 0.02f ? mix(tint, HOT, 0.4f + 0.6f * heat) : mix(tint, 0xFFFF, 0.3f * echo);
    const int N = 28;
    int px[N], py[N];
    for (int i = 0; i < N; i++) {
        float t = i * 6.2832f / N, ex = cosf(t) * hx, ey = sinf(t) * hy;
        px[i] = (int)lrintf(mx + ex * ca - ey * sa);
        py[i] = (int)lrintf(my + ex * sa + ey * ca);
    }
    for (int i = 1; i < N - 1; i++) g.fillTriangle((int)mx, (int)my, px[i], py[i], px[i + 1], py[i + 1], fill);
    g.fillTriangle((int)mx, (int)my, px[N - 1], py[N - 1], px[0], py[0], fill);
    for (int i = 0; i < N; i++) g.drawLine(px[i], py[i], px[(i + 1) % N], py[(i + 1) % N], edge);
    for (int d = 7; d < len - 6; d += 6)   // the radio path itself, dashed
        g.drawLine(cx + (int)(ca * d), cy + (int)(sa * d), cx + (int)(ca * (d + 3)), cy + (int)(sa * (d + 3)), mix(tint, 0xFFFF, 0.35f));
}

// How freshly the beam swept over `ang`: 1 just now, fading over the trailing ~70 degrees.
static float echo_of(float sweep, float ang)
{
    float d = fmodf(sweep * DEG - ang + 720.0f, 360.0f);
    return d < 70 ? 1.0f - d / 70.0f : 0;
}

static void motion_mark(LovyanGFX &g, int cx, int cy, float ang, int len, int r, const pd_state_t *p, const pv_model_t &m, bool big)
{
    if (!live(p) || !p->present) return;
    float a = ang / DEG, ca = cosf(a), sa = sinf(a);
    // Kept to the middle of the zone and smaller than the end icons, so it never covers them.
    int s = big ? r / 19 : r / 17;
    if (s < 2) s = 2;
    float along = len * (0.38f + 0.24f * m.glyph), side = sinf(m.glyph * 12.566f) * len * 0.06f;
    int gx = cx + (int)(ca * along - sa * side), gy = cy + (int)(sa * along + ca * side) + s;
    float fade = p->moving ? 1 : 1.0f - (float)(m.now_ms - p->last_motion_ms) / PD_HOLD_MS;
    if (fade < 0) fade = 0;
    uint16_t pc = p->moving ? 0xFFF4 : mix(SC_BG, WARM, 0.55f + 0.45f * fade);
    if (p->moving) {
        float ph = (m.now_ms % 900) / 900.0f;
        for (int k = 0; k < 3; k++) {
            float t = (k + ph) / 3.0f;
            g.drawCircle(gx, gy - s, (int)(s * 2.2f + t * len * 0.22f), mix(HOT, SC_BG, t));
        }
    }
    person(g, gx, gy, s + 1, SC_BG);   // dark outline: the glyph reads over any zone colour
    person(g, gx, gy, s, pc);
}

// The radar. `big` = external panel (bigger labels, finer ticks).
static void scope(LovyanGFX &g, int cx, int cy, int r, const pv_model_t &m, bool big)
{
    const pd_state_t *p = m.pd, *f = m.faro;
    bool ok = m.status == PV_OK;
    bool on = ok && (m.rate >= 2 || m.faro_rate >= 2);
    const lgfx::IFont *small = big ? (const lgfx::IFont *)&fonts::Font2 : (const lgfx::IFont *)&fonts::Font0;
    g.fillCircle(cx, cy, r, SC_BG);

    // Phosphor trail behind the beam (only while frames arrive: a frozen beam = no data).
    if (on) {
        const int N = 18;
        for (int k = N - 1; k >= 0; k--)
            g.fillArc(cx, cy, r - 1, 0, m.sweep * DEG - (k + 1) * 2.5f, m.sweep * DEG - k * 2.5f, mix(SC_BG, SC_TRAIL, 0.75f * (1.0f - (float)k / N)));
    }

    // Range rings at 1/3 and 2/3, crosshair; bezel with a tick every 10 degrees, long ones every 30.
    float dr = ok ? pd_distance_m(p->rssi) : 4, df = f ? pd_distance_m(f->rssi) : 0;
    float R = scope_range(dr > df ? dr : df);
    for (int i = 1; i <= 2; i++) g.drawCircle(cx, cy, r * i / 3, SC_GRID);
    g.drawFastHLine(cx - r, cy, 2 * r, SC_GRID);
    g.drawFastVLine(cx, cy - r, 2 * r, SC_GRID);
    for (int a = 0; a < 360; a += 10) {
        float t = a / DEG;
        int in = a % 30 ? (big ? 3 : 2) : (big ? 7 : 4);
        g.drawLine(cx + (int)(cosf(t) * (r - in)), cy + (int)(sinf(t) * (r - in)), cx + (int)(cosf(t) * r), cy + (int)(sinf(t) * r), a % 30 ? SC_RIM : SC_TICK);
    }
    g.drawCircle(cx, cy, r, SC_TICK);
    for (int i = 1; i <= 2 && on; i++) {        // two labels set the step; no data: "waiting" owns the lower half
        char t[8];
        float v = R * i / 3;
        if (v < 10 && v != (int)v) snprintf(t, sizeof t, "%.1fm", v); else snprintf(t, sizeof t, "%dm", (int)(v + 0.5f));
        // Along the lower-left diagonal: the one direction no zone, icon or label ever uses.
        // Hung just outside the ring (the text's top-right corner on it), so it moves away from the centre.
        int rr = r * i / 3, k = (int)(rr * 0.707f);
        text(g, &fonts::Font0, cx - k - 1, cy + k + 1, t, SC_TICK, textdatum_t::top_right);
    }

    // The zones: the radio paths you <-> router and, with a Faro, you <-> Faro.
    auto len_of = [&](float d) { float fr = d / R; fr = fr < 0.45f ? 0.45f : fr > 0.85f ? 0.85f : fr; return (int)(fr * r); };
    int lr = len_of(dr), lf = f ? len_of(df) : 0;
    if (ok) {
        zone(g, cx, cy, A_ROUTER, lr, r, live(p) ? p->shown : 0, on ? echo_of(m.sweep, A_ROUTER) : 0, ZONE);
        if (f) zone(g, cx, cy, A_FARO, lf, r, live(f) ? f->shown : 0, on ? echo_of(m.sweep, A_FARO) : 0, mix(FARO, SC_BG, 0.35f));
        motion_mark(g, cx, cy, A_ROUTER, lr, r, p, m, big);
        if (f) motion_mark(g, cx, cy, A_FARO, lf, r, f, m, big);
    }

    // The beam on top, then the ends of the radio paths: icons only. Their names and distances live in
    // the legend beside the map (same icon, same colour), so no word ever lands on the picture.
    if (on) g.drawLine(cx, cy, cx + (int)(cosf(m.sweep) * (r - 1)), cy + (int)(sinf(m.sweep) * (r - 1)), SC_SWEEP);
    if (ok) {
        int s = big ? 3 : 2;
        router_icon(g, cx, cy - lr, s);
        if (f) faro_icon(g, cx + (int)(cosf(A_FARO / DEG) * lf), cy + (int)(sinf(A_FARO / DEG) * lf), s);
    }
    int yw = big ? 15 : 11, yh = big ? 9 : 7;
    g.fillRoundRect(cx - yw / 2, cy - yh / 2, yw, yh, 2, YOU);
    probe(cx - yw / 2, cy - yh / 2, yw, yh, 'm', "you");
    // "you" in the upper-left quarter: no zone, icon or ring label ever goes there.
    text(g, small, cx - yw / 2 - 3, cy - yh / 2 - 2, TR5("tu", "you", "tu", "toi", "du"), YOU, textdatum_t::bottom_right);

    // Alarm: a red perimeter of dashes turning round the rim while armed (it reads as "guarded" at a
    // glance), a yellow ring closing over the exit delay, the whole rim flashing when it fires.
    if (ast(m) == PA_ARMED) {
        float rot = (m.now_ms % 6000) * 0.06f;
        for (int a = 0; a < 360; a += 15) g.fillArc(cx, cy, r + 0, r - (big ? 4 : 3), rot + a, rot + a + 8, ARMED_RED);
    } else if (ast(m) == PA_ARMING) {
        float left = pa_left_s(m.alarm, m.now_ms) / (float)PA_ARM_S[m.alarm->arm_i];
        g.fillArc(cx, cy, r, r - (big ? 5 : 3), -90, -90 + 360 * left, C_YELLOW);
    } else if (ast(m) == PA_TRIGGERED && flash_on(m)) {
        g.fillArc(cx, cy, r, r - (big ? 8 : 5), 0, 360, HOT);
    }

    // Calibration progress round the rim; "waiting" when the radio delivers nothing.
    if (ok && aggregate(m).calib)
        g.fillArc(cx, cy, r - 1, r - (big ? 6 : 4), -90, -90 + 360 * pd_calib_progress(p, m.now_ms), C_YELLOW);
    if (ok && !on)
        text(g, &fonts::Font2, cx, cy + r / 2, TR5("in attesa dati", "waiting for data", "esperando datos", "attente donnees", "warte auf Daten"), C_YELLOW, textdatum_t::middle_center);
}

// The Faro screen's graphic: this Cardputer at the centre, waves leaving it at the transmit rate.
static void beacon_art(LovyanGFX &g, int cx, int cy, int r, const pv_model_t &m)
{
    g.fillCircle(cx, cy, r, SC_BG);
    g.drawCircle(cx, cy, r, SC_RIM);
    float ph = (m.now_ms % 1200) / 1200.0f;
    for (int k = 0; k < 4; k++) {
        float t = (k + ph) / 4.0f;
        g.drawCircle(cx, cy, (int)(r * 0.18f + t * r * 0.8f), mix(FARO, SC_BG, t));
        g.drawCircle(cx, cy, (int)(r * 0.18f + t * r * 0.8f) + 1, mix(FARO, SC_BG, t * 0.6f + 0.4f));
    }
    faro_icon(g, cx, cy + r / 12, r / 14 < 3 ? 3 : r / 14);
}

static const char *const *beacon_lines(void)
{
    static const char *const IT[] = { "Questo Cardputer trasmette:", "mettilo dall'altra parte", "della stanza o sulla porta.", "L'altro (stesso Wi-Fi) vede", "chi passa in mezzo." };
    static const char *const EN[] = { "This Cardputer transmits:", "put it across the room", "or by the door to watch.", "The other one (same Wi-Fi)", "sees who walks between." };
    static const char *const ES[] = { "Este Cardputer emite:", "ponlo al otro lado de", "la sala o en la puerta.", "El otro (mismo Wi-Fi) ve", "quien pasa en medio." };
    static const char *const FR[] = { "Ce Cardputer emet:", "pose-le de l'autre cote", "de la piece ou a la porte.", "L'autre (meme Wi-Fi) voit", "qui passe entre les deux." };
    static const char *const DE[] = { "Dieses Geraet sendet:", "stell es gegenueber auf", "oder an die Tuer.", "Das andere (gleiches WLAN)", "sieht, wer dazwischen geht." };
    static const char *const *const ALL[] = { IT, EN, ES, FR, DE };
    return ALL[TR5("0", "1", "2", "3", "4")[0] - '0'];
}

// ── compact (Cardputer) ───────────────────────────────────────────────────────
static void lines(LovyanGFX &g, int x, int y, int step, const char *const *ls, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) text(g, &fonts::Font2, x, y + i * step, ls[i], c);
}

static void error_card(LovyanGFX &g, int x, int y, int w, const pv_model_t &m, bool big)
{
    const char *l[3];
    int n = 0;
    if (m.status == PV_NOWIFI) {
        l[n++] = TR5("Serve una rete Wi-Fi:", "It needs a Wi-Fi network:", "Necesita una red Wi-Fi:", "Il faut un reseau Wi-Fi:", "Braucht ein WLAN:");
        l[n++] = TR5("il router fa da antenna.", "the router is the antenna.", "el router es la antena.", "le routeur sert d'antenne.", "der Router ist die Antenne.");
        l[n++] = TR5("Impostazioni > Wi-Fi", "Settings > Wi-Fi", "Ajustes > Wi-Fi", "Reglages > Wi-Fi", "Einstellungen > WLAN");
    } else if (m.status == PV_BUSY) {
        l[n++] = TR5("Un altro strumento radio", "Another radio tool", "Otra herramienta de radio", "Un autre outil radio", "Ein anderes Funk-Tool");
        l[n++] = TR5("(Sniffer, Airspace) e' attivo.", "(Sniffer, Airspace) is on.", "(Sniffer, Airspace) activo.", "(Sniffer, Airspace) actif.", "(Sniffer, Airspace) laeuft.");
        l[n++] = TR5("Chiudilo e riapri.", "Close it and reopen.", "Cierralo y reabre.", "Ferme-le et rouvre.", "Schliessen, neu oeffnen.");
    } else {
        l[n++] = TR5("Il firmware non ha CSI", "This firmware has no CSI", "El firmware no tiene CSI", "Firmware sans CSI", "Firmware ohne CSI");
        l[n++] = TR5("o la radio l'ha rifiutato.", "or the radio refused it.", "o la radio lo rechazo.", "ou la radio l'a refuse.", "oder Funk lehnt ab.");
    }
    lines(g, x, y, big ? 22 : 17, l, n, m.status == PV_NOWIFI ? FG : C_YELLOW);
}

static void view_radar(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
    int r = (h - 6) / 2;
    scope(g, 3 + r, top + 3 + r, r, m, false);
    int x = 2 * r + 10, w = 240 - x - 3;
    pill(g, x, top + 3, w, 26, title(m), title_col(m));
    char s[32]; subtitle(s, sizeof s, m);
    text_fit(g, &fonts::Font2, x, top + 34, w, s, FG);
    agg_t a = aggregate(m);
    if (ast(m) != PA_OFF) {
        pin_field(g, x, top + 53, 14, m);
        graph(g, x, top + 75, w, h - 78, m, false);
        return;
    }
    meter(g, x, top + 56, w, 9, a.shown, m.pd->thr);
    const char *wh = where(m);
    if (wh && a.present) {           // with a Faro, say which side; the graph gives way
        text_fit(g, &fonts::Font2, x, top + 71, w, wh, HOT);
        graph(g, x, top + 89, w, h - 92, m, false);
    } else graph(g, x, top + 71, w, h - 74, m, false);
}

// Labelled value + bar, one 26 px row (the app_ui_gauge look, drawn on any target).
static void gauge(LovyanGFX &g, int y, const char *label, const char *val, int pct, uint16_t col)
{
    text(g, &fonts::Font2, 10, y - 5, label, MUTED);
    text(g, &fonts::Font2, 230, y - 5, val, col, textdatum_t::top_right);
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    g.fillRoundRect(10, y + 12, 220, 5, 2, LINE);
    if (pct) g.fillRoundRect(10, y + 12, 220 * pct / 100 < 5 ? 5 : 220 * pct / 100, 5, 2, col);
}

static void view_details(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
    const pd_state_t *p = m.pd, *f = m.faro;
    char v[24], lb[24];
    snprintf(lb, sizeof lb, "%s %s", TR5("Moto", "Motion", "Mov.", "Mvt", "Bewegung"), router_word());
    snprintf(v, sizeof v, "%d%%", (int)(p->motion * 100));
    gauge(g, top + 8, lb, v, (int)(p->motion * 100), p->moving ? HOT : C_GREEN);
    if (f) {
        snprintf(lb, sizeof lb, "%s %s", TR5("Moto", "Motion", "Mov.", "Mvt", "Bewegung"), faro_word());
        snprintf(v, sizeof v, "%d%%", (int)(f->motion * 100));
        gauge(g, top + 34, lb, v, (int)(f->motion * 100), f->moving ? HOT : FARO);
    } else {
        int q = pd_quality(p->rssi, m.rate);
        snprintf(v, sizeof v, "%d dBm", (int)p->rssi);
        gauge(g, top + 34, TR5("Segnale", "Signal", "Senal", "Signal", "Signal"), v, q, q >= 50 ? C_GREEN : q >= 25 ? C_YELLOW : HOT);
    }
    if (f) snprintf(v, sizeof v, "%d + %d/s", m.rate, m.faro_rate); else snprintf(v, sizeof v, "%d/s", m.rate);
    gauge(g, top + 60, TR5("Pacchetti", "Packets", "Paquetes", "Paquets", "Pakete"), v, (m.rate + m.faro_rate) * 2, C_BLUE);

    char ln[48], t1[12], t2[12];
    if (f) snprintf(ln, sizeof ln, "%s %ddBm  %s %ddBm", router_word(), (int)p->rssi, faro_word(), (int)f->rssi);
    else snprintf(ln, sizeof ln, "%.14s  ~%.0f m", m.ssid && m.ssid[0] ? m.ssid : router_word(), pd_distance_m(p->rssi));
    text_fit(g, &fonts::Font2, 10, top + 84, 222, ln, MUTED);
    const pd_state_t *e_src = f && f->n_ev && (!p->n_ev || f->ev[0].start_ms > p->ev[0].start_ms) ? f : p;
    if (e_src->n_ev) {
        const pd_event_t &e = e_src->ev[0];
        uint32_t end = e.end_ms ? e.end_ms : m.now_ms;
        dur(t1, sizeof t1, m.now_ms - end); dur(t2, sizeof t2, end - e.start_ms);
        if (e.end_ms) snprintf(ln, sizeof ln, TR5("ultimo: %s fa, %s", "last: %s ago, %s", "ultimo: hace %s, %s", "dernier: il y a %s, %s", "zuletzt: vor %s, %s"), t1, t2);
        else snprintf(ln, sizeof ln, TR5("in corso da %s", "ongoing for %s", "en curso %s", "en cours %s", "laeuft seit %s"), t2);
    } else snprintf(ln, sizeof ln, "%s", TR5("nessun movimento finora", "no motion so far", "sin movimiento", "aucun mouvement", "keine Bewegung"));
    text_fit(g, &fonts::Font2, 10, top + 101, 222, ln, e_src->n_ev ? FG : DIM);
}

static void view_info(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
    static const char *const IT[] = { "Il Wi-Fi corre tra te e il router.", "Chi si muove in mezzo cambia il",
        "segnale: si accende la zona.", "Non vede direzione ne' distanza.", "Con un 2o Cardputer in modo Faro",
        "hai due zone: sai quale lato", "viene attraversato." };
    static const char *const EN[] = { "Wi-Fi runs between you and the", "router. Anyone moving in between",
        "changes it: the zone lights up.", "It sees no direction or distance.", "A 2nd Cardputer as Beacon adds",
        "a second zone: you learn which", "side is being crossed." };
    static const char *const ES[] = { "El Wi-Fi va de ti al router.", "Quien se mueve en medio cambia",
        "la senal: la zona se enciende.", "No ve direccion ni distancia.", "Con un 2o Cardputer como Faro",
        "hay dos zonas: sabes que lado", "se esta cruzando." };
    static const char *const FR[] = { "Le Wi-Fi va de toi au routeur.", "Qui bouge entre les deux change",
        "le signal: la zone s'allume.", "Ni direction ni distance.", "Un 2e Cardputer en Phare ajoute",
        "une 2e zone: tu sais quel cote", "est traverse." };
    static const char *const DE[] = { "WLAN laeuft von dir zum Router.", "Wer dazwischen geht, aendert",
        "das Signal: die Zone leuchtet.", "Keine Richtung, keine Distanz.", "Ein 2. Cardputer als Bake gibt",
        "eine 2. Zone: du siehst, welche", "Seite durchquert wird." };
    static const char *const *const ALL[] = { IT, EN, ES, FR, DE };
    const char *const *L = ALL[TR5("0", "1", "2", "3", "4")[0] - '0'];
    for (int i = 0; i < 7; i++) text_fit(g, &fonts::Font2, 6, top + 2 + i * 17, 230, L[i], i < 4 ? FG : FARO);
}

// The TAB panel: the app's own control centre. Same rows on the Cardputer (a scrolling list) and on the
// external panel (a grid of touch tiles): a label, a live value, and one line saying what the row does.
static const char *row_label(int r, const pv_model_t &m)
{
    switch (r) {
        case PV_ROW_ALARM:     return ast(m) == PA_OFF ? TR5("Arma allarme", "Arm the alarm", "Armar alarma", "Armer l'alarme", "Alarm scharf")
                                                       : TR5("Disarma (PIN)", "Disarm (PIN)", "Desarmar (PIN)", "Desarmer (PIN)", "Entschaerfen");
        case PV_ROW_THRESHOLD: return TR5("Sensibilita'", "Sensitivity", "Sensibilidad", "Sensibilite", "Empfindlichkeit");
        case PV_ROW_ARMDELAY:  return TR5("Armo dopo", "Arm after", "Armar tras", "Armer apres", "Scharf nach");
        case PV_ROW_DURATION:  return TR5("Sirena per", "Siren for", "Sirena por", "Sirene pendant", "Sirene fuer");
        case PV_ROW_CALIBRATE: return TR5("Ricalibra", "Recalibrate", "Recalibrar", "Recalibrer", "Neu kalibrieren");
        case PV_ROW_FARO:      return m.beacon ? TR5("Ferma il Faro", "Stop the Beacon", "Parar el Faro", "Arreter le Phare", "Bake stoppen")
                                               : TR5("Fai da Faro", "Be the Beacon", "Hacer de Faro", "Devenir Phare", "Als Bake senden");
        case PV_ROW_DETAILS:   return TR5("Dettagli segnale", "Signal details", "Detalles senal", "Details du signal", "Signaldetails");
        default:               return TR5("Come funziona", "How it works", "Como funciona", "Comment ca marche", "So funktioniert's");
    }
}

static const char *row_help(int r, const pv_model_t &m)
{
    switch (r) {
        case PV_ROW_ALARM:     return ast(m) == PA_OFF ? TR5("sirena se qualcuno si muove", "siren when someone moves", "sirena si alguien se mueve", "sirene si quelqu'un bouge", "Sirene bei Bewegung")
                                                       : TR5("digita il PIN e premi OK", "type the PIN, then OK", "escribe el PIN y OK", "tape le PIN puis OK", "PIN tippen, dann OK");
        case PV_ROW_THRESHOLD: return TR5("< >  alta = piu' sensibile", "< >  high = more sensitive", "< >  alta = mas sensible", "< >  haute = plus sensible", "< >  hoch = empfindlicher");
        case PV_ROW_ARMDELAY:  return TR5("< >  il tempo per uscire", "< >  time to walk away", "< >  tiempo para salir", "< >  le temps de sortir", "< >  Zeit zum Gehen");
        case PV_ROW_DURATION:  return TR5("< >  poi si disarma da solo", "< >  then it disarms itself", "< >  luego se desarma solo", "< >  puis se desarme seul", "< >  dann von selbst aus");
        case PV_ROW_CALIBRATE: return TR5("stanza vuota o resta fermo 6s", "empty room or keep still 6s", "sala vacia o quieto 6s", "piece vide ou immobile 6s", "leerer Raum oder 6s still");
        case PV_ROW_FARO:      return m.beacon ? TR5("torna a rilevare", "go back to sensing", "volver a detectar", "revenir a la detection", "wieder erkennen")
                                               : TR5("trasmetto, l'altro Cardputer rileva", "I transmit, the other one senses", "emito, el otro detecta", "j'emets, l'autre detecte", "ich sende, das andere misst");
        case PV_ROW_DETAILS:   return TR5("segnale, pacchetti, ultimo evento", "signal, packets, last event", "senal, paquetes, ultimo evento", "signal, paquets, evenement", "Signal, Pakete, Ereignis");
        default:               return TR5("cosa vede il radar e cosa no", "what the radar sees, what not", "que ve el radar y que no", "ce que voit le radar", "was das Radar sieht");
    }
}

// Sensitivity is what users understand; the detector's threshold is its inverse.
static int sensitivity(const pd_state_t *p) { return (int)((1.0f - p->thr) * 100 + 0.5f); }

static void secs(char *o, int n, int s)
{
    if (s < 60) snprintf(o, n, "%d s", s); else snprintf(o, n, "%d min", s / 60);
}

// The row's live value, with its colour. "" = none.
static const char *row_value(int r, const pv_model_t &m, uint16_t *col)
{
    static char v[20];
    v[0] = 0;
    *col = C_YELLOW;
    switch (r) {
        case PV_ROW_ALARM:     *col = ast(m) == PA_OFF ? MUTED : alarm_col(m); return alarm_word(m);
        case PV_ROW_THRESHOLD: snprintf(v, sizeof v, "%d%%", sensitivity(m.pd)); break;
        case PV_ROW_ARMDELAY:  if (m.alarm) secs(v, sizeof v, PA_ARM_S[m.alarm->arm_i]); break;
        case PV_ROW_DURATION:  if (m.alarm) secs(v, sizeof v, PA_DUR_S[m.alarm->dur_i]); break;
        case PV_ROW_FARO:      if (m.faro && !m.beacon) { *col = C_GREEN; return TR5("collegato", "linked", "enlazado", "relie", "verbunden"); }
                               if (m.beacon) { *col = FARO; return TR5("attivo", "on", "activo", "actif", "an"); }
                               break;
        default: break;
    }
    return v;
}

static bool row_adjustable(int r) { return r == PV_ROW_THRESHOLD || r == PV_ROW_ARMDELAY || r == PV_ROW_DURATION; }
static uint16_t row_accent(int r) { return r == PV_ROW_ALARM ? HOT : r == PV_ROW_FARO ? FARO : C_BLUE; }

static void view_menu(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
    const int rh = 19, y0 = top + 2, vis = 5;
    int first = m.sel - vis / 2;
    first = first < 0 ? 0 : first > PV_NROWS - vis ? PV_NROWS - vis : first;
    char v[24];
    for (int i = 0; i < vis; i++) {
        int r = first + i, y = y0 + i * rh;
        bool on = r == m.sel;
        if (on) g.fillRoundRect(4, y, 226, rh - 2, 8, row_accent(r));
        char k[2] = { (char)('1' + r), 0 };
        text(g, &fonts::Font0, 11, y + 5, k, on ? INK : DIM);
        uint16_t col;
        const char *val = row_value(r, m, &col);
        int vw = 0;
        if (val[0]) {
            if (row_adjustable(r) && on) snprintf(v, sizeof v, "< %s >", val); else snprintf(v, sizeof v, "%s", val);
            g.setFont(&fonts::Font2); vw = (int)g.textWidth(v) + 6;
            text(g, &fonts::Font2, 224, y + 1, v, on ? INK : col, textdatum_t::top_right);
        }
        text_fit(g, &fonts::Font2, 22, y + 1, 202 - vw, row_label(r, m), on ? INK : r == PV_ROW_ALARM ? HOT : r == PV_ROW_FARO ? FARO : FG);
    }
    // Scroll bar: where this window sits in the eight rows.
    int bh = (vis * rh - 4) * vis / PV_NROWS, by = y0 + (vis * rh - 4 - bh) * first / (PV_NROWS - vis);
    g.fillRoundRect(234, y0, 3, vis * rh - 4, 1, LINE);
    g.fillRoundRect(234, by, 3, bh, 1, MUTED);
    int ty = y0 + vis * rh + 2;
    g.drawFastHLine(8, ty - 2, 224, LINE);
    text_fit(g, &fonts::Font2, 8, ty + 1, 226, row_help(m.sel, m), MUTED);
}

// The alarm fired: the whole screen is the alarm. It flashes, says where, counts the siren down and
// takes the PIN.
static void view_intrusion(LovyanGFX &g, int top, int h, const pv_model_t &m, int w)
{
    bool fl = flash_on(m);
    uint16_t bg = fl ? HOT : BG, fg = fl ? INK : HOT;
    g.fillRect(0, top, w, h, bg);
    text(g, &fonts::Font4, w / 2, top + 6, TR5("INTRUSO!", "INTRUDER!", "INTRUSO!", "INTRUS!", "EINDRINGLING!"), fg, textdatum_t::top_center);
    const char *wh = where(m);
    char s[40];
    subtitle(s, sizeof s, m);
    text(g, &fonts::Font2, w / 2, top + 36, wh ? wh : TR5("movimento rilevato", "motion detected", "movimiento detectado", "mouvement detecte", "Bewegung erkannt"), fl ? INK : FG, textdatum_t::top_center);
    text(g, &fonts::Font2, w / 2, top + 54, s, fl ? INK : C_YELLOW, textdatum_t::top_center);
    int n = m.pin_len > 4 ? m.pin_len : 4, bw = 16;
    pin_field(g, (w - n * (bw + 3)) / 2, top + 76, bw, m);
}

static void view_beacon(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
    int r = (h - 6) / 2;
    beacon_art(g, 3 + r, top + 3 + r, r, m);
    int x = 2 * r + 10, w = 240 - x - 3;
    pill(g, x, top + 3, w, 26, title(m), FARO);
    char s[32]; subtitle(s, sizeof s, m);
    text_fit(g, &fonts::Font2, x, top + 34, w, s, FG);
    const char *const *L = beacon_lines();
    for (int i = 0; i < 4; i++) text_fit(g, &fonts::Font0, x, top + 56 + i * 12, w, L[i + 1], i < 2 ? MUTED : DIM);
}

void pv_draw_compact(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
#ifdef PV_LAYOUT_PROBE
    s_probe_oy = 0;
#endif
    g.fillRect(0, top, 240, h, BG);
    if (m.view == PV_MENU) view_menu(g, top, h, m);   // the panel works in every state (Wi-Fi off included)
    else if (m.beacon) view_beacon(g, top, h, m);
    else if (ast(m) == PA_TRIGGERED) view_intrusion(g, top, h, m, 240);
    else if (m.status != PV_OK) {
        pill(g, 6, top + 4, 228, 28, title(m), title_col(m));
        error_card(g, 8, top + 42, 224, m, false);
    } else if (m.view == PV_DETAILS) view_details(g, top, h, m);
    else if (m.view == PV_INFO) view_info(g, top, h, m);
    else view_radar(g, top, h, m);
    g.setFont(&fonts::Font0); g.setTextDatum(textdatum_t::top_left);   // leave the framework default
}

// A key cap: rounded outline, label inside. Returns the x after it.
static int keycap(LovyanGFX &g, int x, int y, const char *k)
{
    g.setFont(&fonts::Font2);
    int w = (int)g.textWidth(k) + 10;
    if (w < 22) w = 22;
    g.drawRoundRect(x, y, w, 17, 4, MUTED);
    text(g, &fonts::Font2, x + w / 2, y + 9, k, FG, textdatum_t::middle_center);
    return x + w + 4;
}

void pv_draw_guide(LovyanGFX &g, int top, int h, const pv_model_t &m)
{
#ifdef PV_LAYOUT_PROBE
    s_probe_oy = 0;
#endif
    if (m.beacon || ast(m) == PA_TRIGGERED) { pv_draw_compact(g, top, h, m); return; }
    g.fillRect(0, top, 240, h, BG);
    if (ast(m) != PA_OFF) {
        char t[40];
        pill(g, 4, top + 3, 232, 30, title(m), title_col(m));
        subtitle(t, sizeof t, m);
        text(g, &fonts::Font2, 120, top + 40, t, FG, textdatum_t::top_center);
        int n = m.pin_len > 4 ? m.pin_len : 4;
        pin_field(g, (240 - n * 21) / 2, top + 62, 18, m);
        text_fit(g, &fonts::Font2, 6, top + h - 18, 230, tip(m), MUTED);
        g.setFont(&fonts::Font0); g.setTextDatum(textdatum_t::top_left);
        return;
    }
    char s[40];
    pill(g, 4, top + 3, 112, 22, title(m), title_col(m));
    subtitle(s, sizeof s, m);
    text_fit(g, &fonts::Font2, 122, top + 7, 114, s, FG);

    const int y0 = top + 31, rh = 20, tx = 62;
    keycap(g, keycap(g, 6, y0, ";"), y0, ".");
    snprintf(s, sizeof s, TR5("sensibilita' %d%%", "sensitivity %d%%", "sensibilidad %d%%", "sensibilite %d%%", "Empfindl. %d%%"), sensitivity(m.pd));
    text(g, &fonts::Font2, tx, y0 + 1, s, C_YELLOW);
    keycap(g, 6, y0 + rh, "OK");
    text_fit(g, &fonts::Font2, tx, y0 + rh + 1, 176, TR5("ricalibra a stanza ferma", "recalibrate, room still", "recalibrar, sala quieta", "recalibrer, piece calme", "neu kalibrieren"), FG);
    keycap(g, 6, y0 + 2 * rh, "TAB");
    text_fit(g, &fonts::Font2, tx, y0 + 2 * rh + 1, 176, TR5("menu, Faro, aiuto", "menu, beacon, help", "menu, Faro, ayuda", "menu, phare, aide", "Menue, Bake, Hilfe"), FG);
    g.drawFastHLine(6, y0 + 3 * rh + 1, 228, LINE);
    text_fit(g, &fonts::Font2, 6, y0 + 3 * rh + 5, 230, tip(m), title_col(m) == C_GREEN ? MUTED : title_col(m));
    g.setFont(&fonts::Font0); g.setTextDatum(textdatum_t::top_left);
}

// ── large (external 320x240) ──────────────────────────────────────────────────
// One link in the right column: its map icon + name + distance, motion %, bar, signal. Returns the y after.
static int link_block(LovyanGFX &g, int x, int y, int w, bool faro, const pd_state_t *p, int rate, bool big)
{
    char s[24];
    uint16_t tint = faro ? FARO : ROUTER;
    if (faro) faro_icon(g, x + 7, y + 10, 2); else router_icon(g, x + 7, y + 10, 2);
    float dist = pd_distance_m(p->rssi);
    snprintf(s, sizeof s, "%s", faro ? faro_word() : router_word());
    int pct = live(p) ? (int)(p->shown * 100 + 0.5f) : 0;
    char v[8]; snprintf(v, sizeof v, "%d%%", pct);
    uint16_t vc = live(p) && p->moving ? HOT : FG;
    if (big) {
        text_fit(g, &fonts::Font2, x + 17, y, w - 17, s, tint);
        text(g, &fonts::Font4, x, y + 18, v, vc);
        text(g, &fonts::Font2, x + w, y + 24, TR5("moto", "motion", "mov.", "mvt", "Bew."), MUTED, textdatum_t::top_right);
        y += 46;
    } else {
        g.setFont(&fonts::Font2);
        int vw = (int)g.textWidth(v);
        text_fit(g, &fonts::Font2, x + 17, y, w - 17 - vw - 4, s, tint);
        text(g, &fonts::Font2, x + w, y, v, vc, textdatum_t::top_right);
        y += 18;
    }
    meter(g, x, y + 3, w, 7, live(p) ? p->shown : 0, p->thr);
    y += 14;
    int q = pd_quality(p->rssi, rate);
    for (int i = 0; i < 4; i++) { int bh = 4 + i * 3; g.fillRect(x + i * 5, y + 13 - bh, 3, bh, q > i * 25 ? tint : LINE); }
    if (big) snprintf(s, sizeof s, "~%.0fm %ddBm %d/s", dist < 1 ? 1.0f : dist, (int)p->rssi, rate);
    else     snprintf(s, sizeof s, "~%.0fm  %d/s", dist < 1 ? 1.0f : dist, rate);
    text_fit(g, &fonts::Font2, x + 24, y, w - 24, s, q >= 25 ? MUTED : C_YELLOW);
    return y + 18;
}

void pv_draw_large(LovyanGFX &g, int oy, const pv_model_t &m)
{
#ifdef PV_LAYOUT_PROBE
    s_probe_oy = oy;
    if (oy != 0) g_pv_nbox = -10000;   // later strips: record nothing (the counter is reset per shot)
#endif
    g.fillRect(0, 0 - oy, 320, 240, BG);
    char s[40];

    // Status bar.
    pill(g, 6, 5 - oy, 178, 32, title(m), title_col(m));
    subtitle(s, sizeof s, m);
    text_fit(g, &fonts::Font2, 192, 5 - oy, 124, s, FG);
    const char *wh = where(m);
    if (ast(m) != PA_OFF) pin_field(g, 192, 21 - oy, 12, m);
    else if (m.beacon && m.ssid && m.ssid[0]) text_fit(g, &fonts::Font2, 192, 21 - oy, 124, m.ssid, ROUTER);   // both must share it
    else if (wh && aggregate(m).present) text_fit(g, &fonts::Font2, 192, 21 - oy, 124, wh, HOT);
    else if (m.status == PV_OK) {
        snprintf(s, sizeof s, TR5("sensibilita' %d%%", "sensitivity %d%%", "sensibilidad %d%%", "sensibilite %d%%", "Empfindl. %d%%"), sensitivity(m.pd));
        text_fit(g, &fonts::Font2, 192, 21 - oy, 124, s, C_YELLOW);
    }
    g.drawFastHLine(0, 41 - oy, 320, LINE);

    if (m.view == PV_MENU) {          // the control centre: one big touch tile per row
        for (int r = 0; r < PV_NROWS; r++) {
            int x = PV_TILE_X(r), y = PV_TILE_Y(r) - oy;
            bool on = r == m.sel;
            uint16_t acc = row_accent(r);
            if (on) g.fillRoundRect(x, y, PV_TILE_W, PV_TILE_H, 9, acc);
            else { g.fillRoundRect(x, y, PV_TILE_W, PV_TILE_H, 9, mix(BG, acc, 0.12f)); g.drawRoundRect(x, y, PV_TILE_W, PV_TILE_H, 9, mix(BG, acc, 0.6f)); }
            text_fit(g, &fonts::Font2, x + 9, y + 5, PV_TILE_W - 14, row_label(r, m), on ? INK : FG);
            uint16_t col;
            const char *val = row_value(r, m, &col);
            if (val[0]) text(g, &fonts::Font2, x + PV_TILE_W - 9, y + PV_TILE_H - 19, val, on ? INK : col, textdatum_t::top_right);
            if (row_adjustable(r)) text(g, &fonts::Font0, x + 9, y + PV_TILE_H - 13, TR5("tocca: +", "tap: +", "toca: +", "touche: +", "tippen: +"), on ? INK : DIM);
        }
        g.setFont(&fonts::Font0); g.setTextDatum(textdatum_t::top_left);
        return;
    }
    if (m.beacon) {
        beacon_art(g, 88, 141 - oy, 84, m);
        // The five instruction lines as one paragraph, re-wrapped by word for the 132 px column.
        const char *const *L = beacon_lines();
        char para[200] = "";
        for (int i = 0; i < 5; i++) { strncat(para, L[i], sizeof para - strlen(para) - 2); strcat(para, " "); }
        g.setFont(&fonts::Font2);
        int y = 48;
        const char *p = para;
        while (*p && y < 196) {
            char ln[40]; int n = 0, cut = 0;
            while (p[n] && n < (int)sizeof ln - 1) {
                ln[n] = p[n]; ln[n + 1] = 0;
                if (g.textWidth(ln) > 132) break;
                if (p[n] == ' ') cut = n;
                n++;
            }
            if (!p[n]) cut = n;
            ln[cut] = 0;
            text(g, &fonts::Font2, 182, y - oy, ln, y == 48 ? FG : MUTED);
            p += cut;
            while (*p == ' ') p++;
            y += 17;
            if (!cut) break;
        }
        snprintf(s, sizeof s, "%lu", (unsigned long)m.beacon_sent);
        text(g, &fonts::Font4, 314, 200 - oy, s, FARO, textdatum_t::top_right);
        text(g, &fonts::Font0, 314, 228 - oy, TR5("pacchetti inviati", "frames sent", "paquetes enviados", "trames envoyees", "Pakete gesendet"), DIM, textdatum_t::top_right);
        return;
    }
    if (m.status != PV_OK) {
        error_card(g, 16, 70 - oy, 288, m, true);
        return;
    }

    scope(g, 100, 141 - oy, 95, m, true);

    if (ast(m) == PA_TRIGGERED) {     // the right column becomes the alarm: where, and the siren countdown
        const int x = 204, w = 112;
        bool fl = flash_on(m);
        g.fillRoundRect(x, 46 - oy, w, 190, 10, fl ? HOT : mix(BG, HOT, 0.25f));
        const char *wh = where(m);
        text_fit(g, &fonts::Font2, x + 8, 54 - oy, w - 14, wh ? wh : TR5("movimento", "motion", "movimiento", "mouvement", "Bewegung"), fl ? INK : FG);
        snprintf(s, sizeof s, "%d", pa_left_s(m.alarm, m.now_ms));
        text(g, &fonts::Font7, x + w / 2, 86 - oy, s, fl ? INK : C_YELLOW, textdatum_t::top_center);
        text(g, &fonts::Font2, x + w / 2, 144 - oy, TR5("secondi di sirena", "seconds of siren", "segundos de sirena", "secondes de sirene", "Sek. Sirene"), fl ? INK : MUTED, textdatum_t::top_center);
        text(g, &fonts::Font2, x + w / 2, 176 - oy, TR5("PIN + OK", "PIN + OK", "PIN + OK", "PIN + OK", "PIN + OK"), fl ? INK : FG, textdatum_t::top_center);
        int n = m.pin_len > 4 ? m.pin_len : 4;
        pin_field(g, x + (w - n * 19) / 2, 198 - oy, 16, m);
        g.setFont(&fonts::Font0); g.setTextDatum(textdatum_t::top_left);
        return;
    }

    // Right column: one legend block per link (icon = the one on the map), history, the zone key, Menu.
    const int x = 204, w = 112;
    int y = 46;
    y = link_block(g, x, y - oy, w, false, m.pd, m.rate, !m.faro) + oy;
    if (m.faro) y = link_block(g, x, y + 2 - oy, w, true, m.faro, m.faro_rate, false) + oy;
    int gy = y + 12, gh = 186 - gy;
    if (gh >= 14) graph(g, x, gy - oy, w, gh, m, true);

    g.fillRoundRect(x, 191 - oy, 9, 7, 2, mix(SC_BG, ZONE, 0.6f));
    g.drawRoundRect(x, 191 - oy, 9, 7, 2, ZONE);
    text_fit(g, &fonts::Font0, x + 13, 191 - oy, w - 13, TR5("zona sensibile", "sensitive zone", "zona sensible", "zone sensible", "Messzone"), MUTED);

    g.fillRoundRect(PV_MENU_X, PV_MENU_Y - oy, PV_MENU_W, PV_MENU_H, 8, LINE);
    text(g, &fonts::Font2, PV_MENU_X + PV_MENU_W / 2, PV_MENU_Y + PV_MENU_H / 2 + 1 - oy,
         TR5("Menu  [TAB]", "Menu  [TAB]", "Menu  [TAB]", "Menu  [TAB]", "Menue  [TAB]"), FG, textdatum_t::middle_center);
    g.setFont(&fonts::Font0); g.setTextDatum(textdatum_t::top_left);
}
