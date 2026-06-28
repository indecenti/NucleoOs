// Host gate for the OTA listening-handshake FSM (ota_listen_fsm.h) — the SAME pure code the firmware run
// loop runs. Proves the user's spec on the PC, no Cardputer needed:
//   OTA request -> launch Remote Control -> proceed only when the posture is live -> on end, restore as before.
// Built+run by tools/anima-host/ota-listen-check.mjs  (npm run otalisten:test).
#include "ota_listen_fsm.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
    else         { printf("  ok  : %s\n", msg); } \
} while (0)

// Drive one tick and report the action, mutating st in place (mirrors the run loop).
static ota_listen_action_t tick(ota_listen_state_t *st, bool force, bool active_is_remote)
{
    st->force = force;
    st->active_is_remote = active_is_remote;
    return ota_listen_step(st);
}

int main(void)
{
    printf("OTA listening-handshake FSM gate\n");

    // ---- Scenario A: OTA from the launcher (no app open) — the from-boot case ----
    printf("[A] OTA from launcher -> auto-launch -> ready -> restore\n");
    {
        ota_listen_state_t s = { 0 };
        // 1) request lands, nothing is foreground -> we must LAUNCH Remote Control and take ownership
        ota_listen_action_t a = tick(&s, true, /*active_is_remote=*/false);
        CHECK(a == OTA_LISTEN_LAUNCH, "tick1 launches Remote Control");
        CHECK(s.owned == true,        "tick1 marks ownership (we opened it)");
        CHECK(s.ready == false,       "tick1 not ready yet (canvas not freed until enter runs)");
        // 2) Remote Control is now foreground (its enter freed the RAM) -> READY for ota_post to proceed
        a = tick(&s, true, /*active_is_remote=*/true);
        CHECK(a == OTA_LISTEN_READY,  "tick2 signals READY");
        CHECK(s.ready == true,        "tick2 ready latch set");
        CHECK(s.owned == true,        "tick2 still owns it");
        // 3) OTA ends (httpd dropped the flag) while Remote Control is up and OWNED -> RESTORE (close it)
        a = tick(&s, false, /*active_is_remote=*/true);
        CHECK(a == OTA_LISTEN_RESTORE, "tick3 restores (closes the Remote Control we opened)");
        CHECK(s.owned == false,        "tick3 drops ownership");
        CHECK(s.ready == false,        "tick3 clears ready");
        // 4) settled back to launcher, no OTA -> nothing
        a = tick(&s, false, /*active_is_remote=*/false);
        CHECK(a == OTA_LISTEN_NONE,    "tick4 idle, nothing to do");
    }

    // ---- Scenario B: user already has Remote Control open by hand — we must NOT close it on end ----
    printf("[B] OTA with user-opened Remote Control -> ready, but NEVER auto-closed\n");
    {
        ota_listen_state_t s = { 0 };
        ota_listen_action_t a = tick(&s, true, /*active_is_remote=*/true);
        CHECK(a == OTA_LISTEN_READY, "ready immediately (posture already live)");
        CHECK(s.owned == false,      "ownership NOT taken (user opened it, not us)");
        CHECK(s.ready == true,       "ready latch set");
        a = tick(&s, false, /*active_is_remote=*/true);
        CHECK(a == OTA_LISTEN_NONE,  "on end: leaves the user's Remote Control open (no RESTORE)");
        CHECK(s.ready == false,      "ready cleared on end");
    }

    // ---- Scenario C: OTA arrives while a different app is foreground -> still launches ----
    printf("[C] OTA from inside another app -> closes it, launches Remote Control\n");
    {
        ota_listen_state_t s = { 0 };
        ota_listen_action_t a = tick(&s, true, /*active_is_remote=*/false);
        CHECK(a == OTA_LISTEN_LAUNCH, "launches (will close the foreground app)");
        CHECK(s.owned == true,        "owns it");
        a = tick(&s, true, true);
        CHECK(a == OTA_LISTEN_READY,  "ready once Remote Control is foreground");
    }

    // ---- Scenario D: no OTA, never touched -> never acts (no spurious launches/closes) ----
    printf("[D] No OTA -> FSM is inert\n");
    {
        ota_listen_state_t s = { 0 };
        for (int i = 0; i < 5; i++) {
            ota_listen_action_t a = tick(&s, false, (i & 1) != 0);
            CHECK(a == OTA_LISTEN_NONE, "inert while no OTA pending");
        }
        CHECK(s.ready == false && s.owned == false, "no state leaked");
    }

    // ---- Scenario E: ready must drop the instant the OTA flag clears (ota_post must re-block) ----
    printf("[E] ready latch is edge-honest\n");
    {
        ota_listen_state_t s = { 0 };
        tick(&s, true, true);                  // -> ready
        CHECK(s.ready == true,  "ready while OTA active");
        tick(&s, false, false);                // flag cleared, posture gone
        CHECK(s.ready == false, "ready cleared the moment the OTA ends");
    }

    printf(fails ? "\nFSM gate: %d FAIL\n" : "\nFSM gate: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
