// Host harness for the firmware notification backbone's PURE pieces — no ESP-IDF, no hardware.
// Compiles notify_synth.h + notify_journal.h (the same headers the firmware includes) with MinGW
// GCC and asserts the WAV synthesis and the size-rotated journal behave. Build+run via build.ps1
// (or: gcc -std=gnu11 -O0 notify_check.c -I<nucleo_app> -lm -o notify_check && ./notify_check).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "notify_synth.h"
#include "notify_journal.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); g_fail++; } } while (0)
// Quiet variant for tight loops (only speaks up on failure).
#define CHECK_SILENT(cond) do { if (!(cond)) { printf("  FAIL bound violated\n"); g_fail++; } } while (0)

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }

// Validate one synthesized earcon WAV: header sane + audible (non-silent) + no clipping (headroom).
static void check_wav(notify_src_t src, notify_snd_t lvl, const char *name, const char *path)
{
    printf("[earcon] %s -> %s\n", name, path);
    int rc = notify_synth_wav(src, lvl, path, 0);     // 0 -> NOTIFY_SND_RATE
    CHECK(rc == 0, "synth returned 0");

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "wav file opened");
    if (!f) return;

    uint8_t h[44];
    size_t got = fread(h, 1, 44, f);
    CHECK(got == 44, "read 44-byte header");
    CHECK(memcmp(h, "RIFF", 4) == 0, "RIFF magic");
    CHECK(memcmp(h + 8, "WAVE", 4) == 0, "WAVE magic");
    CHECK(memcmp(h + 12, "fmt ", 4) == 0, "fmt chunk");
    CHECK(rd16(h + 20) == 1, "PCM format");
    CHECK(rd16(h + 22) == 1, "mono (1 channel)");
    CHECK(rd32(h + 24) == (uint32_t)NOTIFY_SND_RATE, "sample rate == NOTIFY_SND_RATE");
    CHECK(rd16(h + 34) == 16, "16-bit samples");
    CHECK(memcmp(h + 36, "data", 4) == 0, "data chunk");

    uint32_t data_len = rd32(h + 40);
    struct stat st; stat(path, &st);
    CHECK((long)data_len == (long)st.st_size - 44, "data_len matches file size");

    // Scan samples: find peak. Audible but with headroom (no clipping at full scale).
    int peak = 0, nonzero = 0; long n = data_len / 2;
    for (long i = 0; i < n; i++) {
        uint8_t b[2]; if (fread(b, 1, 2, f) != 2) break;
        int16_t s = (int16_t)rd16(b);
        int a = s < 0 ? -s : s; if (a > peak) peak = a; if (a > 200) nonzero++;
    }
    fclose(f);
    printf("       samples=%ld peak=%d (%.1f%% FS) voiced=%ld\n", n, peak, peak * 100.0 / 32767.0, (long)nonzero);
    CHECK(peak > 24000, "normalized to a strong, audible level (~80% FS)");
    CHECK(peak < 32767, "headroom kept (does not clip at full scale)");
    CHECK(n > NOTIFY_SND_RATE * 0.3, "duration >= ~0.3 s");
    CHECK(n < NOTIFY_SND_RATE * 1.2, "duration <= ~1.2 s (short, not annoying)");
}

static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void check_journal(void)
{
    const char *path = "build/notify.jsonl";
    char bak[300]; snprintf(bak, sizeof bak, "%s.0", path);
    remove(path); remove(bak);
    printf("[journal] append + size rotation (cap=512 B)\n");

    const long cap = 512;
    const char *line = "{\"src\":\"system\",\"lvl\":\"info\",\"ttl\":\"riga di prova abbastanza lunga\",\"ts\":1700000000}";
    int rotated = 0;
    for (int i = 0; i < 60; i++) {
        notify_journal_append(path, line, cap);
        if (file_exists(bak)) rotated = 1;
        CHECK_SILENT(fsize(path) <= cap + (long)strlen(line) + 2);   // live file never far past cap
    }
    CHECK(rotated, "rotated to .0 backup once over cap");
    CHECK(fsize(path) <= cap + (long)strlen(line) + 2, "live file stays bounded (~cap)");
    CHECK(fsize(bak) >= 0, "backup file exists and is readable");

    // No-cap mode keeps appending without rotation.
    const char *path2 = "build/notify_nocap.jsonl";
    char bak2[300]; snprintf(bak2, sizeof bak2, "%s.0", path2);
    remove(path2); remove(bak2);
    for (int i = 0; i < 30; i++) notify_journal_append(path2, line, 0);
    CHECK(!file_exists(bak2), "no rotation when cap disabled");
}

// Two earcon files must NOT be byte-identical (different source or level => different sound).
static int files_differ(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int differ = 0, ca, cb;
    do { ca = fgetc(fa); cb = fgetc(fb); if (ca != cb) { differ = 1; break; } } while (ca != EOF);
    fclose(fa); fclose(fb);
    return differ;
}

int main(void)
{
    MKDIR("build");
    // Source x level combos — exercises the signature (who) + chord (urgency) fusion.
    check_wav(NOTIFY_SRC_SYSTEM,   NOTIFY_SND_INFO,     "system/info",     "build/n_sys_info.wav");
    check_wav(NOTIFY_SRC_CALENDAR, NOTIFY_SND_INFO,     "calendar/info",   "build/n_cal_info.wav");
    check_wav(NOTIFY_SRC_ANIMA,    NOTIFY_SND_SUCCESS,  "anima/success",   "build/n_ai_succ.wav");
    check_wav(NOTIFY_SRC_OTA,      NOTIFY_SND_WARN,     "ota/warn",        "build/n_ota_warn.wav");
    check_wav(NOTIFY_SRC_SYSTEM,   NOTIFY_SND_CRITICAL, "system/critical", "build/n_sys_crit.wav");

    printf("[identity] earcons are distinguishable\n");
    CHECK(files_differ("build/n_cal_info.wav", "build/n_sys_info.wav"), "same level, different source => different earcon");
    CHECK(files_differ("build/n_sys_info.wav", "build/n_sys_crit.wav"), "same source, different level => different earcon");

    check_journal();

    printf("\n%s  (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
