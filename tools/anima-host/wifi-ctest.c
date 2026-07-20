// Host gate for the Wi-Fi supervisor decision core (firmware/components/nucleo_setup/wifi_policy.c).
// Proves the four invariants that fix the GitHub issue #3 family ("authentication required" /
// "incorrect password" joining the hotspot, AP flapping/left down, toggle lies) on the PC, before
// anything touches a Cardputer:
//   I1  never scan/join while the soft-AP has a client or recent client activity (grace window)
//   I2  a join attempt with a live AP interface runs in APSTA (beacons continue), never pure STA
//   I3  after a failed join cycle the radio settles back to a beaconing AP (driver-mode based)
//   I4  a failed one-shot manual join never arms the background retry loop
// Compiled by tools/anima-host/wifi-check.mjs (`npm run wifi:test`).
#include <stdio.h>
#include "wifi_policy.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// Supervisor tick cadence (matches the firmware's 2 s loop).
#define TICK_MS 2000u
#define MIN_MS  8000u
#define MAX_MS  60000u
#define GRACE_MS 90000u

static wp_tick_in_t in_fallback(void)   // STA intended, no IP, hotspot up, nobody on it
{
    wp_tick_in_t in = { .auto_join = true, .have_ip = false, .link_alive = false,
                        .driver_sta_count = 0, .mode = WP_MODE_AP };
    return in;
}

static void t_mode_rules(void)
{
    CHECK(wp_join_mode(WP_MODE_NULL)  == WP_MODE_STA,   "join mode from NULL");
    CHECK(wp_join_mode(WP_MODE_STA)   == WP_MODE_STA,   "join mode from STA");
    CHECK(wp_join_mode(WP_MODE_AP)    == WP_MODE_APSTA, "I2: AP up -> join in APSTA");
    CHECK(wp_join_mode(WP_MODE_APSTA) == WP_MODE_APSTA, "I2: APSTA -> join stays APSTA");

    CHECK(!wp_ap_iface_up(WP_MODE_NULL) && !wp_ap_iface_up(WP_MODE_STA), "AP iface down states");
    CHECK(wp_ap_iface_up(WP_MODE_AP) && wp_ap_iface_up(WP_MODE_APSTA),   "AP iface up states");

    CHECK(wp_need_ap_restore(WP_MODE_NULL) && wp_need_ap_restore(WP_MODE_STA),
          "I3: restore needed when the AP interface is gone");
    CHECK(!wp_need_ap_restore(WP_MODE_AP) && !wp_need_ap_restore(WP_MODE_APSTA),
          "I3: no restore while an AP interface is up");
}

static void t_join_result_auto(void)
{
    CHECK(wp_join_result_auto(false, true)  == true,  "success arms the supervisor");
    CHECK(wp_join_result_auto(true,  true)  == true,  "success keeps it armed");
    CHECK(wp_join_result_auto(true,  false) == true,  "failure keeps a previous STA intent");
    CHECK(wp_join_result_auto(false, false) == false, "I4: failure never arms the retry loop");
}

static void t_idle_when_not_auto(void)
{
    wp_state_t s; wp_init(&s, 0);
    wp_tick_in_t in = in_fallback();
    in.auto_join = false;
    for (uint32_t t = 0; t < 10 * MIN_MS; t += TICK_MS)
        CHECK(wp_tick(&s, &in, t) == WP_ACT_IDLE, "explicit hotspot: supervisor stays IDLE (t=%u)", t);
    CHECK(s.backoff_ms == MIN_MS, "backoff reset while idle");
}

static void t_first_attempt_immediate(void)
{
    wp_state_t s; wp_init(&s, 1000);
    wp_tick_in_t in = in_fallback();
    CHECK(wp_tick(&s, &in, 1000) == WP_ACT_TRY_JOIN, "quiet fallback: first attempt eligible at once");
}

static void t_client_defers_and_quiet_gap_after(void)
{
    wp_state_t s; wp_init(&s, 0);
    wp_tick_in_t in = in_fallback();
    uint32_t t = 0;

    // Client associated the whole time: every tick defers, no join ever fires. (I1)
    in.driver_sta_count = 1;
    uint32_t leave = 120000;
    for (; t < leave; t += TICK_MS)
        CHECK(wp_tick(&s, &in, t) == WP_ACT_DEFER, "client on AP: DEFER (t=%u)", t);

    // Client leaves WITHOUT an activity stamp (poll-only view): still a full backoff_min of
    // quiet air before the next attempt, because DEFER kept pushing the retry window.
    in.driver_sta_count = 0;
    uint32_t first_join = 0;
    for (; t < leave + 4 * MIN_MS && !first_join; t += TICK_MS)
        if (wp_tick(&s, &in, t) == WP_ACT_TRY_JOIN) first_join = t;
    CHECK(first_join >= leave - TICK_MS + MIN_MS, "quiet gap after the client left (join at %u)", first_join);
    CHECK(first_join != 0, "retry does resume after the client left");
}

static void t_grace_window(void)
{
    wp_state_t s; wp_init(&s, 0);
    // A client connect/disconnect stamps activity; the AP is busy for the whole grace window.
    wp_ap_activity(&s, 50000);
    CHECK(wp_ap_busy(&s, 0, 50000),                 "I1: busy at the stamp");
    CHECK(wp_ap_busy(&s, 0, 50000 + GRACE_MS - 1),  "I1: busy 1 ms before grace end");
    CHECK(!wp_ap_busy(&s, 0, 50000 + GRACE_MS),     "free exactly at grace end");
    CHECK(wp_ap_busy(&s, 1, 50000 + GRACE_MS + 1),  "driver count overrides an elapsed grace");
    wp_state_t f; wp_init(&f, 0);
    CHECK(!wp_ap_busy(&f, 0, 0), "no activity ever seen: not busy at t=0 (stamp-0 ambiguity)");
}

static void t_backoff_ladder(void)
{
    wp_state_t s; wp_init(&s, 0);
    uint32_t expect[] = { 16000, 32000, 60000, 60000, 60000 };   // 8 doubles, capped at 60
    uint32_t now = 0;
    for (int i = 0; i < 5; i++) {
        wp_cycle_done(&s, false, now);
        CHECK(s.backoff_ms == expect[i], "failure %d -> backoff %u (got %u)", i + 1, expect[i], s.backoff_ms);
        CHECK(s.next_retry_ms == now + expect[i], "failure %d re-arms the window", i + 1);
        now += expect[i];
    }
    wp_cycle_done(&s, true, now);
    CHECK(s.backoff_ms == MIN_MS, "success resets the backoff");

    // The armed window is respected: IDLE until it elapses, TRY_JOIN after.
    wp_state_t w; wp_init(&w, 0);
    wp_tick_in_t in = in_fallback();
    CHECK(wp_tick(&w, &in, 0) == WP_ACT_TRY_JOIN, "cycle starts");
    wp_cycle_done(&w, false, 0);                      // -> next at 16 s
    CHECK(wp_tick(&w, &in, TICK_MS) == WP_ACT_IDLE,   "window not elapsed: IDLE");
    CHECK(wp_tick(&w, &in, 16000) == WP_ACT_TRY_JOIN, "window elapsed: retry");
}

static void t_stale_link(void)
{
    wp_state_t s; wp_init(&s, 0);
    wp_tick_in_t in = { .auto_join = true, .have_ip = true, .link_alive = true,
                        .driver_sta_count = 0, .mode = WP_MODE_STA };
    CHECK(wp_tick(&s, &in, 0) == WP_ACT_IDLE, "healthy link: IDLE");
    in.link_alive = false;
    CHECK(wp_tick(&s, &in, 2000) == WP_ACT_IDLE, "dead poll 1: tolerated");
    CHECK(wp_tick(&s, &in, 4000) == WP_ACT_IDLE, "dead poll 2: tolerated");
    CHECK(wp_tick(&s, &in, 6000) == WP_ACT_RECONNECT, "dead poll 3: force reconnect");
    CHECK(wp_tick(&s, &in, 8000) == WP_ACT_IDLE, "counter reset after the reconnect");
    in.link_alive = true;
    wp_tick(&s, &in, 10000);
    in.link_alive = false;
    CHECK(wp_tick(&s, &in, 12000) == WP_ACT_IDLE, "an alive poll resets the miss counter");
}

static void t_drop_ap_when_idle(void)
{
    wp_state_t s; wp_init(&s, 0);
    wp_tick_in_t in = { .auto_join = true, .have_ip = true, .link_alive = true,
                        .driver_sta_count = 0, .mode = WP_MODE_APSTA };
    CHECK(wp_tick(&s, &in, 0) == WP_ACT_DROP_AP, "joined + idle hotspot -> drop the AP");
    in.mode = WP_MODE_STA;
    CHECK(wp_tick(&s, &in, 2000) == WP_ACT_IDLE, "no AP up -> nothing to drop");
    in.mode = WP_MODE_APSTA; in.driver_sta_count = 1;
    CHECK(wp_tick(&s, &in, 4000) == WP_ACT_IDLE, "client still on the hotspot -> keep it");
    in.driver_sta_count = 0;
    wp_ap_activity(&s, 6000);                        // client just left
    CHECK(wp_tick(&s, &in, 8000) == WP_ACT_IDLE, "recent activity -> keep it through grace");
    CHECK(wp_tick(&s, &in, 6000 + GRACE_MS) == WP_ACT_DROP_AP, "grace elapsed -> drop it");
}

static void t_wraparound(void)
{
    // A device up for ~49.7 days: uint32 ms wraps. Schedule near the wrap and cross it.
    wp_state_t s; wp_init(&s, 0xFFFFF000u);
    wp_tick_in_t in = in_fallback();
    CHECK(wp_tick(&s, &in, 0xFFFFF000u) == WP_ACT_TRY_JOIN, "eligible at init near wrap");
    wp_cycle_done(&s, false, 0xFFFFF000u);           // next retry at 0xFFFFF000 + 16000 (wraps)
    CHECK(wp_tick(&s, &in, 0xFFFFFF00u) == WP_ACT_IDLE, "pre-wrap, window not elapsed: IDLE");
    CHECK(wp_tick(&s, &in, 0x00003000u) == WP_ACT_TRY_JOIN, "post-wrap, window elapsed: retry");
    wp_ap_activity(&s, 0xFFFFFFF0u);                 // activity just before the wrap
    CHECK(wp_ap_busy(&s, 0, 0x00000010u), "grace window spans the wrap");
    CHECK(!wp_ap_busy(&s, 0, 0xFFFFFFF0u + GRACE_MS), "grace ends correctly across the wrap");
}

// The exact GitHub-issue story, replayed at policy level: device in AP fallback (saved net not
// joinable), a phone associates to the hotspot, uses it, fails/leaves — the supervisor must not
// fire a single scan/join from the moment the phone shows up until grace expires after it left.
static void t_issue_story(void)
{
    wp_state_t s; wp_init(&s, 0);
    wp_tick_in_t in = in_fallback();
    uint32_t t = 0;
    int joins_while_protected = 0;

    // Boot: a couple of failed cycles first (net not joinable) — AP must be restored each time.
    for (int cyc = 0; cyc < 2; cyc++) {
        while (wp_tick(&s, &in, t) != WP_ACT_TRY_JOIN) t += TICK_MS;
        // During the cycle the radio went APSTA (I2) and the join failed:
        CHECK(wp_join_mode(WP_MODE_AP) == WP_MODE_APSTA, "story: cycle keeps the AP beaconing");
        wp_cycle_done(&s, false, t);
        CHECK(!wp_need_ap_restore(WP_MODE_APSTA), "story: APSTA after the cycle needs no restart");
        CHECK(wp_need_ap_restore(WP_MODE_STA), "story: a pure-STA leftover WOULD demand a restart");
    }

    // t0: the phone associates (driver sees it + event stamps activity).
    uint32_t t0 = t + TICK_MS;
    wp_ap_activity(&s, t0);
    in.driver_sta_count = 1;
    // The phone stays 3 minutes (web UI use), then leaves.
    uint32_t t1 = t0 + 180000;
    for (t = t0; t < t1; t += TICK_MS)
        if (wp_tick(&s, &in, t) == WP_ACT_TRY_JOIN) joins_while_protected++;
    wp_ap_activity(&s, t1);                          // disconnect event
    in.driver_sta_count = 0;
    // Protected until t1 + grace.
    for (t = t1; t < t1 + GRACE_MS; t += TICK_MS)
        if (wp_tick(&s, &in, t) == WP_ACT_TRY_JOIN) joins_while_protected++;
    CHECK(joins_while_protected == 0, "I1: ZERO scan/join from association to grace end (got %d)",
          joins_while_protected);

    // After the grace the supervisor resumes hunting for the home network.
    uint32_t resumed = 0;
    for (; t < t1 + GRACE_MS + 3 * MAX_MS && !resumed; t += TICK_MS)
        if (wp_tick(&s, &in, t) == WP_ACT_TRY_JOIN) resumed = t;
    CHECK(resumed >= t1 + GRACE_MS, "story: retries resume only after the grace");
    CHECK(resumed != 0, "story: retries DO resume (home Wi-Fi still gets rejoined)");
}

int main(void)
{
    t_mode_rules();
    t_join_result_auto();
    t_idle_when_not_auto();
    t_first_attempt_immediate();
    t_client_defers_and_quiet_gap_after();
    t_grace_window();
    t_backoff_ladder();
    t_stale_link();
    t_drop_ap_when_idle();
    t_wraparound();
    t_issue_story();
    printf("wifi-policy: %d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
