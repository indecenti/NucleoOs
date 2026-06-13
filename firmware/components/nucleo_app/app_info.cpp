// Built-in "Connection" app: shows every address needed to reach the device from a browser
// (Wi-Fi network, IP/.local address, pairing PIN). Redraws ONLY when something it shows
// actually changes (IP settling, PIN regenerated), so the screen is rock-steady instead of
// flickering at the tick rate.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

extern "C" const char *nucleo_auth_pin(void);
extern "C" const char *nucleo_setup_mode(void);
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_setup_ip(void);
extern "C" const char *nucleo_setup_device_name(void);

#include "app_gfx.h"

static char s_info_sig[112];

static void info_enter(void) { nucleo_app_set_hint("esc back"); s_info_sig[0] = 0; nucleo_app_request_draw(); }

static void info_tick(void)
{
    char sig[112];
    snprintf(sig, sizeof(sig), "%s|%s|%s|%s|%s",
             nucleo_setup_mode(), nucleo_setup_ssid(), nucleo_setup_ip(),
             nucleo_auth_pin(), nucleo_setup_device_name());
    if (strcmp(sig, s_info_sig)) { strncpy(s_info_sig, sig, sizeof(s_info_sig) - 1); nucleo_app_request_draw(); }
}

static void info_draw(void)
{
    int top_y = nucleo_app_content_top();
    d.fillRect(0, top_y, W, nucleo_app_content_height(), BG);
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];
    const char *ip = nucleo_setup_ip();
    char url[48], net[40];

    if (sta) {
        snprintf(net, sizeof(net), "%.18s", nucleo_setup_ssid());
        if (ip[0]) snprintf(url, sizeof(url), "%s", ip);
        else       snprintf(url, sizeof(url), "%s.local", nucleo_setup_device_name());
    } else {
        snprintf(net, sizeof(net), "Setup AP");
        snprintf(url, sizeof(url), "192.168.4.1");
    }

    // Network
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, top_y + 4); d.print(sta ? "Wi-Fi" : "Mode");
    d.setTextSize(2); d.setTextColor(C_BLUE, BG); d.setCursor(10, top_y + 14); d.print(net);

    // IP Address
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, top_y + 36); d.print("Address");
    d.setTextSize(2); d.setTextColor(C_GREEN, BG); d.setCursor(10, top_y + 46); d.print(url);

    // Pairing PIN box
    int by = top_y + 68, bh = 24;
    d.drawRoundRect(8, by, W - 16, bh, 6, C_YELLOW);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(16, by + 8); d.print("Pairing PIN");
    if (sta && ip[0]) {
        d.setTextSize(2); d.setTextColor(C_YELLOW, BG); d.setCursor(96, by + 5); d.print(nucleo_auth_pin());
    } else {
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(96, by + 8); d.print("join Wi-Fi to pair");
    }
    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(12, by + bh + 4);
    d.print("Enter IP and PIN in your browser");
}

extern "C" void nucleo_register_info(void)
{
    static const nucleo_app_def_t app = {
        "info", "Connection", "System", "Wi-Fi, IP and web address to reach this device",
        'i', C_BLUE, info_enter, nullptr, info_tick, info_draw, nullptr
    };
    nucleo_app_register(&app);
}
