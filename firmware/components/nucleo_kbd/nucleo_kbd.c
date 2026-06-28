// Cardputer integrated keyboard.
//
// Two backends, picked automatically at init from the board M5GFX detected:
//   * Original Cardputer  : GPIO matrix via a 74HC138 (address {8,9,11}, 7 rows
//                           {13,15,3,4,5,6,7}), active-low with pull-ups -> 4x14 layout.
//   * Cardputer ADV       : a TCA8418 I2C keypad scanner (addr 0x34 on SDA=8/SCL=9). It
//                           reports key-up/down events in a FIFO; the key number remaps to
//                           the SAME 4x14 "picture" coordinates, so both backends share the
//                           KM/KM_SH tables below. (Protocol per the TCA8418 datasheet + the
//                           public Cardputer-ADV reference; verify the layout on real ADV
//                           hardware with the built-in keyboard diagnostic.)
#include "nucleo_kbd.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

// Auto-repeat: hold a navigation/edit key and it fires again after an initial delay, then at
// a steady rate — so scrolling a long list (e.g. 64 tracks) is a hold, not 64 taps. Limited
// to arrows + backspace so a long press never re-triggers Enter/Back or double-types a letter.
#define KEY_REPEAT_DELAY_US 350000   // hold this long before the first repeat
#define KEY_REPEAT_RATE_US   90000   // then repeat this often (~11/s)
static inline bool key_repeats(char c) { return c == ';' || c == '.' || c == ',' || c == '/' || c == '\b'; }

extern bool nucleo_ui_is_adv(void);    // board id from M5GFX (resolved at link; no REQUIRES cycle)

// 74HC138 address pins and the 7 row inputs (original Cardputer).
static const int ADDR[3] = {8, 9, 11};
static const int ROW[7]  = {13, 15, 3, 4, 5, 6, 7};

// 4x14 logical layout (y row 0..3, x col 0..13). 0 marks a modifier/none.
// '\n'=Enter, '\b'=Backspace. Shared by both backends.
static const char KM[4][14] = {
    {'`','1','2','3','4','5','6','7','8','9','0','-','=','\b'},
    {'\t','q','w','e','r','t','y','u','i','o','p','[',']','\\'},
    {0,0,'a','s','d','f','g','h','j','k','l',';','\'','\n'},
    {0,0,0,'z','x','c','v','b','n','m',',','.','/',' '},
};
static const char KM_SH[4][14] = {
    {'~','!','@','#','$','%','^','&','*','(',')','_','+','\b'},
    {'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','|'},
    {0,0,'A','S','D','F','G','H','J','K','L',':','"','\n'},
    {0,0,0,'Z','X','C','V','B','N','M','<','>','?',' '},
};

static bool s_adv = false;
static char s_last;   // raw char last reported (edge detection, GPIO backend)

// ---- shared: a layout char -> key event ------------------------------------
static nucleo_key_t map_char(char c)
{
    if (c == 0) return (nucleo_key_t){NK_NONE, 0};
    switch (c) {
        case '\n': return (nucleo_key_t){NK_ENTER, 0};
        case '\b': return (nucleo_key_t){NK_DEL, 0};
        case '`':  return (nucleo_key_t){NK_BACK, 0};      // top-left = Esc/back
        case ';':  return (nucleo_key_t){NK_UP, ';'};      // arrow legends on ; . , /
        case '.':  return (nucleo_key_t){NK_DOWN, '.'};
        case ',':  return (nucleo_key_t){NK_LEFT, ','};
        case '/':  return (nucleo_key_t){NK_RIGHT, '/'};
        case '\t': return (nucleo_key_t){NK_TAB, 0};
        default:   return (c >= 32) ? (nucleo_key_t){NK_CHAR, c} : (nucleo_key_t){NK_NONE, 0};
    }
}

// Is (x,y) the SHIFT key?
static inline bool is_shift_xy(int x, int y) { return x == 1 && y == 2; }
static inline bool is_mod_xy(int x, int y)
{ return (x == 0 && y == 2) || (x == 0 && y == 3) || (x == 1 && y == 3) || (x == 2 && y == 3); }

// Map a modifier key position to an NK_MOD_* bit. Best-effort labelling (the matrix only tells
// us a key is a non-printing modifier, not which one) — the USB Keyboard app shows the live state
// so a wrong guess is a one-line fix here. Verify with the keyboard diagnostic on real hardware.
static uint8_t mod_for_xy(int x, int y)
{
    if (x == 0 && y == 2) return NK_MOD_CTRL;
    if (x == 0 && y == 3) return NK_MOD_FN;
    if (x == 1 && y == 3) return NK_MOD_ALT;
    if (x == 2 && y == 3) return NK_MOD_GUI;
    return 0;
}

// Held modifiers, updated by each backend's read (see nucleo_kbd_mods).
static uint8_t s_mods;         // GPIO backend: recomputed every scan
static uint8_t s_tca_modbits;  // TCA backend: tracked across press/release events

// Live pressed state of every printable key (indexed by unshifted layout char), for
// nucleo_kbd_char_down(): GPIO backend rebuilds it each scan, TCA backend tracks press/release.
static bool s_down[128];
static inline void down_set(char base, bool on) { if ((unsigned char)base < 128) s_down[(unsigned char)base] = on; }

// ============================================================================
//  TCA8418 backend (Cardputer ADV)
// ============================================================================
#define TCA_ADDR        0x34
#define TCA_REG_CFG     0x01
#define TCA_REG_INT_STAT 0x02
#define TCA_REG_KEY_EVENT_A 0x04   // FIFO: read pops one event (Adafruit_TCA8418 / datasheet)
#define TCA_REG_KEY_LCK_EC  0x03   // low nibble = pending event count
#define TCA_REG_KP_GPIO1 0x1D      // rows  0..7 as keypad
#define TCA_REG_KP_GPIO2 0x1E      // cols  0..7 as keypad
#define TCA_REG_KP_GPIO3 0x1F      // cols  8..9 as keypad

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static char s_tca_shift = 0;
static char s_tca_held = 0;        // layout char currently down (0 = none), for auto-repeat
static int64_t s_tca_rep_us = 0;   // earliest time the held key may repeat

static void tca_w(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    i2c_master_transmit(s_dev, b, 2, 50);
}
static uint8_t tca_r(uint8_t reg)
{
    uint8_t v = 0;
    i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, 50);
    return v;
}

static bool tca_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,                       // auto-pick a free port
        .sda_io_num = 8, .scl_io_num = 9,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .trans_queue_depth = 0,               // synchronous transfers (the init burst needs it)
        .flags = { .enable_internal_pullup = true },
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) return false;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = TCA_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) return false;
    // Select the 7x8 keypad matrix and enable key-event reporting.
    tca_w(TCA_REG_KP_GPIO1, 0x7F);            // ROW0..6
    tca_w(TCA_REG_KP_GPIO2, 0xFF);            // COL0..7
    tca_w(TCA_REG_KP_GPIO3, 0x00);
    tca_w(TCA_REG_CFG, 0x01);                 // KE_IEN: accumulate key events in the FIFO
    tca_w(TCA_REG_INT_STAT, 0x03);            // clear any stale interrupt flags
    // Drain the key-event FIFO: a warm reset leaves the TCA8418 powered, and configuring the
    // matrix can latch a spurious press. An undrained event makes the very first nucleo_kbd_read()
    // report a phantom key — which silently skips the boot splash (any key cancels it) so the ADV
    // boots to a black screen while the original (GPIO matrix, no FIFO) plays it fully.
    for (int i = 0; (tca_r(TCA_REG_KEY_LCK_EC) & 0x0F) && i < 16; i++) tca_r(TCA_REG_KEY_EVENT_A);
    return true;
}

static nucleo_key_t tca_read(void)
{
    int64_t now = esp_timer_get_time();
    // No fresh FIFO event: synthesise an auto-repeat for the held nav/edit key (the TCA only
    // reports key-up/down, never "still held", so the repeat clock lives here).
    if ((tca_r(TCA_REG_KEY_LCK_EC) & 0x0F) == 0) {
        if (s_tca_held && key_repeats(s_tca_held) && now >= s_tca_rep_us) {
            s_tca_rep_us = now + KEY_REPEAT_RATE_US;
            return map_char(s_tca_held);
        }
        return (nucleo_key_t){NK_NONE, 0};
    }
    uint8_t ev = tca_r(TCA_REG_KEY_EVENT_A);
    bool pressed = ev & 0x80;
    int code = (ev & 0x7F);
    if (code == 0) return (nucleo_key_t){NK_NONE, 0};
    code--;                                   // 0-based key number
    int row = code / 10, col = code % 10;     // TCA8418 uses 10-wide internal addressing
    // Remap to the Cardputer 4x14 picture coordinates (shared KM tables).
    int x = row * 2 + (col > 3 ? 1 : 0);
    int y = (col + 4) % 4;
    if (x < 0 || x > 13 || y < 0 || y > 3) return (nucleo_key_t){NK_NONE, 0};

    if (is_shift_xy(x, y)) { s_tca_shift = pressed; return (nucleo_key_t){NK_NONE, 0}; }
    if (is_mod_xy(x, y)) {
        uint8_t b = mod_for_xy(x, y);
        if (pressed) s_tca_modbits |= b; else s_tca_modbits &= ~b;
        return (nucleo_key_t){NK_NONE, 0};
    }
    char base = KM[y][x];                                   // unshifted char, for the live pressed-key set
    char lc = s_tca_shift ? KM_SH[y][x] : KM[y][x];
    if (!pressed) {                                          // key released
        down_set(base, false);
        if (lc == s_tca_held) s_tca_held = 0;               // stop repeating it
        return (nucleo_key_t){NK_NONE, 0};
    }
    down_set(base, true);
    s_tca_held = lc;                                        // fresh press: arm the repeat clock
    s_tca_rep_us = now + KEY_REPEAT_DELAY_US;
    return map_char(lc);
}

// ============================================================================
//  GPIO 74HC138 backend (original Cardputer) — unchanged
// ============================================================================
static void gpio_init(void)
{
    gpio_config_t out = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 0 };
    for (int i = 0; i < 3; i++) out.pin_bit_mask |= (1ULL << ADDR[i]);
    gpio_config(&out);
    gpio_config_t in = { .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pin_bit_mask = 0 };
    for (int i = 0; i < 7; i++) in.pin_bit_mask |= (1ULL << ROW[i]);
    gpio_config(&in);
}

static void set_addr(int i)
{
    gpio_set_level(ADDR[0], i & 1);
    gpio_set_level(ADDR[1], (i >> 1) & 1);
    gpio_set_level(ADDR[2], (i >> 2) & 1);
}

// Scan the matrix; return the first non-modifier key char (with shift applied), or 0.
static char scan_char(void)
{
    uint8_t mods = 0;
    char found = 0;
    memset(s_down, 0, sizeof s_down);                    // rebuild the live pressed-key set each scan
    for (int i = 0; i < 8; i++) {
        set_addr(i);
        esp_rom_delay_us(15);
        for (int j = 0; j < 7; j++) {
            if (gpio_get_level(ROW[j]) != 0) continue;   // active-low: pressed == 0
            int x = (i > 3) ? (2 * j) : (2 * j + 1);
            int y = 3 - (i % 4);
            if (is_shift_xy(x, y)) mods |= NK_MOD_SHIFT;
            else if (is_mod_xy(x, y)) mods |= mod_for_xy(x, y);
            else { down_set(KM[y][x], true); if (!found) found = KM[y][x]; }   // record ALL held printables
        }
    }
    s_mods = mods;                                       // published for nucleo_kbd_mods()
    if (found && (mods & NK_MOD_SHIFT)) {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 14; x++)
                if (KM[y][x] == found) return KM_SH[y][x];
    }
    return found;
}

static int64_t s_gpio_rep_us = 0;   // earliest time the held key may repeat

static nucleo_key_t gpio_read(void)
{
    char c = scan_char();
    if (c == 0) { s_last = 0; return (nucleo_key_t){NK_NONE, 0}; }
    int64_t now = esp_timer_get_time();
    if (c != s_last) {                                    // fresh press: fire now, arm repeat
        s_last = c;
        s_gpio_rep_us = now + KEY_REPEAT_DELAY_US;
        return map_char(c);
    }
    if (key_repeats(c) && now >= s_gpio_rep_us) {          // held nav/edit key: auto-repeat
        s_gpio_rep_us = now + KEY_REPEAT_RATE_US;
        return map_char(c);
    }
    return (nucleo_key_t){NK_NONE, 0};
}

// ============================================================================
//  Public API
// ============================================================================
void nucleo_kbd_init(void)
{
    s_adv = nucleo_ui_is_adv();
    if (s_adv && tca_init()) {
        ESP_LOGI("kbd", "Cardputer ADV keyboard (TCA8418 @0x34)");
    } else {
        if (s_adv) ESP_LOGW("kbd", "ADV detected but TCA8418 init failed -> GPIO fallback");
        s_adv = false;                        // original, or ADV init failed -> GPIO matrix
        gpio_init();
    }
}

// Non-blocking: returns a freshly pressed key, or {NK_NONE,0} if nothing new.
nucleo_key_t nucleo_kbd_read(void)
{
    return s_adv ? tca_read() : gpio_read();
}

// Modifiers held right now (NK_MOD_* bitmask).
unsigned char nucleo_kbd_mods(void)
{
    return s_adv ? (unsigned char)((s_tca_shift ? NK_MOD_SHIFT : 0) | s_tca_modbits) : s_mods;
}

// Is a printable key physically held down right now? (see header)
bool nucleo_kbd_char_down(char c)
{
    return (unsigned char)c < 128 && s_down[(unsigned char)c];
}

// Shared system I2C bus (ADV only; created by tca_init). NULL on the original board.
void *nucleo_kbd_i2c_bus(void) { return (void *)s_bus; }
