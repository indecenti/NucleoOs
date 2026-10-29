// app_gbemu — Game Boy and Game Gear, emulated natively on the Cardputer.
//
// ONE FRONT-END, TWO CONSOLES. Both screens are 160x144, so the shelf, the scaler, the in-game menu,
// the saves and the Solo-boot posture below are shared; a small descriptor (EmuSys, further down)
// names what differs — folders, the core's calls, the frame time, and whether the palette is the
// player's to choose (Game Boy) or the cartridge's (Game Gear). Two launcher entries, "gbemu" and
// "ggemu", open the same code with a different descriptor.
//
// The first NATIVE emulator in NucleoOS: the console runs on the ESP32-S3 itself, not in a browser.
// It is possible because of two properties of the chosen core (Peanut-GB — see
// nucleo_emu/vendor/README.md): the whole console is one ~16.9 KB struct, and the PPU emits the
// picture one SCANLINE at a time, so there is no framebuffer to find room for.
//
// RAM POSTURE — why the app_def is declared the way it is:
//   This board has no PSRAM and the limit that bites is the largest CONTIGUOUS block, not free bytes.
//   The boot trace measures it collapsing to ~31.7 KB the moment the shared 32,400 B UI canvas is
//   allocated, and the runtime reclaim in nucleo_exclusive frees RAM but CANNOT defragment. So this
//   app declares NX_SOLO (reboot into a fresh, unfragmented heap), NX_WIFI (the radio costs ~48 KB
//   and halves the largest block — and an emulator needs no network), and releases the canvas before
//   opening a ROM. That is what makes a 17 KB core plus a 32 KB ROM cache fit at all.
//
// SCREEN — the Game Boy is 160x144, the panel is 240x135. Horizontally there is nothing to solve:
// 160 columns fit inside 240, so every column is one pixel, native and unfiltered. Vertically the
// 144 lines must reach 135, and every 16th is DROPPED (not blended) — see the note above OUT_W.
//
// CONTROLS — built from the keyboard's LIVE pressed set, not its key events, so directions can be
// HELD and two buttons can be down at once. E/S/A/D or the printed ; . , / arrows move; K = A, J = B;
// Enter = Start, Space = Select. TAB held fast-forwards. P cycles the palette, M (or Esc) opens the
// in-game menu — save/load state, volume, screen, line blending, palette, reset, quit.
//
// THE SHELF — the library on the card is ~4,700 Game Boy cartridges. Holding that many names in RAM
// is impossible (4,700 x 64 B = 300 KB), so the browser NEVER holds the whole library: it re-walks
// the directory against a live filter and keeps at most MAXR matches. Type-to-filter is therefore
// not a convenience here, it is the navigation model — which is also the right answer on a device
// with a real keyboard and a 135 px screen (docs/device-ui.md: use the keyboard, exploit every pixel).
#if __has_include("sdkconfig.h")       // the host harnesses compile this file without one
#include "sdkconfig.h"
#endif
// Built in, or (CONFIG_NUCLEO_STORE_GBEMU and _GGEMU) only as the store's native modules, built with NAP_MODULE.
#if !(CONFIG_NUCLEO_STORE_GBEMU && CONFIG_NUCLEO_STORE_GGEMU) || defined(NAP_MODULE)
#include "nucleo_app.h"
#include "app_gfx.h"
#include "nucleo_gb.h"
#include "nucleo_gg.h"
#include "nucleo_gbc.h"       // the Game Boy Color: the Game Boy's host around a CGB core
#include "nucleo_kbd.h"
#include "nucleo_board.h"
#include "nucleo_exclusive.h"
#include "nucleo_extdisp.h"     // two screens: the card and the menu on the Cardputer
#include "nucleo_surface.h"     // ...while the picture fills the external 320x240 panel
#include "nucleo_touch.h"       // the gallery on the panel: wheel rows, drag to turn it
#include "app_large.h"          // ...drawn with the shared two-screen kit
#include "gbemu_art.h"          // the consoles' pictures (public-domain photos, deflated)
#include "miniz.h"              // tinfl, in the chip's ROM: inflates a picture for one paint
#include "nucleo_power.h"
#include "nucleo_setup.h"
#include "nucleo_ble.h"
#include "nucleo_theme.h"
#include "nucleo_audio.h"    // in-game volume row (the OS-wide software volume)
#include "nucleo_prefs.h"    // ...persisted with the other power settings when the menu closes
#include "nucleo_i18n.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // the in-game menu pauses the console with vTaskDelay
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <stdarg.h>

#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define LINE  THEME_LINE
#define INK   THEME_INK
#define ACC   THEME_ACC
#define GRN   0x8FF3      // content colour: "has a save"
#define AMB   0xFE8C      // content colour: Game Boy Color cart on a DMG core

static const char *TAG __attribute__((unused)) = "gbemu";   // unused where ESP_LOG compiles away (host gallery)

// ── the console this session drives ─────────────────────────────────────────────────────────────
// Everything that differs between the Game Boy and the Game Gear, in one place. The instances (SYS_GB,
// SYS_GG) sit below the scanline paths they point at; enter_gb()/enter_gg() pick one.
struct EmuArt { const uint8_t *z; uint16_t n; uint8_t w, h; };   // a deflated RGB565 picture (gbemu_art.h)
struct EmuSys {
    const char *id, *name, *short_name;      // app id, shelf header, 2-letter pillar tag
    const char *dir_a, *dir_b;               // ROM folders; entries found in dir_b carry the badge
    const char *ext_a, *ext_b;               // extensions; ext_b also carries the badge
    const char *badge;                       // "C": a Game Boy Color cart on the DMG core
    const char *empty_it, *empty_en, *empty_es, *empty_fr, *empty_de;  // what an empty shelf says
    const char *state_js, *shelf_swp;        // resume + view options; the shelf while a game runs
    const char *trace, *trace_name;          // the on-card trace, and how a failure box names it
    int64_t     frame_us;                    // the console's real frame time
    bool        palettes;                    // the player picks the colours (GB); else the cartridge does
    uint8_t     band_1to1, band_wide;        // output rows per DMA band, 1:1 and stretched (divide 135)
    esp_err_t (*open)(const char *path);
    esp_err_t (*probe)(const char *path);    // header check, no allocation
    void (*explain)(esp_err_t e, const char *path, char *buf, size_t n);   // a refusal, in words
    void (*close)(void);
    void (*reset)(void);
    void (*run_frame)(void);
    void (*set_buttons)(uint8_t);
    const char *(*title)(void);
    void (*set_frameskip)(bool);
    esp_err_t (*state_save)(int);
    esp_err_t (*state_load)(int);
    bool (*state_exists)(int);
    void (*save)(void);
    bool (*autosave)(void);
    void (*get_stats)(nucleo_gb_stats_t *);
    void (*reset_counters)(void);
    void (*geometry)(int *w, int *h);        // what the open cartridge emits: 160x144 or 256x192
    bool zip;                                // .zip in the library folders opens too (the core unpacks it)
    EmuArt art_a, art_b;                     // the console's picture in the gallery; art_b for entries with the badge
    const char *sys_a, *sys_b;               // ...and its name there
    // The console that plays `name` from this shelf (null = this one): the Game Boy's hands a Color cartridge
    // to SYS_GBC. A field, not a check in play(), so the Game Gear module never links the Game Boy cores.
    const EmuSys *(*pick)(const char *name);
    // The console's small mark (gbemu_art.h ART_*_S), drawn wherever its name would be written: icon_b for the
    // entries with the badge (a Color cartridge, a Master System one). A new emulator brings its own.
    EmuArt icon_a, icon_b;
};

static uint16_t *inflate565(const uint8_t *zb, size_t n, int w, int h);
// A console's mark on a light chip, at (x, y), 20 px tall. Inflated once per mark (two kept, ~1 KB each).
// Returns the chip's width.
static int icon_draw(LovyanGFX *g, int x, int y, const EmuArt *a)
{
    static uint16_t *buf[2];
    static const EmuArt *of[2];
    int k = of[0] == a ? 0 : of[1] == a ? 1 : -1;
    if (k < 0) {
        k = of[0] ? 1 : 0;
        if (of[0] && of[1]) { free(buf[1]); buf[1] = buf[0]; of[1] = of[0]; k = 0; }
        buf[k] = inflate565(a->z, a->n, a->w, a->h);
        of[k] = buf[k] ? a : nullptr;
    }
    const int cw = a->w + 8;
    g->fillRoundRect(x, y, cw, 20, 5, ART_TILE);
    if (buf[k]) g->pushImage(x + 4, y + (20 - a->h) / 2, a->w, a->h, (const lgfx::rgb565_t *)buf[k]);
    return cw;
}
static const EmuSys *SYS = nullptr;

// ── on-card trace ───────────────────────────────────────────────────────────────────────────────
// There is NO usable serial console on this device (the USB PHY belongs to TinyUSB), so ESP_LOG
// reaches nobody. Every launch therefore leaves a breadcrumb on the SD card: pull the card, read
// /gbemu_trace.txt (/ggemu_trace.txt), and see exactly how far a start got and what the heap looked
// like at each step. This is the technique that finally explained the silent-video bug.
#define TRACE_WIN  10        // seconds of play per aggregated trace line
static void trace(const char *fmt, ...)
{
    if (!SYS) return;
    FILE *f = fopen(SYS->trace, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("\n", f);
    fclose(f);
}
static void trace_heap(const char *label)
{
    trace("  %-16s free=%u largest=%u", label,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// The library lives where the Arcade web app and firmware provisioning already agree it does.
#define DIR_GB   NUCLEO_SD_MOUNT "/data/ROMs/gb"
#define DIR_GBC  NUCLEO_SD_MOUNT "/data/ROMs/gbc"
#define DIR_GG   NUCLEO_SD_MOUNT "/data/ROMs/gg"
#define DIR_SMS  NUCLEO_SD_MOUNT "/data/ROMs/sms"

// ── screen mapping ──────────────────────────────────────────────────────────────────────────────
// NO SCALING HORIZONTALLY. The panel is 240 px wide and the Game Boy is 160, so every column goes to
// exactly one pixel: native, unfiltered, no interpolation to soften it. (An earlier version dropped
// every 16th column to make the picture 150 wide. That threw away real detail — thin sprites lost
// limbs — to solve a problem the panel did not have.)
//
// VERTICALLY there is no such luxury: 144 source lines must reach a 135 px panel, so one line in
// sixteen has to go. A plain DROP loses whatever was on it — a row of every 8x8 font glyph, a 1 px
// ledge, the top of a sprite — and that is exactly what reads as "the emulator draws it wrong". So by
// default the line is BLENDED into the one below it (per pixel, the exact midpoint of the two
// palette colours): nothing disappears, a thin line on it shows at half strength instead of not at
// all. "Sharp" in the menu restores the pure drop for players who prefer only original colours.
// The merged lines are 7, 23, ... 135 — every one has a successor, so the last group needs no case.
#define SRC_W 160            // the Game Boy's own width
#define OUT_H 135            // 144 * 15/16 — forced by the panel height
#define SRC_X ((240 - SRC_W) / 2)
// TWO OUTPUT MODES, one buffer size. 1:1 draws 160 px centred and keeps the two 40 px pillars for the
// HUD; STRETCH scales to the full 240 px panel (exactly 2:3, so it is a pure integer expansion) and the
// pillars are gone. A band buffer is sized once for BOTH (band_bytes), so switching modes mid-game never
// re-allocates on a heap that has no room to re-allocate. The Game Boy pushes 15/9-row bands (4.8 KB
// each); the Game Gear 5/3-row bands (1.6 KB each): its cartridge cache is the scarcer resource, and
// the 6.4 KB given back is six more 1 KB ROM pages � the step from 4 SD reads a frame to under 1 on
// Sonic 2 � for 27 DMA pushes a frame instead of 9.
#define BAND_1TO1  15        // Game Boy rows per push at 1:1 (15 divides 135 evenly)
#define BAND_WIDE  9         // ...and stretched (9 divides 135 evenly)
static inline size_t band_bytes(void)
{
    // A 180 px Master System picture pushes wide-sized bands (band_wide rows), so 240 x band_wide covers it.
    size_t a = (size_t)SRC_W * SYS->band_1to1, b = (size_t)240 * SYS->band_wide;
    return (a > b ? a : b) * sizeof(uint16_t);
}

// Four-shade palettes, lightest to darkest. A Game Boy is not theme-able — this is CONTENT, not
// chrome, so it deliberately does not follow THEME_*.
//
// GREEN is the default and it is the Game Boy's own green, CALIBRATED rather than copied.
//
// There are two defensible "original" palettes and neither transplants cleanly onto this panel:
//   #9BBC0F #8BAC0F #306230 #0F380F — the literal colours of the DMG's reflective LCD
//   #E0F8D0 #88C070 #346856 #081820 — what emulators have drawn a Game Boy as for decades
// The literal set was designed to be read by AMBIENT LIGHT bouncing off a passive panel. Emitted by a
// backlit ST7789 its two dark shades land near black, so mid-tones stop reading as green at all and
// the picture goes muddy — which is exactly what "the palette leaves something to be desired" meant.
// The emulator-standard set solves that by going mint, and stops looking like a Game Boy.
//
// So GREEN keeps the two light shades EXACTLY as the hardware had them — #9BBC0F is the colour
// everyone pictures — and lifts only the two dark ones, which is precisely the work ambient light
// used to do. Shade 1 is nudged down a little at the same time, because the real panel's top two
// shades are nearly indistinguishable and a 135 px screen cannot afford to waste a whole shade.
// The result reads as a Game Boy across the whole ramp instead of only at the top of it.
//
// Then three alternatives, each earning its place rather than padding a list: the emulator-standard
// mint, maximum contrast for a bright room, and amber for night.
#define PAL_COUNT 4
static const uint16_t PALETTE[PAL_COUNT][4] = {
    { 0x9DE1, 0x7CE2, 0x4385, 0x1A23 },   // Green  - #9BBC0F #7D9C10 #43712F #1A4419
    { 0xE7DA, 0x8E0E, 0x334A, 0x08C4 },   // DMG    - the emulator-standard mint
    { 0xFFFF, 0xAD55, 0x52AA, 0x0000 },   // Mono   - maximum contrast, bright rooms
    { 0xFF00, 0xC540, 0x7A00, 0x2100 },   // Amber  - a plasma/VFD look, easy at night
};
static const char *const PAL_NAME[PAL_COUNT] = { "Green", "DMG", "Mono", "Amber" };
static int s_pal = 0;                     // persisted alongside the resume entry; 0 = Green
static const uint16_t *SHADE = PALETTE[0];
// Midpoint of every pair of shades, panel byte order: [a*4+b]. The blend a merged line is drawn with.
static uint16_t MIX_TX[16];

// The SAME palette with the two bytes already swapped. The panel latches a pixel most-significant
// byte first; an ESP32 stores a uint16_t little-endian. setSwapBytes(true) papered over that, but it
// costs a PER-PIXEL conversion inside pushImage (LovyanGFX picks a converting copy instead of the raw
// one) on 21,600 pixels every frame. Pre-swapping the four palette entries ONCE moves that work from
// the hot loop to a table of four, lets the push run as a straight no-convert copy — which is also
// the only form that can go out over DMA — and costs nothing at all.
static uint16_t SHADE_TX[4];

// Player-visible options, persisted next to the resume entry (see state_load/state_save).
static bool s_hud     = true;    // the two pillars: fps, cost breakdown, palette + key legend
static bool s_stretch = false;   // false = 1:1 with pillars, true = filled 240 px panel
static bool s_sharp   = false;   // false = blend the 1-in-16 line away, true = drop it outright

// Output geometry for the mode in force. Set by geom_apply(), read by the scanline path.
static int s_out_w = SRC_W, s_out_x = SRC_X, s_band_h = BAND_1TO1;

// TWO SCREENS. With an external 320x240 panel the picture goes there, scaled exactly 3/2 on both axes:
// 160x144 -> 240x216, centred, its own aspect, nothing merged or dropped (every source pair becomes three
// pixels, every pair of lines three rows). The Cardputer's screen carries the game's card — title, fps,
// battery, the key legend — and the pause menu, so the picture is never covered. Same band buffers.
static bool s_big = false;
static int  s_out_y0 = 0, s_out_h = OUT_H;
// A 256x192 Master System picture fills it whole at exactly 5/4: 320x240, every fourth pixel and line twice.
// "Filled" (s_stretch) takes a 160x144 picture to the whole panel too: every pixel twice across, lines 1-2-2
// down (5/3), 320x240 — wider than its own aspect, as the Cardputer's filled view is.
#define BIG_W (s_src_w == SRC_W ? (s_stretch ? 320 : 240) : 320)
#define BIG_H (s_src_w == SRC_W ? (s_stretch ? 240 : 216) : 240)
#define BIG_X ((320 - BIG_W) / 2)
#define BIG_Y ((240 - BIG_H) / 2)
#ifdef NUCLEO_UI_HOST
static int s_host_view;                  // the ui-host gallery's scene for an in-game view (ui_host_scene)
static LovyanGFX *igfx(void) { return s_host_view ? &d : s_big ? (LovyanGFX *)nucleo_extdisp_internal_gfx() : nullptr; }
#else
static LovyanGFX *igfx(void) { return s_big ? (LovyanGFX *)nucleo_extdisp_internal_gfx() : nullptr; }
#endif

static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static void pal_apply(void)
{
    SHADE = PALETTE[s_pal];
    for (int i = 0; i < 4; i++) SHADE_TX[i] = swap16(SHADE[i]);
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) {
        uint16_t x = SHADE[a], y = SHADE[b];
        uint16_t r = (uint16_t)((((x >> 11) & 31) + ((y >> 11) & 31) + 1) / 2);
        uint16_t g = (uint16_t)((((x >> 5) & 63) + ((y >> 5) & 63) + 1) / 2);
        uint16_t bl = (uint16_t)(((x & 31) + (y & 31) + 1) / 2);
        MIX_TX[a * 4 + b] = swap16((uint16_t)((r << 11) | (g << 5) | bl));
    }
}
// The picture the core emits: 160x144 (Game Boy, Game Gear) or 256x192 (a Game Gear cartridge in
// Master System mode, or a .sms file). The big one fits the panel at 45/64 on BOTH axes — 180x135,
// its own aspect — or at 60/64 across and 45/64 down when filled (240x135).
static int s_src_w = SRC_W, s_src_h = 144;
static void hscale_build(void);
static void geom_apply(void)
{
    if (s_big) {
        s_out_w = BIG_W; s_out_x = BIG_X; s_out_y0 = BIG_Y; s_out_h = BIG_H;
        s_band_h = SYS ? SYS->band_wide : BAND_WIDE;
        if (SYS && s_band_h * BIG_W * sizeof(uint16_t) > band_bytes())   // 320-px rows: as many as the band holds
            s_band_h = (int)(band_bytes() / (BIG_W * sizeof(uint16_t)));
        return;
    }
    s_out_y0 = 0; s_out_h = OUT_H;
    int w1 = s_src_w == SRC_W ? SRC_W : 180;
    if (s_stretch) { s_out_w = 240; s_out_x = 0;              s_band_h = SYS ? SYS->band_wide : BAND_WIDE; }
    else           { s_out_w = w1;  s_out_x = (240 - w1) / 2;
                     s_band_h = !SYS ? BAND_1TO1 : w1 == SRC_W ? SYS->band_1to1 : SYS->band_wide; }
    if (s_src_w != SRC_W) hscale_build();
}
// The HUD needs the picture to be 160 px wide — stretched, or a 180 px Master System picture, leaves
// no pillars wide enough to put it in.
static inline bool hud_on(void) { return s_hud && !s_stretch && !s_big && s_src_w == SRC_W; }

// ── session state (heap on enter, never .bss: the app is closed almost always) ──────────────────
#define MAXR    120          // matches held at once. The filter, not this cap, is how you reach a game.
#define NAMEMAX 56           // DISPLAY width of a title, never the width a filename is stored at
#define PATHMAX 160          // a whole filename (FATFS long names; No-Intro names run past 80)
#define POOL    6144         // shelf filenames, packed end to end
#define FILTMAX 20

// Filenames live in one packed pool, NOT in fixed 56-byte slots. The slots truncated every longer
// name — "Arcade Classic No. 4 - Defender & Joust (USA, Europe).gb" became "....g" — and play() then
// looked for a file that does not exist: "ROM not found" on a cartridge sitting right there. The pool
// holds the full names in about the same RAM, because most names are far shorter than a fixed slot.
struct RomEnt {
    uint16_t off;            // into EState::pool
    bool     alt;            // shows SYS->badge (a Game Boy Color cartridge on the DMG core)
};

// The shelf's names, in their OWN block — so that for as long as a game runs they can live on the card
// instead of in RAM (see shelf_swap_out). ~6.6 KB is six to seven ROM-cache pages: on a 32 KB-SRAM
// cartridge that is part of the difference between thrashing the SD card and a playable game.
struct Shelf {
    RomEnt   list[MAXR];
    char     pool[POOL];
    int      pool_used;
};

struct EState {
    Shelf   *sh;             // nullptr while a game runs (swapped to the card)
    int      n;              // matches kept
    int      total;          // matches that EXIST (may exceed n — we say so honestly)
    int      sel, scroll;
    char     filter[FILTMAX + 1];
    int      flen;
    char     resume[PATHMAX];   // last cartridge played, offered at the top of an unfiltered shelf
    bool     have_resume;

    uint16_t *band[2];       // two band buffers: one streams over DMA while the next is filled
    int      band_idx;       // which one the scanline path is writing into
    int      band_line, band_y, out_y;
    bool     running;
    uint32_t fps_frames;
    int      fps;
    int64_t  fps_t0;
    uint32_t us_blit;        // time inside pushImage, accumulated over the measurement window
    int      ms_cpu, ms_blit, ms_aud;   // last window's breakdown, in tenths of a millisecond
    bool     turbo;          // TAB held: run unpaced
    int      relief;         // 0 = full picture, 1 = interlaced, 2 = 30 fps frame-skip
    bool     menu;           // the in-game menu is open
    int      msel;           // its selected row
    int      mscroll;        // first visible row (the menu is taller than the panel: it scrolls)
    char     toast[28];      // transient confirmation ("saved", "loaded")
    int64_t  toast_until;
    // Cached geometry of the focused shelf row, so the ~5 Hz tick can scroll its title without
    // recomputing the whole layout. Filled by draw(), consumed by tick().
    int      sel_y, sel_avail;
    char     sel_title[NAMEMAX];
    int      slow_secs;      // consecutive sub-50fps seconds, for the one-way relief debounce
    // The on-card trace, aggregated: one line per TRACE_WIN seconds instead of an fopen/append/fclose
    // every second — each of those is a FAT update on the SD bus in the middle of a frame (a visible
    // hitch) and a wake of the card on a 120 mAh battery. Averages keep the picture; min keeps the dips.
    int      tw_n, tw_fps_sum, tw_fps_min, tw_cpu_sum, tw_blit_sum, tw_aud_sum;
    uint32_t tw_miss;
    uint8_t  stash[SRC_W];   // the line being blended into the next one (see on_line)
    uint16_t stash16[256];   // ...the same for a colour line (on_line16), up to a Master System line
    uint8_t  hsrc[240];      // Master System lines: output column -> first source column (hscale_build)
    uint8_t  fps_hist[32];   // two screens: the last 32 seconds of fps, the companion's sparkline
    uint8_t  fps_hn;         // ...how many are filled (a ring once full)
    int64_t  play_t0;        // when this session's game started (the companion's clock)
    uint8_t  hmix[240];      // ...and whether it is the midpoint of that column and the next
    bool     have_stash;
    // Two screens: the external panel keeps what it was sent, so a source line identical to the one it
    // had last frame is not sent again (a hash per line; a static HUD or a still background costs no bus).
    uint32_t lhash[192];
    bool     diff_ok;        // lhash describes what the panel shows; false after anything else drew there
    bool     crash_told;     // the crash menu opens once per crash, not every second
    bool     mute_keys;      // buttons held when the menu closed stay OUT of the game until released
    bool     state_ok;       // a save state exists — looked up once per menu open, not per repaint
    char     fail[48];       // why the last start failed; drawn over the shelf until the next key
    M5Canvas *mq;            // dedicated sprite for the scrolling shelf title (ANTI-FLICKER technique 3)
    int      mq_w;           // its current width, 0 = none
    char     autoplay[PATHMAX];   // the cartridge the Games gallery opened us on ("" = none / already started):
                                  // quitting it goes back to the gallery, not to this shelf
};
static EState *st = nullptr;
static inline const char *ent_name(int i) { return st->sh->pool + st->sh->list[i].off; }

// ── tiny persisted state (resume) ───────────────────────────────────────────────────────────────
// Deliberately a flat file, not JSON: one line, no parser, no cJSON allocation on a heap this tight.
static void state_load(void)
{
    FILE *f = fopen(SYS->state_js, "r");
    if (!f) return;
    if (fgets(st->resume, sizeof st->resume, f)) {
        size_t l = strlen(st->resume);
        while (l && (st->resume[l - 1] == '\n' || st->resume[l - 1] == '\r')) st->resume[--l] = '\0';
        st->have_resume = (l > 0);
    }
    char line[16];
    if (fgets(line, sizeof line, f)) {          // second line: chosen palette (absent on an older file)
        int v = atoi(line);
        if (v >= 0 && v < PAL_COUNT) s_pal = v;
    }
    // Lines three and four are the view options. Absent on a file written by an older build, in which
    // case the defaults above stand — that is the whole reason this is a flat file and not a parser.
    if (fgets(line, sizeof line, f)) s_hud     = (atoi(line) != 0);
    if (fgets(line, sizeof line, f)) s_stretch = (atoi(line) != 0);
    if (fgets(line, sizeof line, f)) s_sharp   = (atoi(line) != 0);
    pal_apply();
    geom_apply();
    fclose(f);
}
static void state_save(const char *name)
{
    FILE *f = fopen(SYS->state_js, "w");
    if (!f) return;
    fprintf(f, "%s\n%d\n%d\n%d\n%d\n", name ? name : "", s_pal, s_hud ? 1 : 0, s_stretch ? 1 : 0,
            s_sharp ? 1 : 0);
    fclose(f);
}

// ── ROM discovery ───────────────────────────────────────────────────────────────────────────────
static bool is_rom(const char *n, bool *alt)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    if (!strcasecmp(dot, SYS->ext_a))                { return true; }
    if (SYS->ext_b && !strcasecmp(dot, SYS->ext_b))  { *alt = true; return true; }
    if (SYS->zip && !strcasecmp(dot, ".zip"))        { return true; }      // its folder decides the badge
    return false;
}

// Case-insensitive substring — the filter must feel like search, not like a prefix rule.
static bool matches(const char *hay, const char *needle)
{
    if (!needle || !*needle) return true;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) if (!strncasecmp(p, needle, nl)) return true;
    return false;
}

static void scan_dir(const char *dir, bool mark_alt)
{
    if (!dir) return;
    DIR *dp = opendir(dir);   // NB: never name a local `d` — app_gfx.h defines `d` as the draw target
    if (!dp) return;
    struct dirent *de;
    unsigned seen = 0;
    while ((de = readdir(dp)) != NULL) {
        // This directory holds ~4,700 entries and FATFS walks it over SPI. At roughly a millisecond
        // each that is several SECONDS in the watchdog-subscribed UI task, which panics the device —
        // and the scan re-runs on every typed character. Pet the dog while walking.
        if ((++seen & 63) == 0) esp_task_wdt_reset();
        bool alt = mark_alt;
        if (de->d_name[0] == '.' || !is_rom(de->d_name, &alt)) continue;
        if (!matches(de->d_name, st->filter)) continue;
        st->total++;
        size_t len = strlen(de->d_name) + 1;
        if (st->n < MAXR && len <= PATHMAX && st->sh->pool_used + (int)len <= POOL) {
            memcpy(st->sh->pool + st->sh->pool_used, de->d_name, len);
            st->sh->list[st->n].off = (uint16_t)st->sh->pool_used;
            st->sh->list[st->n].alt = alt;
            st->sh->pool_used += (int)len;
            st->n++;
        }
    }
    closedir(dp);
}

static int cmp_ent(const void *a, const void *b)
{ return strcasecmp(st->sh->pool + ((const RomEnt *)a)->off, st->sh->pool + ((const RomEnt *)b)->off); }

static void rescan(void)
{
    if (!st->sh) return;
    st->n = 0; st->total = 0; st->sh->pool_used = 0;
    scan_dir(SYS->dir_a, false);
    scan_dir(SYS->dir_b, true);
    if (st->n > 1) qsort(st->sh->list, st->n, sizeof(RomEnt), cmp_ent);
    st->sel = 0; st->scroll = 0;
}

// ── the shelf, out of RAM while a game runs ─────────────────────────────────────────────────────────
// Written as one block before the cartridge opens, read back as one block when the player returns:
// ~20 ms each way, against re-walking a directory of thousands of files. If the write fails the shelf
// simply stays in RAM (a smaller cache, never a lost shelf); if the read fails it is rebuilt by rescan.
static void shelf_swap_out(void)
{
    if (!st->sh) return;
    FILE *f = fopen(SYS->shelf_swp, "wb");
    if (!f) return;
    bool ok = fwrite(st->sh, 1, sizeof(Shelf), f) == sizeof(Shelf);
    ok = (fclose(f) == 0) && ok;
    if (ok) { free(st->sh); st->sh = nullptr; }
    else remove(SYS->shelf_swp);
}
static void rescan(void);
static void shelf_swap_in(void)
{
    if (st->sh) return;
    st->sh = (Shelf *)calloc(1, sizeof(Shelf));
    if (!st->sh) { st->n = st->total = 0; return; }   // draw() says so; the next key retries
    FILE *f = fopen(SYS->shelf_swp, "rb");
    bool ok = f && fread(st->sh, 1, sizeof(Shelf), f) == sizeof(Shelf);
    if (f) fclose(f);
    remove(SYS->shelf_swp);
    if (!ok) {                                 // the card said no: rebuild it, keeping the selection
        int sel = st->sel, scroll = st->scroll;
        rescan();
        st->sel = sel < st->n ? sel : 0; st->scroll = scroll;
    }
}

// Resolve a bare filename to a full path, trying both system folders.
// A path (the Games gallery finds cartridges anywhere on the card) is taken as it is.
static bool rom_path(const char *name, char *out, size_t n)
{
    struct stat sb;
    if (strchr(name, '/')) { snprintf(out, n, "%s", name); return stat(out, &sb) == 0; }
    snprintf(out, n, "%s/%s", SYS->dir_a, name);
    if (stat(out, &sb) == 0) return true;
    if (!SYS->dir_b) return false;
    snprintf(out, n, "%s/%s", SYS->dir_b, name);
    return stat(out, &sb) == 0;
}

// ── the picture ─────────────────────────────────────────────────────────────────────────────────
// One scanline out of the core: drop every 16th line, map the 160 pixels straight through, accumulate
// BAND output lines, push once. Nine SPI transactions per frame instead of 135 — the difference
// between the bus being a cost and being the bottleneck.
// -O2 for this function alone. nucleo_app is a large component built at -Os like the rest of the
// firmware, but this is the emulator's per-scanline path: 135 lines x 60 fps, and at -Os the inner
// copy does not get unrolled. The attribute keeps the exception to the one function that earns it.
static void row_done(void);
static void rows_skip(int rows);
static bool line_same(const void *px, int bytes, int line);
__attribute__((optimize("-O2")))
static void on_line(const uint8_t *px, int line, void *user)
{
    (void)user;
    if (!st || !st->band[0]) return;
    if (s_big) {                                 // 3/2: a pair of pixels -> 3, every odd line drawn twice
        if (st->out_y >= s_out_h) return;
        if (s_stretch) {                         // filled: 2x across, rows 1-2-2 down (5/3)
            const int reps = (line + 1) * 5 / 3 - line * 5 / 3;
            if (line_same(px, SRC_W, line)) { rows_skip(reps); return; }
            uint16_t *row = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
            const uint16_t *sh = SHADE_TX;
            for (int x = 0; x < SRC_W; x++) { uint16_t a = sh[px[x] & 3]; row[2 * x] = a; row[2 * x + 1] = a; }
            row_done();
            for (int r = 1; r < reps && st->out_y < s_out_h; r++) {
                uint16_t *dup = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
                memcpy(dup, row, (size_t)s_out_w * sizeof *dup);
                row_done();
            }
            return;
        }
        if (line_same(px, SRC_W, line)) { rows_skip((line & 1) ? 2 : 1); return; }
        uint16_t *row = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
        const uint16_t *sh = SHADE_TX;
        for (int x = 0, o = 0; x < SRC_W; x += 2, o += 3) {
            uint16_t a = sh[px[x] & 3], b = sh[px[x + 1] & 3];
            row[o] = a; row[o + 1] = a; row[o + 2] = b;
        }
        row_done();
        if ((line & 1) && st->out_y < s_out_h) {
            uint16_t *dup = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
            memcpy(dup, row, (size_t)s_out_w * sizeof *dup);   // the row stays valid: a queued push only reads it
            row_done();
        }
        return;
    }
    if ((line & 15) == 7) {                      // the one line in sixteen the panel has no row for
        if (!s_sharp) { memcpy(st->stash, px, SRC_W); st->have_stash = true; }
        return;
    }
    if (st->out_y >= OUT_H) return;

    // A flat palette lookup — no strides, no run bookkeeping, no branch. The entries are already in
    // the panel's byte order (SHADE_TX), so what lands in the buffer is exactly what goes on the wire.
    uint16_t *row = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
    const uint16_t *sh = SHADE_TX;
    if (st->have_stash) {
        // The line after a merged one: each pixel is the midpoint of the two source pixels. Nine rows
        // a frame take this path; the other 126 stay on the plain lookup below.
        st->have_stash = false;
        const uint8_t *q = st->stash;
        if (!s_stretch) {
            for (int x = 0; x < SRC_W; x++) row[x] = MIX_TX[((q[x] & 3) << 2) | (px[x] & 3)];
        } else {
            for (int x = 0, o = 0; x < SRC_W; x += 2, o += 3) {
                uint16_t a = MIX_TX[((q[x] & 3) << 2) | (px[x] & 3)];
                uint16_t b = MIX_TX[((q[x + 1] & 3) << 2) | (px[x + 1] & 3)];
                row[o] = a; row[o + 1] = a; row[o + 2] = b;
            }
        }
    } else if (!s_stretch) {
        for (int x = 0; x < SRC_W; x++) row[x] = sh[px[x] & 3];
    } else {
        // 160 -> 240 is exactly 2:3, so each source PAIR becomes three output pixels and the loop
        // stays integer, branch-free and unrollable. Nearest-neighbour on purpose: a Game Boy is
        // meant to have hard pixel edges, and blending 21,600 pixels per frame would cost more than
        // the stretch itself.
        for (int x = 0, o = 0; x < SRC_W; x += 2, o += 3) {
            uint16_t a = sh[px[x] & 3], b = sh[px[x + 1] & 3];
            row[o] = a; row[o + 1] = a; row[o + 2] = b;
        }
    }
    row_done();
}

// Midpoint of two panel-order RGB565 pixels: the channels are averaged in place (the mask drops each
// field's low bit so no carry crosses into its neighbour), then the bytes go back to the panel's order.
static inline uint16_t mix565_tx(uint16_t a, uint16_t b)
{
    a = (uint16_t)((a >> 8) | (a << 8)); b = (uint16_t)((b >> 8) | (b << 8));
    uint16_t m = (uint16_t)((a & b) + (((a ^ b) & 0xF7DE) >> 1));
    return (uint16_t)((m >> 8) | (m << 8));
}

// Master System columns: 256 source columns onto s_out_w (180 or 240). Source column i lands on
// output i*k/64; where two neighbours land on the same output they are blended into it — the same
// "merge, never drop" rule as the lines — so a 1 px detail survives at half strength.
static void hscale_build(void)
{
    if (!st) return;
    int k = s_out_w * 64 / 256;                 // 45 (1:1) or 60 (filled)
    for (int i = 0, o = 0; i < 256 && o < s_out_w; o++) {
        st->hsrc[o] = (uint8_t)i;
        st->hmix[o] = (i + 1 < 256 && ((i + 1) * k >> 6) == (i * k >> 6));
        i += st->hmix[o] ? 2 : 1;
    }
}
static inline uint16_t hscale_px(const uint16_t *px, int o)
{
    uint16_t c = px[st->hsrc[o]];
    return st->hmix[o] ? mix565_tx(c, px[st->hsrc[o] + 1]) : c;
}

// The Game Gear's line: already colour, already in the panel's byte order (nucleo_gg.h), so 1:1 is a
// copy. Same line-merge and 2:3 stretch as the Game Boy path above. A 256x192 Master System picture
// merges 57 of its 192 lines (45/64) and resamples its columns through hscale_build's table.
__attribute__((optimize("-O2")))
static void on_line16(const uint16_t *px, int line, void *user)
{
    (void)user;
    if (!st || !st->band[0]) return;
    if (s_big) {                                 // Game Gear 160x144: the same exact 3/2; Master System 256x192: 5/4
        if (st->out_y >= s_out_h) return;
        if (s_stretch && s_src_w == SRC_W) {     // filled: 2x across, rows 1-2-2 down (5/3)
            const int reps = (line + 1) * 5 / 3 - line * 5 / 3;
            if (line_same(px, SRC_W * 2, line)) { rows_skip(reps); return; }
            uint16_t *row = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
            for (int x = 0; x < SRC_W; x++) { uint16_t a = px[x]; row[2 * x] = a; row[2 * x + 1] = a; }
            row_done();
            for (int r = 1; r < reps && st->out_y < s_out_h; r++) {
                uint16_t *dup = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
                memcpy(dup, row, (size_t)s_out_w * sizeof *dup);
                row_done();
            }
            return;
        }
        if (line_same(px, s_src_w * 2, line)) { rows_skip(s_src_w == SRC_W ? ((line & 1) ? 2 : 1) : ((line & 3) == 1 ? 2 : 1)); return; }
        uint16_t *row = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
        bool twice;
        if (s_src_w == SRC_W) {
            for (int x = 0, o = 0; x < SRC_W; x += 2, o += 3) {
                uint16_t a = px[x], b = px[x + 1];
                row[o] = a; row[o + 1] = a; row[o + 2] = b;
            }
            twice = line & 1;
        } else {
            for (int x = 0, o = 0; x < 256; x += 4, o += 5) {
                uint16_t a = px[x], b = px[x + 1], c = px[x + 2], e = px[x + 3];
                row[o] = a; row[o + 1] = b; row[o + 2] = b; row[o + 3] = c; row[o + 4] = e;
            }
            twice = (line & 3) == 1;
        }
        row_done();
        if (twice && st->out_y < s_out_h) {
            uint16_t *dup = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
            memcpy(dup, row, (size_t)s_out_w * sizeof *dup);
            row_done();
        }
        return;
    }
    bool merge = s_src_h == 144 ? (line & 15) == 7 : (line * 45 >> 6) == ((line + 1) * 45 >> 6);
    if (merge) {
        if (!s_sharp) { memcpy(st->stash16, px, (size_t)s_src_w * sizeof *px); st->have_stash = true; }
        return;
    }
    if (st->out_y >= OUT_H) return;
    uint16_t *row = st->band[st->band_idx] + (size_t)st->band_line * s_out_w;
    if (s_src_w != SRC_W) {
        if (st->have_stash) {
            st->have_stash = false;
            for (int o = 0; o < s_out_w; o++) row[o] = mix565_tx(hscale_px(st->stash16, o), hscale_px(px, o));
        } else {
            for (int o = 0; o < s_out_w; o++) row[o] = hscale_px(px, o);
        }
    } else if (st->have_stash) {
        st->have_stash = false;
        const uint16_t *q = st->stash16;
        if (!s_stretch) {
            for (int x = 0; x < SRC_W; x++) row[x] = mix565_tx(q[x], px[x]);
        } else {
            for (int x = 0, o = 0; x < SRC_W; x += 2, o += 3) {
                uint16_t a = mix565_tx(q[x], px[x]), b = mix565_tx(q[x + 1], px[x + 1]);
                row[o] = a; row[o + 1] = a; row[o + 2] = b;
            }
        }
    } else if (!s_stretch) {
        memcpy(row, px, SRC_W * sizeof *row);
    } else {
        for (int x = 0, o = 0; x < SRC_W; x += 2, o += 3) {
            uint16_t a = px[x], b = px[x + 1];
            row[o] = a; row[o + 1] = a; row[o + 2] = b;
        }
    }
    row_done();
}

static void band_push(void);
// One output row is in the band buffer: count it, and push the band once it is full.
static void row_done(void)
{
    if (st->band_line == 0) st->band_y = st->out_y;
    st->band_line++;
    st->out_y++;
    if (st->band_line == s_band_h || st->out_y >= s_out_h) band_push();
}
// Two screens: this source line is what the panel already shows. Send what is pending and step over
// its output rows; the next band starts below them.
static void rows_skip(int rows)
{
    if (st->band_line) band_push();
    st->out_y += rows;
    if (st->out_y > s_out_h) st->out_y = s_out_h;
}
// True when source line `line` (bytes long) is unchanged since the panel last got it. FNV-1a over words.
__attribute__((optimize("-O2")))
static bool line_same(const void *px, int bytes, int line)
{
    if (line < 0 || line >= 192 || ((uintptr_t)px & 3)) return false;
    const uint32_t *w = (const uint32_t *)px;
    uint32_t h = 2166136261u;
    for (int i = 0, n = bytes >> 2; i < n; i++) h = (h ^ w[i]) * 16777619u;
    bool same = st->diff_ok && st->lhash[line] == h;
    st->lhash[line] = h;
    return same;
}
// Rows at the top of the panel that the on-demand system bar covers this frame (0 = none): read once per frame.
static int s_ov;
static void band_push(void)
{
    if (s_big && s_ov) {                       // the bar is up: never paint under it (it would flicker)
        const int top = s_out_y0 + st->band_y;
        if (top + st->band_line <= s_ov) { st->band_line = 0; return; }   // wholly under the bar
        if (top < s_ov) {                      // partly: only the rows below it
            const int skip = s_ov - top;
            d.pushImageDMA(s_out_x, s_ov, s_out_w, st->band_line - skip, st->band[st->band_idx] + (size_t)skip * s_out_w);
            st->band_idx ^= 1;
            st->band_line = 0;
            return;
        }
    }
    {
        // No startWrite here: play() holds ONE transaction open across the whole frame, so every band
        // streams to the panel with no bus re-arbitration between them — a tighter, more continuous
        // update, which is what keeps the moving picture from tearing band by band.
        //
        // DMA + two buffers. pushImageDMA returns as soon as the transfer is queued, so the CPU runs
        // the next scanlines WHILE this band is on the wire; the emulator's two big costs stop being
        // additive. Alternating buffers is what makes that safe: the driver cannot start band N+1
        // until band N has drained the bus, so by the time a buffer comes round again nothing is
        // reading it. Only possible because the data is already panel-order — a converting push
        // builds its own temporary copy and cannot DMA from ours at all.
        int64_t b0 = esp_timer_get_time();
        d.pushImageDMA(s_out_x, s_out_y0 + st->band_y, s_out_w, st->band_line, st->band[st->band_idx]);
        st->us_blit += (uint32_t)(esp_timer_get_time() - b0);
        st->band_idx ^= 1;
        st->band_line = 0;
    }
}

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ── shelf title scroller (ANTI-FLICKER technique 3) ─────────────────────────────────────────────
// The shelf is a normal foreground app, so it is redrawn only when something changes — which means a
// scrolling title cannot ride on the list redraw (forcing a full-list repaint every frame just to move
// one row is the clear-then-draw flicker the earlier version had). Instead the selected title lives in
// its own small off-screen sprite, blitted in ONE pushSprite over just its row from a 50 Hz poll. The
// panel never sees a clear, so it is flicker-free and smooth, and the rest of the list is never touched.
static bool mq_acquire(int w)
{
    if (w < 8) w = 8;
    if (st->mq && st->mq_w == w) return true;
    if (!st->mq) { st->mq = new (std::nothrow) M5Canvas(nucleo_app_gfx()); if (!st->mq) return false; }
    else st->mq->deleteSprite();
    st->mq->setColorDepth(16);             // exact INK/ACC (the shelf has RAM to spare; the game is not loaded yet)
    st->mq->setPsram(false);               // no SPIRAM on this board — skip the doomed probe
    if (!st->mq->createSprite(w, 16)) { st->mq_w = 0; return false; }
    st->mq->setTextWrap(false);            // CRUCIAL: without this a long title wraps instead of scrolling
    st->mq_w = w;
    return true;
}
static void mq_free(void)
{
    if (st && st->mq) { st->mq->deleteSprite(); delete st->mq; st->mq = nullptr; st->mq_w = 0; }
}

// ~50 Hz. Scrolls the selected title if it overflows its row; a title that fits was already drawn
// static by draw(), so this does nothing. Returns false: it must NEVER request a list repaint (that
// is the flicker). It draws straight to the panel over just the one row.
static int wheel_offset(void);
static int64_t s_wheel_t0_peek(void);
static void play(const char *name);
static bool shelf_poll(void)
{
    // Opened from the Games gallery on one cartridge: start it here, once the run loop has set the surface up
    // (on_enter is too early for the second screen). However the game ends — quit from its menu, or a cartridge
    // the core refuses (the reason goes to the trace) — the player goes back to the gallery, never to this shelf.
    if (st && st->autoplay[0] && !st->running) {
        char name[PATHMAX];
        snprintf(name, sizeof name, "%s", st->autoplay);
        st->autoplay[0] = '\0';
        play(name);
        if (st && st->fail[0]) trace("  back to the gallery after: %s", st->fail);
        nucleo_app_exit();
        return false;
    }
    if (nucleo_app_two_screens()) {                  // the gallery on the panel: frames only while the wheel turns
        if (!s_wheel_t0_peek()) return false;
        return true;                                 // the paint itself ends the turn (wheel_offset clears t0)
    }
    if (!st || st->running || !st->sel_title[0] || st->fail[0]) return false;   // never draw over the failure box
    const int avail = st->sel_avail;
    int tw = (int)strlen(st->sel_title) * 12;          // size-2 glyphs are ~12 px wide
    if (tw <= avail) return false;                      // fits: draw()'s static title is enough
    if (!mq_acquire(avail)) return false;
    const int span = tw + 24;                           // gap before the wrap copy, for a seamless loop
    int off = (int)((now_ms() * 26 / 1000) % span);     // 26 px/s, continuous
    M5Canvas *cv = st->mq;
    cv->fillSprite(ACC);
    cv->setTextSize(2); cv->setTextColor(INK, ACC);
    cv->setCursor(-off, 0);        cv->print(st->sel_title);
    cv->setCursor(-off + span, 0); cv->print(st->sel_title);   // second copy so the loop never shows a gap
    d.setClipRect(20, st->sel_y, avail, 16);            // own the clip so the blit repaints the whole band
    cv->pushSprite(20, st->sel_y);
    d.clearClipRect();
    return false;
}

// ── in-game furniture ───────────────────────────────────────────────────────────────────────────
// The picture is 160 px wide inside a 240 px panel, which leaves two 40 px pillars. They are not
// padding: they are the only place a running emulator can say anything without covering the game.
// LEFT is identity and control (frame rate, palette, how to reach the menu); RIGHT is the honest
// cost breakdown, because "it feels slow" is not a bug report until it says which part is slow.
#define PILL_L 0
#define PILL_R (SRC_X + SRC_W)
#define PILL_W SRC_X

static inline bool dn(char c) { return nucleo_kbd_char_down(c); }

// Smart marquee. Text that fits is drawn once, left-aligned. Text that does not ping-pongs: it holds
// at the start long enough to read the beginning, slides left at a steady pace, holds at the end, and
// slides back. `phase_ms` is a free-running clock, so the same call animates smoothly from the 50 Hz
// menu loop and coarsely from the 5 Hz shelf tick without either needing its own timer. It fills its
// own background and clips to its box, so a caller just points it at a rectangle.
static void marquee(int x, int y, int w, int h, const char *t,
                    uint16_t fg, uint16_t bg, int size, int64_t phase_ms)
{
    d.setTextSize(size);
    d.setTextColor(fg, bg);
    int tw = (int)d.textWidth(t);
    if (tw <= w) { d.fillRect(x, y, w, h, bg); d.setCursor(x, y); d.print(t); return; }

    const int over  = tw - w;
    const int PAUSE = 850;                 // ms held at each end
    const int SPEED = 26;                  // px/second while sliding
    int travel = over * 1000 / SPEED; if (travel < 1) travel = 1;
    int period = 2 * (PAUSE + travel);
    int ph = (int)(phase_ms % period);
    int off;
    if      (ph < PAUSE)                 off = 0;
    else if (ph < PAUSE + travel)        off = (ph - PAUSE) * over / travel;
    else if (ph < 2 * PAUSE + travel)    off = over;
    else                                 off = over - (ph - 2 * PAUSE - travel) * over / travel;

    // Opaque glyphs cover the whole clipped width (the line overruns the box), so no fillRect is
    // needed and the box is never, even for one frame, just background. That is what stops the flicker.
    d.setClipRect(x, y, w, h);
    d.setCursor(x - off, y);
    d.print(t);
    d.clearClipRect();
}

// ONE relief lever, not two. Interlacing was the second, and it was wrong for THIS renderer: the core
// emits alternate scanlines under interlace, but our model counts an output row per scanline RECEIVED,
// so with half the lines arriving the picture only reaches y~67 and the bottom half of the screen
// simply stops updating — the "split down the middle" the player saw. Frame-skip has no such problem:
// a skipped frame calls the scanline callback zero times, so nothing half-draws. It is the only lever.
static void set_relief(int level)
{
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    st->relief = level;
    SYS->set_frameskip(level == 1);
}

static void toast(const char *msg)
{
    snprintf(st->toast, sizeof st->toast, "%s", msg);
    st->toast_until = esp_timer_get_time() + 1500000;
}

// Frame rate big enough to read at arm's length, then the cost breakdown under it. Tenths of a
// millisecond, because the interesting differences live below a whole one.
static void hud_draw(void)
{
    if (!hud_on()) return;             // info hidden (or stretched: there are no pillars to draw in)
    d.fillRect(PILL_L, 0, PILL_W - 2, 30, BG);
    d.setTextSize(2);
    d.setTextColor(st->fps >= 55 ? (SYS->palettes ? SHADE[1] : GRN) : AMB, BG);
    d.setCursor(3, 3); d.printf("%2d", st->fps);
    d.setTextSize(1);
    d.setTextColor(DIM, BG);
    d.setCursor(3, 20); d.print("fps");

    d.fillRect(PILL_R + 2, 0, PILL_W - 2, 46, BG);
    d.setTextSize(1);
    d.setTextColor(MUTED, BG);
    d.setCursor(PILL_R + 4, 3);  d.printf("c%2d.%d", st->ms_cpu  / 10, st->ms_cpu  % 10);
    d.setCursor(PILL_R + 4, 14); d.printf("b%2d.%d", st->ms_blit / 10, st->ms_blit % 10);
    d.setCursor(PILL_R + 4, 25); d.printf("a%2d.%d", st->ms_aud  / 10, st->ms_aud  % 10);
    d.setTextColor(st->relief ? AMB : DIM, BG);
    d.setCursor(PILL_R + 4, 36); d.print(st->relief == 1 ? "30fps" : "  --");
}

// Everything in the pillars that is NOT the per-second HUD: palette name, the keys that are not
// guessable, and any transient confirmation. Repainted whenever it can have been covered.
static void chrome_draw(void)
{
    if (!hud_on()) return;
    d.fillRect(PILL_L, 88, PILL_W - 2, 47, BG);
    d.setTextSize(1);
    // The palette is the player's on a Game Boy; on a Game Gear the cartridge owns the colours, so
    // the slot names the console instead and "P" has nothing to cycle.
    d.setTextColor(SYS->palettes ? SHADE[1] : GRN, BG);
    d.setCursor(3, 101); d.print(SYS->palettes ? PAL_NAME[s_pal] : SYS->short_name);
    d.setTextColor(DIM, BG);
    if (SYS->palettes) { d.setCursor(3, 113); d.print("P pal"); }
    d.setCursor(3, 124); d.print("M menu");
    d.setCursor(3, 90);  d.print("-/= vol");

    d.fillRect(PILL_R + 2, 100, PILL_W - 2, 35, BG);
    if (st->toast[0]) {
        d.setTextColor(GRN, BG);
        d.setCursor(PILL_R + 4, 113); d.print(st->toast);
    } else {
        d.setTextColor(DIM, BG);
        d.setCursor(PILL_R + 4, 101); d.print("I hide");     // I hides these pillars entirely
        d.setCursor(PILL_R + 4, 113); d.print("TAB");
        d.setCursor(PILL_R + 4, 124); d.print("fast");
    }
}

// TWO SCREENS: the card on the Cardputer's own screen. `full` repaints it whole; otherwise only the live line
// (fps, battery, volume, a toast) — once a second, never per frame.
// The console now playing, as its mark: the Color core has its own, a Master System cartridge on the Game Gear
// shelf the Master System's.
static const EmuArt *sys_icon(void) { return s_src_w == SRC_W ? &SYS->icon_a : &SYS->icon_b; }

// The game's title for the screens: the cartridge header's, else the file name made readable.
static void pretty(const char *file, char *out, size_t n);
static void game_title(char *out, size_t n)
{
    const char *t = SYS->title();
    if (t && t[0]) { snprintf(out, n, "%s", t); return; }
    pretty(st->resume, out, n);
}

// A key cap: the key on a raised cap, what it does beside it. Returns the x after it.
static int keycap(LovyanGFX *g, int x, int y, const char *key, const char *what)
{
    g->setFont(&fonts::Font0); g->setTextSize(1);
    const int kw = (int)g->textWidth(key) + 8;
    g->fillRoundRect(x, y, kw, 13, 3, LINE);
    g->drawFastHLine(x + 2, y + 12, kw - 4, DIM);            // the cap's lower lip
    g->setTextColor(FG, LINE); g->setCursor(x + 4, y + 3); g->print(key);
    g->setTextColor(MUTED, BG); g->setCursor(x + kw + 3, y + 3); g->print(what);
    return x + kw + 3 + (int)g->textWidth(what) + 7;
}

// TWO SCREENS — the Cardputer while the game plays on the panel: never a copy of the picture, a companion. A
// header (the console, the session's clock, the battery), the title, the frame rate with its last 32 seconds as
// a sparkline, the volume, and the controls as key caps. full = everything; else only what changes each second.
static void card_draw(bool full)
{
    LovyanGFX *g = igfx();
    if (!g || st->menu) return;
    g->startWrite();
    if (full) {
        g->fillScreen(BG);
        const int iw = icon_draw(g, 4, 1, sys_icon());
        char t[40]; game_title(t, sizeof t);
        g->setFont(&fonts::FreeSansBold9pt7b);
        while (t[0] && (int)g->textWidth(t) > 228 - iw) t[strlen(t) - 1] = '\0';
        g->setTextColor(FG, BG); g->setCursor(iw + 10, 5); g->print(t);
        g->setFont(&fonts::Font0);
        g->drawFastHLine(6, 87, 228, LINE);
        // The controls, as the keys they are on this keyboard.
        // Three rows of caps, 14 px apart: the last one ends on row 132 of 135.
        int x = keycap(g, 4, 91, "E S A D", TR5("muovi", "move", "mover", "bouger", "bewegen"));
        if (!strcmp(SYS->id, "gbemu")) {
            x = keycap(g, x, 91, "K", "A");
            keycap(g, x, 91, "J", "B");
            x = keycap(g, 4, 105, TR5("Invio", "Enter", "Intro", "Entree", "Enter"), "START");
            keycap(g, x, 105, TR5("Spazio", "Space", "Espacio", "Espace", "Leer"), "SELECT");
        } else {
            x = keycap(g, x, 91, "J", "1");
            keycap(g, x, 91, "K", "2");
            keycap(g, 4, 105, TR5("Invio", "Enter", "Intro", "Entree", "Enter"), s_src_w == SRC_W ? "START" : TR5("PAUSA", "PAUSE", "PAUSA", "PAUSE", "PAUSE"));
        }
        x = keycap(g, 4, 119, "M", "menu");
        x = keycap(g, x, 119, "TAB", TR5("veloce", "fast", "rapido", "vite", "schnell"));
        keycap(g, x, 119, "- =", "volume");
    }
    // ── what changes: the clock and battery, the frame rate and its sparkline, the volume (or the toast) ──
    g->setFont(&fonts::Font0); g->setTextSize(1);
    g->fillRect(0, 26, 240, 14, BG);
    const int secs = (int)((esp_timer_get_time() - st->play_t0) / 1000000);
    char hb[24];
    int bp = nucleo_power_battery_pct();
    if (bp >= 0) snprintf(hb, sizeof hb, "%d:%02d   %d%%", secs / 60, secs % 60, bp);
    else         snprintf(hb, sizeof hb, "%d:%02d", secs / 60, secs % 60);
    g->setTextDatum(top_left); g->setTextColor(bp >= 0 && bp < 20 ? AMB : MUTED, BG);
    g->drawString(hb, 6, 29);

    const uint16_t fc = st->fps >= 55 ? GRN : AMB;
    g->fillRect(0, 44, 240, 42, BG);
    g->setTextSize(3); g->setTextColor(fc, BG); g->setCursor(6, 46); g->printf("%2d", st->fps);
    g->setTextSize(1); g->setTextColor(MUTED, BG); g->setCursor(46, 60); g->print("fps");
    // The sparkline: one bar per second, 60 fps at the top of its 26 px, the dips in amber.
    const int sx = 74, sy = 46, sh = 26, n = st->fps_hn < 32 ? st->fps_hn : 32;
    g->drawFastHLine(sx, sy + sh, 32 * 5, LINE);
    for (int i = 0; i < n; i++) {
        int v = st->fps_hist[(st->fps_hn - n + i) & 31]; if (v > 60) v = 60;
        int h = v * sh / 60; if (h < 1) h = 1;
        g->fillRect(sx + i * 5, sy + sh - h, 4, h, v >= 55 ? GRN : AMB);
    }
    // Volume (or the last toast: "saved", "loaded", "vol 70%").
    if (st->toast[0]) { g->setTextColor(GRN, BG); g->setCursor(6, 77); g->print(st->toast); }
    else {
        const int v = nucleo_audio_is_muted() ? 0 : nucleo_audio_volume();
        g->setTextColor(MUTED, BG); g->setCursor(6, 77); g->print("vol");
        g->fillRoundRect(28, 77, 100, 7, 3, LINE);
        if (v) g->fillRoundRect(28, 77, v, 7, 3, ACC);
        g->setCursor(134, 77); g->printf("%d%%", v);
    }
    g->endWrite();
}

// TWO SCREENS — the panel around a 3/2 picture: a console's bezel, not a black void. A dark frame, a rounded
// rim around the picture, the console's name in the bottom margin. Filled (or a Master System picture) covers
// the whole panel, so there is nothing to frame. The next frame repaints the picture itself.
#define BEZEL 0x10A2
static void bezel_draw(void)
{
    if (!s_big) return;
    d.fillScreen(BEZEL);
    if (s_out_w >= 320) return;
    d.drawRoundRect(s_out_x - 4, s_out_y0 - 4, s_out_w + 8, s_out_h + 8, 6, LINE);
    d.drawRoundRect(s_out_x - 3, s_out_y0 - 3, s_out_w + 6, s_out_h + 6, 5, DIM);
    const EmuArt *ic = sys_icon();
    if (s_out_x >= ic->w + 12) icon_draw(&d, (s_out_x - 4 - (ic->w + 8)) / 2, 240 - 30, ic);   // the console's mark
}

// Everything outside the picture, blanked in one go. Called when the info is switched off (the
// pillars must not keep the last frame's numbers frozen on screen) and when the mode changes.
static void frame_clear(void)
{
    if (s_stretch || s_big) return;    // the picture covers the panel (or is on the external one): nothing to clear
    d.fillRect(0, 0, s_out_x, 135, BG);
    d.fillRect(s_out_x + s_out_w, 0, 240 - (s_out_x + s_out_w), 135, BG);
}

// Apply a change of view (info on/off, 1:1 vs stretched) to a RUNNING game. The PPU repaints the
// whole picture on its very next frame, so only the furniture around it has to be dealt with here —
// and going back from stretched to 1:1 needs the full screen wiped, because the picture is about to
// stop covering the pillars.
static void view_apply(void)
{
    if (s_big) {                       // two screens: 3/2 or filled on the panel; the next frame repaints it all
        geom_apply();
        st->diff_ok = false;
        bezel_draw();
        return;
    }
    geom_apply();
    d.fillScreen(BG);
    hud_draw();
    chrome_draw();
}

static void pal_cycle(void)
{
    if (!SYS->palettes) return;
    s_pal = (s_pal + 1) % PAL_COUNT;
    pal_apply();
    st->diff_ok = false;                          // every line changes colour
    state_save(st->resume);
    chrome_draw();
}

// ── the in-game menu ────────────────────────────────────────────────────────────────────────────
// Opened with M or Esc. Esc opening a MENU rather than quitting outright is the deliberate choice:
// leaving a game means losing the session (the app runs in a Solo boot and exiting reboots), so the
// one key a player hits by reflex must not be the destructive one. Quit is still one row away.
enum { MI_RESUME = 0, MI_SAVE, MI_LOAD, MI_VOL, MI_SCREEN, MI_LINES, MI_INFO, MI_PAL, MI_PIC, MI_RESET,
       MI_QUIT, MI_N };

static const char *menu_label(int i, char *buf, size_t n)
{
    switch (i) {
        case MI_RESUME: return TR5("Riprendi", "Resume", "Reanudar", "Reprendre", "Weiter");
        case MI_SAVE:   return TR5("Salva stato", "Save state", "Guardar", "Sauvegarder", "Speichern");
        // Cached: menu_label runs on every repaint and every 20 ms for the scrolling row, and a stat()
        // on the SD card per call is exactly the kind of work that must not ride a UI loop.
        case MI_LOAD:   return st->state_ok ? TR5("Carica stato", "Load state", "Cargar", "Charger", "Laden")
                                            : TR5("Carica stato (vuoto)", "Load state (empty)", "Cargar (vacio)", "Charger (vide)", "Laden (leer)");
        case MI_VOL:    snprintf(buf, n, "%s: %d%%", TR5("Volume", "Volume", "Volumen", "Volume", "Lauts."), nucleo_audio_volume()); return buf;
        case MI_SCREEN: snprintf(buf, n, "%s: %s", TR5("Schermo", "Screen", "Pantalla", "Ecran", "Bild"),
                                 s_stretch ? TR5("Pieno", "Filled", "Lleno", "Complet", "Voll")
                                           : TR5("Originale", "Original", "Original", "Original", "Original")); return buf;
        case MI_LINES:  snprintf(buf, n, "%s: %s", TR5("Righe", "Lines", "Lineas", "Lignes", "Linien"),
                                 s_sharp ? TR5("Nette", "Sharp", "Nitida", "Nette", "Scharf") : TR5("Fuse", "Blended", "Fundido", "Fondu", "Gemischt")); return buf;
        case MI_RESET:  return TR5("Reset gioco", "Reset game", "Reiniciar", "Reinit.", "Zuruecksetzen");
        case MI_INFO:   snprintf(buf, n, "%s: %s", TR5("Info a schermo", "On-screen info", "Info en pantalla", "Info a l'ecran", "Info auf Bild"),
                                 (s_stretch || s_src_w != SRC_W) ? TR5("n.d.", "n/a", "n.d.", "n.d.", "n.v.") : s_hud ? TR5("si", "on", "si", "on", "an") : TR5("no", "off", "no", "off", "aus"));
                        return buf;
        case MI_PAL:    snprintf(buf, n, "%s: %s", TR5("Colori", "Palette", "Paleta", "Palette", "Palette"),
                                 SYS->palettes ? PAL_NAME[s_pal] : TR5("della cartuccia", "cartridge", "del cartucho", "de cartouche", "der Patrone"));
                        return buf;
        case MI_PIC:    snprintf(buf, n, "%s: %s", TR5("Immagine", "Picture", "Imagen", "Image", "Bild"),
                                 st->relief == 1 ? "30 fps" : TR5("Piena", "Full", "Completa", "Complete", "Vollst."));
                        return buf;
        default:        return TR5("Esci dal gioco", "Quit game", "Salir juego", "Quitter jeu", "Spiel beenden");
    }
}

// The menu grew past what 135 px can show, so it SCROLLS: MENU_VIS rows are on screen and the
// selection carries the window with it. Six rows is what fits without shrinking the type — the
// panel is small enough already, and docs/device-ui.md is explicit that legibility wins.
#define MENU_VIS 6
#define MENU_W 200
#define MENU_H (12 + MENU_VIS * 17 + 8)
#define MENU_X ((240 - MENU_W) / 2)
#define MENU_Y ((135 - MENU_H) / 2)
#define MENU_LX (MENU_X + 22)                    // label column x
#define MENU_LW (MENU_W - 22 - 6)                // label column width

// One row. The selected row's label scrolls (marquee) so a long entry like "Load state (empty)" or a
// palette name is fully readable instead of clipped; the rest are drawn once, clipped, no animation.
static void menu_row(int i)
{
    char buf[40];
    int slot = i - st->mscroll;
    if (slot < 0 || slot >= MENU_VIS) return;        // outside the scrolled window
    int y = MENU_Y + 14 + slot * 17;
    bool on = (i == st->msel);
    d.fillRect(MENU_X + 4, y, MENU_W - 8, 16, on ? INK : BG);
    d.setTextSize(1);
    d.setTextColor(on ? DIM : LINE, on ? INK : BG);
    if (i < 9) { d.setCursor(MENU_X + 8, y + 5); d.printf("%d", i + 1); }   // 1-9 always picks the same entry
    const char *t = menu_label(i, buf, sizeof buf);
    uint16_t fg = on ? FG : MUTED, bg = on ? INK : BG;
    if (on) {
        marquee(MENU_LX, y + 1, MENU_LW, 15, t, fg, bg, 2, now_ms());
    } else {
        d.fillRect(MENU_LX, y + 1, MENU_LW, 15, bg);
        d.setClipRect(MENU_LX, y + 1, MENU_LW, 15);
        d.setTextSize(2); d.setTextColor(fg, bg);
        d.setCursor(MENU_LX, y + 1); d.print(t);
        d.clearClipRect();
    }
}

// TWO SCREENS: the pause menu is a card on the panel, over the paused picture, with the kit's rows (label left,
// value right) and direct touch — tap a row to pick it, tap it again to do it, drag to scroll, tap outside the
// card to resume. The Cardputer keeps the game's card. Rows are drawn straight to the panel, each over itself.
#define BMENU_W   288
#define BMENU_H   224
#define BMENU_ROW 28
static int s_bm_scroll;
static bool menu_danger(int i) { return i == MI_RESET || i == MI_QUIT; }
// On the panel the line blending and the pillars' info do not exist (the picture is scaled whole): no rows.
static bool menu_shown(int i) { return !s_big || (i != MI_LINES && i != MI_INFO); }
static int  bm_rows(int *map)
{
    int n = 0;
    for (int i = 0; i < MI_N; i++) if (menu_shown(i)) map[n++] = i;
    return n;
}
static int  bm_index(int item) { int map[MI_N], n = bm_rows(map); for (int i = 0; i < n; i++) if (map[i] == item) return i; return 0; }
static void menu_row_big(int row, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    int map[MI_N]; bm_rows(map);
    const int i = map[row];
    char buf[48];
    const char *t = menu_label(i, buf, sizeof buf);
    // "Label: value" becomes the row's label and its right-hand value.
    char label[48]; const char *val = nullptr;
    snprintf(label, sizeof label, "%s", t);
    char *colon = strstr(label, ": ");
    if (colon) { *colon = '\0'; val = colon + 2; }
    lg::row_text(x, y, w, h, focus, bg, label, val, menu_danger(i) ? AMB : FG);
}
static void menu_draw_big(void)
{
    const int W = d.width(), H = d.height(), cx = (W - BMENU_W) / 2, cy = (H - BMENU_H) / 2;
    nucleo_touch_clear();
    d.fillScreen(BEZEL);                                // nothing of the bezel peeks out from under the card
    lg::panel(cx, cy, BMENU_W, BMENU_H, BG);
    d.drawRoundRect(cx, cy, BMENU_W, BMENU_H, 10, LINE);
    nucleo_gb_stats_t w = {}; SYS->get_stats(&w);
    lg::font(1); d.setTextDatum(middle_left);
    d.setTextColor(w.core_errors ? AMB : ACC, BG);
    const char *head = w.core_errors ? TR5("CPU bloccata", "CPU crashed", "CPU bloqueada", "CPU bloquee", "CPU blockiert")
                                     : TR5("Pausa", "Paused", "Pausa", "Pause", "Pause");
    d.drawString(head, cx + 14, cy + 17);
    const int hw = (int)d.textWidth(head);
    char gt[40]; game_title(gt, sizeof gt);
    char title[40]; lg::font(0); lg::fit(title, sizeof title, gt, BMENU_W - 28 - hw - 16);   // never under the heading
    d.setTextDatum(middle_right); d.setTextColor(MUTED, BG);
    d.drawString(title, cx + BMENU_W - 14, cy + 17);
    d.setTextDatum(top_left);
    int map[MI_N]; const int nrows = bm_rows(map);
    lg::list(cx + 6, cy + 34, BMENU_W - 12, BMENU_H - 40, BMENU_ROW, nrows, bm_index(st->msel), &s_bm_scroll, menu_row_big, nullptr, ACC);
    // Outside the card: resume (the panel's whole top band, sides and bottom).
    nucleo_touch_add(0, 0, W, cy, NK_BACK, 0);
    nucleo_touch_add(0, cy + BMENU_H, W, H - cy - BMENU_H, NK_BACK, 0);
    nucleo_touch_add(0, cy, cx, BMENU_H, NK_BACK, 0);
    nucleo_touch_add(cx + BMENU_W, cy, W - cx - BMENU_W, BMENU_H, NK_BACK, 0);
    nucleo_touch_commit();
}

static void menu_draw(void)
{
    if (s_big) { menu_draw_big(); return; }
    if (st->msel < st->mscroll)                st->mscroll = st->msel;
    if (st->msel >= st->mscroll + MENU_VIS)    st->mscroll = st->msel - MENU_VIS + 1;
    if (st->mscroll > MI_N - MENU_VIS)         st->mscroll = MI_N - MENU_VIS;
    if (st->mscroll < 0)                       st->mscroll = 0;

    d.fillRect(MENU_X, MENU_Y, MENU_W, MENU_H, BG);
    d.drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 6, LINE);
    d.setTextSize(1);
    nucleo_gb_stats_t w; SYS->get_stats(&w);
    if (w.core_errors) {                  // the game ran into data: say it, the Reset row is selected
        d.setTextColor(AMB, BG);
        d.setCursor(MENU_X + 8, MENU_Y + 4);
        d.printf(TR5("CPU bloccata @%04X - Reset?", "CPU crashed @%04X - Reset?", "CPU bloq. @%04X - Reiniciar?", "CPU bloquee @%04X - Reinit?", "CPU blockiert @%04X - Reset?"), (unsigned)w.core_err_addr);
    } else {
        d.setTextColor(DIM, BG);
        d.setCursor(MENU_X + 8, MENU_Y + 4); d.print(SYS->title());
    }
    // "there is more" has to be visible without arrowing blindly: a rail on the right of the card.
    int rh = MENU_VIS * 17, kh = rh * MENU_VIS / MI_N;
    int ky = MENU_Y + 14 + (rh - kh) * st->mscroll / (MI_N - MENU_VIS);
    d.fillRect(MENU_X + MENU_W - 5, MENU_Y + 14, 2, rh, LINE);
    d.fillRect(MENU_X + MENU_W - 5, ky, 2, kh, ACC);
    for (int i = 0; i < MI_N; i++) menu_row(i);
}

// Called from the paused loop at ~50 Hz. It must NOT redraw the whole row: menu_row() clears the row
// to a solid fill first, and doing that fifty times a second is precisely the clear-then-draw flicker
// (ANTI-FLICKER.md). It re-draws ONLY the selected label, and only when that label actually overflows
// its column — the marquee's scrolling path is opaque and never clears, so it moves without flashing.
// A label that fits was drawn once by menu_draw and is left untouched (static, so nothing to animate).
static void menu_anim(void)
{
    if (!st->menu || s_big) return;                      // on the panel the rows fit: nothing scrolls
    char buf[40];
    const char *t = menu_label(st->msel, buf, sizeof buf);
    d.setTextSize(2);
    if ((int)d.textWidth(t) <= MENU_LW) return;          // fits: static, no redraw, no flicker
    int y = MENU_Y + 14 + (st->msel - st->mscroll) * 17;
    marquee(MENU_LX, y + 1, MENU_LW, 15, t, FG, INK, 2, now_ms());
}

// Leaving the menu: the PPU repaints the whole 160x135 picture on its very next frame, so only the
// pillars and the panel edges need restoring by hand.
static void menu_close(void)
{
    st->menu = false;
    st->diff_ok = false;                          // a state load / reset changed the picture under the menu
    if (s_big) {                                  // the menu card was on the panel: clear it, the next frame repaints
        nucleo_touch_clear(); nucleo_touch_commit();
        bezel_draw();
        card_draw(true);
    }
    // Enter, Space and K both drive this menu AND are Start, Select and A. Whatever is still held
    // at the moment the menu closes stays out of the game until it is released — otherwise picking
    // "Resume" with Enter pressed Start and paused the game again.
    st->mute_keys = true;
    // The card covered the middle of the screen; the PPU repaints the picture on its next frame, so
    // only the furniture needs restoring. Stretched, there is no furniture at all.
    frame_clear();
    hud_draw();
    chrome_draw();
    // The volume the player just set is an OS-wide setting, so it is saved where every other surface
    // reads it — once, on the way out of the menu, never per keypress (that would be an SD write per
    // press). The view options go in the emulator's own one-line state file.
    nucleo_prefs_save(nucleo_app_brightness(), nucleo_audio_volume(), nucleo_audio_is_muted());
    state_save(st->resume);
}

// One volume step. 5% is fine-grained enough to find the level a tiny speaker actually sounds good
// at, and coarse enough to cross the whole range in a few presses.
static void vol_step(int dir)
{
    int v = nucleo_audio_volume() + dir * 5;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    nucleo_audio_set_volume(v);
}

// Left/Right on a row that HAS a value: volume slides, everything else toggles. Enter still cycles,
// so nothing is reachable only one way.
static void menu_adjust(int dir)
{
    switch (st->msel) {
        case MI_VOL:    vol_step(dir); break;
        case MI_SCREEN: s_stretch = !s_stretch; view_apply(); break;
        case MI_LINES:  s_sharp = !s_sharp; break;
        case MI_INFO:   s_hud = !s_hud; if (!s_hud) frame_clear(); else { hud_draw(); chrome_draw(); } break;
        case MI_PAL:    if (!SYS->palettes) return;
                        s_pal = (s_pal + PAL_COUNT + dir) % PAL_COUNT; pal_apply(); break;
        case MI_PIC:    set_relief((st->relief + 1) % 2); break;
        default:        return;
    }
    menu_draw();
}

static void menu_activate(void)
{
    switch (st->msel) {
        case MI_RESUME: menu_close(); return;
        case MI_VOL:    vol_step(+1); menu_draw(); return;
        case MI_SCREEN: s_stretch = !s_stretch; view_apply(); menu_draw(); return;
        case MI_LINES:  s_sharp = !s_sharp; menu_draw(); return;
        case MI_RESET:
            SYS->reset();
            st->crash_told = false;
            toast(TR5("reset", "reset", "reset", "reinit", "zurueck"));
            menu_close(); return;
        case MI_INFO:   s_hud = !s_hud;
                        if (!s_hud) frame_clear(); else { hud_draw(); chrome_draw(); }
                        menu_draw(); return;
        case MI_SAVE: {
            esp_err_t e = SYS->state_save(0);
            if (e == ESP_OK) st->state_ok = true;
            toast(e == ESP_OK ? TR5("salvato", "saved", "guardado", "enreg.", "speich.") : TR5("errore", "failed", "error", "erreur", "fehler"));
            menu_close(); return;
        }
        case MI_LOAD: {
            esp_err_t e = SYS->state_load(0);
            toast(e == ESP_OK ? TR5("caricato", "loaded", "cargado", "charge", "geladen")
                              : e == ESP_ERR_NOT_FOUND ? TR5("nessuno stato", "no state", "sin estado", "sans etat", "kein zustand")
                                                       : TR5("stato non valido", "bad state", "estado malo", "etat mauvais", "Bad Zustand"));
            menu_close(); return;
        }
        case MI_PAL: if (!SYS->palettes) return;
                     s_pal = (s_pal + 1) % PAL_COUNT; pal_apply(); state_save(st->resume);
                     menu_draw(); return;
        case MI_PIC: set_relief((st->relief + 1) % 2); menu_draw(); return;
        default:     st->running = false; return;
    }
}

static void menu_key(nucleo_key_t k)
{
    if (s_big && nucleo_touch_is_row(k.key, k.ch)) {   // a row of the panel's menu: pick it, or do it
        int map[MI_N]; const int nrows = bm_rows(map);
        int row = bm_index(st->msel);
        int r = lg::row_tap(k.key, k.ch, s_bm_scroll, nrows, &row);
        st->msel = map[row];
        if (r == 2) menu_activate(); else if (r == 1) menu_draw();
        return;
    }
    if (k.key == NK_BACK || k.ch == '`' || k.ch == 'm' || k.ch == 'M') { menu_close(); return; }
    if (k.ch >= '1' && k.ch <= '9') {
        int map[MI_N]; const int nrows = bm_rows(map), want = k.ch - '1';
        if (want < nrows) { st->msel = map[want]; menu_activate(); }
        return;
    }
    if (k.key == NK_UP   || k.ch == 'e' || k.ch == 'E' || k.ch == ';') { do st->msel = (st->msel + MI_N - 1) % MI_N; while (!menu_shown(st->msel)); menu_draw(); return; }
    if (k.key == NK_DOWN || k.ch == 's' || k.ch == 'S' || k.ch == '.') { do st->msel = (st->msel + 1) % MI_N; while (!menu_shown(st->msel)); menu_draw(); return; }
    if (k.key == NK_LEFT  || k.ch == 'a' || k.ch == 'A' || k.ch == ',') { menu_adjust(-1); return; }
    if (k.key == NK_RIGHT || k.ch == 'd' || k.ch == 'D' || k.ch == '/') { menu_adjust(+1); return; }
    if (k.key == NK_ENTER || k.ch == '\n' || k.ch == ' ' || k.ch == 'k' || k.ch == 'K') menu_activate();
}

// ── the run loop ────────────────────────────────────────────────────────────────────────────────
// Show a failure where the user is actually looking — the middle of the screen — not only in the
// hint bar, which is easy to miss when nothing else appears to happen.
// The box is part of the shelf's STATE, drawn by draw(): the caller repaints the shelf right after a
// failed start, and a box painted straight to the panel was wiped by that repaint before anyone could
// read it. It stays until the next key.
static void fail_draw(void)
{
    if (!st->fail[0]) return;
    int top = nucleo_app_content_top(), ch = nucleo_app_content_height();
    int y = top + ch / 2 - 24;
    d.fillRect(8, y, 224, 48, BG);
    d.drawRoundRect(8, y, 224, 48, 8, AMB);
    d.setTextSize(1);
    d.setTextColor(AMB, BG);   d.setCursor(16, y + 8);  d.print(TR5("Avvio non riuscito", "Could not start", "No se pudo iniciar", "Pas possible", "Nicht starten"));
    d.setTextColor(FG, BG);    d.setCursor(16, y + 22); d.print(st->fail);
    d.setTextColor(DIM, BG);   d.setCursor(16, y + 34); d.print(SYS->trace_name);
}
static void fail_box(const char *what)
{
    snprintf(st->fail, sizeof st->fail, "%s", what);
    nucleo_app_set_hint(what);
}

// One wording for every way a cartridge can fail to start, whether the header probe or the full open
// said so — the same cause must never read two different ways.
// `cart` names an unsupported controller (the Game Boy core's words for it); the caller passes it so this
// shared front-end never reaches into one core from the other's module.
static void start_error(esp_err_t e, const char *cart, char *buf, size_t n)
{
    switch (e) {
        case ESP_ERR_INVALID_VERSION: snprintf(buf, n, "%s", TR5("Solo Game Boy Color", "Game Boy Color only", "Solo Game Boy Color", "Game Boy Color seul", "Nur Game Boy Color")); break;
        case ESP_ERR_NOT_SUPPORTED:   snprintf(buf, n, "%s %s", TR5("Cartuccia non supportata:", "Unsupported cartridge:", "Cartucho no soportado:", "Cartouche non supportee:", "Patrone nicht unterstutzt"),
                                               cart ? cart : "?"); break;
        case ESP_ERR_NOT_FOUND:       snprintf(buf, n, "%s", TR5("ROM non trovata", "ROM not found", "ROM no hallada", "ROM non trouvee", "ROM nicht da")); break;
        case ESP_ERR_NO_MEM:          snprintf(buf, n, "%s", TR5("RAM insufficiente", "Not enough RAM", "RAM insuficiente", "RAM insuffisante", "Zu wenig RAM")); break;
        case ESP_ERR_INVALID_SIZE:    snprintf(buf, n, "%s", TR5("File troppo corto", "File too short", "Archivo muy corto", "Fichier court", "Datei zu kurz")); break;
        default:                      snprintf(buf, n, "%s 0x%x", TR5("ROM non valida", "Invalid ROM", "ROM invalida", "ROM invalide", "ROM ungueltig"), (unsigned)e); break;
    }
}

static void play_session(const char *name);
static void pic_free(void);
// Every way out of a session — a refused cartridge, a failed open, a quit — lands back on the shelf,
// so the shelf comes back from the card HERE, once, rather than on each of those paths.
static void play(const char *name)
{
    // A Color cartridge on the Game Boy shelf plays on the Color core, for this session only: the shelf,
    // its saves and its trace stay the Game Boy's (SYS_GBC shares them), only the console changes.
    const EmuSys *shelf = SYS;
    if (SYS->pick) SYS = SYS->pick(name);
    play_session(name);
    SYS = shelf;
    if (st) shelf_swap_in();
}

static void play_session(const char *name)
{
    trace("--- play '%s' ---", name);
    // Record what is actually RUNNING, not what we intended: "solo=1 wifi=off" is the only honest
    // answer to "is the radio really down while I play?", and the heap line proves what it bought.
    trace("  solo=%d exclusive=%d wifi=%s ble=%d",
          (int)nucleo_anima_solo_active(), (int)nucleo_exclusive_active(),
          nucleo_setup_mode() ? nucleo_setup_mode() : "off",
          (int)nucleo_ble_radio_present());
    trace_heap("at entry");

    char path[300];
    if (!rom_path(name, path, sizeof path)) {
        trace("  FAIL: not found in either ROM folder");
        fail_box(TR5("ROM non trovata", "ROM not found", "ROM no hallada", "ROM non trouvee", "ROM nicht da"));
        return;
    }
    trace("  path=%s", path);
    esp_task_wdt_reset();          // each step below walks the library directory once — feed between them

    // Read the header while the shelf is still up. A cartridge this core cannot run is refused here,
    // in words, without tearing the UI down and building it back — "Invalid ROM" told nobody anything.
    {
        esp_err_t pe = SYS->probe(path);
        if (pe != ESP_OK) {
            char why[48];
            SYS->explain(pe, path, why, sizeof why);
            trace("  REFUSED: %s (err 0x%x)", why, (unsigned)pe);
            fail_box(why);
            return;
        }
    }

    // The shelf's scrolling-title sprite (~6.4 KB) is idle for as long as a game runs; it used to stay
    // allocated underneath the whole session, six ROM-cache pages' worth. shelf_poll() re-creates it
    // on demand when the shelf comes back.
    mq_free();
    pic_free();                                  // the gallery's picture too: re-read when the shelf is back
    // The name bytes in `name` point INTO the shelf pool, which is about to leave RAM: keep a copy.
    char rom_name[PATHMAX];
    snprintf(rom_name, sizeof rom_name, "%s", name);
    name = rom_name;
    shelf_swap_out();
    // Hand the 32 KB canvas back BEFORE the core allocates: it is the single biggest contiguous block
    // on the heap, and the ROM cache wants exactly that kind of room.
    nucleo_app_release_buffers();
    nucleo_screen_release();
    nucleo_app_set_direct_draw(true);

    trace_heap("canvas released");
    // Two DMA-capable band buffers (see on_line): one is on the wire while the other is being filled.
    // MALLOC_CAP_DMA, not DEFAULT — a DMA push from a buffer the controller cannot reach would have to
    // be copied first, which is exactly the cost this is here to remove.
    s_src_w = SRC_W; s_src_h = 144;              // until the open cartridge says otherwise
    geom_apply();
    st->band_idx = 0;
    st->band[0] = (uint16_t *)heap_caps_malloc(band_bytes(), MALLOC_CAP_DMA);
    st->band[1] = (uint16_t *)heap_caps_malloc(band_bytes(), MALLOC_CAP_DMA);
    if (!st->band[0] || !st->band[1]) {
        trace("  FAIL: band buffers (2 x %u B) alloc failed", (unsigned)band_bytes());
        free(st->band[0]); free(st->band[1]); st->band[0] = st->band[1] = nullptr;
        nucleo_app_set_direct_draw(false);
        fail_box(TR5("RAM insufficiente (video)", "Not enough RAM (video)", "RAM insuf. (video)", "RAM insuf. (video)", "Zu wenig RAM (video)"));
        return;
    }

    esp_task_wdt_reset();
    esp_err_t err = SYS->open(path);
    esp_task_wdt_reset();
    if (err != ESP_OK) {
        // The numeric code, not esp_err_to_name(): the name table is compiled out of this firmware,
        // so every error used to reach the trace as "UNKNOWN ERROR".
        trace("  FAIL: open -> 0x%x", (unsigned)err);
        trace_heap("after open fail");
        free(st->band[0]); free(st->band[1]); st->band[0] = st->band[1] = nullptr;
        nucleo_app_set_direct_draw(false);
        char why[48];
        SYS->explain(err, path, why, sizeof why);
        fail_box(why);
        return;
    }

    // What the cartridge draws decides the scaler: a Game Gear cartridge can turn out to be a Master
    // System game (its CRC says so), known only now. The bands were sized for the widest case.
    SYS->geometry(&s_src_w, &s_src_h);
    // Two screens: the picture goes to the external panel (160x144 at 3/2, 256x192 at 5/4: full screen).
    s_big = ((s_src_w == SRC_W && s_src_h == 144) || (s_src_w == 256 && s_src_h == 192)) && nucleo_surface_large_ok();
    st->diff_ok = false;
    if (s_big && !nucleo_surface_large() && !nucleo_surface_set_large(true)) s_big = false;
    ESP_LOGW("gbemu", "two screens: src %dx%d ok=%d large=%d big=%d", s_src_w, s_src_h, (int)nucleo_surface_large_ok(), (int)nucleo_surface_large(), (int)s_big);
    geom_apply();

    snprintf(st->resume, sizeof st->resume, "%s", name);
    st->have_resume = true;
    state_save(name);
    st->state_ok = false;          // looked up when the menu first opens

    nucleo_gb_stats_t stt; SYS->get_stats(&stt);
    char cache[24];
    if (stt.rom_resident) snprintf(cache, sizeof cache, "resident");
    else                  snprintf(cache, sizeof cache, "%dx1KB-pages", stt.rom_pages);
    trace("  OPEN OK '%s' rom=%uKB cache=%s core-heap=%u", SYS->title(),
          (unsigned)(stt.rom_bytes / 1024), cache, (unsigned)stt.heap_bytes);
    trace_heap("running");
    ESP_LOGI(TAG, "'%s' %uKB %s | core heap %u B | free %u",
             SYS->title(), (unsigned)(stt.rom_bytes / 1024), cache,
             (unsigned)stt.heap_bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    // DFS ranges 80-240 MHz and cannot tell a render loop from an idle launcher; at the floor a frame
    // does not fit. Hold the clock at maximum for exactly as long as a game runs.
    nucleo_power_perf_begin();

    d.fillScreen(BG);
    // BYTE ORDER — handled in the PALETTE, not in the push. The ST7789 latches each pixel
    // most-significant byte first while an ESP32 stores a uint16_t little-endian, so the two bytes
    // have to be swapped somewhere. setSwapBytes(true) does it inside pushImage, per pixel, on 21,600
    // pixels a frame, and it also forces LovyanGFX onto a CONVERTING copy that cannot DMA from our
    // buffer at all. Swapping the four palette entries once (pal_apply -> SHADE_TX) makes the band
    // buffer already panel-order, so the push is a raw no-convert DMA transfer. Leave the flag OFF:
    // it affects only the raw-buffer path, never the colour-argument primitives the HUD uses.
    d.setSwapBytes(false);
    pal_apply();

    st->running = true;
    st->menu = false; st->msel = 0; st->turbo = false;
    st->toast[0] = '\0'; st->toast_until = 0;
    st->us_blit = 0; st->ms_cpu = st->ms_blit = st->ms_aud = 0;
    st->slow_secs = 0;
    st->tw_n = 0; st->tw_fps_sum = st->tw_cpu_sum = st->tw_blit_sum = st->tw_aud_sum = 0; st->tw_miss = 0;
    set_relief(0);                       // every cartridge starts at the full picture and earns relief
    SYS->reset_counters();
    st->fps_frames = 0; st->fps = 0; st->fps_t0 = esp_timer_get_time();
    st->play_t0 = st->fps_t0; st->fps_hn = 0;
    bezel_draw();
    hud_draw();
    chrome_draw();
    card_draw(true);
    uint8_t held = 0;
    int64_t next = esp_timer_get_time();
    const int64_t FRAME_US = SYS->frame_us;             // the console's real frame time (DMG 59.73 Hz, GG 59.92 Hz)

    while (st->running) {
        esp_task_wdt_reset();
        if (st->toast[0] && esp_timer_get_time() > st->toast_until) { st->toast[0] = '\0'; chrome_draw(); card_draw(false); }

        // INPUT — read the keyboard's LIVE pressed set, not its key EVENTS.
        //
        // This is the difference between a game being playable and not. nucleo_kbd_read() reports a
        // printable key exactly once per press and never repeats, so building the button mask from it
        // meant a held direction released itself on the very next frame: the character took one step
        // and stopped, and two buttons could never be down together — no running jump, no diagonal.
        // It read as the emulator being slow. It was not; the D-pad was tapping itself.
        //
        // nucleo_kbd_char_down() exists for exactly this (see nucleo_kbd.h — "games that need a key
        // to act as a hold button"). It is rebuilt from the matrix on every scan, and the scan is
        // driven by the read() below, so both still get called each frame.
        nucleo_key_t k = nucleo_kbd_read();

        uint8_t b = 0;
        // The layout the player asked for: E S A D as a movement diamond (E up, S down, A left,
        // D right), the two thumb buttons on J and K, START on Enter and SELECT on Space. The
        // Cardputer's printed arrow cluster ; . , / stays live in parallel — it costs nothing and it
        // is the one set of keys the hardware itself labels, so a first-time player finds it blind.
        if (dn('e') || dn(';'))  b |= NUCLEO_GB_UP;
        if (dn('s') || dn('.'))  b |= NUCLEO_GB_DOWN;
        if (dn('a') || dn(','))  b |= NUCLEO_GB_LEFT;
        if (dn('d') || dn('/'))  b |= NUCLEO_GB_RIGHT;
        if (dn('k'))             b |= NUCLEO_GB_A;
        if (dn('j'))             b |= NUCLEO_GB_B;
        if (dn('\n'))            b |= NUCLEO_GB_START;    // Enter
        if (dn(' '))             b |= NUCLEO_GB_SELECT;   // Space
        if (st->mute_keys) { if (b) b = 0; else st->mute_keys = false; }
        held = b;

        // TAB held = unpaced. Menus, grinding and long text are what a real player fast-forwards
        // through, and holding a key is the right gesture: release and the game is back at speed.
        st->turbo = dn('\t');

        if (k.key != NK_NONE || k.ch) {
            if (st->menu)                        { menu_key(k); }
            else if (k.key == NK_BACK || k.ch == '`' || k.ch == 'm' || k.ch == 'M') {
                // The menu comes up FIRST, then the SD work: it is a pause, so it is the one moment a
                // write costs the player nothing — flush battery RAM before they can pick Quit or pull
                // the power, and look up whether a state exists. The time is traced: an SD step that
                // outgrew its budget once rebooted the console from exactly this key.
                st->menu = true; st->msel = 0; s_bm_scroll = 0;   // two screens: the menu is a card on the panel
                menu_draw();
                int64_t t0 = esp_timer_get_time();
                SYS->save();
                st->state_ok = SYS->state_exists(0);
                esp_task_wdt_reset();
                int ms = (int)((esp_timer_get_time() - t0) / 1000);
                if (ms > 200) trace("  menu: save+lookup took %d ms", ms);
                menu_draw();
            }
            else if (k.ch == 'p' || k.ch == 'P') pal_cycle();
            // Two things a player changes mid-game without wanting to pause: how loud it is, and
            // whether the pillars are there at all. Both are one key, both echo in the toast.
            else if (k.ch == 'i' || k.ch == 'I') {
                s_hud = !s_hud;
                if (!s_hud) frame_clear(); else { hud_draw(); chrome_draw(); }
            }
            else if (k.ch == '-' || k.ch == '=') {
                vol_step(k.ch == '=' ? +1 : -1);
                char vb[16]; snprintf(vb, sizeof vb, "vol %d%%", nucleo_audio_volume());
                toast(vb);
            }
        }
        if (st->menu) {                          // paused: the console does not advance
            menu_anim();                         // ...but a long selected entry keeps scrolling
            vTaskDelay(pdMS_TO_TICKS(20));
            esp_task_wdt_reset();
            continue;
        }
        SYS->set_buttons(held);

        st->out_y = 0; st->band_line = 0; st->have_stash = false;
        if (s_big) {                             // the on-demand system bar (a tap shows it, 3 s later it goes)
            const int ov = nucleo_extdisp_hud_overlay();
            if (s_ov && !ov) {                   // it went: repaint what it covered, bezel and picture
                bezel_draw();
                st->diff_ok = false;
            }
            s_ov = ov;
        }
        // One SPI transaction for the ENTIRE frame. run_frame calls on_line 135 times and each pushes
        // its band inside this single startWrite/endWrite, so the panel sees one uninterrupted sweep
        // top to bottom instead of nine separately-arbitrated writes.
        d.startWrite();
        SYS->run_frame();
        if (st->band_line) band_push();          // a frame that ended on a partly filled band
        d.endWrite();
        st->diff_ok = s_big;                     // the panel now holds exactly this frame's lines

        st->fps_frames++;
        int64_t now = esp_timer_get_time();
        if (now - st->fps_t0 >= 1000000) {
            st->fps = (int)st->fps_frames;
            st->fps_hist[st->fps_hn++ & 31] = (uint8_t)(st->fps > 255 ? 255 : st->fps);
            // Divide the window's accumulated microseconds by the frames in it, in TENTHS of a ms —
            // a breakdown rounded to whole milliseconds hides exactly the differences worth seeing.
            nucleo_gb_stats_t w; SYS->get_stats(&w);
            uint32_t nf = w.frames ? w.frames : 1;
            st->ms_cpu  = (int)(w.us_cpu   / nf / 100);
            st->ms_aud  = (int)(w.us_audio / nf / 100);
            st->ms_blit = (int)(st->us_blit / nf / 100);
            st->tw_n++;
            st->tw_fps_sum += st->fps;
            if (st->tw_n == 1 || st->fps < st->tw_fps_min) st->tw_fps_min = st->fps;
            st->tw_cpu_sum += st->ms_cpu; st->tw_blit_sum += st->ms_blit; st->tw_aud_sum += st->ms_aud;
            st->tw_miss += w.bank_misses;
            if (st->tw_n >= TRACE_WIN || w.core_errors) {
                int n = st->tw_n;
                trace("  %ds fps avg=%d min=%d cpu=%d.%dms blit=%d.%dms audio=%d.%dms misses=%u relief=%d err=%u@%04X",
                      n, st->tw_fps_sum / n, st->tw_fps_min,
                      st->tw_cpu_sum / n / 10, st->tw_cpu_sum / n % 10,
                      st->tw_blit_sum / n / 10, st->tw_blit_sum / n % 10,
                      st->tw_aud_sum / n / 10, st->tw_aud_sum / n % 10,
                      (unsigned)st->tw_miss, st->relief,
                      (unsigned)w.core_errors, (unsigned)w.core_err_addr);
                st->tw_n = 0; st->tw_fps_sum = st->tw_cpu_sum = st->tw_blit_sum = st->tw_aud_sum = 0;
                st->tw_miss = 0;
            }
            // A core error is an invalid opcode: the game has run off into data and will not come
            // back on its own. Say so once, and point at the way out, instead of leaving a frozen
            // picture that looks like the emulator hung.
            if (w.core_errors && !st->crash_told && !st->menu) {
                st->crash_told = true;
                st->menu = true; st->msel = MI_RESUME;  // paused; Reset is a deliberate choice, never a mashed key
                st->state_ok = SYS->state_exists(0);
                s_bm_scroll = 0;
                menu_draw();
            }
            // Battery RAM reaches the card within ~2 s of an in-game save, not only on quit.
            if (SYS->autosave()) trace("  autosave");
            st->us_blit = 0; SYS->reset_counters();
            st->fps_frames = 0; st->fps_t0 = now;
            hud_draw();
            card_draw(false);

            // AUTOMATIC RELIEF — one-way, and that is the whole point. The earlier version stepped back
            // UP when fps recovered, which turned into a per-second flicker: enabling frame-skip halves
            // the blit work, so the measured fps jumps back over the "recover" line, relief switches
            // off, fps drops under the "engage" line, and it toggles forever. The metric feeds back
            // into the thing it measures. So relief only ever engages, and only after two consecutive
            // slow seconds (a brief dip does not trip it); the player raises it again from the menu when
            // they want to. No automatic step-up means no oscillation, which means no flicker.
            if (st->relief == 0) {
                if (st->fps < 50) { if (++st->slow_secs >= 2) { set_relief(1); st->slow_secs = 0; } }
                else st->slow_secs = 0;
            }
        }

        // Pace to real Game Boy speed. FreeRTOS runs at 100 Hz here, so ONE tick is 10 ms against a
        // 16.7 ms frame: sleeping for "the remaining milliseconds" quantises to 0 or 10+ and jitters
        // the frame by more than half its budget. Sleep only whole ticks that comfortably fit, then
        // spin out the remainder on the microsecond timer — the spin is bounded by one tick, this
        // loop owns the CPU, and the watchdog is fed inside it.
        // A frame that overran is NOT chased: catching up only compounds into stutter, and running a
        // touch slow reads better than jerking.
        // The sleep threshold must be ONE tick plus margin, not two. At two (20 ms) it could never be
        // reached — a whole frame is 16.75 ms — so the loop never slept, spun the core at 100% for the
        // entire idle remainder, and starved the IDLE task that feeds the watchdog. One tick is 10 ms,
        // so 12 ms of slack safely absorbs a 10 ms sleep and leaves the rest to the spin.
        static const int64_t SLEEP_MIN = (int64_t)portTICK_PERIOD_MS * 1000 + 2000;
        next += FRAME_US;
        if (st->turbo) {
            // Unpaced — but never for free. Running frames back to back yields the CPU to nothing,
            // and the IDLE task is what feeds the system watchdog; starving it is how a fast-forward
            // turns into a reboot. One tick every sixteen frames costs about 4% of the speed-up and
            // keeps the scheduler honest.
            static int spin = 0;
            if (++spin >= 16) { spin = 0; vTaskDelay(1); }
            next = esp_timer_get_time();
            continue;
        }
        for (;;) {
            int64_t slack = next - esp_timer_get_time();
            if (slack <= 200) break;                                   // close enough: start the next frame
            if (slack > SLEEP_MIN) vTaskDelay(1);                      // give the core back; IDLE runs
            esp_task_wdt_reset();                                      // ...and feed the dog either way
        }
        if (esp_timer_get_time() - next > FRAME_US * 4) next = esp_timer_get_time();   // far behind: resync
    }

    nucleo_power_perf_end();
    // One SD write per SESSION, on the way out: the in-game hotkeys change these live but must never
    // touch the card mid-frame (an SD write inside the 16.7 ms budget is a visible stutter).
    state_save(st->resume);
    nucleo_prefs_save(nucleo_app_brightness(), nucleo_audio_volume(), nucleo_audio_is_muted());
    trace("  END fps=%d palette=%s", st->fps, PAL_NAME[s_pal]);
    SYS->close();
    free(st->band[0]); free(st->band[1]); st->band[0] = st->band[1] = nullptr;
    d.setSwapBytes(false);                 // leave the shared display as every other app expects it
    if (s_big) { nucleo_app_set_gfx(nullptr); s_big = false; nucleo_surface_set_large(false); geom_apply(); }
    nucleo_app_set_direct_draw(false);
    nucleo_app_force_repaint();
}

// ── the shelf ───────────────────────────────────────────────────────────────────────────────────
#define ROW_H 22

// Titles read better without the extension or the region/dump tags every set carries.
static void pretty(const char *file, char *out, size_t n)
{
    char tmp[PATHMAX];
    snprintf(tmp, sizeof tmp, "%s", file);
    char *dot = strrchr(tmp, '.'); if (dot) *dot = '\0';
    char *cut = strstr(tmp, " (");  if (cut) *cut = '\0';
    cut = strstr(tmp, " [");        if (cut) *cut = '\0';
    snprintf(out, n, "%s", tmp);
}

static void draw_companion(void);
#ifdef NUCLEO_UI_HOST
// ui-host gallery scenes for what only a running game shows: 1 = playing at 3/2 (the companion, the bezel),
// 2 = the pause menu on the panel, 3 = playing filled. A fake session: the selected cartridge's name, 58 fps
// with a dip in the sparkline, 12:34 played, the picture a pattern where the game would be.
extern "C" void ui_host_scene(int n)
{
    s_host_view = n;
    s_big = true; s_src_w = SRC_W; s_src_h = 144; s_stretch = n == 3;
    geom_apply();
    snprintf(st->resume, sizeof st->resume, "%s", st->n ? ent_name(st->sel) : "Game");
    st->fps = 58; st->fps_hn = 32;
    for (int i = 0; i < 32; i++) st->fps_hist[i] = (uint8_t)(i > 20 && i < 24 ? 44 : 59);
    st->play_t0 = esp_timer_get_time() - 754LL * 1000000;
    st->menu = n == 2; st->msel = 3;
}
static void host_view_large(void)
{
    bezel_draw();
    for (int y = 0; y < s_out_h; y += 8)               // a stand-in picture: bands where the game would be
        d.fillRect(s_out_x, s_out_y0 + y, s_out_w, 8, (y / 8) & 1 ? 0x6D2B : 0x8E2E);
    if (st->menu) menu_draw_big();
}
#endif
static void draw(void)
{
#ifdef NUCLEO_UI_HOST
    if (s_host_view) { bool m = st->menu; st->menu = false; card_draw(true); st->menu = m; return; }
#endif
    if (!st) return;
    if (!st->sh) shelf_swap_in();                // a failed swap-in retries on the next repaint
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int ch = nucleo_app_content_height(), top = nucleo_app_content_top();
    d.fillRect(0, top, 240, ch, BG);

    // ── header: glyph + system, and either the live filter or the counts ──
    int hx = 4 + icon_draw(&d, 4, top, &SYS->icon_a);
    hx += 3 + icon_draw(&d, hx + 3, top, &SYS->icon_b);
    if (st->flen) {
        d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(hx + 8, top + 3);
        char q[FILTMAX + 2]; snprintf(q, sizeof q, "%s_", st->filter);
        d.print(q);
    }
    char cnt[24];
    if (st->total > st->n) snprintf(cnt, sizeof cnt, "%d/%d", st->n, st->total);
    else                   snprintf(cnt, sizeof cnt, "%d", st->total);
    int cw = (int)strlen(cnt) * 6;
    d.setTextSize(1); d.setTextColor(st->total > st->n ? AMB : MUTED, BG);
    d.setCursor(234 - cw, top + 8); d.print(cnt);
    d.drawFastHLine(6, top + 21, 228, LINE);

    if (st->n == 0) {
        st->sel_title[0] = '\0';
        d.setTextSize(1); d.setTextColor(MUTED, BG);
        d.setCursor(8, top + 34);
        d.print(st->flen ? TR5("Nessun risultato", "No match", "Nada", "Rien", "Nichts")
                         : TR5(SYS->empty_it, SYS->empty_en, SYS->empty_es, SYS->empty_fr, SYS->empty_de));
        if (st->flen) { d.setCursor(8, top + 48); d.setTextColor(DIM, BG); d.print(TR5("Canc per correggere", "Backspace to edit", "Retroceso editar", "Retour pour modifier", "Zurueck bearbeiten")); }
        fail_draw();
        return;
    }

    int y0 = top + 25, rows = (ch - 27) / ROW_H;
    if (st->sel < st->scroll) st->scroll = st->sel;
    if (st->sel >= st->scroll + rows) st->scroll = st->sel - rows + 1;

    for (int i = 0; i < rows && st->scroll + i < st->n; i++) {
        int idx = st->scroll + i, y = y0 + i * ROW_H;
        bool foc = (idx == st->sel);
        if (foc) d.fillRoundRect(4, y, 232, ROW_H - 2, 7, ACC);

        // 1-9 quick pick: the visible row number, so a game is one keypress away without arrowing.
        if (i < 9) {
            d.setTextSize(1); d.setTextColor(foc ? INK : DIM, foc ? ACC : BG);
            d.setCursor(9, y + (ROW_H - 8) / 2); char nb[3]; snprintf(nb, sizeof nb, "%d", i + 1); d.print(nb);
        }

        char title[NAMEMAX];
        pretty(ent_name(idx), title, sizeof title);
        // The badges are drawn first so the title can be clipped to whatever is left, never overlap.
        int right = 232;
        if (st->sh->list[idx].alt) {           // a Color / Master System cartridge: its console's mark, not a letter
            const int iw = SYS->icon_b.w + 8;
            icon_draw(&d, right - iw - 2, y, &SYS->icon_b); right -= iw + 6;
        }
        if (st->have_resume && !strcmp(ent_name(idx), st->resume)) {
            d.fillCircle(right - 6, y + ROW_H / 2 - 1, 3, foc ? INK : GRN); right -= 14;
        }
        int avail = right - 20;
        int ty = y + (ROW_H - 16) / 2;
        d.setTextSize(2); d.setTextColor(foc ? INK : FG, foc ? ACC : BG);
        if (foc) {
            // Record what the 50 Hz poll needs to scroll this title in its own sprite; draw it CLIPPED
            // and static here so a title that fits is complete, and one that overflows has a correct
            // frame underneath for the instant before the poll's first overlay lands.
            st->sel_y = ty; st->sel_avail = avail;
            snprintf(st->sel_title, sizeof st->sel_title, "%s", title);
            d.setClipRect(20, ty, avail, 16);
            d.setCursor(20, ty); d.print(title);
            d.clearClipRect();
        } else {
            int maxc = avail / 12; if (maxc < 1) maxc = 1; if (maxc > 17) maxc = 17;
            char shown[20]; snprintf(shown, sizeof shown, "%.*s", maxc, title);
            d.setCursor(20, ty); d.print(shown);
        }
    }

    // Scroll rail: on a shelf this deep, "where am I" has to be visible without counting rows.
    if (st->n > rows) {
        int rh = ch - 27, kh = rh * rows / st->n; if (kh < 8) kh = 8;
        int ky = y0 + (rh - kh) * st->sel / (st->n - 1);
        d.fillRect(237, y0, 2, rh, LINE);
        d.fillRect(237, ky, 2, kh, ACC);
    }
    fail_draw();
}

static void hint(void)
{
    nucleo_app_set_hint(st && st->flen
        ? TR5("Invio gioca  Canc modif  Esc pulisce",  "Enter plays  Bksp edits  Esc clears", "Intro juega/Bksp edita/Esc limpia", "Entree joue/Ret. edite/Esc efface", "Enter spielt/Bearb./Esc loescht")
        : TR5("Scrivi cerca  1-9  Invio gioca",       "Type to search  1-9  Enter plays", "Escribe buscar/1-9/Intro juega", "Tapez chercher/1-9/Entree joue", "Tippen Suchen/1-9/Enter spielt"));
}

// ── two screens: the gallery ────────────────────────────────────────────────────────────────────
// With the external panel attached the shelf becomes a small HyperSpin-style gallery there: a circular wheel of
// titles on the right (the selection in the middle, its neighbours fading), the game's own picture and its whole
// title on the left, the live search on top. Typing filters exactly as on the Cardputer; a drag turns the wheel,
// a tap brings a title to the middle, a tap on the middle one (or on the picture) plays it. The Cardputer keeps
// the search, the position and the console's controls (the companion). Composed with lg::frame: the panel never
// shows a half-painted wheel, and a Fn+P screenshot can capture it.
#define WHEEL_SLOTS 7                        // visible titles; the middle one is the selection
#define WHEEL_X     168                      // the wheel's left edge; the picture column is [0, WHEEL_X)
static const uint8_t WHEEL_H[WHEEL_SLOTS] = { 21, 23, 25, 32, 25, 23, 21 };
#define PIC_H       108                      // the picture's tile on the left
// A game's own picture: <rom folder>/.snaps/<rom file>.snp, put there by tools/fetch-rom-snaps.py (the device never
// downloads anything). "SNP1", u16 w, u16 h, u32 deflated size, then raw-deflated RGB565 in native byte order.
#define SNAP_MAXW   128
#define SNAP_MAXH   112
#define SNAP_MAXZ   40000
static int s_gal_h;                          // the surface height (240 built in; 210 under a store app's system bar)
// The wheel turns, it does not jump: a new selection slides in from the side it came from through the fixed
// pill, ~150 ms with an ease-out, and ticks as it lands (HyperSpin's wheel). Drawn by the gallery's poll.
#define WHEEL_ANIM_US 150000
static int     s_wheel_last = -1;            // the selection the wheel last showed (-1 = none yet)
static int     s_wheel_dir;                  // +1: the titles move up (the next one comes from below), -1: down
static int64_t s_wheel_t0;                   // when the turn started (0 = still)
static int     s_wheel_off;                  // this frame's slide, read ONCE per frame (lg::frame paints in strips)
static int64_t s_wheel_t0_peek(void) { return s_wheel_t0; }
static int wheel_offset(void)
{
    if (!s_wheel_t0) return 0;
    const int64_t t = esp_timer_get_time() - s_wheel_t0;
    if (t >= WHEEL_ANIM_US) { s_wheel_t0 = 0; return 0; }
    const int q = (int)(t * 256 / WHEEL_ANIM_US), rest = 256 - q;          // ease-out: (1 - p)^2 of the step
    return s_wheel_dir * WHEEL_H[WHEEL_SLOTS / 2] * rest * rest / 65536;
}
// The selection's picture, kept while the selection stays (a repaint, a typed letter that keeps it) and freed
// before a game starts: the card is read and the picture inflated once per selection, not once per paint.
static uint16_t *s_pic;
static int  s_pic_w, s_pic_h;
static bool s_pic_snap;                      // the game's screenshot (else the console's photo)
static char s_pic_key[PATHMAX];              // whose picture it is ("" = none tried yet)

static const EmuArt *art_for(int i) { return st->sh->list[i].alt ? &SYS->art_b : &SYS->art_a; }

// The title in wheel slot k (-1 = none). With more titles than slots the wheel is circular, as in HyperSpin:
// past the last title comes the first again, so the wheel is always full.
static int wheel_at(int k)
{
    const int i = st->sel + k - WHEEL_SLOTS / 2;
    if (st->n > WHEEL_SLOTS) return ((i % st->n) + st->n) % st->n;
    return i >= 0 && i < st->n ? i : -1;
}

// Raw deflate -> w x h RGB565 in a fresh block (the ~11 KB tinfl state lives only for the call). Null on failure.
static uint16_t *inflate565(const uint8_t *zb, size_t n, int w, int h)
{
    uint16_t *px = (uint16_t *)malloc((size_t)w * h * 2);
    tinfl_decompressor *z = (tinfl_decompressor *)malloc(sizeof *z);
    size_t in = n, out = (size_t)w * h * 2;
    bool ok = px && z;
    if (ok) {
        tinfl_init(z);
        ok = tinfl_decompress(z, zb, &in, (mz_uint8 *)px, (mz_uint8 *)px, &out, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) == TINFL_STATUS_DONE
             && out == (size_t)w * h * 2;
    }
    free(z);
    if (!ok) { free(px); px = nullptr; }
    return px;
}

static void pic_free(void) { free(s_pic); s_pic = nullptr; s_pic_key[0] = '\0'; }

static uint16_t *snap_load(const char *name, int *w, int *h)
{
    char path[PATHMAX + 40];
    if (!rom_path(name, path, sizeof path - 20)) return nullptr;
    char *slash = strrchr(path, '/');
    if (!slash) return nullptr;
    char snap[sizeof path + 32];
    snprintf(snap, sizeof snap, "%.*s/.snaps/%s.snp", (int)(slash - path), path, name);
    FILE *f = fopen(snap, "rb");
    if (!f) return nullptr;
    uint8_t hd[12];
    uint16_t *px = nullptr;
    if (fread(hd, 1, sizeof hd, f) == sizeof hd && !memcmp(hd, "SNP1", 4)) {
        const int sw = hd[4] | hd[5] << 8, sh = hd[6] | hd[7] << 8;
        const uint32_t zn = hd[8] | hd[9] << 8 | (uint32_t)hd[10] << 16 | (uint32_t)hd[11] << 24;
        uint8_t *zb = (sw > 0 && sh > 0 && sw <= SNAP_MAXW && sh <= SNAP_MAXH && zn && zn <= SNAP_MAXZ) ? (uint8_t *)malloc(zn) : nullptr;
        if (zb && fread(zb, 1, zn, f) == zn) { px = inflate565(zb, zn, sw, sh); *w = sw; *h = sh; }
        free(zb);
    }
    fclose(f);
    return px;
}

static void pic_for(int i)
{
    const char *name = ent_name(i);
    if (s_pic_key[0] && !strcmp(s_pic_key, name)) return;
    pic_free();
    snprintf(s_pic_key, sizeof s_pic_key, "%s", name);
    s_pic = snap_load(name, &s_pic_w, &s_pic_h);
    s_pic_snap = s_pic != nullptr;
    if (!s_pic) {
        const EmuArt *a = art_for(i);
        s_pic = inflate565(a->z, a->n, a->w, a->h);
        s_pic_w = a->w; s_pic_h = a->h;
    }
}

// The search line: what was typed, with a cursor, or how to start; the position at the right.
static void gal_search(int y, int w)
{
    const unsigned short cb = lg::card(), ic = st->flen ? ACC : MUTED;
    d.fillRect(0, y, w, 36, BG);
    d.fillRoundRect(8, y + 4, w - 16, 28, 14, cb);
    d.drawCircle(24, y + 16, 6, ic); d.drawCircle(24, y + 16, 5, ic);
    d.drawLine(28, y + 20, 32, y + 24, ic); d.drawLine(29, y + 20, 33, y + 24, ic);
    char cnt[24];
    if (!st->n)                    snprintf(cnt, sizeof cnt, "0");
    else if (st->total > st->n)    snprintf(cnt, sizeof cnt, "%d/%d+", st->sel + 1, st->n);
    else                           snprintf(cnt, sizeof cnt, "%d/%d", st->sel + 1, st->n);
    lg::font(0); d.setTextDatum(middle_right); d.setTextColor(st->total > st->n ? AMB : MUTED, cb);
    d.drawString(cnt, w - 22, y + 18);
    const int cw = d.textWidth(cnt);
    d.setTextDatum(middle_left);
    if (st->flen) {
        const char *t = st->filter;
        while (*t && d.textWidth(t) > w - 86 - cw) t++;
        d.setTextColor(FG, cb); d.drawString(t, 42, y + 18);
        d.fillRect(43 + d.textWidth(t), y + 10, 2, 17, ACC);
    } else {
        d.setTextColor(MUTED, cb);
        d.drawString(TR5("Scrivi per cercare", "Type to search", "Escribe para buscar", "Tapez pour chercher", "Tippen zum Suchen"), 42, y + 18);
    }
    d.setTextDatum(top_left);
}

static void gal_paint(void *)
{
    const int W = d.width(), H = s_gal_h;
    int y = 0;
#ifndef NAP_MODULE
    if (H >= 240) y = lg::header(SYS->name, nullptr, ACC, NK_BACK);   // built in: the app draws its own Back
#endif                                                                 // (a store module: the OS bar, on a tap)
    gal_search(y, W);
    y += 36;
    const int top = y, bh = H - top;
    d.fillRect(0, top, W, bh, BG);
    if (st->n == 0) {
        lg::message(0, top, W, bh, st->flen ? TR5("Nessun risultato", "No match", "Nada", "Rien", "Nichts")
                                            : TR5(SYS->empty_it, SYS->empty_en, SYS->empty_es, SYS->empty_fr, SYS->empty_de),
                    st->flen ? TR5("Canc per correggere", "Backspace to edit", "Retroceso editar", "Retour pour modifier", "Zurueck bearbeiten") : "",
                    MUTED);
        return;
    }
    // ── left: the picture on its tile (a screenshot sits on a dark screen, a console photo on a light card),
    //    then the system with the resume mark, then the whole title ──
    const int sel = st->sel, tx = 8, tw = WHEEL_X - 16;
    d.fillRoundRect(tx, top + 2, tw, PIC_H, 10, s_pic_snap ? (unsigned short)0x0841 : (unsigned short)ART_TILE);
    if (s_pic) {
        const int px = tx + (tw - s_pic_w) / 2, py = top + 2 + (PIC_H - s_pic_h) / 2;
        d.pushImage(px, py, s_pic_w, s_pic_h, (const lgfx::rgb565_t *)s_pic);
        if (s_pic_snap) d.drawRect(px - 1, py - 1, s_pic_w + 2, s_pic_h + 2, LINE);
    }
    if (s_pic_snap) {                                   // a screenshot: which console, as its mark in the corner
        const EmuArt *ic = st->sh->list[sel].alt ? &SYS->icon_b : &SYS->icon_a;
        icon_draw(&d, tx + tw - ic->w - 12, top + PIC_H - 20, ic);
    }
    int ly = top + PIC_H + 8;
    lg::font(0);
    if (st->have_resume && !strcmp(ent_name(sel), st->resume)) {
        const char *lp = TR5("ultima", "last", "ultima", "derniere", "zuletzt");
        d.setTextDatum(top_right); d.setTextColor(GRN, BG);
        d.drawString(lp, tx + tw, ly);
        d.fillCircle(tx + tw - d.textWidth(lp) - 7, ly + 8, 3, GRN);
        d.setTextDatum(top_left);
    }
    ly += 20;
    char title[NAMEMAX];
    pretty(ent_name(sel), title, sizeof title);
    lg::font(1); d.setTextColor(FG, BG);
    lg::wrap(title, tx + 2, ly, tw - 4, 20, H - ly >= 60 ? 3 : 2);
    // ── right: the wheel. Slot k shows the title k - 3 away; the middle one is the selection, on its pill ──
    const int ww = W - WHEEL_X - 6;
    int sum = 0; for (int k = 0; k < WHEEL_SLOTS; k++) sum += WHEEL_H[k];
    const int wy0 = top + (bh - sum) / 2, off = s_wheel_off;
    int my = wy0; for (int k = 0; k < WHEEL_SLOTS / 2; k++) my += WHEEL_H[k];
    const int mh = WHEEL_H[WHEEL_SLOTS / 2];
    d.fillRoundRect(WHEEL_X, my + 1, ww, mh - 2, (mh - 2) / 2, ACC);  // the selection's pill: fixed, the titles move
    int wy = wy0;
    for (int k = 0; k < WHEEL_SLOTS; k++) {
        const int i = wheel_at(k), h = WHEEL_H[k], dist = abs(k - WHEEL_SLOTS / 2), ty = wy + off;
        if (i >= 0 && ty >= top && ty + h <= H) {                  // a row sliding out of the wheel is not drawn
            char t[NAMEMAX], fitted[NAMEMAX];
            pretty(ent_name(i), t, sizeof t);
            const bool mid = dist == 0;
            const int ix = WHEEL_X + 12 + dist * 5;                        // the wheel's curve: far rows step in
            lg::font(mid ? 1 : 0);                                         // 16 px everywhere: readable at arm's length
            lg::fit(fitted, sizeof fitted, t, WHEEL_X + ww - 8 - ix);
            d.setTextDatum(middle_left);
            // No text background: a title sliding across the pill's edge must not drag a box of the wrong colour.
            const bool on_pill = ty + h / 2 > my && ty + h / 2 < my + mh;
            d.setTextColor(on_pill ? INK : dist <= 1 ? FG : MUTED);
            d.drawString(fitted, ix, ty + h / 2 + (mid ? 0 : 1));
            d.setTextDatum(top_left);
            if (st->have_resume && !strcmp(ent_name(i), st->resume))
                d.fillCircle(ix - 6, ty + h / 2, 2, on_pill ? INK : GRN);
        }
        if (i >= 0) nucleo_touch_add(WHEEL_X, wy, ww, h, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + k));
        wy += h;
    }
    // A drag on the wheel turns it, one title per 22 px of finger travel (finger up = later titles).
    nucleo_touch_add_scroll(WHEEL_X, top, W - WHEEL_X, bh, 22, NK_UP, 0, NK_DOWN, 0);
    nucleo_touch_add(tx, top + 2, tw, PIC_H, NK_ENTER, 0);              // the picture plays the selection too
    if (st->n > 1) {                                                    // where am I in a long shelf: a thin rail
        int kh = bh * WHEEL_SLOTS / st->n; if (kh < 10) kh = 10; if (kh > bh) kh = bh;
        const int ky = top + (bh - kh) * sel / (st->n - 1);
        d.fillRect(W - 3, top, 2, bh, LINE);
        d.fillRect(W - 3, ky, 2, kh, ACC);
    }
}

static void draw_large(void)
{
    if (!st) return;
#ifdef NUCLEO_UI_HOST
    if (s_host_view) { host_view_large(); return; }
#endif
    if (!st->sh) shelf_swap_in();
    if (!st->sh) return;
    s_gal_h = d.height();
    if (s_wheel_last >= 0 && st->sel != s_wheel_last && st->n > 1) {
        int dlt = st->sel - s_wheel_last;
        if (st->n > WHEEL_SLOTS && abs(dlt) > st->n / 2) dlt = dlt > 0 ? dlt - st->n : dlt + st->n;   // round the ring
        s_wheel_dir = dlt > 0 ? 1 : -1;
        s_wheel_t0 = esp_timer_get_time();
        nucleo_audio_tone(2600, 4, 12);                             // the wheel's click
    }
    s_wheel_last = st->sel;
    s_wheel_off = wheel_offset();
    if (st->n) pic_for(st->sel);
    nucleo_touch_clear();
    lg::frame(gal_paint, nullptr, 0, 0, true);    // exact: the picture and the theme's cards in true colour
    if (st->fail[0])                               // a refused cartridge says why, over the gallery
        lg::message(40, s_gal_h / 2 - 34, d.width() - 80, 68,
                    TR5("Avvio non riuscito", "Could not start", "No se pudo iniciar", "Pas possible", "Nicht starten"), st->fail, AMB);
    nucleo_touch_commit();
}

// What the card knows about a cartridge — its size, whether it has battery RAM saved and a save state — looked
// up once per selection (a few stat()s on the card), never per repaint.
struct GameFacts { char key[PATHMAX]; uint32_t kb; bool sav, state; };
static GameFacts s_facts;
static const GameFacts *facts_for(const char *name)
{
    if (!strcmp(s_facts.key, name)) return &s_facts;
    snprintf(s_facts.key, sizeof s_facts.key, "%s", name);
    s_facts.kb = 0; s_facts.sav = s_facts.state = false;
    char path[PATHMAX + 40];
    struct stat sb;
    if (!rom_path(name, path, sizeof path)) return &s_facts;
    if (stat(path, &sb) == 0) s_facts.kb = (uint32_t)(sb.st_size / 1024);
    // The cores keep saves at "<root>/Saves/<sys>/<name>" for "<root>/ROMs/<sys>/<name>.<ext>" (save_base_init).
    const char *roms = strstr(path, "/ROMs/"), *file = strrchr(path, '/'), *dot = strrchr(path, '.');
    if (!roms || !file || roms >= file) return &s_facts;
    char base[PATHMAX + 60];
    snprintf(base, sizeof base, "%.*s/Saves/%.*s/%.*s", (int)(roms - path), path, (int)(file - roms - 6), roms + 6,
             (int)((dot && dot > file ? dot : path + strlen(path)) - file - 1), file + 1);
    char p2[sizeof base + 8];
    snprintf(p2, sizeof p2, "%s.sav", base); s_facts.sav = stat(p2, &sb) == 0 && sb.st_size > 0;
    snprintf(p2, sizeof p2, "%s.st0", base); s_facts.state = stat(p2, &sb) == 0 && sb.st_size > 0;
    return &s_facts;
}

// The Cardputer while the gallery is on the panel: the search, big; the selection's title, where it is in the
// list and what it has (size, battery save, save state); the console's buttons on this keyboard. What the panel
// already shows (the wheel, the picture) is not repeated here.
static void draw_companion(void)
{
    int top = nucleo_app_content_top(), ch = nucleo_app_content_height();
    st->sel_title[0] = '\0';                      // no marquee down here: the title is wrapped instead
    d.fillRect(0, top, 240, ch, BG);
    // The header: the shelf's consoles as their marks, the position at the right (no names written).
    int hx = 4 + icon_draw(&d, 4, top + 1, &SYS->icon_a);
    icon_draw(&d, hx + 3, top + 1, &SYS->icon_b);
    int y = top + 24;
    // The search field: a rounded well with a lens, the query in the kit's 16 px face and a cursor (or the hint).
    const unsigned short cb = lg::card();
    d.fillRoundRect(4, y, 196, 22, 11, cb);
    d.drawCircle(15, y + 10, 4, st->flen ? ACC : MUTED); d.drawLine(18, y + 13, 21, y + 16, st->flen ? ACC : MUTED);
    lg::font(0); d.setTextDatum(middle_left);
    if (st->flen) {
        const char *q = st->filter;
        while (*q && d.textWidth(q) > 160) q++;
        d.setTextColor(FG, cb); d.drawString(q, 27, y + 11);
        d.fillRect(28 + (int)d.textWidth(q), y + 4, 2, 14, ACC);
    } else {
        d.setTextColor(MUTED, cb);
        d.drawString(TR5("Scrivi e cerca", "Type to search", "Escribe y busca", "Tapez, cherchez", "Tippen = Suche"), 27, y + 11);
    }
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (st->sh && st->n) {
        char pos[16]; snprintf(pos, sizeof pos, "%d/%d", st->sel + 1, st->n);
        d.setTextDatum(top_right); d.setTextColor(MUTED, BG); d.drawString(pos, 234, top + 7); d.setTextDatum(top_left);
        char title[NAMEMAX]; pretty(ent_name(st->sel), title, sizeof title);
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(ACC, BG);
        const int lines = lg::wrap(title, 8, y + 28, 224, 19, 2);
        d.setFont(&fonts::Font0);
        // The facts line: the console, the size, and two dots — green when there is a save / a state.
        const GameFacts *f = facts_for(ent_name(st->sel));
        const int fy = y + 30 + lines * 19;
        char sz[24];
        if (f->kb >= 1024) snprintf(sz, sizeof sz, "%u.%u MB", (unsigned)(f->kb / 1024), (unsigned)(f->kb % 1024 * 10 / 1024));
        else               snprintf(sz, sizeof sz, "%u KB", (unsigned)f->kb);
        const int iw = icon_draw(&d, 6, fy - 6, st->sh->list[st->sel].alt ? &SYS->icon_b : &SYS->icon_a);
        d.setTextColor(MUTED, BG); d.setCursor(iw + 12, fy); d.print(sz);
        int fx = iw + 12 + (int)d.textWidth(sz) + 10;
        d.fillCircle(fx + 3, fy + 3, 3, f->sav ? GRN : LINE);
        d.setTextColor(f->sav ? FG : DIM, BG); d.setCursor(fx + 9, fy);
        d.print(TR5("salvataggio", "save", "partida", "sauvegarde", "Spielstand"));
        fx += 9 + (int)d.textWidth(TR5("salvataggio", "save", "partida", "sauvegarde", "Spielstand")) + 8;
        d.fillCircle(fx + 3, fy + 3, 3, f->state ? GRN : LINE);
        d.setTextColor(f->state ? FG : DIM, BG); d.setCursor(fx + 9, fy);
        d.print(TR5("stato", "state", "estado", "etat", "Zustand"));
    }
    // The console's buttons on this keyboard, for the game about to start: the same key caps as in the game.
    d.fillRect(0, top + ch - 30, 240, 16, BG);
    int kx = keycap(&d, 4, top + ch - 29, "K", !strcmp(SYS->id, "gbemu") ? "A" : "2");
    kx = keycap(&d, kx, top + ch - 29, "J", !strcmp(SYS->id, "gbemu") ? "B" : "1");
    kx = keycap(&d, kx, top + ch - 29, TR5("Invio", "Enter", "Intro", "Entree", "Enter"), "START");
    if (!strcmp(SYS->id, "gbemu")) keycap(&d, kx, top + ch - 29, TR5("Spazio", "Space", "Esp.", "Esp.", "Leer"), "SEL");
    lg::comp_keys(st->flen ? TR5("INVIO gioca  CANC modif.  ESC pulisce", "ENTER play  DEL edit  ESC clear", "INTRO juega  ESC limpia", "ENTREE joue  ESC efface", "ENTER spielt  ESC leert")
                           : TR5("su/giu  INVIO gioca  scrivi = cerca", "up/dn  ENTER play  type = search", "arr/ab  INTRO juega", "haut/bas  ENTREE joue", "auf/ab  ENTER spielt"));
}

static void on_key(int key, char ch)
{
    if (!st) return;
    if (!st->sh) { shelf_swap_in(); nucleo_app_request_draw(); if (!st->sh) return; }
    if (st->fail[0]) { st->fail[0] = '\0'; nucleo_app_request_draw(); }   // any key dismisses the failure box

    // The gallery's wheel: slot k is the title k - 3 away from the selection; tapping the middle one plays it.
    if (nucleo_touch_is_row(key, ch)) {
        const int k = ch - NUCLEO_TOUCH_ROW, i = k < WHEEL_SLOTS ? wheel_at(k) : -1;
        if (k == WHEEL_SLOTS / 2) { if (st->n) { play(ent_name(st->sel)); hint(); } }
        else if (i >= 0) st->sel = i;
        nucleo_app_request_draw();
        return;
    }

    // On the gallery's circular wheel the ends meet; the compact list stops at them.
    const bool ring = nucleo_app_two_screens() && st->n > WHEEL_SLOTS;
    if (key == NK_UP)    { if (st->sel > 0) st->sel--; else if (ring) st->sel = st->n - 1; nucleo_app_request_draw(); return; }
    if (key == NK_DOWN)  { if (st->sel < st->n - 1) st->sel++; else if (ring) st->sel = 0; nucleo_app_request_draw(); return; }
    // NB: NK_LEFT and NK_BACK never arrive here — the framework routes both to the back handler
    // (see on_back). Only NK_RIGHT pages forward from on_key.
    if (key == NK_RIGHT) { st->sel += 5; if (st->sel > st->n - 1) st->sel = st->n - 1; nucleo_app_request_draw(); return; }

    if (key == NK_ENTER) {
        if (st->n) { play(ent_name(st->sel)); hint(); nucleo_app_request_draw(); }
        return;
    }

    // Backspace edits the filter one character at a time. (NK_BACK itself never reaches on_key —
    // the framework routes it to the back handler, which clears the whole search; see on_back.)
    if (key == NK_DEL || ch == 8 || ch == 127) {
        if (st->flen) { st->filter[--st->flen] = '\0'; rescan(); hint(); nucleo_app_request_draw(); }
        return;
    }

    // Digits are quick-pick, not filter input: on a keyboard device the fastest path to a visible
    // row is its number. Titles starting with a digit are still reachable by typing more letters.
    if (ch >= '1' && ch <= '9') {
        // On two screens the digits count the wheel's slots from its top, the selection being the 4th.
        int want = nucleo_app_two_screens() ? (ch - '1' < WHEEL_SLOTS ? wheel_at(ch - '1') : -1) : st->scroll + (ch - '1');
        if (want < 0) return;
        if (want < st->n) { st->sel = want; play(ent_name(want)); hint(); nucleo_app_request_draw(); }
        return;
    }

    // Anything else printable extends the filter. This IS the navigation model for ~4,700 carts.
    if (ch >= 32 && ch < 127 && st->flen < FILTMAX) {
        st->filter[st->flen++] = ch;
        st->filter[st->flen] = '\0';
        rescan(); hint(); nucleo_app_request_draw();
        return;
    }
}

// Esc / Left: clear the filter first, and only let the framework close the app when the shelf is
// already unfiltered — a mistyped search must never throw you out of the library.
static bool on_back(int key)
{
    if (!st) return false;
    if (st->fail[0]) { st->fail[0] = '\0'; nucleo_app_request_draw(); return true; }
    if (key == NK_LEFT) {                          // page back through a deep shelf
        st->sel -= 5; if (st->sel < 0) st->sel = 0;
        nucleo_app_request_draw();
        return true;
    }
    if (st->flen) {                                // Esc clears the search before it closes anything
        st->filter[0] = '\0'; st->flen = 0;
        rescan(); hint(); nucleo_app_request_draw();
        return true;
    }
    return false;                                  // unfiltered shelf: let the framework close the app
}

// ── the two consoles ────────────────────────────────────────────────────────────────────────────
static esp_err_t gb_open(const char *p)  { return nucleo_gb_open(p, on_line, nullptr); }
static esp_err_t gb_probe(const char *p) { nucleo_gb_info_t i; return nucleo_gb_probe(p, &i); }
static void gb_geometry(int *w, int *h) { *w = NUCLEO_GB_W; *h = NUCLEO_GB_H; }
static void gb_explain(esp_err_t e, const char *path, char *buf, size_t n)
{
    nucleo_gb_info_t inf;
    nucleo_gb_probe(path, &inf);
    start_error(e, nucleo_gb_cart_name(inf.cart_type), buf, n);
}
static const EmuSys *gb_pick(const char *name);
static const EmuSys SYS_GB = {
    "gbemu", "Game Boy", "GB", DIR_GB, DIR_GBC, ".gb", ".gbc", "C",
    "Nessuna ROM in /data/ROMs/gb", "No ROMs in /data/ROMs/gb", "Ninguna ROM en /data/ROMs/gb", "Aucune ROM dans /data/ROMs/gb", "Keine ROMs in /data/ROMs/gb",
    NUCLEO_SD_MOUNT "/system/config/gbemu.json", NUCLEO_SD_MOUNT "/system/config/gbemu.shelf",
    NUCLEO_SD_MOUNT "/gbemu_trace.txt", "/gbemu_trace.txt",
    16743,                                   // 59.727 Hz — the DMG's real frame time
    true, BAND_1TO1, BAND_WIDE,
    gb_open, gb_probe, gb_explain, nucleo_gb_close, nucleo_gb_reset, nucleo_gb_run_frame,
    nucleo_gb_set_buttons, nucleo_gb_title, nucleo_gb_set_frameskip, nucleo_gb_state_save,
    nucleo_gb_state_load, nucleo_gb_state_exists, nucleo_gb_save, nucleo_gb_autosave,
    nucleo_gb_get_stats, nucleo_gb_reset_counters, gb_geometry, false,
    { ART_GB_Z, sizeof ART_GB_Z, ART_GB_W, ART_GB_H }, { ART_GBC_Z, sizeof ART_GBC_Z, ART_GBC_W, ART_GBC_H },
    "Game Boy", "Game Boy Color", gb_pick,
    { ART_GB_S_Z, sizeof ART_GB_S_Z, ART_GB_S_W, ART_GB_S_H }, { ART_GBC_S_Z, sizeof ART_GBC_S_Z, ART_GBC_S_W, ART_GBC_S_H },
};

// The Game Boy Color: the same shelf, folders, saves and trace as the Game Boy (a .gbc file lives in either
// folder), the Color core and its colour scanline, and the narrow bands of the colour path (the Color's
// 48 KB of console RAM leave less for the cartridge cache, so every KB of band buffer is a ROM page).
static esp_err_t gbc_open(const char *p)  { return nucleo_gbc_open(p, on_line16, nullptr); }
static esp_err_t gbc_probe(const char *p) { nucleo_gb_info_t i; return nucleo_gbc_probe(p, &i); }
static void gbc_explain(esp_err_t e, const char *path, char *buf, size_t n)
{
    nucleo_gb_info_t inf;
    nucleo_gbc_probe(path, &inf);
    start_error(e, nucleo_gbc_cart_name(inf.cart_type), buf, n);
}
static const EmuSys SYS_GBC = {
    "gbemu", "Game Boy Color", "GC", DIR_GB, DIR_GBC, ".gb", ".gbc", "C",
    "Nessuna ROM in /data/ROMs/gb", "No ROMs in /data/ROMs/gb", "Ninguna ROM en /data/ROMs/gb", "Aucune ROM dans /data/ROMs/gb", "Keine ROMs in /data/ROMs/gb",
    NUCLEO_SD_MOUNT "/system/config/gbemu.json", NUCLEO_SD_MOUNT "/system/config/gbemu.shelf",
    NUCLEO_SD_MOUNT "/gbemu_trace.txt", "/gbemu_trace.txt",
    16743,                                   // the Color keeps the Game Boy's 59.727 Hz (double speed or not)
    false, 5, 3,
    gbc_open, gbc_probe, gbc_explain, nucleo_gbc_close, nucleo_gbc_reset, nucleo_gbc_run_frame,
    nucleo_gbc_set_buttons, nucleo_gbc_title, nucleo_gbc_set_frameskip, nucleo_gbc_state_save,
    nucleo_gbc_state_load, nucleo_gbc_state_exists, nucleo_gbc_save, nucleo_gbc_autosave,
    nucleo_gbc_get_stats, nucleo_gbc_reset_counters, gb_geometry, false,
    { ART_GBC_Z, sizeof ART_GBC_Z, ART_GBC_W, ART_GBC_H }, { ART_GBC_Z, sizeof ART_GBC_Z, ART_GBC_W, ART_GBC_H },
    "Game Boy Color", "Game Boy Color", nullptr,
    { ART_GBC_S_Z, sizeof ART_GBC_S_Z, ART_GBC_S_W, ART_GBC_S_H }, { ART_GBC_S_Z, sizeof ART_GBC_S_Z, ART_GBC_S_W, ART_GBC_S_H },
};
// Which console plays `name` on the Game Boy shelf: a Game Boy cartridge whose header asks for the Color
// (byte 0x143 bit 7: Color-only, or Color-enhanced and played in colour) goes to the Color core.
static const EmuSys *gb_pick(const char *name)
{
    char path[PATHMAX + 40];
    nucleo_gb_info_t inf;
    if (rom_path(name, path, sizeof path) && nucleo_gbc_probe(path, &inf) == ESP_OK && nucleo_gbc_wants(inf.cgb_flag))
        return &SYS_GBC;
    return SYS;
}

static esp_err_t gg_open(const char *p)  { return nucleo_gg_open(p, on_line16, nullptr); }
static esp_err_t gg_probe(const char *p) { nucleo_gg_info_t i; return nucleo_gg_probe(p, &i); }
static void gg_explain(esp_err_t e, const char *path, char *buf, size_t n)
{
    // No controller the core refuses: "not supported" can only be a zip without a cartridge in it.
    if (e == ESP_ERR_NOT_SUPPORTED && strcasestr(path, ".zip"))
        snprintf(buf, n, "%s", TR5("Nessun gioco nello zip", "No game in the zip", "Sin juego en zip", "Aucun jeu dans zip", "Kein Spiel in zip"));
    else start_error(e, nullptr, buf, n);
}
static const EmuSys SYS_GG = {
    "ggemu", "Game Gear/Master System", "GG", DIR_GG, DIR_SMS, ".gg", ".sms", "SMS",
    "Nessuna ROM in /data/ROMs/gg o /sms", "No ROMs in /data/ROMs/gg or /sms", "Ninguna ROM en /data/ROMs/gg o /sms", "Aucune ROM dans /data/ROMs/gg ou /sms", "Keine ROMs in /data/ROMs/gg oder /sms",
    NUCLEO_SD_MOUNT "/system/config/ggemu.json", NUCLEO_SD_MOUNT "/system/config/ggemu.shelf",
    NUCLEO_SD_MOUNT "/ggemu_trace.txt", "/ggemu_trace.txt",
    16688,                                   // 262 x 228 cycles at 3.579545 MHz = 59.92 Hz
    false, 5, 3,
    gg_open, gg_probe, gg_explain, nucleo_gg_close, nucleo_gg_reset, nucleo_gg_run_frame,
    nucleo_gg_set_buttons, nucleo_gg_title, nucleo_gg_set_frameskip, nucleo_gg_state_save,
    nucleo_gg_state_load, nucleo_gg_state_exists, nucleo_gg_save, nucleo_gg_autosave,
    nucleo_gg_get_stats, nucleo_gg_reset_counters, nucleo_gg_geometry, true,
    { ART_GG_Z, sizeof ART_GG_Z, ART_GG_W, ART_GG_H }, { ART_SMS_Z, sizeof ART_SMS_Z, ART_SMS_W, ART_SMS_H },
    "Game Gear", "Master System", nullptr,
    { ART_GG_S_Z, sizeof ART_GG_S_Z, ART_GG_S_W, ART_GG_S_H }, { ART_SMS_S_Z, sizeof ART_SMS_S_Z, ART_SMS_S_W, ART_SMS_S_H },
};

static void enter(void)
{
    // The RAM window is declarative (see the app_def): by the time this runs we are already on a
    // fresh heap with the radio down. All this has to do is allocate and read the shelf.
    // View options start from their defaults for each console; state_load() then applies its file.
    s_pal = 0; s_hud = true; s_stretch = false; s_sharp = false;
    pal_apply(); geom_apply();
    if (!st) st = (EState *)calloc(1, sizeof(EState));
    if (st && !st->sh) st->sh = (Shelf *)calloc(1, sizeof(Shelf));
    if (st && !st->sh) { free(st); st = nullptr; }
    if (!st) { nucleo_app_set_hint(TR5("RAM insufficiente", "Not enough RAM", "RAM insuficiente", "RAM insuffisante", "Zu wenig RAM")); return; }
    trace("=== %s enter: solo=%d exclusive=%d ===", SYS->id,
          (int)nucleo_anima_solo_active(), (int)nucleo_exclusive_active());
    trace_heap("on enter");
    state_load();
    rescan();
    trace("  shelf: %d shown of %d", st->n, st->total);
    // Land on the last cartridge played, so "carry on" costs no keystrokes.
    if (st->have_resume) {
        for (int i = 0; i < st->n; i++)
            if (!strcmp(ent_name(i), st->resume)) { st->sel = i; break; }
    }
    // The Games gallery opened us on one cartridge ("open with", carried across the Solo reboot), by its whole path:
    // it may be in any folder. shelf_poll() plays it straight away, and quitting it goes back to the gallery.
    if (const char *of = nucleo_app_take_open_file()) {
        const char *base = strrchr(of, '/');
        snprintf(st->autoplay, sizeof st->autoplay, "%s", of);
        for (int i = 0; i < st->n; i++)
            if (!strcmp(ent_name(i), base ? base + 1 : of)) { st->sel = i; break; }
        trace("  opened on '%s'", st->autoplay);
    }
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_poll_handler(shelf_poll);   // 50 Hz title scroll, sprite-blitted (no list repaint)
    hint();
}

static void enter_gb(void) { SYS = &SYS_GB; enter(); }
static void enter_gg(void) { SYS = &SYS_GG; enter(); }

static void leave(void)
{
    if (SYS) SYS->close();                       // a no-op when no cartridge is open
    mq_free();
    pic_free();
    if (st) { free(st->band[0]); free(st->band[1]); free(st->sh); free(st); st = nullptr; }
    if (SYS) remove(SYS->shelf_swp);             // never leave a stale shelf image for the next visit
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();
}

// Two registrations, one per console: built in, both are called; from the store each is its own module
// (gbemu, ggemu) and only reaches its own core, so the linker leaves the other one out.
#if !CONFIG_NUCLEO_STORE_GBEMU || defined(NAP_MODULE)
extern "C" void nucleo_register_gbemu(void)
{
    static const nucleo_app_def_t app = {
        "gbemu", "Game Boy", "Games",
        "Play Game Boy cartridges natively — the emulator runs on the Cardputer itself.",
        'G', 0x8FF3, enter_gb, on_key, nullptr, draw, leave,
        NX_NET_APP | NX_SOLO | NX_WIFI,
            // SOLO: only a fresh boot yields a contiguous block big enough for the core + ROM cache;
            // the runtime reclaim frees RAM but cannot defragment (see nucleo_exclusive.h).
            // WIFI: the radio costs ~48 KB and halves the largest free block, and an emulator has no
            // use for a network. Leaving reboots into the full OS, so the radio returns on its own
            // without the fragile in-place restore.
        nullptr, draw_large,                     // two screens: the gallery on the panel
    };
    nucleo_app_register(&app);
}
#endif

#if !CONFIG_NUCLEO_STORE_GGEMU || defined(NAP_MODULE)
// The Game Gear: same shelf, same menu, same RAM posture — only the descriptor differs.
extern "C" void nucleo_register_ggemu(void)
{
    static const nucleo_app_def_t gg = {
        "ggemu", "Game Gear/Master System", "Games",
        "Play Game Gear and Master System cartridges natively — the emulator runs on the Cardputer itself.",
        'G', 0x5D7F, enter_gg, on_key, nullptr, draw, leave,
        NX_NET_APP | NX_SOLO | NX_WIFI, nullptr, draw_large
    };
    nucleo_app_register(&gg);
}
#endif
#endif  // !(CONFIG_NUCLEO_STORE_GBEMU && CONFIG_NUCLEO_STORE_GGEMU) || NAP_MODULE
