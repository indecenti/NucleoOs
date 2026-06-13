// ANIMA Phase 0 on-device micro-benchmark (docs/anima.md §8).
//
// Measures the three numbers that decide whether the L1 retrieval tier is feasible
// without ever risking the "one token per minute" regime:
//   1) int8 MAC throughput   -> bounds the encoder forward-pass time
//   2) Hamming/popcount rate  -> bounds the binary coarse-search corpus size
//   3) SD sequential read MB/s -> bounds the rerank cluster read cost
//
// Roofline estimates only (they ignore attention/activation/memory overhead), so treat
// the derived encoder time as an optimistic lower bound. Enable with
// CONFIG_NUCLEO_ANIMA_BENCH and read the results over `idf.py -p COM3 monitor`.
#include "nucleo_anima.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nucleo_board.h"

static const char *TAG = "anima.bench";

// Sink kept volatile so the optimizer can't delete the measured loops.
static volatile int64_t g_sink;

// --- 1) int8 multiply-accumulate throughput ---------------------------------
static void bench_int8_mac(void)
{
    enum { N = 1024, ITER = 100000 };
    static int8_t a[N], b[N];
    for (int i = 0; i < N; i++) { a[i] = (int8_t)(i * 7 - 13); b[i] = (int8_t)(i * 3 + 5); }

    int64_t t0 = esp_timer_get_time();
    int32_t acc = 0;
    for (int it = 0; it < ITER; it++) {
        int32_t s = 0;
        for (int i = 0; i < N; i++) s += (int32_t)a[i] * (int32_t)b[i];
        acc += s;
    }
    int64_t dt = esp_timer_get_time() - t0;
    g_sink = acc;

    double macs = (double)N * ITER;
    double mmac_s = macs / (double)dt;                 // MAC per microsecond == MMAC/s
    // Roofline: a ~4M-param encoder, 32-token query ~= 2 * 4e6 * 32 MACs.
    double enc_macs = 2.0 * 4.0e6 * 32.0;
    double enc_ms = enc_macs / mmac_s / 1000.0;
    ESP_LOGI(TAG, "[1] int8 MAC: %.0f MMAC/s  (%.0f MMACs in %lld us)",
             mmac_s, macs / 1e6, (long long)dt);
    ESP_LOGI(TAG, "    -> encoder(4M,32tok) roofline ~ %.0f ms (optimistic lower bound)", enc_ms);
}

// --- 2) Hamming distance (XOR + popcount) throughput ------------------------
static void bench_popcount(void)
{
    enum { BITS = 128, WORDS = BITS / 64, COMPARES = 2000000 };
    uint64_t q[WORDS], db[WORDS];
    for (int w = 0; w < WORDS; w++) { q[w] = 0x0123456789abcdefULL ^ w; db[w] = 0xfedcba9876543210ULL + w; }

    int64_t t0 = esp_timer_get_time();
    int32_t acc = 0;
    for (int c = 0; c < COMPARES; c++) {
        int d = 0;
        for (int w = 0; w < WORDS; w++) d += __builtin_popcountll(q[w] ^ (db[w] + c));
        acc += d;
    }
    int64_t dt = esp_timer_get_time() - t0;
    g_sink = acc;

    double cmp_s = (double)COMPARES / ((double)dt / 1e6);
    ESP_LOGI(TAG, "[2] Hamming(%d-bit): %.1f M cmp/s  (%d cmp in %lld us)",
             BITS, cmp_s / 1e6, COMPARES, (long long)dt);
    ESP_LOGI(TAG, "    -> coarse scan in 50 ms ~ %.0fk vectors (linear, RAM-resident)",
             cmp_s * 0.050 / 1000.0);
}

// --- 3) SD sequential read throughput ---------------------------------------
static void bench_sd(void)
{
    const char *path = NUCLEO_SD_MOUNT "/anima_bench.tmp";
    enum { CHUNK = 16384, MB = 1024 * 1024, TOTAL = 1 * MB };
    static uint8_t buf[CHUNK];
    memset(buf, 0xA5, sizeof(buf));

    // Write a 1 MB scratch file, then close+reopen to read past most of the FATFS cache.
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "[3] SD: cannot open %s for write (card mounted?)", path); return; }
    for (int w = 0; w < TOTAL; w += CHUNK)
        if (fwrite(buf, 1, CHUNK, f) != CHUNK) { ESP_LOGW(TAG, "[3] SD: short write"); fclose(f); return; }
    fclose(f);

    f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "[3] SD: cannot reopen for read"); return; }
    int64_t t0 = esp_timer_get_time();
    size_t total = 0, n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) total += n;
    int64_t dt = esp_timer_get_time() - t0;
    fclose(f);
    remove(path);

    double mb_s = ((double)total / (1024.0 * 1024.0)) / ((double)dt / 1e6);
    ESP_LOGI(TAG, "[3] SD read: %.2f MB/s  (%u bytes in %lld us)",
             mb_s, (unsigned)total, (long long)dt);
    ESP_LOGI(TAG, "    -> 30 KB rerank cluster ~ %.1f ms per query", 30.0 / 1024.0 / mb_s * 1000.0);
}

// --- 4) SD RANDOM-read latency (the number that gates the L2 MOSAICO Sentence Bank) ----------
// The planned multi-GB Sentence Bank does coarse search in RAM (free) then RANDOM fseek+fread of
// small records scattered across a big file. Sequential MB/s (test 3) does NOT predict that — seek +
// SD command overhead dominates and is roughly size-independent. This measures avg latency per random
// small read and derives how many fit a 100 ms query budget == the max per-query candidate set, which
// is what decides whether the bank can scale (sub-second) or must stay a small/opt-in deep mode.
// 8 MB proxy file: true 10 GB latency may be marginally higher (more FAT cluster-chain traversal),
// mitigated by keeping the bank in few large, forward-mostly-read shards.
static void bench_sd_random(void)
{
    const char *path = NUCLEO_SD_MOUNT "/anima_bench_rnd.tmp";
    enum { FILEMB = 8, MB = 1024 * 1024, REC = 260, READS = 600, WCHUNK = 16384 };
    long fsize = (long)FILEMB * MB;
    static uint8_t wbuf[WCHUNK];
    memset(wbuf, 0x5A, sizeof wbuf);

    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "[4] SD rnd: cannot open %s (card mounted?)", path); return; }
    for (long w = 0; w < fsize; w += WCHUNK)
        if (fwrite(wbuf, 1, WCHUNK, f) != WCHUNK) { ESP_LOGW(TAG, "[4] SD rnd: short write"); fclose(f); return; }
    fclose(f);

    f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "[4] SD rnd: reopen failed"); return; }
    static uint8_t rec[REC];
    uint32_t st = 0x12345678u;                          // xorshift32: deterministic scattered offsets
    long maxoff = fsize - REC;
    int64_t t0 = esp_timer_get_time();
    int32_t acc = 0;
    int ok = 0;
    for (int i = 0; i < READS; i++) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        long off = (long)(st % (uint32_t)maxoff);
        if (fseek(f, off, SEEK_SET) != 0) continue;
        if (fread(rec, 1, REC, f) != REC) continue;
        acc += rec[0] + rec[REC - 1]; ok++;
    }
    int64_t dt = esp_timer_get_time() - t0;
    g_sink = acc;
    fclose(f);
    remove(path);

    if (!ok) { ESP_LOGW(TAG, "[4] SD rnd: no successful reads"); return; }
    double us_per = (double)dt / ok;
    ESP_LOGI(TAG, "[4] SD random: %.0f us / %d-byte seek+read  (%d reads over %d MB in %lld us)",
             us_per, REC, ok, FILEMB, (long long)dt);
    ESP_LOGI(TAG, "    -> ~%.0f random records / 100 ms  (== max per-query candidate reads at a 100 ms budget)",
             100000.0 / us_per);
    ESP_LOGI(TAG, "    -> rerank of 64 survivors ~ %.1f ms ; 256 survivors ~ %.1f ms",
             64.0 * us_per / 1000.0, 256.0 * us_per / 1000.0);
}

void nucleo_anima_benchmark(void)
{
    ESP_LOGI(TAG, "=== ANIMA Phase 0 benchmark (the three numbers) ===");
    bench_int8_mac();
    bench_popcount();
    bench_sd();
    bench_sd_random();          // [4] the number that gates the L2 Sentence Bank scaling
    ESP_LOGI(TAG, "=== done. See docs/anima.md §8 to interpret. ===");
}
