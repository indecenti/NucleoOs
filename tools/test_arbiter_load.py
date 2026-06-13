"""
Arbiter load test — "does the web server hold under load, with RAM?"

Hammers the heavy (TLS) endpoints of a FLASHED NucleoOS device CONCURRENTLY while a monitor thread
polls /api/status, and proves the three properties the heavy-work arbiter (firmware/components/nucleo_arb)
exists to guarantee on the PSRAM-less ESP32-S3:

  1. LIVENESS   — /api/status answers 200 throughout (the single-task web server never crashes under load).
  2. GRACEFUL   — a heavy request that can't get the budget returns a clean 503 (retry), NEVER a dropped/
                  reset connection. 503 = the arbiter degraded on purpose; a connection error = a crash.
  3. HEAP FLOOR — the largest contiguous internal block never dips below a safe floor (the OOM cliff),
                  because only ONE TLS handshake allocates at a time (the arbiter serializes them).

The arbiter lives in firmware, so this runs against a real device (or anything exposing /api/status with
an "arbiter" object). It SKIPs cleanly (exit 0) if the device is unreachable or the arbiter firmware isn't
flashed yet — so it never falses-red in an offline CI run.

  python tools/test_arbiter_load.py --url http://192.168.0.166 [--workers 8] [--rounds 6]
                                    [--heap-floor 18000] [--strict] [--proxy-url https://example.com]

--strict makes the heap-floor a HARD gate (else it's measured + reported). Exit 0 = pass/skip, 1 = fail.
"""

import sys
import time
import json
import argparse
import threading
import urllib.request
import urllib.error
import urllib.parse
from concurrent.futures import ThreadPoolExecutor


def _get(url: str, timeout: float = 20.0):
    """GET -> (status, body). status -1 means the connection itself failed (a crash signal, not an HTTP error)."""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:                      # a real HTTP status (incl. 503) — server is ALIVE
        try:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception:
            return e.code, ""
    except Exception as e:                                   # connection refused/reset/timeout — server unreachable
        return -1, str(e)


def fetch_status(base: str):
    st, body = _get(base + "/api/status", timeout=8.0)
    if st != 200:
        return None
    try:
        return json.loads(body)
    except Exception:
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description="NucleoOS arbiter load test")
    ap.add_argument("--url", default="http://192.168.0.166", help="device base URL")
    ap.add_argument("--workers", type=int, default=8, help="concurrent heavy clients")
    ap.add_argument("--rounds", type=int, default=6, help="heavy requests per worker")
    ap.add_argument("--heap-floor", type=int, default=18000, help="min acceptable largest_free_block (bytes)")
    ap.add_argument("--proxy-url", default="https://example.com", help="https target for /api/proxy TLS load")
    ap.add_argument("--strict", action="store_true", help="make the heap-floor a HARD gate")
    args = ap.parse_args()
    base = args.url.rstrip("/")

    # --- preflight: device reachable? arbiter firmware present? (else SKIP, never a false red) ----------
    st0 = fetch_status(base)
    if st0 is None:
        print(f"[arbiter-load] SKIP — {base}/api/status unreachable (no device on this run)")
        return 0
    if "arbiter" not in st0:
        print("[arbiter-load] SKIP — device reachable but no 'arbiter' field in /api/status "
              "(flash the arbiter firmware first)")
        return 0

    print(f"[arbiter-load] target {base} | {args.workers} workers x {args.rounds} rounds "
          f"= {args.workers * args.rounds} heavy TLS requests via /api/proxy")

    # --- monitor thread: liveness + heap-floor watermark, sampled while the load runs -------------------
    stop = threading.Event()
    mon = {"polls": 0, "alive": 0, "dead": 0, "min_block": st0["largest_free_block"],
           "min_free": st0.get("free_heap", 0), "busy_seen": 0, "dev_denials0": st0["arbiter"]["denials"],
           "dev_grants0": st0["arbiter"]["grants"]}

    def monitor():
        while not stop.is_set():
            s = fetch_status(base)
            mon["polls"] += 1
            if s is None:
                mon["dead"] += 1                            # /api/status didn't answer -> server stalled/crashed
            else:
                mon["alive"] += 1
                mon["min_block"] = min(mon["min_block"], s["largest_free_block"])
                mon["min_free"] = min(mon["min_free"], s.get("free_heap", mon["min_free"]))
                if s["arbiter"]["busy"]:
                    mon["busy_seen"] += 1
            time.sleep(0.15)

    mt = threading.Thread(target=monitor, daemon=True)
    mt.start()

    # --- the load: concurrent server-side TLS fetches (each exercises the arbiter token) ----------------
    proxy = base + "/api/proxy?url=" + urllib.parse.quote(args.proxy_url, safe="")
    tally = {"served": 0, "deferred": 0, "crashed": 0, "other": 0}
    tally_lock = threading.Lock()

    def heavy(_i: int):
        st, _ = _get(proxy, timeout=25.0)
        with tally_lock:
            if st == 200:
                tally["served"] += 1                        # fetched through the arbiter
            elif st == 503:
                tally["deferred"] += 1                      # GOOD: arbiter said "busy, retry" (graceful)
            elif st == -1:
                tally["crashed"] += 1                       # BAD: connection dropped (possible OOM crash)
            else:
                tally["other"] += 1                         # 4xx/5xx from the fetch target (not a server crash)

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        list(ex.map(heavy, range(args.workers * args.rounds)))
    elapsed = time.time() - t0

    stop.set()
    mt.join(timeout=2.0)

    st1 = fetch_status(base) or st0
    dev = st1.get("arbiter", {})
    dev_grants = dev.get("grants", 0) - mon["dev_grants0"]
    dev_denials = dev.get("denials", 0) - mon["dev_denials0"]

    # --- verdict ----------------------------------------------------------------------------------------
    server_alive = (mon["dead"] == 0) and (tally["crashed"] == 0) and (fetch_status(base) is not None)
    heap_ok = mon["min_block"] >= args.heap_floor

    print(f"[arbiter-load] {args.workers * args.rounds} requests in {elapsed:.1f}s | "
          f"served={tally['served']} deferred(503)={tally['deferred']} "
          f"crashed={tally['crashed']} other={tally['other']}")
    print(f"[arbiter-load] device: grants+{dev_grants} denials+{dev_denials} "
          f"heap_free_min={dev.get('heap_free_min', '?')} | "
          f"monitor: {mon['alive']}/{mon['polls']} status-200, busy_seen={mon['busy_seen']}, "
          f"min_block={mon['min_block']} (floor {args.heap_floor}), min_free={mon['min_free']}")

    failed = False
    if not server_alive:
        print("[arbiter-load] FAIL — server became unreachable / a request crashed the connection "
              "(LIVENESS broken)")
        failed = True
    else:
        print("[arbiter-load] LIVENESS ok — server answered every status poll, zero dropped connections")

    if not heap_ok:
        msg = "FAIL" if args.strict else "WARN"
        print(f"[arbiter-load] {msg} — largest free block dipped to {mon['min_block']} "
              f"< floor {args.heap_floor}")
        if args.strict:
            failed = True
    else:
        print(f"[arbiter-load] HEAP FLOOR ok — never below {args.heap_floor} bytes contiguous")

    print("[arbiter-load] " + ("PASS" if not failed else "FAILED"))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[arbiter-load] interrupted")
        sys.exit(1)
