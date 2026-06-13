// Built-in "Remote Control" app: status of the web-client handoff + an on/off toggle.
//
// When a browser connects over WebSocket the run loop suspends the on-device UI and hands
// CPU+RAM to the HTTP server (see nucleo_app.cpp). This app shows that state and lets the
// user disable the automatic handoff. It owns the on/off flag; the run loop reads it via
// nucleo_remote_enabled().
#include "nucleo_app.h"
#include "launcher_theme.h"
#include <M5GFX.h>
#include <stdio.h>

extern "C" int nucleo_ws_client_count(void);   // resolved at link (no component dep)

#include "app_gfx.h"

static bool s_remote_enabled = true;            // feature on by default

extern "C" bool nucleo_remote_enabled(void) { return s_remote_enabled; }

static int s_sig = -1;

static void remote_enter(void) { nucleo_app_set_hint("enter toggle  esc back"); s_sig = -1; nucleo_app_request_draw(); }
static void remote_key(int key, char ch) { (void)ch; if (key == NK_ENTER) { s_remote_enabled = !s_remote_enabled; nucleo_app_request_draw(); } }

static void remote_tick(void)   // redraw only when the client count / toggle actually changes
{
    int sig = (nucleo_ws_client_count() << 1) | (s_remote_enabled ? 1 : 0);
    if (sig != s_sig) { s_sig = sig; nucleo_app_request_draw(); }
}

static void remote_draw(void)
{
    int top_y = nucleo_app_content_top();
    d.fillRect(0, top_y, W, nucleo_app_content_height(), BG);
    d.setTextSize(1);
    d.setTextColor(MUTED, BG); d.setCursor(10, top_y + 8); d.print("Auto handoff");
    d.setTextSize(2);
    d.setTextColor(s_remote_enabled ? C_GREEN : C_RED, BG); d.setCursor(10, top_y + 18); d.print(s_remote_enabled ? "ON" : "OFF");

    char l[24]; snprintf(l, sizeof(l), "%d", nucleo_ws_client_count());
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, top_y + 40); d.print("Active web clients");
    d.setTextSize(2); d.setTextColor(C_BLUE, BG); d.setCursor(10, top_y + 50); d.print(l);

    d.setTextSize(1); d.setTextColor(DIM, BG);
    d.setCursor(10, top_y + 74); d.print("When a browser connects, the");
    d.setCursor(10, top_y + 86); d.print("device focuses CPU+RAM on it.");
}

extern "C" void nucleo_register_remote(void)
{
    static const nucleo_app_def_t app = {
        "remote", "Remote Control", "Connect", "Hand the device to a web client when one connects",
        'r', C_BLUE, remote_enter, remote_key, remote_tick, remote_draw, nullptr
    };
    nucleo_app_register(&app);
}
