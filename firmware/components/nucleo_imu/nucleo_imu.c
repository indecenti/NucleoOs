// BMI270 6-axis IMU on the Cardputer ADV — tilt controller (accelerometer only; gyro/temp off).
// Pure ESP-IDF i2c_master, same arrangement as the ES8311 codec: attaches to the keyboard-owned
// system I2C bus and is a hard no-op on the original Cardputer.
//
// The BMI270 needs an ~8 KB config blob uploaded at init before it streams data (the well-known
// BMI270 quirk). Those bytes are Bosch's (BSD-3-Clause) and are NOT reproduced here by hand: run
//     python tools/gen-bmi270-config.py
// once to vendor them into bmi270_config.h. Without that header this file still compiles and simply
// reports "absent" (the tilt feature stays hidden / off), so the build never breaks.

#include "nucleo_imu.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"      // esp_rom_delay_us
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("bmi270_config.h")
#    include "bmi270_config.h"   // const uint8_t BMI270_CONFIG_FILE[]; + BMI270_CONFIG_FILE_LEN
#    define IMU_HAVE_CONFIG 1
#  endif
#endif

// --- tilt axis mapping (the one thing that depends on how the BMI270 sits on the ADV board) ------
// IMU_AXIS_X / IMU_AXIS_Y pick which gravity component drives screen horizontal / vertical
// (0=accel X, 1=Y, 2=Z); IMU_SX / IMU_SY flip the sign. Defaults match the common Cardputer
// mounting (long edge = accel X = left/right, short edge = accel Y = front/back). If on your unit
// an axis is swapped or inverted, change these four numbers — no other code moves.
#ifndef IMU_AXIS_X
#define IMU_AXIS_X 0
#endif
#ifndef IMU_AXIS_Y
#define IMU_AXIS_Y 1
#endif
#ifndef IMU_SX
#define IMU_SX (+1)
#endif
#ifndef IMU_SY
#define IMU_SY (+1)
#endif

static const char *TAG = "imu";

// --- BMI270 registers ---
#define R_CHIP_ID    0x00      // expect 0x24
#define R_ACC_DATA   0x0C      // ACC_X_LSB .. ACC_Z_MSB (6 bytes, little-endian int16)
#define R_INT_STATUS 0x21      // low nibble (message) == 0x01 means config init OK
#define R_ACC_CONF   0x40
#define R_ACC_RANGE  0x41
#define R_INIT_CTRL  0x59
#define R_INIT_ADDR0 0x5B
#define R_INIT_ADDR1 0x5C
#define R_INIT_DATA  0x5E
#define R_PWR_CONF   0x7C
#define R_PWR_CTRL   0x7D
#define R_CMD        0x7E
#define BMI270_CHIP_ID 0x24

static i2c_master_dev_handle_t s_dev;       // NULL = absent
static bool   s_up;                         // streaming
static bool   s_lpf_seeded;
static float  s_fx, s_fy, s_fz;             // low-passed accel (raw LSB)
static float  s_nx, s_ny, s_nz;             // neutral unit vector
static bool   s_have_neutral, s_recenter_pending;
static nk_motion_state s_motion;            // coarse still/hand/move sense (independent of tilt neutral)
static nk_step_state   s_step;              // pedometer step counter (fed from every accel read)
static float s_lvx, s_lvy, s_lvz;           // dedicated precise gravity filter for the bubble level
static bool  s_lv_seeded;
static char  s_diag[160];                   // last init I2C scan / probe / config result (on-device "no sensor" screen)

static esp_err_t reg_r(uint8_t r, uint8_t *buf, size_t n) { return i2c_master_transmit_receive(s_dev, &r, 1, buf, n, 60); }

#ifdef IMU_HAVE_CONFIG
static esp_err_t reg_w(uint8_t r, uint8_t v) { uint8_t b[2] = { r, v }; return i2c_master_transmit(s_dev, b, 2, 60); }

// Retry a register write / a buffer transmit on a TRANSIENT I2C NACK. THIS is the real BMI270 bug on
// the ADV: the shared, weak-pull-up bus makes the chip occasionally NACK a config-burst write; a
// re-send ~300us later succeeds. (Small reads like CHIP_ID always pass, which is why detection works.)
static esp_err_t reg_w_retry(uint8_t r, uint8_t v)
{
    esp_err_t e = ESP_FAIL;
    for (int t = 0; t < 5; t++) { e = reg_w(r, v); if (e == ESP_OK) return e; esp_rom_delay_us(300); }
    return e;
}
static esp_err_t tx_retry(const uint8_t *buf, size_t len)
{
    esp_err_t e = ESP_FAIL;
    for (int t = 0; t < 5; t++) { e = i2c_master_transmit(s_dev, buf, len, 100); if (e == ESP_OK) return e; esp_rom_delay_us(300); }
    return e;
}

// Canonical Bosch upload: stream the blob to INIT_DATA in chunks, setting INIT_ADDR (in 16-bit
// words) before each chunk; then latch INIT_CTRL=1 and wait for INTERNAL_STATUS.message == 1.
// Append a short failure marker to the on-device diagnostic line and return false.
#define CFG_FAIL(...) do { size_t dn_ = strlen(s_diag); snprintf(s_diag + dn_, sizeof(s_diag) - dn_, __VA_ARGS__); return false; } while (0)

static bool upload_config(void)
{
    const uint8_t *cfg = BMI270_CONFIG_FILE;
    size_t len = BMI270_CONFIG_FILE_LEN;
    // Every write is NACK-retried (the real failure mode), and the marker pins which step finally gives up.
    if (reg_w_retry(R_PWR_CONF, 0x00) != ESP_OK) CFG_FAIL(" | cfgPWR");   // disable advanced power save
    esp_rom_delay_us(500);
    if (reg_w_retry(R_INIT_CTRL, 0x00) != ESP_OK) CFG_FAIL(" | cfgIC0");  // begin config load
    const size_t CHUNK = 16;                 // small bursts ACK far more reliably on the weak-pull-up bus
    uint8_t tmp[1 + 16];
    for (size_t i = 0; i < len; i += CHUNK) {
        size_t n = (len - i < CHUNK) ? (len - i) : CHUNK;
        uint16_t w = (uint16_t)(i / 2);      // address counter is in 16-bit words
        if (reg_w_retry(R_INIT_ADDR0, (uint8_t)(w & 0x0F)) != ESP_OK ||
            reg_w_retry(R_INIT_ADDR1, (uint8_t)((w >> 4) & 0xFF)) != ESP_OK) CFG_FAIL(" | cfgADDR@%u", (unsigned)i);
        tmp[0] = R_INIT_DATA;
        memcpy(&tmp[1], &cfg[i], n);
        if (tx_retry(tmp, 1 + n) != ESP_OK) CFG_FAIL(" | cfgWR@%u", (unsigned)i);
    }
    if (reg_w_retry(R_INIT_CTRL, 0x01) != ESP_OK) CFG_FAIL(" | cfgIC1");  // latch config done
    vTaskDelay(pdMS_TO_TICKS(20));           // let the ASIC start processing before the first status read
    uint8_t st = 0;
    for (int i = 0; i < 30; i++) {           // poll up to ~320ms for INTERNAL_STATUS.message == init_ok
        // ESP32 quirk (Bosch Sensortec forum): the FIRST read of INTERNAL_STATUS after a config load
        // returns a STALE 0; a back-to-back second read returns the real value. Read twice, trust #2.
        reg_r(R_INT_STATUS, &st, 1);
        if (reg_r(R_INT_STATUS, &st, 1) == ESP_OK && (st & 0x0F) == 0x01) {
            ESP_LOGI(TAG, "BMI270 config OK (internal_status=0x%02X)", st);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "BMI270 config upload failed (internal_status=0x%02X)", st);
    CFG_FAIL(" | cfgST=0x%02x", st);
}
#endif

bool nucleo_imu_init(void *i2c_bus)
{
    s_dev = NULL; s_up = false; s_lpf_seeded = false; s_have_neutral = false; s_recenter_pending = false;
    nk_motion_reset(&s_motion);
    nk_step_reset(&s_step);
    s_lv_seeded = false;
    s_diag[0] = 0;
    if (!i2c_bus) return false;              // original Cardputer: no system I2C bus
#ifndef IMU_HAVE_CONFIG
    ESP_LOGW(TAG, "BMI270 config blob absent -> tilt disabled (run tools/gen-bmi270-config.py)");
    return false;
#else
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)i2c_bus;
    // DIAGNOSTIC: list every device answering on the system I2C bus, so we can see exactly where the
    // BMI270 sits (or that it isn't on this bus at all). Expect 0x18 (ES8311) + 0x34 (TCA8418) on the
    // ADV; the BMI270 should appear at 0x68 or 0x69. One boot log line resolves the wiring question.
    {
        char list[96]; int li = 0; list[0] = 0;
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (i2c_master_probe(bus, a, 20) == ESP_OK && li < (int)sizeof(list) - 6)
                li += snprintf(list + li, sizeof(list) - li, "0x%02x ", a);
        }
        ESP_LOGW(TAG, "system I2C scan: %s", list[0] ? list : "(no devices)");
        snprintf(s_diag, sizeof(s_diag), "bus: %s", list[0] ? list : "(vuoto)");
    }
    // Probe both possible 7-bit addresses (SDO low = 0x68, high = 0x69) by reading CHIP_ID. BMI270
    // cold-boot can need a few ms and a couple of dummy reads before CHIP_ID is valid, so retry.
    int found_addr = 0;
    uint8_t last_id = 0xFF; esp_err_t last_rd = ESP_FAIL;
    for (int a = 0; a < 2 && !found_addr; a++) {
        uint8_t addr = a ? 0x69 : 0x68;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = addr, .scl_speed_hz = 100000 };  // 100k: the 8KB config burst is marginal at 400k on the shared bus's weak pull-ups
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) continue;
        s_dev = dev;
        uint8_t id = 0; esp_err_t rd = ESP_FAIL;
        for (int t = 0; t < 5; t++) {        // retry: cold-boot first reads can be stale
            rd = reg_r(R_CHIP_ID, &id, 1);
            if (rd == ESP_OK && id == BMI270_CHIP_ID) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        last_id = id; last_rd = rd;
        ESP_LOGW(TAG, "probe 0x%02X: chip_id=0x%02X (read err=%d, want 0x24)", addr, id, (int)rd);
        if (rd == ESP_OK && id == BMI270_CHIP_ID) found_addr = addr;
        else { i2c_master_bus_rm_device(dev); s_dev = NULL; }
    }
    {   // record the probe outcome on the on-device diagnostic line
        size_t n = strlen(s_diag);
        snprintf(s_diag + n, sizeof(s_diag) - n, " | id=0x%02x rd=%d", last_id, (int)last_rd);
    }
    if (!s_dev) { ESP_LOGW(TAG, "BMI270 not found on system I2C"); return false; }

    reg_w(R_CMD, 0xB6);                      // soft reset
    vTaskDelay(pdMS_TO_TICKS(8));
    uint8_t id = 0; reg_r(R_CHIP_ID, &id, 1);
    if (id != BMI270_CHIP_ID) {
        ESP_LOGW(TAG, "BMI270 chip id 0x%02X after reset", id);
        size_t n = strlen(s_diag);
        snprintf(s_diag + n, sizeof(s_diag) - n, " | rst=0x%02x", id);
        return false;
    }

    // Config upload (8 KB blob) — up to 3 attempts, soft-reset between. s_diag keeps only the LAST
    // attempt's failure marker (reset to the probe info each try) so the on-screen line stays readable.
    size_t diag_base = strlen(s_diag);
    bool cfg_ok = false;
    for (int attempt = 0; attempt < 3 && !cfg_ok; attempt++) {
        s_diag[diag_base] = 0;
        if (attempt > 0) {
            ESP_LOGW(TAG, "BMI270 config retry %d", attempt);
            reg_w(R_CMD, 0xB6); vTaskDelay(pdMS_TO_TICKS(8));
        }
        cfg_ok = upload_config();
    }
    ESP_LOGW(TAG, "IMU diag: %s", s_diag);   // mirror the on-screen marker into /api/logs (WiFi debug)
    if (!cfg_ok) return false;   // upload_config recorded the precise step (cfgADDR/cfgWR/cfgST) in s_diag

    reg_w(R_PWR_CTRL, 0x04);                 // accelerometer ON (gyro/temp off — tilt only)
    reg_w(R_ACC_CONF, 0xA8);                 // 100 Hz, normal BWP, performance filter
    reg_w(R_ACC_RANGE, 0x00);                // +/-2g -> best tilt resolution
    reg_w(R_PWR_CONF, 0x02);                 // keep advanced power save OFF (continuous data)
    vTaskDelay(pdMS_TO_TICKS(10));
    s_up = true;
    ESP_LOGI(TAG, "BMI270 ready @0x%02X (tilt controller)", found_addr);
    return true;
#endif
}

bool nucleo_imu_present(void) { return s_up; }

const char *nucleo_imu_debug(void) { return s_diag; }

static bool read_accel_raw(float *ax, float *ay, float *az)
{
    if (!s_up) return false;
    uint8_t b[6];
    if (reg_r(R_ACC_DATA, b, 6) != ESP_OK) return false;
    *ax = (float)(int16_t)(b[0] | (b[1] << 8));
    *ay = (float)(int16_t)(b[2] | (b[3] << 8));
    *az = (float)(int16_t)(b[4] | (b[5] << 8));
    return true;
}

static bool read_accel(float *ax, float *ay, float *az)
{
    if (!read_accel_raw(ax, ay, az)) return false;
    nk_motion_push(&s_motion, *ax, *ay, *az);   // feeds the coarse motion sense (tilt math untouched)
    nk_step_push(&s_step, *ax, *ay, *az);        // feeds the pedometer
    return true;
}

void nucleo_imu_recenter(void) { s_recenter_pending = true; }

bool nucleo_imu_tilt(float *tx, float *ty)
{
    if (tx) *tx = 0;
    if (ty) *ty = 0;
    float ax, ay, az;
    if (!read_accel(&ax, &ay, &az)) return false;
    // low-pass the raw vector (kills high-frequency hand jitter)
    if (!s_lpf_seeded) { s_fx = ax; s_fy = ay; s_fz = az; s_lpf_seeded = true; }
    else { const float A = 0.30f; s_fx += (ax - s_fx) * A; s_fy += (ay - s_fy) * A; s_fz += (az - s_fz) * A; }
    float m = sqrtf(s_fx * s_fx + s_fy * s_fy + s_fz * s_fz);
    if (m < 1.0f) return false;              // implausible (bad read / free fall)
    float u[3] = { s_fx / m, s_fy / m, s_fz / m };
    if (s_recenter_pending || !s_have_neutral) {
        s_nx = u[0]; s_ny = u[1]; s_nz = u[2]; s_have_neutral = true; s_recenter_pending = false;
    }
    float n[3] = { s_nx, s_ny, s_nz };
    // tilt = deviation of the gravity direction from neutral, along the two chosen device axes.
    float dxv = (u[IMU_AXIS_X] - n[IMU_AXIS_X]) * (float)IMU_SX;
    float dyv = (u[IMU_AXIS_Y] - n[IMU_AXIS_Y]) * (float)IMU_SY;
    if (dxv > 1) dxv = 1; else if (dxv < -1) dxv = -1;
    if (dyv > 1) dyv = 1; else if (dyv < -1) dyv = -1;
    if (tx) *tx = dxv;
    if (ty) *ty = dyv;
    return true;
}

bool nucleo_imu_gravity_screen(float *gx, float *gy, float *gz)
{
    if (gx) *gx = 0;
    if (gy) *gy = 0;
    if (gz) *gz = 0;
    float ax, ay, az;
    if (!read_accel(&ax, &ay, &az)) return false;
    if (!s_lpf_seeded) { s_fx = ax; s_fy = ay; s_fz = az; s_lpf_seeded = true; }
    else { const float A = 0.30f; s_fx += (ax - s_fx) * A; s_fy += (ay - s_fy) * A; s_fz += (az - s_fz) * A; }
    float m = sqrtf(s_fx * s_fx + s_fy * s_fy + s_fz * s_fz);
    if (m < 1.0f) return false;
    float u[3] = { s_fx / m, s_fy / m, s_fz / m };
    int zc = 3 - IMU_AXIS_X - IMU_AXIS_Y;               // the remaining (screen-normal) axis
    if (gx) *gx = u[IMU_AXIS_X] * (float)IMU_SX;
    if (gy) *gy = u[IMU_AXIS_Y] * (float)IMU_SY;
    if (gz) *gz = u[zc];
    return true;
}

bool nucleo_imu_sample(void)
{
    // Plain single accel read (refreshes s_motion + s_step); false (no-op) when the IMU is absent. The
    // pedometer/dice/alarm call this at their loop rate and need every sample — do NOT throttle here.
    // The /api/status poll path applies its own ~4 Hz cap at the call site (nucleo_httpd.c).
    float a, b, c;
    return read_accel(&a, &b, &c);
}

nk_motion_t nucleo_imu_motion(void) { return s_up ? s_motion.state : NK_MOTION_UNKNOWN; }

const char *nucleo_imu_motion_str(void) { return nk_motion_name(nucleo_imu_motion()); }

nk_orient_t nucleo_imu_orient(void)
{
    return s_up ? nk_orient_classify(s_motion.gx, s_motion.gy, s_motion.gz) : NK_ORIENT_UNKNOWN;
}

const char *nucleo_imu_orient_str(void) { return nk_orient_name(nucleo_imu_orient()); }

float nucleo_imu_energy(void) { return s_up ? s_motion.energy : 0.0f; }

unsigned nucleo_imu_steps(void) { return s_up ? s_step.steps : 0; }
void     nucleo_imu_steps_reset(void) { nk_step_reset(&s_step); }

bool nucleo_imu_level(float *lx, float *ly, float *deg)
{
    if (lx) *lx = 0;
    if (ly) *ly = 0;
    if (deg) *deg = 0;
    if (!s_up) return false;
    // Precision: oversample to beat sensor noise, then a light EMA so the bubble is steady when held
    // yet still tracks within ~0.1 s. A DEDICATED filter (not the coarse motion gravity EMA).
    float ax = 0, ay = 0, az = 0; int got = 0;
    for (int i = 0; i < 6; i++) {
        float x, y, z;
        if (read_accel_raw(&x, &y, &z)) { ax += x; ay += y; az += z; got++; }
    }
    if (got) {
        ax /= got; ay /= got; az /= got;
        if (!s_lv_seeded) { s_lvx = ax; s_lvy = ay; s_lvz = az; s_lv_seeded = true; }
        else {
            const float A = 0.18f;
            s_lvx += (ax - s_lvx) * A; s_lvy += (ay - s_lvy) * A; s_lvz += (az - s_lvz) * A;
        }
    }
    if (!s_lv_seeded) return false;
    return nk_level(s_lvx, s_lvy, s_lvz, lx, ly, deg) != 0;
}
