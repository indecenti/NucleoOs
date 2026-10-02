// ui-host: the simulated device a scene sets up before it draws (network, battery, audio, clock, heap).
#pragma once
#include <stdint.h>
#include <time.h>
#include "nucleo_kbd.h"
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
    size_t largest_block = 0;                 // > 0: the heap's largest free block, as measured on a busy ADV
    int  canvas_rows = 135;                   // < 135: the back-buffer fitted to a short block (the ADV at ui-init)
    bool stale_mark = false;                  // .short scenes: paint the rows a short canvas can't cover (HOST_STALE)
    uint32_t stale_px = 0;                    // HOST_STALE as the display reads it back (0 = never painted)
    const nucleo_key_t *kbd = nullptr;        // scripted keys for the BLOCKING modals (nucleo_ui_menu/input/message)
    int  kbd_n = 0, kbd_i = 0;
    bool adv = false;
    int64_t now_us = 5000000;                 // esp_timer clock
    time_t wall = 1790678520;                 // 2026-09-29 09:22 UTC (scripted wall clock)
};
extern HostState g_host;
// The mark for the panel rows a short back-buffer cannot cover (magenta: never a theme colour). A surface that
// blits the canvas must repaint those rows itself, or the device shows whatever was there before.
#define HOST_STALE 0xF81Fu
