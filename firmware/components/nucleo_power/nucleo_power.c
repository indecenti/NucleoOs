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

static const char *TAG = "power";

void nucleo_power_init(void)
{
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
#define BAT_REFRESH_US  2000000   // re-sample at most every 2 s; UI can call us every frame
#define BAT_EMA_DEN     4         // EMA over refreshes: smooth = (smooth*(N-1) + fresh) / N
#define BAT_MIN_MV      3300      // anything below this we treat as flat / sensor floor

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static adc_channel_t              s_chan;
static adc_unit_t                 s_unit;
static bool    s_adc_ok;      // channel configured -> raw reads work
static bool    s_cali_ok;     // eFuse calibration present -> raw->mV is trustworthy
static int     s_mv_ema = -1; // smoothed terminal voltage (mV); -1 until the first sample
static int64_t s_next_us;     // earliest time the cache may refresh
static SemaphoreHandle_t s_lock;

// Single-cell LiPo SoC curve (terminal mV under ACTIVE load -> % SoC), descending. Two fixes over the
// old table, which read ~4% at 3.55 V while Bruce's linear map gives ~32% there — far closer to truth:
// (1) the old curve was wildly pessimistic across the whole mid-range (3.80 V -> 40% vs a real ~58%);
// (2) we sample while Wi-Fi + CPU are busy, so the terminal voltage sags ~150-250 mV below the resting
// curve — this table is calibrated for that LOADED reading, not a resting cell. Keeps the LiPo plateau
// shape (fairly flat 3.7-4.0 V) a straight 3.3-4.2 V line would over-report. De-divided terminal mV.
static const struct { int mv; int pct; } LIPO[] = {
    {4200,100},{4150, 96},{4100, 90},{4050, 84},{4000, 78},{3950, 72},
    {3900, 66},{3850, 60},{3800, 54},{3750, 48},{3700, 42},{3650, 36},
    {3600, 30},{3550, 24},{3500, 18},{3450, 12},{3400,  7},{3350,  3},
    {3300,  0},
};

static int mv_to_pct(int mv)
{
    if (mv >= LIPO[0].mv) return 100;
    int n = (int)(sizeof(LIPO) / sizeof(LIPO[0]));
    if (mv <= LIPO[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= LIPO[i].mv) {
            // Linear interpolation inside the [i-1, i] segment.
            int dv = LIPO[i - 1].mv - LIPO[i].mv;
            int dp = LIPO[i - 1].pct - LIPO[i].pct;
            return LIPO[i].pct + (mv - LIPO[i].mv) * dp / dv;
        }
    }
    return 0;
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

// Sample the ADC and fold the result into the EMA. Caller holds s_lock.
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
    int mv = (int)(adc_mv * BAT_DIVIDER + 0.5f);
    if (mv < BAT_MIN_MV - 400) return;   // implausibly low -> no cell / bad read, don't poison the EMA

    if (s_mv_ema < 0) s_mv_ema = mv;     // seed instantly so the first reading isn't lagged
    else s_mv_ema = (s_mv_ema * (BAT_EMA_DEN - 1) + mv) / BAT_EMA_DEN;
}

// Lazily init, then refresh at most every BAT_REFRESH_US. Returns smoothed mV or -1.
static int battery_mv_locked(void)
{
    static bool tried;
    if (!tried) { tried = true; battery_setup(); }
    if (!s_adc_ok) return -1;
    int64_t now = esp_timer_get_time();
    if (now >= s_next_us) { battery_refresh(); s_next_us = now + BAT_REFRESH_US; }
    return s_mv_ema;
}

static int battery_mv(void)
{
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); if (!s_lock) return -1; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return s_mv_ema;
    int mv = battery_mv_locked();
    xSemaphoreGive(s_lock);
    return mv;
}

bool nucleo_power_battery_available(void) { return battery_mv() >= 0; }

int nucleo_power_battery_mv(void) { return battery_mv(); }

int nucleo_power_battery_pct(void)
{
    int mv = battery_mv();
    return mv < 0 ? -1 : mv_to_pct(mv);
}
