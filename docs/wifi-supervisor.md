# Wi-Fi supervisor & hotspot policy

How NucleoOS decides when the background Wi-Fi supervisor may touch the radio, and how the
fallback hotspot is kept joinable. This design exists because of a real field failure
(GitHub issue #3): phones failing to join the hotspot with "authentication required" /
"incorrect password" / "couldn't authenticate connection", and an AP that flapped or stayed
down. Root causes and the contract that fixes them are below.

## Architecture

Two layers, split so the decision logic is host-testable:

- **`firmware/components/nucleo_setup/wifi_policy.c`** — the decision core. Pure C, zero
  ESP-IDF dependencies. Owns retry scheduling (backoff), the soft-AP busy/grace guard, the
  radio-mode rules, and the stale-IP detector. Compiled unchanged into the firmware **and**
  by the host gate: `npm run wifi:test` (tools/anima-host/wifi-check.mjs + wifi-ctest.c,
  registered as `gate-wifi-policy`).
- **`firmware/components/nucleo_setup/nucleo_setup.c`** — the mechanics. Samples the live
  driver state each 2 s tick, feeds it to `wp_tick()`, executes the returned action
  (`DEFER` / `TRY_JOIN` / `RECONNECT` / `DROP_AP` / `IDLE`), and forwards soft-AP client
  events (`AP_STACONNECTED` / `AP_STADISCONNECTED`) to `wp_ap_activity()`.

## The invariants (the contract)

- **I1 — never disturb a hotspot in use.** While any client is associated with the soft-AP
  (driver station list — a client mid WPA2 4-way handshake is already in it), or client
  activity happened within the **90 s grace window**, the supervisor performs no scan and no
  join attempt. The grace window is the part that protects a phone whose handshake just
  failed: its automatic retries must find quiet, on-channel air. The busy check also runs
  *between* join candidates inside a cycle (mid-cycle abort).
- **I2 — a join never takes a live hotspot down.** With the AP interface up, every STA
  connect attempt runs in **APSTA** (the AP keeps beaconing through it), never pure STA.
  The pre-fix code flipped to `WIFI_MODE_STA`, killing the AP for up to ~8 s per candidate —
  phones saw the SSID vanish mid-join.
- **I3 — after a failed join cycle the radio always settles on a beaconing AP.** The check
  reads the **actual driver mode** (`wp_need_ap_restore`), never the `"ap"`/`"sta"` intent
  strings. (The old `strcmp(s_mode,"ap")` guard was already `"ap"` in the standard fallback,
  so the restore never ran and the hotspot stayed down for a whole backoff window.) An idle
  leftover APSTA is trimmed to pure AP between retries (battery), but only when not busy.
- **I4 — a failed one-shot manual join never arms the retry loop.** `nucleo_setup_join()`
  keeps the previous `s_auto` on failure (`wp_join_result_auto`). One wrong password can no
  longer put the supervisor into an eternal scan/join loop that knocks the hotspot over.
  If the device was an intentional hotspot before the attempt, `start_ap()` restores it
  immediately (which also clears `s_want_sta`, stopping the driver's own bad-cred retries).

Plus one comfort rule: after a **successful** join the fallback AP is *not* cut immediately.
The supervisor drops it (`WP_ACT_DROP_AP`) only once it has been idle past the grace window —
so a user who drove the join from the hotspot web UI keeps their session until they leave.

## Scheduling

Retry backoff: 8 s doubling to a 60 s ceiling, reset on success (`wp_cycle_done`). While the
AP is busy, `DEFER` keeps pushing the retry window so the first attempt after the AP goes
quiet still has ≥ 8 s of calm. Stale-IP detection: 3 consecutive "not associated" polls while
holding an IP → forced reconnect (`net_trace`'d to `/sd/net_trace.txt`). All time comparisons
are wraparound-safe (signed diff of unsigned ms; the uint32 ms counter wraps at ~49.7 days).

## Known residual window (accepted)

A phone that starts its 802.11 auth/assoc exchange in the instant between the busy check and
the radio action can still lose that first attempt (the driver exposes no pre-association
signal). It is self-healing by design: with I2 the SSID never disappears, the failure fires
`AP_STADISCONNECTED` → 90 s grace → the phone's automatic retry lands on a quiet AP. The
pre-fix behaviour lost *most* first joins; this loses a rare one, once.

## Testing

`npm run wifi:test` — 160 assertions over the policy: mode rules, busy/grace, backoff ladder,
stale link, drop-AP, uint32 wraparound, and a full replay of the issue #3 story (fallback →
phone associates → zero scans until grace end after it leaves → retries resume). The gate is
in the test registry (`gate-wifi-policy`, category connect-transfer) and runs in `test:all`.
