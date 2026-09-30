// ui-host: the simulated device a scene sets up before it draws (network, battery, audio, clock, heap).
#pragma once
#include <stdint.h>
#include <time.h>
struct HostState {
    const char *wifi_mode = "sta";            // "sta" | "ap"
    const char *ssid = "Casa-Rossi";
    const char *ip = "192.168.1.42";          // fictional: never a real device address or PIN
    const char *pin = "314159";
    int  rssi = -58;
    bool time_synced = true;
    int  battery = 76;                        // -1 = no gauge
    int  brightness = 70, volume = 60;
    bool muted = false, playing = false;
    const char *track = "";
    bool ap_active = false, ap_intended = false, cfg_loaded = true;
    bool canvas_ok = true;                    // false = the 32 KB back-buffer could not be allocated (ADV after Wi-Fi)
    bool adv = false;
    int64_t now_us = 5000000;                 // esp_timer clock
    time_t wall = 1790678520;                 // 2026-09-29 09:22 UTC (scripted wall clock)
};
extern HostState g_host;
