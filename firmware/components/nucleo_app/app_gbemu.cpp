// app_gbemu — Game Boy, emulated natively on the Cardputer.
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
// HELD and two buttons can be down at once. WASD or the printed ; . , / arrows move; K/L = A, J = B;
// Space = Start, N = Select. TAB held fast-forwards. P cycles the palette, M (or Esc) opens the
// in-game menu — save state, load state, palette, picture quality, quit.
//
// THE SHELF — the library on the card is ~4,700 Game Boy cartridges. Holding that many names in RAM
// is impossible (4,700 x 64 B = 300 KB), so the browser NEVER holds the whole library: it re-walks
// the directory against a live filter and keeps at most MAXR matches. Type-to-filter is therefore
// not a convenience here, it is the navigation model — which is also the right answer on a device
// with a real keyboard and a 135 px screen (docs/device-ui.md: use the keyboard, exploit every pixel).
#include "nucleo_app.h"
#include "app_gfx.h"
#include "nucleo_gb.h"
#include "nucleo_kbd.h"
#include "nucleo_board.h"
#include "nucleo_exclusive.h"
#include "nucleo_power.h"
#include "nucleo_setup.h"
#include "nucleo_ble.h"
#include "nucleo_theme.h"
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

static const char *TAG = "gbemu";

// ── on-card trace ───────────────────────────────────────────────────────────────────────────────
// There is NO usable serial console on this device (the USB PHY belongs to TinyUSB), so ESP_LOG
// reaches nobody. Every launch therefore leaves a breadcrumb on the SD card: pull the card, read
// /gbemu_trace.txt, and see exactly how far a start got and what the heap looked like at each step.
// This is the technique that finally explained the silent-video bug — guessing cost days there.
#define TRACE_PATH NUCLEO_SD_MOUNT "/gbemu_trace.txt"
static void trace(const char *fmt, ...)
{
    FILE *f = fopen(TRACE_PATH, "a");
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
#define STATE_JS NUCLEO_SD_MOUNT "/system/config/gbemu.json"

// ── screen mapping ──────────────────────────────────────────────────────────────────────────────
// NO SCALING HORIZONTALLY. The panel is 240 px wide and the Game Boy is 160, so every column goes to
// exactly one pixel: native, unfiltered, no interpolation to soften it. (An earlier version dropped
// every 16th column to make the picture 150 wide. That threw away real detail — thin sprites lost
// limbs — to solve a problem the panel did not have.)
//
// VERTICALLY there is no such luxury: 144 source lines must reach a 135 px panel, and the only
// alternatives to dropping 9 of them are cropping the picture or letterboxing a screen that is
// already tiny. Dropping every 16th line is the least destructive of the three, and it is a DROP,
// not a blend — no averaging, so the remaining pixels stay exactly the colours the PPU produced.
#define OUT_W 160            // 1:1 with the Game Boy
#define OUT_H 135            // 144 * 15/16 — forced by the panel height
#define OUT_X ((240 - OUT_W) / 2)
#define BAND  15             // output lines buffered before one SPI push; 15 divides 135 evenly

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

// ── session state (heap on enter, never .bss: the app is closed almost always) ──────────────────
#define MAXR    120          // matches held at once. The filter, not this cap, is how you reach a game.
#define NAMEMAX 56
#define FILTMAX 20

struct RomEnt {
    char name[NAMEMAX];
    bool gbc;
};

struct EState {
    RomEnt   list[MAXR];
    int      n;              // matches kept
    int      total;          // matches that EXIST (may exceed n — we say so honestly)
    int      sel, scroll;
    char     filter[FILTMAX + 1];
    int      flen;
    char     resume[NAMEMAX];   // last cartridge played, offered at the top of an unfiltered shelf
    bool     have_resume;

    uint16_t *band;          // OUT_W * BAND pixels, pushed one band at a time
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
    char     toast[28];      // transient confirmation ("saved", "loaded")
    int64_t  toast_until;
    // Cached geometry of the focused shelf row, so the ~5 Hz tick can scroll its title without
    // recomputing the whole layout. Filled by draw(), consumed by tick().
    int      sel_y, sel_avail;
    char     sel_title[NAMEMAX];
    int      slow_secs;      // consecutive sub-50fps seconds, for the one-way relief debounce
};
static EState *st = nullptr;

// ── tiny persisted state (resume) ───────────────────────────────────────────────────────────────
// Deliberately a flat file, not JSON: one line, no parser, no cJSON allocation on a heap this tight.
static void state_load(void)
{
    FILE *f = fopen(STATE_JS, "r");
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
    SHADE = PALETTE[s_pal];
    fclose(f);
}
static void state_save(const char *name)
{
    FILE *f = fopen(STATE_JS, "w");
    if (!f) return;
    fprintf(f, "%s\n%d\n", name ? name : "", s_pal);
    fclose(f);
}

// ── ROM discovery ───────────────────────────────────────────────────────────────────────────────
static bool is_gb(const char *n, bool *gbc)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    if (!strcasecmp(dot, ".gb"))  { *gbc = false; return true; }
    if (!strcasecmp(dot, ".gbc")) { *gbc = true;  return true; }
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

static void scan_dir(const char *dir, bool mark_gbc)
{
    DIR *dp = opendir(dir);   // NB: never name a local `d` — app_gfx.h defines `d` as the draw target
    if (!dp) return;
    struct dirent *de;
    unsigned seen = 0;
    while ((de = readdir(dp)) != NULL) {
        // This directory holds ~4,700 entries and FATFS walks it over SPI. At roughly a millisecond
        // each that is several SECONDS in the watchdog-subscribed UI task, which panics the device —
        // and the scan re-runs on every typed character. Pet the dog while walking.
        if ((++seen & 63) == 0) esp_task_wdt_reset();
        bool gbc = mark_gbc;
        if (de->d_name[0] == '.' || !is_gb(de->d_name, &gbc)) continue;
        if (!matches(de->d_name, st->filter)) continue;
        st->total++;
        if (st->n < MAXR) {
            snprintf(st->list[st->n].name, NAMEMAX, "%s", de->d_name);
            st->list[st->n].gbc = gbc;
            st->n++;
        }
    }
    closedir(dp);
}

static int cmp_ent(const void *a, const void *b)
{ return strcasecmp(((const RomEnt *)a)->name, ((const RomEnt *)b)->name); }

static void rescan(void)
{
    st->n = 0; st->total = 0;
    scan_dir(DIR_GB,  false);
    scan_dir(DIR_GBC, true);
    if (st->n > 1) qsort(st->list, st->n, sizeof(RomEnt), cmp_ent);
    st->sel = 0; st->scroll = 0;
}

// Resolve a bare filename to a full path, trying both system folders.
static bool rom_path(const char *name, char *out, size_t n)
{
    struct stat sb;
    snprintf(out, n, "%s/%s", DIR_GB, name);
    if (stat(out, &sb) == 0) return true;
    snprintf(out, n, "%s/%s", DIR_GBC, name);
    return stat(out, &sb) == 0;
}

// ── the picture ─────────────────────────────────────────────────────────────────────────────────
// One scanline out of the core: drop every 16th line, map the 160 pixels straight through, accumulate
// BAND output lines, push once. Nine SPI transactions per frame instead of 135 — the difference
// between the bus being a cost and being the bottleneck.
// -O2 for this function alone. nucleo_app is a large component built at -Os like the rest of the
// firmware, but this is the emulator's per-scanline path: 135 lines x 60 fps, and at -Os the inner
// copy does not get unrolled. The attribute keeps the exception to the one function that earns it.
__attribute__((optimize("-O2")))
static void on_line(const uint8_t *px, int line, void *user)
{
    (void)user;
    if (!st || !st->band) return;
    if ((line & 15) == 15) return;
    if (st->out_y >= OUT_H) return;

    // A flat 160-entry palette lookup — no strides, no run bookkeeping, no branch. Dropping the
    // column decimation did not just improve the picture, it made this loop cheaper: a straight
    // count-up over two contiguous arrays is what the compiler unrolls best, and it is the hottest
    // loop in the app at 135 lines x 60 fps.
    uint16_t *row = st->band + (size_t)st->band_line * OUT_W;
    for (int x = 0; x < OUT_W; x++) row[x] = SHADE[px[x] & 3];

    if (st->band_line == 0) st->band_y = st->out_y;
    st->band_line++;
    st->out_y++;

    if (st->band_line == BAND || st->out_y >= OUT_H) {
        // No startWrite here: play() holds ONE transaction open across the whole frame, so all nine
        // bands stream to the panel with no bus re-arbitration between them — a tighter, more
        // continuous update, which is what keeps the moving picture from tearing band by band.
        int64_t b0 = esp_timer_get_time();
        d.pushImage(OUT_X, st->band_y, OUT_W, st->band_line, st->band);
        st->us_blit += (uint32_t)(esp_timer_get_time() - b0);
        st->band_line = 0;
    }
}

// ── in-game furniture ───────────────────────────────────────────────────────────────────────────
// The picture is 160 px wide inside a 240 px panel, which leaves two 40 px pillars. They are not
// padding: they are the only place a running emulator can say anything without covering the game.
// LEFT is identity and control (frame rate, palette, how to reach the menu); RIGHT is the honest
// cost breakdown, because "it feels slow" is not a bug report until it says which part is slow.
#define PILL_L 0
#define PILL_R (OUT_X + OUT_W)
#define PILL_W OUT_X

static inline bool dn(char c) { return nucleo_kbd_char_down(c); }
static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

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
    nucleo_gb_set_frameskip(level == 1);
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
    d.fillRect(PILL_L, 0, PILL_W - 2, 30, BG);
    d.setTextSize(2);
    d.setTextColor(st->fps >= 55 ? SHADE[1] : AMB, BG);
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
    d.fillRect(PILL_L, 100, PILL_W - 2, 35, BG);
    d.setTextSize(1);
    d.setTextColor(SHADE[1], BG);
    d.setCursor(3, 101); d.print(PAL_NAME[s_pal]);
    d.setTextColor(DIM, BG);
    d.setCursor(3, 113); d.print("P pal");
    d.setCursor(3, 124); d.print("M menu");

    d.fillRect(PILL_R + 2, 100, PILL_W - 2, 35, BG);
    if (st->toast[0]) {
        d.setTextColor(GRN, BG);
        d.setCursor(PILL_R + 4, 113); d.print(st->toast);
    } else {
        d.setTextColor(DIM, BG);
        d.setCursor(PILL_R + 4, 113); d.print("TAB");
        d.setCursor(PILL_R + 4, 124); d.print("fast");
    }
}

static void pal_cycle(void)
{
    s_pal = (s_pal + 1) % PAL_COUNT;
    SHADE = PALETTE[s_pal];
    state_save(st->resume);
    chrome_draw();
}

// ── the in-game menu ────────────────────────────────────────────────────────────────────────────
// Opened with M or Esc. Esc opening a MENU rather than quitting outright is the deliberate choice:
// leaving a game means losing the session (the app runs in a Solo boot and exiting reboots), so the
// one key a player hits by reflex must not be the destructive one. Quit is still one row away.
enum { MI_RESUME = 0, MI_SAVE, MI_LOAD, MI_PAL, MI_PIC, MI_QUIT, MI_N };

static const char *menu_label(int i, char *buf, size_t n)
{
    switch (i) {
        case MI_RESUME: return TR("Riprendi", "Resume");
        case MI_SAVE:   return TR("Salva stato", "Save state");
        case MI_LOAD:   return nucleo_gb_state_exists(0) ? TR("Carica stato", "Load state")
                                                         : TR("Carica stato (vuoto)", "Load state (empty)");
        case MI_PAL:    snprintf(buf, n, "%s: %s", TR("Colori", "Palette"), PAL_NAME[s_pal]); return buf;
        case MI_PIC:    snprintf(buf, n, "%s: %s", TR("Immagine", "Picture"),
                                 st->relief == 1 ? "30 fps" : TR("Piena", "Full"));
                        return buf;
        default:        return TR("Esci dal gioco", "Quit game");
    }
}

#define MENU_W 200
#define MENU_H (12 + MI_N * 17 + 8)
#define MENU_X ((240 - MENU_W) / 2)
#define MENU_Y ((135 - MENU_H) / 2)
#define MENU_LX (MENU_X + 22)                    // label column x
#define MENU_LW (MENU_W - 22 - 6)                // label column width

// One row. The selected row's label scrolls (marquee) so a long entry like "Load state (empty)" or a
// palette name is fully readable instead of clipped; the rest are drawn once, clipped, no animation.
static void menu_row(int i)
{
    char buf[40];
    int y = MENU_Y + 14 + i * 17;
    bool on = (i == st->msel);
    d.fillRect(MENU_X + 4, y, MENU_W - 8, 16, on ? INK : BG);
    d.setTextSize(1);
    d.setTextColor(on ? DIM : LINE, on ? INK : BG);
    d.setCursor(MENU_X + 8, y + 5); d.printf("%d", i + 1);
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

static void menu_draw(void)
{
    d.fillRect(MENU_X, MENU_Y, MENU_W, MENU_H, BG);
    d.drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 6, LINE);
    d.setTextSize(1);
    d.setTextColor(DIM, BG);
    d.setCursor(MENU_X + 8, MENU_Y + 4); d.print(nucleo_gb_title());
    for (int i = 0; i < MI_N; i++) menu_row(i);
}

// Called from the paused loop at ~50 Hz: only the selected row can be moving, so only it is redrawn.
static void menu_anim(void) { if (st->menu) menu_row(st->msel); }

// Leaving the menu: the PPU repaints the whole 160x135 picture on its very next frame, so only the
// pillars and the panel edges need restoring by hand.
static void menu_close(void)
{
    st->menu = false;
    d.fillRect(PILL_L, 0, PILL_W, 135, BG);
    d.fillRect(PILL_R, 0, 240 - PILL_R, 135, BG);
    hud_draw();
    chrome_draw();
}

static void menu_activate(void)
{
    switch (st->msel) {
        case MI_RESUME: menu_close(); return;
        case MI_SAVE:
            toast(nucleo_gb_state_save(0) == ESP_OK ? TR("salvato", "saved") : TR("errore", "failed"));
            menu_close(); return;
        case MI_LOAD: {
            esp_err_t e = nucleo_gb_state_load(0);
            toast(e == ESP_OK ? TR("caricato", "loaded")
                              : e == ESP_ERR_NOT_FOUND ? TR("nessuno stato", "no state")
                                                       : TR("stato non valido", "bad state"));
            menu_close(); return;
        }
        case MI_PAL: s_pal = (s_pal + 1) % PAL_COUNT; SHADE = PALETTE[s_pal]; state_save(st->resume);
                     menu_draw(); return;
        case MI_PIC: set_relief((st->relief + 1) % 2); menu_draw(); return;
        default:     st->running = false; return;
    }
}

static void menu_key(nucleo_key_t k)
{
    if (k.key == NK_BACK || k.ch == '`' || k.ch == 'm' || k.ch == 'M') { menu_close(); return; }
    if (k.ch >= '1' && k.ch <= '0' + MI_N) { st->msel = k.ch - '1'; menu_activate(); return; }
    if (k.key == NK_UP   || k.ch == 'e' || k.ch == 'E' || k.ch == ';') { st->msel = (st->msel + MI_N - 1) % MI_N; menu_draw(); return; }
    if (k.key == NK_DOWN || k.ch == 's' || k.ch == 'S' || k.ch == '.') { st->msel = (st->msel + 1) % MI_N; menu_draw(); return; }
    if (k.key == NK_ENTER || k.ch == '\n' || k.ch == ' ' || k.ch == 'k' || k.ch == 'K') menu_activate();
}

// ── the run loop ────────────────────────────────────────────────────────────────────────────────
// Show a failure where the user is actually looking — the middle of the screen — not only in the
// hint bar, which is easy to miss when nothing else appears to happen.
static void fail_box(const char *what)
{
    int top = nucleo_app_content_top(), ch = nucleo_app_content_height();
    int y = top + ch / 2 - 24;
    d.fillRect(8, y, 224, 48, BG);
    d.drawRoundRect(8, y, 224, 48, 8, AMB);
    d.setTextSize(1);
    d.setTextColor(AMB, BG);   d.setCursor(16, y + 8);  d.print(TR("Avvio non riuscito", "Could not start"));
    d.setTextColor(MUTED, BG); d.setCursor(16, y + 22); d.print(what);
    d.setTextColor(DIM, BG);   d.setCursor(16, y + 34); d.print("/gbemu_trace.txt");
    nucleo_app_set_hint(what);
}

static void play(const char *name)
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
        fail_box(TR("ROM non trovata", "ROM not found"));
        return;
    }
    trace("  path=%s", path);

    // Hand the 32 KB canvas back BEFORE the core allocates: it is the single biggest contiguous block
    // on the heap, and the ROM cache wants exactly that kind of room.
    nucleo_app_release_buffers();
    nucleo_screen_release();
    nucleo_app_set_direct_draw(true);

    trace_heap("canvas released");
    st->band = (uint16_t *)heap_caps_malloc((size_t)OUT_W * BAND * sizeof(uint16_t), MALLOC_CAP_DEFAULT);
    if (!st->band) {
        trace("  FAIL: band buffer (%u B) alloc failed", (unsigned)((size_t)OUT_W * BAND * 2));
        nucleo_app_set_direct_draw(false);
        fail_box(TR("RAM insufficiente (video)", "Not enough RAM (video)"));
        return;
    }

    esp_err_t err = nucleo_gb_open(path, on_line, nullptr);
    if (err != ESP_OK) {
        trace("  FAIL: nucleo_gb_open -> %s", esp_err_to_name(err));
        trace_heap("after open fail");
        free(st->band); st->band = nullptr;
        nucleo_app_set_direct_draw(false);
        fail_box(err == ESP_ERR_NO_MEM ? TR("RAM insufficiente (core)", "Not enough RAM (core)")
                                       : TR("ROM non valida", "Invalid ROM"));
        return;
    }

    snprintf(st->resume, sizeof st->resume, "%s", name);
    st->have_resume = true;
    state_save(name);

    nucleo_gb_stats_t stt; nucleo_gb_get_stats(&stt);
    char cache[24];
    if (stt.rom_resident) snprintf(cache, sizeof cache, "resident");
    else                  snprintf(cache, sizeof cache, "%dx1KB-pages", stt.rom_pages);
    trace("  OPEN OK '%s' rom=%uKB cache=%s core-heap=%u", nucleo_gb_title(),
          (unsigned)(stt.rom_bytes / 1024), cache, (unsigned)stt.heap_bytes);
    trace_heap("running");
    ESP_LOGI(TAG, "'%s' %uKB %s | core heap %u B | free %u",
             nucleo_gb_title(), (unsigned)(stt.rom_bytes / 1024), cache,
             (unsigned)stt.heap_bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    // DFS ranges 80-240 MHz and cannot tell a render loop from an idle launcher; at the floor a frame
    // does not fit. Hold the clock at maximum for exactly as long as a game runs.
    nucleo_power_perf_begin();

    d.fillScreen(BG);
    // BYTE ORDER. This is the first app to push a raw 16bpp host-order buffer straight to the panel;
    // everything else is either 8bpp (one byte, no order) or goes through a file decoder that handles
    // this itself. The ST7789 latches each pixel most-significant-byte first, but an ESP32 stores a
    // uint16_t little-endian, so without a swap the two bytes arrive reversed and #9BBC0F green comes
    // out #E19D purple. setSwapBytes(true) tells pushImage to emit big-endian; it affects ONLY the
    // raw-buffer push path, never the colour-argument primitives (fillRect/text), so the pillars and
    // HUD keep rendering from the same palette values correctly.
    d.setSwapBytes(true);

    st->running = true;
    st->menu = false; st->msel = 0; st->turbo = false;
    st->toast[0] = '\0'; st->toast_until = 0;
    st->us_blit = 0; st->ms_cpu = st->ms_blit = st->ms_aud = 0;
    st->slow_secs = 0;
    set_relief(0);                       // every cartridge starts at the full picture and earns relief
    nucleo_gb_reset_counters();
    st->fps_frames = 0; st->fps = 0; st->fps_t0 = esp_timer_get_time();
    hud_draw();
    chrome_draw();
    uint8_t held = 0;
    int64_t next = esp_timer_get_time();
    const int64_t FRAME_US = 16743;                    // 59.727 Hz — the DMG's real frame time

    while (st->running) {
        esp_task_wdt_reset();
        if (st->toast[0] && esp_timer_get_time() > st->toast_until) { st->toast[0] = '\0'; chrome_draw(); }

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
        held = b;

        // TAB held = unpaced. Menus, grinding and long text are what a real player fast-forwards
        // through, and holding a key is the right gesture: release and the game is back at speed.
        st->turbo = dn('\t');

        if (k.key != NK_NONE || k.ch) {
            if (st->menu)                        { menu_key(k); }
            else if (k.key == NK_BACK || k.ch == '`') { st->menu = true; st->msel = 0; menu_draw(); }
            else if (k.ch == 'm' || k.ch == 'M') { st->menu = true; st->msel = 0; menu_draw(); }
            else if (k.ch == 'p' || k.ch == 'P') pal_cycle();
        }
        if (st->menu) {                          // paused: the console does not advance
            menu_anim();                         // ...but a long selected entry keeps scrolling
            vTaskDelay(pdMS_TO_TICKS(20));
            esp_task_wdt_reset();
            continue;
        }
        nucleo_gb_set_buttons(held);

        st->out_y = 0; st->band_line = 0;
        // One SPI transaction for the ENTIRE frame. run_frame calls on_line 135 times and each pushes
        // its band inside this single startWrite/endWrite, so the panel sees one uninterrupted sweep
        // top to bottom instead of nine separately-arbitrated writes.
        d.startWrite();
        nucleo_gb_run_frame();
        d.endWrite();

        st->fps_frames++;
        int64_t now = esp_timer_get_time();
        if (now - st->fps_t0 >= 1000000) {
            st->fps = (int)st->fps_frames;
            // Divide the window's accumulated microseconds by the frames in it, in TENTHS of a ms —
            // a breakdown rounded to whole milliseconds hides exactly the differences worth seeing.
            nucleo_gb_stats_t w; nucleo_gb_get_stats(&w);
            uint32_t nf = w.frames ? w.frames : 1;
            st->ms_cpu  = (int)(w.us_cpu   / nf / 100);
            st->ms_aud  = (int)(w.us_audio / nf / 100);
            st->ms_blit = (int)(st->us_blit / nf / 100);
            trace("  fps=%d cpu=%d.%dms blit=%d.%dms audio=%d.%dms misses=%u relief=%d",
                  st->fps, st->ms_cpu / 10, st->ms_cpu % 10, st->ms_blit / 10, st->ms_blit % 10,
                  st->ms_aud / 10, st->ms_aud % 10, (unsigned)w.bank_misses, st->relief);
            st->us_blit = 0; nucleo_gb_reset_counters();
            st->fps_frames = 0; st->fps_t0 = now;
            hud_draw();

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
    trace("  END fps=%d palette=%s", st->fps, PAL_NAME[s_pal]);
    nucleo_gb_close();
    free(st->band); st->band = nullptr;
    d.setSwapBytes(false);                 // leave the shared display as every other app expects it
    nucleo_app_set_direct_draw(false);
    nucleo_app_force_repaint();
}

// ── the shelf ───────────────────────────────────────────────────────────────────────────────────
#define ROW_H 22

// A small Game Boy glyph, the same silhouette as the launcher icon, drawn at the header.
static void glyph_gb(int x, int y, int h, uint16_t col)
{
    int w = h * 62 / 100;
    d.fillRoundRect(x, y, w, h, h / 8, col);
    d.fillRect(x + w / 5, y + h / 8, w * 3 / 5, h * 3 / 10, BG);
    d.fillRect(x + w / 4, y + h * 6 / 10, w / 10, h / 5, BG);
    d.fillRect(x + w / 8, y + h * 7 / 10, w * 3 / 10, h / 12, BG);
    d.fillCircle(x + w * 3 / 4, y + h * 7 / 10, h / 14, BG);
}

// Titles read better without the extension or the region/dump tags every set carries.
static void pretty(const char *file, char *out, size_t n)
{
    char tmp[NAMEMAX];
    snprintf(tmp, sizeof tmp, "%s", file);
    char *dot = strrchr(tmp, '.'); if (dot) *dot = '\0';
    char *cut = strstr(tmp, " (");  if (cut) *cut = '\0';
    cut = strstr(tmp, " [");        if (cut) *cut = '\0';
    snprintf(out, n, "%s", tmp);
}

static void draw(void)
{
    if (!st) return;
    int ch = nucleo_app_content_height(), top = nucleo_app_content_top();
    d.fillRect(0, top, 240, ch, BG);

    // ── header: glyph + system, and either the live filter or the counts ──
    glyph_gb(6, top + 2, 18, ACC);
    if (st->flen) {
        d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(28, top + 3);
        char q[FILTMAX + 2]; snprintf(q, sizeof q, "%s_", st->filter);
        d.print(q);
    } else {
        d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(28, top + 3); d.print("Game Boy");
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
        d.print(st->flen ? TR("Nessun risultato", "No match")
                         : TR("Nessuna ROM in /data/ROMs/gb", "No ROMs in /data/ROMs/gb"));
        if (st->flen) { d.setCursor(8, top + 48); d.setTextColor(DIM, BG); d.print(TR("Canc per correggere", "Backspace to edit")); }
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
        pretty(st->list[idx].name, title, sizeof title);
        // The badges are drawn first so the title can be clipped to whatever is left, never overlap.
        int right = 232;
        if (st->list[idx].gbc) {
            d.setTextSize(1); d.setTextColor(foc ? INK : AMB, foc ? ACC : BG);
            d.setCursor(right - 12, y + (ROW_H - 8) / 2); d.print("C"); right -= 14;
        }
        if (st->have_resume && !strcmp(st->list[idx].name, st->resume)) {
            d.fillCircle(right - 6, y + ROW_H / 2 - 1, 3, foc ? INK : GRN); right -= 14;
        }
        int avail = right - 20;
        int ty = y + (ROW_H - 16) / 2;
        if (foc) {
            // The selected title is shown IN FULL and scrolls if it overruns — no more silent
            // truncation on the one row the player is actually looking at. Cache the geometry so the
            // 5 Hz tick can keep it moving without redrawing the whole shelf.
            st->sel_y = ty; st->sel_avail = avail;
            snprintf(st->sel_title, sizeof st->sel_title, "%s", title);
            marquee(20, ty, avail, 16, title, INK, ACC, 2, now_ms());
        } else {
            int maxc = avail / 12; if (maxc < 1) maxc = 1; if (maxc > 17) maxc = 17;
            char shown[20]; snprintf(shown, sizeof shown, "%.*s", maxc, title);
            d.setTextSize(2); d.setTextColor(FG, BG);
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
}

static void hint(void)
{
    nucleo_app_set_hint(st && st->flen
        ? TR("Invio gioca · Canc corregge · Esc pulisce",  "Enter plays · Backspace edits · Esc clears")
        : TR("Scrivi per cercare · 1-9 · Invio gioca",     "Type to search · 1-9 · Enter plays"));
}

// ~5x/second while the shelf is foreground: keep the selected, overrunning title scrolling. play()
// owns the CPU while a game runs, so this never fires mid-game.
static void tick(void)
{
    if (!st || st->running || !st->sel_title[0]) return;
    d.setTextSize(2);
    if ((int)d.textWidth(st->sel_title) <= st->sel_avail) return;   // fits: nothing to animate
    marquee(20, st->sel_y, st->sel_avail, 16, st->sel_title, INK, ACC, 2, now_ms());
}

static void on_key(int key, char ch)
{
    if (!st) return;

    if (key == NK_UP)    { if (st->sel > 0) st->sel--; nucleo_app_request_draw(); return; }
    if (key == NK_DOWN)  { if (st->sel < st->n - 1) st->sel++; nucleo_app_request_draw(); return; }
    // NB: NK_LEFT and NK_BACK never arrive here — the framework routes both to the back handler
    // (see on_back). Only NK_RIGHT pages forward from on_key.
    if (key == NK_RIGHT) { st->sel += 5; if (st->sel > st->n - 1) st->sel = st->n - 1; nucleo_app_request_draw(); return; }

    if (key == NK_ENTER) {
        if (st->n) { play(st->list[st->sel].name); hint(); nucleo_app_request_draw(); }
        return;
    }

    // Backspace edits the filter one character at a time. (NK_BACK itself never reaches on_key —
    // the framework routes it to the back handler, which clears the whole search; see on_back.)
    if (ch == 8 || ch == 127) {
        if (st->flen) { st->filter[--st->flen] = '\0'; rescan(); hint(); nucleo_app_request_draw(); }
        return;
    }

    // Digits are quick-pick, not filter input: on a keyboard device the fastest path to a visible
    // row is its number. Titles starting with a digit are still reachable by typing more letters.
    if (ch >= '1' && ch <= '9') {
        int want = st->scroll + (ch - '1');
        if (want < st->n) { st->sel = want; play(st->list[want].name); hint(); nucleo_app_request_draw(); }
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

static void enter(void)
{
    // The RAM window is declarative (see the app_def): by the time this runs we are already on a
    // fresh heap with the radio down. All this has to do is allocate and read the shelf.
    if (!st) st = (EState *)calloc(1, sizeof(EState));
    if (!st) { nucleo_app_set_hint(TR("RAM insufficiente", "Not enough RAM")); return; }
    trace("=== gbemu enter: solo=%d exclusive=%d ===",
          (int)nucleo_anima_solo_active(), (int)nucleo_exclusive_active());
    trace_heap("on enter");
    state_load();
    rescan();
    trace("  shelf: %d shown of %d", st->n, st->total);
    // Land on the last cartridge played, so "carry on" costs no keystrokes.
    if (st->have_resume) {
        for (int i = 0; i < st->n; i++)
            if (!strcmp(st->list[i].name, st->resume)) { st->sel = i; break; }
    }
    nucleo_app_set_back_handler(on_back);
    hint();
}

static void leave(void)
{
    nucleo_gb_close();
    if (st) { free(st->band); free(st); st = nullptr; }
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();
}

extern "C" void nucleo_register_gbemu(void)
{
    static const nucleo_app_def_t app = {
        "gbemu", "Game Boy", "Games",
        "Play Game Boy cartridges natively — the emulator runs on the Cardputer itself.",
        'G', 0x8FF3, enter, on_key, tick, draw, leave,
        NX_NET_APP | NX_SOLO | NX_WIFI
            // SOLO: only a fresh boot yields a contiguous block big enough for the core + ROM cache;
            // the runtime reclaim frees RAM but cannot defragment (see nucleo_exclusive.h).
            // WIFI: the radio costs ~48 KB and halves the largest free block, and an emulator has no
            // use for a network. Leaving reboots into the full OS, so the radio returns on its own
            // without the fragile in-place restore.
    };
    nucleo_app_register(&app);
}
