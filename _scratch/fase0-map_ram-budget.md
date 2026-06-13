Perfetto! Ora ho tutte le informazioni. Compilo il rapporto:

## MAPPA RAM FIRMWARE NucleoOS (G:\Nucleo\firmware)

### 1. **CONFIGURAZIONI SDKCONFIG (RAM-critica)**
File: `firmware/sdkconfig.defaults` + `firmware/build/config/sdkconfig.h`

| Chiave | Valore | Note |
|--------|--------|------|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 8192 B | Stack launcher (main task) |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | 16384 B | Buffer RX TLS (dinamico lazy con VARIABLE_BUFFER_LENGTH) |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | 4096 B | Buffer TX TLS |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` | Enable | Alloca/libera buffer handshake dinamicamente |
| `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y` | Enable | Cresce RX buffer solo se necessario (evita 16 KB costante) |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | 1024 B | Header HTTP max |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 4 | Ridotto da 10 (risparmi ~25 KB Wi-Fi buffer pool) |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 8 | Ridotto da 32 |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 4 | Ridotto da 32 |
| `CONFIG_ESP_WIFI_TX_BA_WIN` / `RX_BA_WIN` | 3 | Block ACK window dimezzato |
| `CONFIG_LWIP_MAX_SOCKETS` | 13 | 7 httpd + 3 outbound client + DNS (evita collision) |
| `CONFIG_LWIP_IPV6=n` | Disabled | Rimuove ND6 tables (~5 KB statici) |

### 2. **TASK STACK ALLOCATION (xTaskCreate/xTaskCreatePinnedToCore)**

| Task | Stack (B) | File:Riga | PrioritÃ  | Note |
|------|-----------|----------|----------|------|
| anima_sh | 30720 | nucleo_app/app_anima.cpp:901 | tskIDLE+2 | Worker ANIMA (online TLS + recursion) |
| voice_task | 16384 | nucleo_voice/nucleo_voice.c:711 | 4 | Core 1 pinned; I2S + MFCC + DTW |
| rec-ai | 16384 | nucleo_app/app_recorder.cpp:442,456 | tskIDLE+2 | Transcribe/summarize con cloud TLS |
| ssh | 16384 | nucleo_app/app_ssh.cpp:303 | tskIDLE+2 | Terminal SSH con mbedTLS |
| audio | 8192 | nucleo_audio/nucleo_audio.c:321 | 5 | Core 1 pinned; MP3 decode + I2S DMA |
| ir_sweep | 4096 | nucleo_ir/nucleo_ir.c:158 | 5 | IR database scan |
| ir_jam | 4096 | nucleo_ir/nucleo_ir.c:203 | 5 | IR transmission |
| ep_dns | 3072 | nucleo_evilportal/nucleo_evilportal.c:708 | 5 | Captive DNS (portal) |
| ep_twin | 2048 | nucleo_evilportal/nucleo_evilportal.c:724 | 4 | Twin AP deauth |
| rec | 4096 | nucleo_recorder/nucleo_recorder.c:179 | 5 | Audio recording |
| rec_stream | 4096 | nucleo_recorder/nucleo_recorder.c:315 | 5 | Stream worker async |
| wa_deauth | 4096 | nucleo_wifiatk/nucleo_wifiatk.c:556 | 5 | Wi-Fi flood deauth |
| wa_beacon | 4096 | nucleo_wifiatk/nucleo_wifiatk.c:705 | 5 | Beacon injection |
| music_idx | 4096 | nucleo_app/app_player_db.cpp:131 | 3 | Core 0; SD index scan |
| wifi | 4096 | nucleo_app/app_wifi.cpp:297 | tskIDLE+2 | Wi-Fi connect/scan |
| cal-svc | 4096 | nucleo_app/calendar_svc.cpp:104 | 2 | Calendar sync (low priority) |

### 3. **BUFFER STATICI GRANDI (>1KB)**

| Buffer | Dimensione | File:Riga | Scopo |
|--------|-----------|----------|-------|
| s_screen canvas (M5Canvas) | 32400 B (240Ã—135@8bpp) | nucleo_ui/nucleo_ui.cpp:38 | Framebuffer UI (lazily acquired) |
| vdsp_ctx | ~7 KB | nucleo_voice/nucleo_voice.c:113 | MFCC precompute tables (resident se voice abilitata) |
| vdsp_acc | ~24 KB | nucleo_voice/nucleo_voice.c:114 | Streaming accumulator (solo su PTT press) |
| s_scan (vdsp_template) | ~3.3 KB | nucleo_voice/nucleo_voice.c:120 | Template read buffer SD (per-PTT) |
| s_win (vdsp_template) | ~3.3 KB | nucleo_voice/nucleo_voice.c:121 | Best-match copy (per-PTT) |
| s_pcm (I2S chunk) | 1024 B (512 Ã— int16) | nucleo_voice/nucleo_voice.c:492 | PCM buffer voice (per-PTT, freed on release) |
| REC_TXTBUF | 4096 B | nucleo_app/app_recorder.cpp:59 | Transcript buffer |

### 4. **MALLOC RUNTIME (allocazioni dinamiche >1KB)**

| Alloc | Size | File:Riga | Uso | Ciclo di vita |
|-------|------|----------|-----|---------------|
| auth JSON buf | 2048 B | nucleo_auth/nucleo_auth.c:62,91 | Parse tokens/settings | alloc@boot, parse, free subito |
| httpd transcribe text | 4096 B | nucleo_httpd/nucleo_httpd.c:1372 | /api/transcribe output | request-scoped, free post-response |
| httpd transcribe summary | 2048 B | nucleo_httpd/nucleo_httpd.c:1386 | Summary generato | request-scoped, free post-response |
| app_radio buf | 8192 B | nucleo_app/app_radio.cpp:75 | Streaming buffer | alloc on-demand, free on close |

### 5. **FRAMEBUFFER DISPLAY & SCREEN-OFF LIBERATION**

**Allocazione:**
- M5Canvas s_screen @ 32400 B (nucleo_ui/nucleo_ui.cpp:38)
- 240Ã—135 pixel @ 8bpp RGB332
- Allocato una volta @ boot (nucleo_ui_init:104) mentre heap pulito
- Shared fra launcher list + tutte app foreground (mutually exclusive)

**Meccanismo screen-off (32 KB rilasciati):**
- `nucleo_screen_release()` (nucleo_ui.cpp:57): chiama `s_screen.deleteSprite()` per liberare
- Triggered da media modals (video/music) che necessitano decoder contiguous block
- Lazy re-acquire @ `nucleo_screen_acquire()` (nucleo_ui.cpp:45) con retry ogni ~400 ms
- Risolve flickering: dopo encoder libera RAM, canvas riacquisce senza framentation

### 6. **NUCLEO_EXCLUSIVE MODE (rilascia ~70 KB)**

File: `firmware/components/nucleo_app/nucleo_exclusive.c` + header

**Flags & stop order (dal piÃ¹ cheap al piÃ¹ disruptivo):**
1. `NX_DISCOVERY`: stop mDNS discovery
2. `NX_VOICE`: suspend voice (Task 16 KB + vdsp_ctx ~7 KB)
3. `NX_RECORDER`: stop audio recording
4. `NX_HTTPD`: stop HTTP server + Wi-Fi sockets
5. `NX_ANIMA_L1`: unload L1 model cache (lazy reload next query)
6. `NX_WIFI`: suspend auto-reconnect + stop driver (~25â€“30 KB LWIP/driver)

**Liberazione misurata:**
- nucleo_exclusive_enter() (nucleo_exclusive.c:36): log `free â†’ â†’ freefore/largest_before` vs `free_after/largest_after`
- Uso: `nucleo_exclusive_enter(NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY, &inf); ...heavy work...; nucleo_exclusive_exit();`

### 7. **PARTITION TABLE**

File: `firmware/partitions.csv`

| Name | Type | Size | Offset | Uso |
|------|------|------|--------|-----|
| factory | app | 2.5 MB (0x280000) | 0x20000 | Firmware (1.44 MB immagine, 44% libero) |
| ota_0 / ota_1 | app | 2.25 MB (0x240000 x2) | 0x2A0000, 0x4E0000 | OTA slots (38% headroom) |
| cfg | data/SPIFFS | 640 KB (0xA0000) | 0x760000 | LittleFS config power-safe |
| anima_brain | - | (non ancora impl.) | - | Future: 4â€“8 MB mmap XIP brain |
| nvs, otadata, phy_init, nvs_keys, coredump | data | ~100 KB total | 0x9000â€“0x720000 | Metadata, OTA, RF calib, core dumps |

### 8. **MEMORY BUDGET ARCHITECTURE (docs/memory-budget.md)**

**Fixed reservations (~186 KB siempre presenti):**
- ROM/IDF data/bss: ~80 KB
- FreeRTOS + task stacks: ~40 KB
- Event bus ring: ~16 KB
- SD/FATFS buffers: ~16 KB
- Display partial framebuffers (2Ã—~1/10 frame): ~14 KB
- App runtime + micro-VM: ~20 KB

**Available: ~326 KB** per trasporto attivo (Wi-Fi/BLE/ESPNOW)

**Profile A (Wi-Fi STA + LWIP + HTTP/WS):** ~150 KB (active, tight)
- TLS min block: 18 KB (16 KB SSL_IN + margin) [nucleo_anima_online.c:40]
- TLS min free total: 40 KB (handshake ~35 KB + margin) [nucleo_anima_online.c:41]

### 9. **VOICE DSP MEMORY (lazy PTT-scoped)**

File: `firmware/components/nucleo_voice/nucleo_voice.c`

- **vdsp_ctx (~7 KB):** MFCC tabelle precompute (resident se voice ENABLED, free on disable)
- **vdsp_acc (~24 KB):** Streaming frame accumulator (alloc su PTT press linea 492, free su release)
- **s_pcm (1 KB):** Capture chunk (16000 Hz, 512 samples = 32 ms) 
- **s_scan + s_win (~6.6 KB):** Template buffers streamed da SD, **no cached pool** (scanned one-at-a-time)
- **VAD adaptive:** noise floor tracking (scale-robust margin gates)

### 10. **ANIMA MEMORY HIERARCHY (docs/anima-memory.md)**

**Tre tier (capacity not latency con coarse-prefix prefilter):**
- **SRAM:** ~64 KB hot working set + LRU hottest HV (~instant)
- **FLASH XIP mmap:** 4â€“8 MB bulk codebook scanned in-place (~2.4 ms full 8 MB con 64-bit prefix)
- **SD:** millions HV cold overflow + append-only journal (~1 ms/HV)

**Endurance math:** 1024 flash sectors Ã— 100k cycles Ã· 4 rebuilds/day = ~70k anni (mai wear-out)

---

**Sintesi heap-tight:** Cardputer PSRAM-less costringe architettura a **mode-switching** (esclusivitÃ  Wi-Fi/BLE/ESPNOW), **lazy allocation** (voice/screen su-demand), **streaming** (voice template, SD), e **dynamic buffers** (mbedTLS variable-len). Watermark real: ~18 KB min_free@runtime con HTTP client + web connesso.
