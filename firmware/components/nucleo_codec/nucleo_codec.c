// ES8311 codec driver for the Cardputer ADV. See nucleo_codec.h for the board story.
#include "nucleo_codec.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "nucleo_board.h"
#include "esp_log.h"
#include <string.h>   // memmove for the decimation carry
#include <stdlib.h>   // malloc/free for the heap-on-open native-rate scratch

#define ES8311_ADDR 0x18
// The ES8311 on the ADV has NO dedicated MCLK wire: it derives its master clock from the BCLK pin
// (reg 0x01/0x02), so the codec only clocks CORRECTLY at one rate — 48 kHz, the rate Bruce ships and
// the only one that satisfies the codec's fixed DIG_MCLK = LRCK*256 = BCLK*4 constraint here. We
// therefore CAPTURE at 48 kHz and DECIMATE to the caller's requested rate (16 kHz) in software, so
// the WAV, the Vosk dictation stream and the Spectrum FFT all stay at a true 16 kHz. Requesting
// 16 kHz directly mis-clocked the ADC -> audio captured ~3x too long and pitched down (the "slow
// recording" bug). The original Cardputer's PDM mic clocks fine at 16 kHz, so it is left untouched.
#define ES8311_NATIVE_RATE 48000
static const char *TAG = "codec";

static i2c_master_dev_handle_t s_dev;   // NULL = codec absent (original Cardputer)

// Decimation state for the ADV capture path (single mic owner -> a single shared scratch is safe;
// the recorder, voice stream and Spectrum are mutually exclusive). s_decim = native/requested
// (3 for 48k->16k, 1 for the PDM path = no resampling). See nucleo_codec_mic_read.
static int     s_decim = 1;
// Native-rate scratch (ADV decimation path only). Was a permanent 3 KB .bss array; now allocated
// heap-on-open (nucleo_codec_mic_open) and freed on close, so it costs ZERO RAM while idle — and
// NOTHING on the original Cardputer, whose PDM path (s_decim == 1) never touches it. Freeing 3 KB of
// always-resident .bss matters on this no-PSRAM chip where the web OS already grazes the heap floor.
#define NAT_CAP 1536            // holds up to 512 output samples worth at 3x decimation
static int16_t *s_nat = NULL;
static int     s_nat_rem = 0;   // native samples carried over from the previous read (keeps phase)

// One {reg,val} write. Short timeout: a wedged bus must never stall audio start.
static void wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    i2c_master_transmit(s_dev, b, sizeof(b), 50);
}
static void wr_seq(const uint8_t seq[][2], int n) { for (int i = 0; i < n; i++) wr(seq[i][0], seq[i][1]); }

void nucleo_codec_init(void *i2c_bus)
{
    if (!i2c_bus) return;                 // original Cardputer: no ES8311 on the bus
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = ES8311_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device((i2c_master_bus_handle_t)i2c_bus, &dc, &s_dev) != ESP_OK) {
        s_dev = NULL;
        ESP_LOGW(TAG, "ES8311 add_device failed -> audio stays silent");
        return;
    }
    // Start SILENT. An unconfigured ES8311 hisses on the speaker from boot (the DAC/output driver come
    // up live), and nothing plays audio until the user does — so mute the DAC and power its output +
    // analog stages DOWN here. The first nucleo_codec_speaker(true) re-powers everything before playback.
    static const uint8_t silent[][2] = {
        {0x00,0x80},  // CSM power on so the writes below take effect
        {0x31,0x60},  // DAC mute
        {0x13,0x00},  // disable output to the HP/line driver
        {0x12,0x02},  // power down DAC
        {0x0D,0xFC},  // power down analog
    };
    wr_seq(silent, sizeof(silent) / sizeof(silent[0]));
    ESP_LOGI(TAG, "ES8311 @0x18 on system I2C (Cardputer ADV) - started muted");
}

bool nucleo_codec_present(void) { return s_dev != NULL; }

// ES8311 -> speaker (DAC). Clock stays on the proven Cardputer-ADV path (MCLK from the SCK
// pin + pre-multiplier; the ADV routes no dedicated MCLK so the coeff-table path needs a
// clock we don't have). On top of Bruce's bare-minimum we pin the I2S SDP to 16-bit (matches
// our stream), un-mute the DAC and bypass its EQ — register meanings per the Espressif
// es8311 driver, so the codec interprets our frames correctly instead of relying on resets.
void nucleo_codec_speaker(bool enable)
{
    if (!s_dev) return;
    if (!enable) { wr(0x31, 0x60); return; }   // DAC mute — click-free; kills the idle/boot hiss when nothing plays
    static const uint8_t on[][2] = {
        {0x00,0x80},  // CSM power on
        {0x01,0xB5},  // clock manager: MCLK taken from the SCK pin
        {0x02,0x18},  // pre-divider / x8 pre-multiplier -> usable internal clock from SCK
        {0x09,0x0C},  // SDP IN  = 16-bit I2S (matches the stream)
        {0x0A,0x0C},  // SDP OUT = 16-bit I2S
        {0x0D,0x01},  // power up analog
        {0x12,0x00},  // power up DAC
        {0x13,0x10},  // enable output to the HP/line driver
        {0x31,0x00},  // DAC un-mute
        {0x32,0xBF},  // DAC volume 0 dB
        {0x37,0x08},  // bypass DAC equalizer
    };
    wr_seq(on, sizeof(on) / sizeof(on[0]));
}

// ES8311 -> microphone (ADC). Gain staging matches the Espressif es8311 driver defaults
// (max analog PGA + ADC volume), which are notably hotter than Bruce's bare-minimum, plus
// 16-bit SDP out and the ADC high-pass / DC-offset cancel for a clean capture.
void nucleo_codec_mic(bool enable)
{
    if (!s_dev) return;
    static const uint8_t on[][2] = {
        {0x00,0x80},  // CSM power on
        {0x01,0xBA},  // clock manager (ADC), MCLK from SCK
        {0x02,0x18},  // pre-divider / pre-multiplier
        {0x09,0x0C},  // SDP IN  = 16-bit
        {0x0A,0x0C},  // SDP OUT = 16-bit (ADC -> I2S, matches our read)
        {0x0D,0x01},  // power up analog
        {0x0E,0x02},  // enable analog PGA + ADC modulator
        {0x14,0x1A},  // select analog mic + max analog PGA gain (Espressif default)
        {0x17,0xC8},  // ADC volume (Espressif default; well above Bruce's minimum)
        {0x1C,0x6A},  // ADC HPF + digital DC-offset cancel
    };
    static const uint8_t off[][2] = { {0x0D,0xFC},{0x0E,0x6A},{0x00,0x00} };
    if (enable) wr_seq(on,  sizeof(on)  / sizeof(on[0]));
    else        wr_seq(off, sizeof(off) / sizeof(off[0]));
}

esp_err_t nucleo_codec_mic_open(int rate, i2s_chan_handle_t *out)
{
    *out = NULL;
    s_nat_rem = 0;                  // fresh session: drop any decimation carry from a prior take
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan, NULL, &rx);
    if (err != ESP_OK) return err;
    if (s_dev) {
        // ADV: ES8311 ADC over standard I2S. Shares BCLK/WS with the speaker; the codec's SDOUT
        // lands on the mic data pin (46). Capture at the codec's native 48 kHz (see ES8311_NATIVE_RATE)
        // with Bruce's proven slot timing — left-justified, 16-bit slot, LEFT mask, 16-clk WS — then
        // decimate to `rate` in nucleo_codec_mic_read. Power the ADC before the clocks start.
        nucleo_codec_mic(true);
        i2s_std_config_t std = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)ES8311_NATIVE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = NUCLEO_SPK_PIN_BCLK, .ws = NUCLEO_SPK_PIN_WS,
                          .dout = I2S_GPIO_UNUSED, .din = NUCLEO_MIC_PIN_DATA, .invert_flags = { 0 } },
        };
        std.clk_cfg.clk_src      = I2S_CLK_SRC_PLL_160M;     // Bruce's source for the 12.288 MHz @48k
        std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
        std.slot_cfg.slot_mask   = I2S_STD_SLOT_LEFT;
        std.slot_cfg.ws_width    = 16;
        std.slot_cfg.bit_shift   = true;
        std.slot_cfg.left_align  = true;
        err = i2s_channel_init_std_mode(rx, &std);
        s_decim = (rate > 0) ? (ES8311_NATIVE_RATE + rate / 2) / rate : 1;   // 48000/16000 = 3
        if (s_decim < 1) s_decim = 1;
        // Grab the native-rate scratch only when we'll actually decimate. Freed in mic_close.
        if (s_decim > 1 && !s_nat) {
            s_nat = malloc(NAT_CAP * sizeof(int16_t));   // internal RAM (no PSRAM on this chip)
            if (!s_nat) err = ESP_ERR_NO_MEM;            // the shared cleanup below deletes the channel
        }
    } else {
        // Original Cardputer: PDM mic (SPM1423) on CLK43/DIN46 — clocks fine at the requested rate.
        i2s_pdm_rx_config_t cfg = {
            .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)rate),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = { .clk = NUCLEO_MIC_PIN_CLK, .din = NUCLEO_MIC_PIN_DATA, .invert_flags = { .clk_inv = false } },
        };
        err = i2s_channel_init_pdm_rx_mode(rx, &cfg);
        s_decim = 1;                                          // no resampling on the PDM path
    }
    if (err == ESP_OK) err = i2s_channel_enable(rx);
    if (err != ESP_OK) { i2s_del_channel(rx); if (s_dev) nucleo_codec_mic(false); return err; }
    *out = rx;
    return ESP_OK;
}

// Read mono 16-bit PCM at the rate requested in nucleo_codec_mic_open, hiding the ADV's native-rate
// capture: on the PDM path this is a pass-through to i2s_channel_read; on the ADV it reads native
// 48 kHz samples and box-averages every `s_decim` of them into one output sample (a cheap anti-alias
// decimation). Mirrors i2s_channel_read's contract — *got is OUTPUT bytes, a partial/timeout read
// returns the bytes it has — so callers can keep their existing read loops. out_cap must be a
// multiple of 2 (whole int16 samples). Native remainder is carried across calls so no phase drifts.
esp_err_t nucleo_codec_mic_read(i2s_chan_handle_t rx, void *out, size_t out_cap, size_t *got, uint32_t ticks)
{
    if (got) *got = 0;
    if (!rx || !out || out_cap < 2) return ESP_ERR_INVALID_ARG;
    int factor = s_decim < 1 ? 1 : s_decim;
    if (factor == 1 || !s_nat)                        // PDM / original, or scratch unavailable: byte-for-byte
        return i2s_channel_read(rx, out, out_cap, got, ticks);

    int16_t *o = (int16_t *)out;
    int out_cap_s = (int)(out_cap / 2);               // output samples wanted
    int budget_s = NAT_CAP / factor;                  // max output the scratch holds
    if (out_cap_s > budget_s) out_cap_s = budget_s;
    int want_nat = out_cap_s * factor;                // native samples needed for this chunk

    size_t got_b = 0;
    esp_err_t rd = ESP_OK;
    int need = want_nat - s_nat_rem;                  // top up past whatever we carried over
    if (need > 0) rd = i2s_channel_read(rx, s_nat + s_nat_rem, (size_t)need * 2, &got_b, ticks);
    int avail = s_nat_rem + (int)(got_b / 2);

    int n_out = avail / factor;
    if (n_out > out_cap_s) n_out = out_cap_s;
    for (int i = 0; i < n_out; i++) {
        int32_t acc = 0;
        for (int k = 0; k < factor; k++) acc += s_nat[i * factor + k];
        o[i] = (int16_t)(acc / factor);
    }
    int used = n_out * factor, rem = avail - used;
    if (rem > 0) memmove(s_nat, s_nat + used, (size_t)rem * 2);
    s_nat_rem = rem;
    if (got) *got = (size_t)n_out * 2;
    return rd;
}

void nucleo_codec_mic_close(i2s_chan_handle_t rx)
{
    if (s_nat) { free(s_nat); s_nat = NULL; }   // release the 3 KB native-rate scratch (heap-on-open)
    s_nat_rem = 0;
    if (!rx) return;
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    if (s_dev) nucleo_codec_mic(false);
}
