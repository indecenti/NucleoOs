// Host gate for the OTA deferred confirm + trial (firmware/components/nucleo_app/include/ota_confirm_fsm.h),
// driven across REBOOTS by a small device model: two banks, the bootloader's rollback rule, the RTC trial
// marker and main.c's glue reduced to its decisions. Wired as `npm run otaconfirm:test` + the ANIMA gate.
//
// What must hold: a good image is never rolled back by a deliberate restart once the OS has run; an image that
// crashes before proving itself always goes back — including one confirmed EARLY by the web-profile handoff;
// a failed confirm is retried; a brown-out or power cut is not mistaken for a crash.
#include "ota_confirm_fsm.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_total = 0;
#define CHECK(cond, name) do { g_total++; if (!(cond)) { g_fail++; printf("FAIL %s\n", name); } } while (0)
#define S(x) ((int64_t)(x) * 1000000)

// ---- device model -----------------------------------------------------------------------------------
typedef enum { IMG_VALID, IMG_PENDING, IMG_INVALID } img_t;
typedef enum { RST_POWERON, RST_SW, RST_PANIC, RST_WDT, RST_BROWNOUT } rst_t;
static struct {
    int    running;                 // 0 = old firmware, 1 = new firmware
    img_t  state[2];                // per bank
    bool   trial;                   // RTC marker (survives every reset but power loss)
    bool   flash_fail;              // the next confirm attempts fail (flash busy / malloc)
    int    flash_fail_n;
} D;
static ocf_t F;                     // main.c's per-boot state
static bool  g_trial_clear_pending;

static void ota_installed(void)     // POST /api/ota on the old image: the new bank becomes the boot bank
{
    memset(&D, 0, sizeof D);
    D.running = 1; D.state[0] = IMG_VALID; D.state[1] = IMG_PENDING;
}
static bool crash(rst_t r) { return r == RST_PANIC || r == RST_WDT; }

// The bootloader, then app_main up to httpd. healthy=false: httpd never came up (degraded).
static void boot(rst_t why, bool healthy)
{
    // (the bootloader's own rollback of a PENDING image happens in reset(), before this boot)
    if (why == RST_POWERON || why == RST_BROWNOUT) D.trial = false;          // RTC memory lost
    memset(&F, 0, sizeof F); g_trial_clear_pending = false;
    // app_main, early: the trial verdict (main.c)
    if (D.trial) {
        if (ocf_trial_rollback(D.trial, crash(why))) {
            D.trial = false; D.state[D.running] = IMG_INVALID; D.running ^= 1;   // mark_app_invalid_rollback_and_reboot
            memset(&F, 0, sizeof F);
        } else g_trial_clear_pending = true;
    }
    // healthy boot of a pending image: arm the deferred confirm
    if (healthy && D.state[D.running] == IMG_PENDING) ocf_arm(&F);
}
// Any reset of a still-PENDING image: the bootloader rolls back before the app runs.
static void reset(rst_t why)
{
    if (why == RST_SW && ocf_on_restart(&F)) {                               // the shutdown handler
        bool ok = !D.flash_fail;
        if (ok) { D.state[D.running] = IMG_VALID; D.trial = true; F.armed = false; }
    }
    if (D.state[D.running] == IMG_PENDING) { D.state[D.running] = IMG_INVALID; D.running ^= 1; }
}
// The app loop running from t0 to t1 (seconds), one turn per 40 ms like the device.
static void run_loop(int64_t t0, int64_t t1)
{
    for (int64_t now = t0; now <= t1; now += 40000) {
        if (ocf_tick(&F, now)) {
            bool ok = !(D.flash_fail && D.flash_fail_n-- > 0);
            if (ok) D.state[D.running] = IMG_VALID;
            ocf_result(&F, ok, now);
        }
        if (g_trial_clear_pending && ocf_trial_cleared(&F, now)) { D.trial = false; g_trial_clear_pending = false; }
    }
}

int main(void)
{
    // A. The happy path: confirmed after 20 s of loop, not before; no trial left behind.
    ota_installed(); boot(RST_SW, true);
    run_loop(S(3), S(22));
    CHECK(D.state[1] == IMG_PENDING, "A: 19 s of loop -> still pending");
    run_loop(S(22) + 40000, S(24));
    CHECK(D.state[1] == IMG_VALID && D.running == 1 && !D.trial, "A: 20 s of loop -> confirmed, no trial");

    // B. Crash during start-up, before the loop (ANIMA / voice / registry): rolled back.
    ota_installed(); boot(RST_SW, true);
    reset(RST_PANIC);
    CHECK(D.running == 0, "B: panic before the loop -> previous firmware");

    // C. Crash in the loop at 10 s: still pending -> rolled back.
    ota_installed(); boot(RST_SW, true); run_loop(S(3), S(13)); reset(RST_WDT);
    CHECK(D.running == 0, "C: watchdog at 10 s -> previous firmware");

    // D. The web-profile handoff 2 s into the loop confirms EARLY (trial), and the web profile then crashes:
    //    the trial rolls the (valid) image back.
    ota_installed(); boot(RST_SW, true); run_loop(S(3), S(5)); reset(RST_SW);
    CHECK(D.state[1] == IMG_VALID && D.trial, "D: handoff after the loop ran -> confirmed early, trial set");
    boot(RST_SW, true); run_loop(S(2), S(7)); reset(RST_PANIC); boot(RST_PANIC, true);
    CHECK(D.running == 0 && D.state[1] == IMG_INVALID && !D.trial, "D: crash in the trial boot -> previous firmware");

    // E. Same early confirm, but the web profile runs fine for 20 s: trial cleared; a later crash is just a crash.
    ota_installed(); boot(RST_SW, true); run_loop(S(3), S(5)); reset(RST_SW);
    boot(RST_SW, true); run_loop(S(2), S(23));
    CHECK(!D.trial, "E: 20 s of loop in the trial boot -> trial cleared");
    reset(RST_PANIC); boot(RST_PANIC, true);
    CHECK(D.running == 1 && D.state[1] == IMG_VALID, "E: a later crash keeps the proven image");

    // F. A deliberate restart BEFORE the loop (a failure path at start-up): no confirm -> rolled back.
    ota_installed(); boot(RST_SW, true); reset(RST_SW);
    CHECK(D.running == 0, "F: esp_restart before the loop -> previous firmware");

    // G. A failed confirm is retried, never dropped.
    ota_installed(); boot(RST_SW, true); D.flash_fail = true; D.flash_fail_n = 2;
    run_loop(S(3), S(23) + 40000);
    CHECK(D.state[1] == IMG_PENDING && F.armed && F.tries >= 1, "G: first attempt fails -> still armed");
    run_loop(S(23) + 80000, S(35));
    CHECK(D.state[1] == IMG_VALID && !F.armed, "G: a retry 5 s later confirms");

    // H. Trial across a deliberate restart: kept (SW is no crash), then a crash in the next boot rolls back.
    ota_installed(); boot(RST_SW, true); run_loop(S(3), S(5)); reset(RST_SW);
    boot(RST_SW, true); run_loop(S(2), S(6)); reset(RST_SW);
    CHECK(D.trial && D.running == 1, "H: deliberate restart in the trial boot -> trial kept, image kept");
    boot(RST_SW, true); reset(RST_WDT); boot(RST_WDT, true);
    CHECK(D.running == 0, "H: then a watchdog reset -> previous firmware");

    // I. Brown-out / power cut during the trial: RTC memory is lost, nothing to judge -> image kept.
    ota_installed(); boot(RST_SW, true); run_loop(S(3), S(5)); reset(RST_SW);
    boot(RST_BROWNOUT, true);
    CHECK(D.running == 1 && !D.trial, "I: brown-out is not a crash and clears the trial");

    // J. Degraded boot (httpd down) of a pending image never arms; the loop alone never confirms it.
    ota_installed(); boot(RST_SW, false); run_loop(S(3), S(40));
    CHECK(D.state[1] == IMG_PENDING, "J: no healthy boot -> never confirmed");

    // K. Settled: what /api/sdcontent/arm and /api/reboot ask before restarting.
    { ocf_t f = { 0 };
      CHECK(ocf_settled(&f, false), "K: nothing pending -> settled");
      CHECK(!ocf_settled(&f, true), "K: pending, loop not yet run -> NOT settled (a restart would roll back)");
      ocf_tick(&f, S(1));
      CHECK(ocf_settled(&f, true), "K: pending, loop running -> settled (the handler confirms first)"); }

    // M. Early confirm by the SD-content arm restart, then the install boot (no app loop) crashes mid-download:
    //    main.c drops the marker when the install boot starts — a download crash says nothing about the firmware.
    ota_installed(); boot(RST_SW, true); run_loop(S(3), S(5)); reset(RST_SW);
    CHECK(D.trial, "M: early confirm by the arm restart -> on trial");
    boot(RST_SW, false);                                  // the install boot: no httpd, no loop
    if (g_trial_clear_pending) { D.trial = false; g_trial_clear_pending = false; }   // main.c: sdc_install && trial
    reset(RST_PANIC); boot(RST_PANIC, true);
    CHECK(D.running == 1 && D.state[1] == IMG_VALID, "M: a crash in the install boot keeps the firmware");

    // L. The deadline counts from the loop's FIRST turn, not from boot (a long blocking prologue proves nothing).
    { ocf_t f = { 0 }; ocf_arm(&f);
      CHECK(!ocf_tick(&f, S(60)), "L: first turn at 60 s -> not due yet");
      CHECK(!ocf_tick(&f, S(79)), "L: 19 s later -> not due");
      CHECK(ocf_tick(&f, S(80)), "L: 20 s after the first turn -> due"); }

    printf("ota-confirm: %d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
