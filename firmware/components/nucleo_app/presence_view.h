// presence_view — every pixel of the Presence app, as pure drawing over a LovyanGFX target.
// No radio, no framework state: app_presence.cpp fills a pv_model_t and calls these; tools/presence-host
// renders the same functions on the PC (npm run presence:test).
//
// What the scope means (the user must be able to read it without a manual):
//   centre  = you (this Cardputer)          top marker = the router, at its distance estimated from RSSI
//   rings   = true metres (linear)          the ellipse between the two = the sensitive zone: the radio path
//   a person glyph + ripples inside the zone = motion there, coloured by how recent it is.
// With a second Cardputer in Faro mode there are TWO radio paths, so two zones (router up, Faro lower right):
// each lights on its own, which says WHICH path someone is crossing. The Faro's real bearing is unknown too,
// so its zone sits at a fixed, labelled place.
// CSI has no bearing: nothing is ever drawn outside the zone, and the glyph's place inside it is not a
// position. The sweep turns only while CSI frames arrive, so a frozen beam means "no data".
#pragma once
#include <M5GFX.h>
#include <stdint.h>
#include "presence_detect.h"
#include "presence_alarm.h"

typedef enum { PV_OK, PV_NOWIFI, PV_BUSY, PV_NOCSI } pv_status_t;
// Screens of the Cardputer: the radar, the TAB panel (settings + contextual help), and two pages it opens.
typedef enum { PV_RADAR, PV_MENU, PV_DETAILS, PV_INFO } pv_view_t;
// TAB panel rows, in order.
typedef enum { PV_ROW_ALARM, PV_ROW_THRESHOLD, PV_ROW_ARMDELAY, PV_ROW_DURATION, PV_ROW_CALIBRATE, PV_ROW_FARO,
               PV_ROW_DETAILS, PV_ROW_INFO, PV_NROWS } pv_row_t;

typedef struct {
    const pd_state_t *pd;
    pv_status_t status;
    pv_view_t   view;
    int         sel;           // focused TAB-panel row (pv_row_t)
    uint32_t    now_ms;
    int         rate;          // CSI frames per second, last second
    float       sweep;         // beam angle, radians
    float       glyph;         // 0..1: where along the zone the person glyph sits (drifts while moving)
    const char *ssid;          // associated network, "" if unknown
    const pd_state_t *faro;    // the second link (a Cardputer in Faro mode), NULL while none is heard
    int         faro_rate;     // its frames per second
    bool        beacon;        // THIS Cardputer is the Faro: it transmits, it does not sense
    uint32_t    beacon_sent;   // frames transmitted so far
    const pa_t *alarm;         // the intrusion alarm
    int         pin_len;       // digits typed so far (shown as dots, never as text)
    bool        pin_bad;       // the last PIN was wrong
} pv_model_t;

// The Cardputer's 240-wide content area [top, top+h): the radar, the TAB panel or one of its pages.
void pv_draw_compact(LovyanGFX &g, int top, int h, const pv_model_t &m);
// The Cardputer while the external panel shows the radar: status, the keys and what to do now.
// (TAB still opens the panel here: then pv_draw_compact draws it.)
void pv_draw_guide(LovyanGFX &g, int top, int h, const pv_model_t &m);
// The external 320x240 panel, composited in strips: draws the whole frame shifted up by `oy`, so a
// 320xN strip buffer receives panel rows [oy, oy+N).
void pv_draw_large(LovyanGFX &g, int oy, const pv_model_t &m);

// The control centre on the large panel: a 2 x 4 grid of tiles, one per pv_row_t (panel coordinates).
#define PV_TILE_W 151
#define PV_TILE_H 44
#define PV_TILE_X(i) (6 + ((i) % 2) * (PV_TILE_W + 6))
#define PV_TILE_Y(i) (47 + ((i) / 2) * (PV_TILE_H + 4))

// The large panel's "Menu" button (opens the control centre by touch; panel coordinates).
#define PV_MENU_X 204
#define PV_MENU_Y 206
#define PV_MENU_W 112
#define PV_MENU_H 30

#ifdef PV_LAYOUT_PROBE
// Host-only layout probe (tools/presence-host): every text and every map mark records its box, so the
// harness can fail on two of them touching or on anything leaving the screen.
typedef struct { short x, y, w, h; char kind; char what[24]; } pv_box_t;   // kind: 't' text, 'm' mark
extern pv_box_t g_pv_box[256];
extern int g_pv_nbox;
#endif
