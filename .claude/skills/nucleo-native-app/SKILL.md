---
name: nucleo-native-app
description: Scaffold and implement a NucleoOS native firmware app (C++) correctly — anti-flicker, poll handler, RAM discipline, input routing, registration. Use when creating or fixing a native app in firmware/components/nucleo_app/.
---

# App nativa NucleoOS — guida ottimizzata

Un'app nativa è un `app_<id>.cpp` registrato in `nucleo_app.cpp` + `CMakeLists.txt`.
Schermo 240×135, ~18 KB heap libero frammentato, no PSRAM.

---

## 1. Struttura minima

```cpp
#include "nucleo_app.h"      // nucleo_app_def_t, register, helpers
#include "nucleo_kbd.h"      // NK_UP/DOWN/LEFT/RIGHT/ENTER/BACK/TAB ...
#include "launcher_theme.h"  // W, H, HINT, BG, FG, C_BLUE/GREEN/... colori tema
#include "app_gfx.h"         // macro 'd' = *nucleo_app_gfx() — il canvas/display

// stato tutto static (zero heap, in .bss)
static bool s_dirty = true;

static void on_enter(void) {
    nucleo_app_set_hint("TAB tab  ESC esci");
    nucleo_app_request_draw();
}
static void on_key(int key, char ch) { /* ... */ nucleo_app_request_draw(); }
static void on_tick(void) {}   // 5 Hz — solo per aggiornamenti lenti (es. orologio)
static void on_draw(void) {
    if (!s_dirty) return;
    s_dirty = false;
    // disegna qui
}
static void on_exit(void) {}

extern "C" void nucleo_register_<id>(void) {
    static const nucleo_app_def_t app = {
        "<id>", "<Nome>", "<Categoria>", "<desc>",
        '<I>', COLOR,        // icona lettera + colore RGB565
        on_enter, on_key, on_tick, on_draw, on_exit
    };
    nucleo_app_register(&app);
}
```

---

## 2. Anti-flicker — la regola principale

**MAI** fare `d.fillScreen()` + draw in modalità `direct_draw=true`: scrive al TFT in due passate
e produce sfarfallio evidente. Scegli una sola tecnica:

### Tecnica 1 — Canvas + blit (DEFAULT, quasi sempre)
Il run loop blitta il canvas al TFT **una volta sola** dopo ogni `on_draw()`.
```
poll_fn() → true  →  on_draw() disegna sul canvas  →  framework blitta → TFT aggiornato atomicamente
```
- Usa `d.fillScreen()`, `d.fillRect()`, etc. liberamente: scrivono in SRAM (veloce).
- Il TFT vede un frame completo, mai uno stato intermedio.
- `set_direct_draw` **NON va usato** (default false = canvas mode).
- Per animazioni: registra `nucleo_app_set_poll_handler(poll_fn)` che restituisce `true`
  alla frequenza target (vedi §3).

```cpp
static bool poll_fn(void) {
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < TARGET_INTERVAL_US) return false;
    s_last_us = now;
    return true;   // framework chiama on_draw + blitta
}
static void on_draw(void) {
    d.fillScreen(0x0000);   // canvas nero, SRAM write, NO sfarfallio
    draw_stuff();           // aggiungi elementi sul canvas
}
```

### Tecnica 2 — Direct draw (solo per app che liberano il canvas)
Solo quando l'app libera i 32 KB del canvas (es. ANIMA per il suo indice L1):
- `nucleo_app_release_buffers()` → libera canvas
- `nucleo_app_set_direct_draw(true)` → il framework non riprende il canvas
- `on_draw()` deve pulire **solo le aree modificate** (self-clear delle regioni cambiate).
- **MAI** fillScreen: scriverebbe direttamente al TFT producendo un flash nero visibile.

```cpp
// Tecnica 2 corretta: erase solo rect vecchio, draw nuovo
static void on_draw(void) {
    d.fillRect(old_x, old_y, W, H_elem, BG);   // erase preciso
    draw_element(new_x, new_y);                  // draw nuovo
    old_x = new_x; old_y = new_y;
}
```

---

## 3. Poll handler vs on_tick

| | `on_tick` | `poll_handler` |
|---|---|---|
| Frequenza | ~5 Hz (200 ms) | ~50 Hz loop, blitta alla rate che decidi |
| Uso | Aggiornamenti lenti (orologio, stato) | Animazioni ≥ 10 fps |
| Come | Chiama `nucleo_app_request_draw()` quando cambia | Restituisce `true` quando il frame è pronto |
| Anti-flicker | OK per UI statiche | Necessario per moto fluido |

```cpp
// Poll handler a 30 fps per un gioco/animazione
#define FPS30_US 33333
static int64_t s_last_us;

static bool poll_fn(void) {
    if (!s_animating) return false;          // non chiedere blit se non serve
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < FPS30_US) return false;
    s_last_us = now;
    return true;
}
// Registra in on_enter():
nucleo_app_set_poll_handler(poll_fn);
```

---

## 4. Fullscreen

```cpp
nucleo_app_set_fullscreen(true);   // in saver_start() o simile
// → canvas copre 240×135, hint bar soppressa
// → on_draw può usare H pieno
nucleo_app_set_fullscreen(false);  // ripristina footer
```

---

## 5. RAM discipline — zero heap nelle app

```cpp
// BENE: static in .bss (zero costo boot, sempre disponibile)
static uint8_t fire_buf[FH][FW];   // 920 byte
static uint16_t palette[256];      // 512 byte

// MALE: heap nell'animazione (frammenta, può fallire in runtime)
uint8_t *buf = (uint8_t*)malloc(FH * FW);  // NON fare questo
```

Se l'app ha bisogno di RAM contigua grande (SSH, ANIMA, video): usa `exclusive_flags`:
```cpp
#include "nucleo_exclusive.h"
static const nucleo_app_def_t app = {
    "id", "Nome", ..., on_enter, on_key, on_tick, on_draw, on_exit,
    NX_NET_APP   // libera ~47 KB sospendendo httpd/mDNS/L1 prima di on_enter
};
```

---

## 6. Input routing — gotcha

```cpp
// BACK e LEFT vanno SEMPRE al back_handler, non a on_key
static bool on_back(int key) {
    if (s_in_submenu) { go_up(); return true; }  // consuma
    return false;                                  // lascia chiudere l'app
}
// TAB va al tab_handler
static void on_tab(void) { s_tab = (s_tab + 1) % NTABS; nucleo_app_request_draw(); }

// In on_enter():
nucleo_app_set_back_handler(on_back);
nucleo_app_set_tab_handler(on_tab);
```

Key constants in `nucleo_kbd.h`: `NK_UP NK_DOWN NK_LEFT NK_RIGHT NK_ENTER NK_BACK NK_TAB NK_NONE`.

---

## 7. Persistenza settings

```cpp
#define CFG "/sd/system/config/<id>.json"

static void load(void) {
    FILE *f = fopen(CFG, "rb"); if (!f) return;
    char buf[128]; int n = fread(buf,1,sizeof(buf)-1,f); fclose(f); buf[n]=0;
    int val = 0;
    sscanf(buf, "{\"key\":%d}", &val);
    s_val = val;
}
static void save(void) {
    mkdir("/sd/system", 0775); mkdir("/sd/system/config", 0775);
    FILE *f = fopen(CFG, "wb"); if (!f) return;
    fprintf(f, "{\"key\":%d}\n", s_val); fclose(f);
}
```

---

## 8. Registrazione

**`nucleo_app.cpp`** — aggiungi vicino agli altri extern:
```cpp
extern "C" void nucleo_register_<id>(void);
```
e nella `nucleo_app_register_builtins()`:
```cpp
nucleo_register_<id>();
```

**`CMakeLists.txt`** — aggiungi nella lista SRCS:
```
"app_<id>.cpp"
```

---

## 9. Checklist prima del build

- [ ] `app_gfx.h` incluso (serve `d`)
- [ ] `launcher_theme.h` incluso (serve `W H BG FG C_*`)
- [ ] Nessuna allocazione heap in frame/animazione (solo static/stack)
- [ ] poll_handler restituisce `false` quando non serve animare (non blitta inutilmente)
- [ ] `on_draw` controlla `s_dirty` per UI statiche (evita ridisegni inutili)
- [ ] back_handler registrato se l'app ha navigazione gerarchica
- [ ] `nucleo_app_set_fullscreen(false)` chiamato in saver_stop/on_exit se usato
- [ ] Se liberi il canvas (`release_buffers`): imposta `direct_draw=true` E usa solo Tecnica 2
- [ ] extern + chiamata in `nucleo_app.cpp` + file in `CMakeLists.txt`
