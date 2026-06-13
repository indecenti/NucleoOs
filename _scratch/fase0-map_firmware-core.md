Perfetto. Ora compilo la mappatura finale:

## MAPPATURA FIRMWARE CORE NUCLEOOS

**1. SEQUENZA BOOT (firmware/main/main.c:79-186)**

firmware/main/main.c:79-186 sequenza ordinata:
1. NVS init + healing (riga 84-88)
2. Storage mount config LittleFS (riga 92-93)
3. Event bus init, log ring init (riga 95-96)
4. UI init (M5GFX + keyboard, riga 106)
5. USB MSC mode check (riga 112-114, se pending â†’ loop dedicato senza OS)
6. SD mount + provision + registry load (riga 121-126)
7. Network + power (riga 134-142: setup network AP/STA, DFS, discovery mDNS, recorder PDM, auth PIN, arb init, IR LED)
8. HTTP server start (riga 144)
9. OTA confirmation (riga 145)
10. Setup wizard se first-run (riga 161-162)
11. ANIMA init + TTS + voice engine (riga 168-179)
12. App launcher (riga 183-185: register builtins, setup_register_apps, nucleo_app_run forever loop)

**2. COMPONENTI firmware/components/nucleo_* (escluso nucleo_anima)**

| Componente | Scopo + File Principali | Dipendenze Notevoli |
|---|---|---|
| **nucleo_app** | OS app launcher + 29 native app (app_*.cpp), sistema widget M5GFX | nucleo_ui, nucleo_kbd, M5GFX, ANIMA (include-only link cycle break) |
| **nucleo_arb** | Token arbiter per OOM race (TLS/SD/heap jobs), 1 lock max, FG preempts BG | freertos, esp_timer, eventbus |
| **nucleo_audio** | Audio playback WAV/MP3 (Helix decoder), task core 1 pinned (8KB stack) | esp_driver_i2s (shared GPIO43 WS con mic), mbedtls (HTTP stream) |
| **nucleo_auth** | Pairing PIN + session token su /cfg LittleFS, guard /api/fs/ota/rec/ws | httpd, /cfg config (JSON auth.json) |
| **nucleo_board** | Pin map M5Cardputer ESP32-S3, HLOG heap diagnostics, config SPI2 SD vs SPI3 display | M5GFX (display SPI3), SD SPI2, PDM mic GPIO43/46, I2S speaker GPIO43/42/41, IR GPIO44 |
| **nucleo_discovery** | mDNS advertise <id>.local _nucleoos._tcp:80 | lwip, esp_netif |
| **nucleo_eventbus** | Event journal (SD /data/events.log) + replay ring (208B payload max) | nucleo_board (RAM ring alloc) |
| **nucleo_evilportal** | Rogue AP + captive DNS + portal template server, toggle discovery stop | esp_wifi, esp_netif, httpd (steals port 80), task DNS (3KB, prio 5), task twin deauth (2KB, prio 4) |
| **nucleo_fsapi** | File API /api/fs/* (list/read/write/delete/mkdir SD) | esp_http_server, nucleo_auth gate |
| **nucleo_gpio** | GPIO raw /api/gpio (read, write, safe pin allowlist per app richiesta) | esp_http_server (no task, no state) |
| **nucleo_httpd** | HTTP server :80 con 6 socket max, 30KB stack task, CPU idle probe, route registration | esp_http_server, httpd config (lru_purge, keep-alive 5s idle), cpu_sample_cb esp_timer (500ms cadence), socket close hook |
| **nucleo_ir** | IR TX GPIO44 via RMT (1Âµs res) + TV-B-Gone sweep task (4KB, prio 5) | esp_driver_rmt, tv-b-gone code table, nucleo_ir_proto |
| **nucleo_kbd** | Cardputer GPIO matrix (74HC138 addr {8,9,11} + 7 row input {13,15,3,4,5,6,7}) | esp_driver_i2c (ADV variant), esp_timer (debounce poll) |
| **nucleo_power** | DFS CPU scaling (full speed load, low idle frequency) | esp_pm (no sleep per spec) |
| **nucleo_recorder** | PDM mic I2S RX â†’ WAV file SD, task rec (4KB, prio 5) + async stream worker | I2S RX (PDM GPIO43/46, shared WS con speaker), nucleo_audio exclusion lock |
| **nucleo_registry** | Load /sd/system/registry/apps.json (max 48 app) | cJSON, nucleo_storage |
| **nucleo_setup** | First-run wizard (AP/join), network apply, device name, NTP time sync, Wi-Fi scan | esp_wifi, esp_netif, nucleo_ui (blocking menu/input) |
| **nucleo_storage** | SD mount FATFS + /cfg LittleFS internal (power-loss-safe), provision /system tree | sdmmc SPI2, fatfs, littlefs, nucleo_board SPI pin map |
| **nucleo_tts** | Voice concatenativa clip (.wav /sd/data/tts/it/) â†’ temporaneo WAV â†’ nucleo_audio | nucleo_audio, JSON manifest tts_index.json |
| **nucleo_ui** | M5GFX init + Cardputer kbd, menu/input/message UI primitives | M5GFX, M5Unified, nucleo_kbd |
| **nucleo_usbhid** | USB HID keyboard per host, live in app (TinyUSB, no OTA serial) | esp_tinyusb |
| **nucleo_usbmsc** | USB Mass Storage reboot mode (SD card raw blocks), loop dedicato no OS | esp_tinyusb, sdmmc, nucleo_ui (status), nucleo_kbd (exit) |
| **nucleo_voice** | PTT voice engine AVCEB (I2S capture + MFCC + DTW template match), task core 1 (16KB, prio 4) | nucleo_eventbus (publish match), nucleo_anima (try_lock), nucleo_ui, nucleo_tts (optional voice response) |
| **nucleo_webfs** | Static file server shell + apps: / â†’ shell/index.html, /apps/<id>/ â†’ app www/ | nucleo_board, esp_http_server catch-all |
| **nucleo_wifiatk** | Deauth flood + beacon jamming + evil twin clone, task flood (4KB, prio 5), task beacon (4KB, prio 5) | esp_wifi (promiscuous + 802.11 raw TX), ieee80211_raw_frame_sanity_check override |
| **nucleo_ws** | WebSocket /ws live event delta broadcast (eventbus sink) | httpd, nucleo_auth gate, nucleo_eventbus |

**3. nucleo_httpd config + route registration**

**Config (firmware/components/nucleo_httpd/nucleo_httpd.c:1496-1524)**
- max_open_sockets: 6 (4â†’6, balance keep-alive vs heap fragmentation, min_free ~10-13 KB)
- uri_match_fn: httpd_uri_match_wildcard (enable /* static)
- max_uri_handlers: 48 (headroom 30+ API route + static)
- close_fn: on_sock_close (immediate disconnect detect)
- lru_purge_enable: true (recycle oldest idle socket, prevent ERR_CONNECTION_RESET)
- stack_size: 30720 (16Kâ†’24Kâ†’30K: L1 cascade + TLS X.509 recursion, HWM /api/heap)
- recv/send_wait_timeout: 30s (SD block flush + large uploads)
- keep_alive: idle 5s, interval 5s, count 3

**Route registration (riga 1550-1585)**
30+ handler: /api/{status, apps, associations, logs, diag, heap, cpu, wifi/scan, ota, reboot, anima*, tts*, proxy, llm*, transcribe, voice/learn, voice/always, time/set, pair, auth/status, fs/*, rec/*, ir/*, gpio, display}, /ws, /* (static catch-all)

**4. nucleo_app app native**

**File app_*.cpp in firmware/components/nucleo_app/ (29 app built-in, registrati before app_run)**
1. app_clock.cpp - orologio
2. app_info.cpp - info sistema
3. app_remote.cpp - smartwatch monitor RAM/CPU
4. app_calc.cpp + calc_eval.c - calcolatore
5. app_sysmon.cpp - system monitor
6. app_recorder.cpp - registratore voce + cloud transcribe/summary
7. app_files.cpp - file browser SD
8. app_photos.cpp - photo gallery
9. app_player.cpp + app_player_db.cpp - music player (indexer task core 0, 4KB, prio 3)
10. app_video.cpp - NFV v3 video (A/V sync)
11. app_notepad.cpp - text editor
12. app_calendar.cpp + calendar_svc.cpp - calendario (svc task 4KB, prio 2)
13. app_notify.cpp - notifiche
14. app_ui.cpp - control center
15. app_usb.cpp - USB MSC request
16. app_radio.cpp - radio HTTP stream
17. app_anima.cpp - ANIMA chat (shell worker task 30KB, prio idle+2)
18. app_torch.cpp - flashlight
19. app_usbkbd.cpp - USB HID keyboard per host
20. app_evilportal.cpp - rogue AP portal
21. app_wifiatk.cpp - Wi-Fi deauth/beacon
22. app_beacon.cpp - AP beacon jammer
23. app_voice.cpp - voice command PTT UI
24. app_voicelab.cpp - voice template learning lab
25. app_ir.cpp - IR remote send + TV-B-Gone
26. app_ssh.cpp - SSH client (ssh_task 16KB, prio idle+2)
27. app_wifi.cpp - Wi-Fi config (wifi_task 4KB, prio idle+2)
28. app_evilportal (app_evilportal.cpp) - giÃ  elencato
29. nucleo_notify.cpp, launcher_*, app_info.cpp - helper/launcher

**File sorgenti core raccolti in CMakeLists.txt**
nucleo_app.cpp, launcher_menu.cpp, launcher_render.cpp, app_info.cpp, app_remote.cpp, â€¦ (vedi nucleo_app/CMakeLists.txt SRCS)

No optional/ o staged/ (tutti compilati build-time nel firmware monolitico).

**5. FreeRTOS task creati (xTaskCreate + varianti)**

| Task Name | Stack (byte) | Priority | File:Riga | Scopo |
|---|---|---|---|---|
| audio | 8192 | 5 | nucleo_audio/nucleo_audio.c:321 xTaskCreatePinnedToCore core 1 | Audio player I2S MP3/WAV |
| voice_task | 16384 | 4 | nucleo_voice/nucleo_voice.c:711 xTaskCreatePinnedToCore core 1 | PTT capture + MFCC + DTW match |
| rec | 4096 | 5 | nucleo_recorder/nucleo_recorder.c:179 | PDM mic I2S RX loop WAV |
| rec_stream | 4096 | 5 | nucleo_recorder/nucleo_recorder.c:315 | Async WAV stream /api/rec/stream |
| ir_sweep | 4096 | 5 | nucleo_ir/nucleo_ir.c:158 | TV-B-Gone power sweep (on demand) |
| ir_jam | 4096 | 5 | nucleo_ir/nucleo_ir.c:203 | IR jam mode sweep |
| wa_deauth | 4096 | 5 | nucleo_wifiatk/nucleo_wifiatk.c:556 | Deauth flood (on demand) |
| wa_beacon | 4096 | 5 | nucleo_wifiatk/nucleo_wifiatk.c:705 | Beacon jammer (on demand) |
| ep_dns | 3072 | 5 | nucleo_evilportal/nucleo_evilportal.c:708 | Captive DNS (portal running) |
| ep_twin | 2048 | 4 | nucleo_evilportal/nucleo_evilportal.c:724 | Evil-twin deauth (on demand) |
| music_idx | 4096 | 3 | nucleo_app/app_player_db.cpp:131 xTaskCreatePinnedToCore core 0 | Music DB indexer (async) |
| cal-svc | 4096 | 2 | nucleo_app/calendar_svc.cpp:104 | Calendar sync service (low prio) |
| rec-ai | 16384 | idle+2 | nucleo_app/app_recorder.cpp:442/456 | Recorder cloud transcribe+summary |
| ssh | 16384 | idle+2 | nucleo_app/app_ssh.cpp:303 | SSH client (on demand) |
| wifi | 4096 | idle+2 | nucleo_app/app_wifi.cpp:297 | Wi-Fi config async (on demand) |
| anima_sh | 30720 | idle+2 | nucleo_app/app_anima.cpp:901 | ANIMA shell worker (foreground, heap-backed) |

**httpd task** (task principale) stack=30720, prio 18 (ESP-IDF default), corre handler + L1 cascade + TLS

**6. Timer periodici + polling loop**

| Meccanismo | Cadenza | File:Riga | Scopo |
|---|---|---|---|
| **esp_timer** cpuload | 500 ms | nucleo_httpd/nucleo_httpd.c:96-98 | CPU idle probe per core (idle hook callback) |
| **esp_timer** disp_deadman | 3s once | nucleo_app/nucleo_app.cpp:100-103 | Display idle timeout (app leave safety) |
| **esp_timer** keyboard debounce | (impl via matrix poll, no explicit timer) | nucleo_kbd/nucleo_kbd.c | GPIO matrix scan (non-periodic polling) |
| **Main UI loop** (for;;) | vTaskDelay 40-80ms | nucleo_app/nucleo_app.cpp:864-1091 | Launcher event loop: kbd read, key dispatch, app on_tick, render (~12-25 Hz) |
| **vTaskDelay (sleep)** | 40ms idle, 80ms remote monitor | nucleo_app/nucleo_app.cpp:879, 954 | Throttle loop CPU (FreeRTOS task yield) |
| **voice_task loop** (for;;) | vTaskDelay 50ms capture, 100ms init | nucleo_voice/nucleo_voice.c:518-687 | PTT listen loop, MFCC extraction, DTW match poll |
| **audio player_task** (for;;) | loop in, implicit I2S DMA | nucleo_audio/nucleo_audio.c | Stream decode + I2S TX DMA |
| **recorder task** (for;;) | loop in, I2S RX DMA driven | nucleo_recorder/nucleo_recorder.c | Mic capture + WAV write |

**Polling pattern (no explicit timer)**
- Keyboard: passive read nucleo_kbd_read() in main loop, no ISR (matrix sampling su richiesta)
- Network: httpd LWIP event-driven, no polling task
- WebSocket: on_sock_close hook + lru_purge + keep-alive probe (per config)
- Recorder/audio: I2S DMA ISR-driven, task wait su queue

**Heap context**
- ~18 KB runtime heap (collo), SRAM no PSRAM, frammentazione critica
- arb acquirer gate su OTA/TLS per OOM prevention
- L1 ANIMA cascade sul httpd task (30 KB stack per TLS cert chain ricorsiva)
- Voice + audio task pinned core 1 (isolate from core 0 Wi-Fi/httpd contention)

---

**File principali raccolti**
firmware/main/main.c:1-186
firmware/components/nucleo_httpd/nucleo_httpd.c:1496-1589 (config + route), :96-98 (timer cpuload)
firmware/components/nucleo_app/nucleo_app.cpp:864-1091 (main loop), :100-103 (disp deadman)
firmware/components/nucleo_voice/nucleo_voice.c:518-687 (voice loop), :711 (task create)
firmware/components/nucleo_audio/nucleo_audio.c:321 (audio task)
firmware/components/nucleo_*/*.c (CMakeLists.txt: SRCS, REQUIRES, PRIV_REQUIRES)
