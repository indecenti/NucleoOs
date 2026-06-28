#include "nucleo_power.h"
#include "esp_pm.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The battery mutex lives in static storage (xSemaphoreCreateMutexStatic) so the gauge's lock costs
// ZERO heap. Static allocation is on by default in ESP-IDF; fail the build loudly if it is ever off.
#if !defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION)
#error "nucleo_power: enable CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION for the zero-heap battery mutex"
#endif

static const char *TAG = "power";

// Serialises battery_mv() across the launcher (UI) task and the HTTP server task. Storage is static
// (BSS), so creating the mutex allocates nothing on the heap.
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

// Create the lock in place inside s_lock_buf (no heap). Called once from nucleo_power_init() while
// the system is still single-threaded; the lazy guard only covers a battery read that beats init.
static SemaphoreHandle_t battery_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    return s_lock;
}

void nucleo_power_init(void)
{
    battery_lock();   // bring up the zero-heap battery mutex once, before any task can race on it

    // DFS only: max == the configured default CPU freq (240 MHz, set in sdkconfig.defaults),
    // min 80 MHz. We keep min at 80 (not 40) so the Wi-Fi radio stays stable while idle.
    // light_sleep_enable = false: the launcher + HTTP server must remain instantly
    // responsive and the device reachable, and light sleep is unavailable anyway with the
    // USB-Serial/JTAG console. DFS alone already saves meaningful power when idle.
    esp_pm_config_t cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pm configure failed: %s (running at fixed frequency)", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "DFS enabled: %d-%d MHz, light-sleep off", cfg.min_freq_mhz, cfg.max_freq_mhz);
}

// ---- Battery gauge ---------------------------------------------------------------------
// M5Cardputer wires the cell to G10 through a 2:1 resistor divider, so the ADC sees half the
// real voltage. Same pin Bruce uses (-D ANALOG_BAT_PIN=10). G10 maps to an ADC1 channel on the
// ESP32-S3; we let the driver resolve GPIO->unit/channel so the pin number stays the only constant.
#define BAT_GPIO        10
#define BAT_DIVIDER     2.0f      // VBAT = Vadc * 2 (matches Bruce's ANALOG_BAT_MULTIPLIER)
#define BAT_SAMPLES     16        // raw reads averaged per refresh — kills per-read ADC jitter
#define BAT_REFRESH_US  2000000   // re-read at most every 2 s; the UI can call us every frame
#define BAT_MIN_MV      3300      // Bruce's 0% anchor (de-divided terminal mV)
#define BAT_MAX_MV      4100      // Bruce's 100% anchor (3300 + 800 mV span)

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static adc_channel_t              s_chan;
static adc_unit_t                 s_unit;
static bool    s_adc_ok;      // channel configured -> raw reads work
static bool    s_cali_ok;     // eFuse calibration present -> raw->mV is trustworthy
static int     s_mv = -1;     // last terminal voltage (mV); -1 until the first reading lands
static int64_t s_next_us;     // earliest time the cache may refresh

// Bruce-style LINEAR map: terminal mV -> %, 0% at BAT_MIN_MV, 100% at BAT_MAX_MV, clamped 0..100
// (matches getBattery() in Bruce's src/core/utils.cpp). No SoC curve, no EMA, no load-sag modelling.
// The M5Cardputer has no fuel-gauge IC and — like Bruce — no charge-detect line, so a voltage-only
// reading is inherently a coarse estimate and reads ~full while on USB; trying to be cleverer than
// this only produced a confidently-wrong number. Keep it plain and honest.
static int mv_to_pct(int mv)
{
    int pct = (mv - BAT_MIN_MV) * 100 / (BAT_MAX_MV - BAT_MIN_MV);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// Bring up ADC oneshot + calibration once. Cheap to leave installed for the whole session.
static void battery_setup(void)
{
    if (adc_oneshot_io_to_channel(BAT_GPIO, &s_unit, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "G%d is not an ADC pin on this chip; battery gauge disabled", BAT_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = s_unit };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc unit init failed; battery gauge disabled");
        return;
    }
    // 12 dB attenuation -> full-scale ~3.1 V, enough headroom for a full cell halved (~2.1 V).
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK) {
        ESP_LOGW(TAG, "adc channel config failed; battery gauge disabled");
        adc_oneshot_del_unit(s_adc); s_adc = NULL;
        return;
    }
    s_adc_ok = true;

    adc_cali_curve_fitting_config_t calcfg = {
        .unit_id  = s_unit,
        .chan     = s_chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&calcfg, &s_cali) == ESP_OK);
    if (!s_cali_ok) ESP_LOGW(TAG, "no ADC calibration eFuse; battery voltage is approximate");
    ESP_LOGI(TAG, "battery gauge on G%d (ADC%d ch%d, cali=%d)", BAT_GPIO, s_unit + 1, s_chan, s_cali_ok);
}

// Read the ADC and cache the terminal voltage. Caller holds s_lock. This is the IDF equivalent of
// Bruce's analogReadMilliVolts(): one calibrated reading (denoised over a few raw samples here), then
// de-divided. No smoothing across time — the value the UI shows IS the latest reading.
static void battery_refresh(void)
{
    long acc = 0; int got = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) { acc += raw; got++; }
    }
    if (got == 0) return;
    int raw_avg = (int)(acc / got);

    int adc_mv;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw_avg, &adc_mv) != ESP_OK) return;
    } else {
        // Fallback: 12-bit full scale over ~3100 mV. Coarse, but better than nothing.
        adc_mv = raw_avg * 3100 / 4095;
    }
    s_mv = (int)(adc_mv * BAT_DIVIDER + 0.5f);   // de-divided terminal mV; mapped to % on read
    ESP_LOGW(TAG, "[bat-diag] raw=%d adc_mv=%d term_mv=%d pct=%d cali=%d",  // TEMP: gauge bring-up
             raw_avg, adc_mv, s_mv, mv_to_pct(s_mv), (int)s_cali_ok);
}

// Lazily init, then re-read at most every BAT_REFRESH_US. Returns the cached terminal mV, or -1.
static int battery_mv_locked(void)
{
    static bool tried;
    if (!tried) { tried = true; battery_setup(); }
    if (!s_adc_ok) return -1;
    int64_t now = esp_timer_get_time();
    if (now >= s_next_us) { battery_refresh(); s_next_us = now + BAT_REFRESH_US; }
    return s_mv;
}

static int battery_mv(void)
{
    SemaphoreHandle_t lock = battery_lock();
    if (!lock) return s_mv;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) return s_mv;
    int mv = battery_mv_locked();
    xSemaphoreGive(lock);
    return mv;
}

bool nucleo_power_battery_available(void) { return battery_mv() >= 0; }

int nucleo_power_battery_mv(void) { return battery_mv(); }

int nucleo_power_battery_pct(void)
{
    int mv = battery_mv();
    return mv < 0 ? -1 : mv_to_pct(mv);
}
