"""
NL soak test on the REAL Cardputer — "does the device sustain the whole NL corpus, one query at a time?"

The natural-language gates normally run on PC (against anima.exe). This runner instead sends the SAME
NL query corpus to a FLASHED device over GET /api/anima (mode=off → pure offline cascade, no cloud/TLS),
ONE request at a time with a deliberate pause between them, and proves the device survives the sustained
load on the PSRAM-less ESP32-S3:

  1. LIVENESS  — /api/anima answers every query (HTTP 200 + JSON); the single-task web server never
                 drops a connection (a dropped/reset connection = possible OOM crash, the failure signal).
  2. PROGRESS  — aborts early (FAIL) after N consecutive connection failures (device wedged), instead of
                 soaking for hours against a dead box.

It also REPORTS, as information (not a hard gate by default): the tier histogram and the abstention rate
on trap queries (entries marked expect=="abstain"). Over HTTP the device returns {tier,intent,reply,
confidence,ms} — not the full host trace — so this is a device SMOKE/SOAK, not the host correctness gate.

  python tools/test_nl_device.py --url http://192.168.0.166 [--delay 12] [--mode off]
         [--limit N] [--max-len 159] [--max-fails 5] [--strict]

--delay is the CADENCE between request-starts in seconds (clamped to a >=3 floor; default 12): if a request
already spent part of that time, only the remainder is slept (zero if it ran longer) — no time is wasted.
--strict makes a high trap-hallucination rate a FAIL too. Exit 0 = pass/skip, 1 = fail.

SAFETY — this runner deliberately does NOT pair. /api/anima is public, but the side-effecting tools it can
reach (create_file, add_event) are gated behind a paired session in the firmware, so when unpaired they are
BLOCKED ("pairing required") instead of executed. launch/volume/brightness are returned as descriptors the
web shell would apply, never applied server-side. Net effect: the soak is READ-ONLY — it sends command
phrases too, but the device's files/calendar/volume are never mutated.
"""

import sys
import os
import glob
import json
import time
import argparse
import urllib.request
import urllib.parse
import urllib.error
from datetime import datetime

DELAY_FLOOR = 3          # the UI also enforces this; defend it here too
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
REPORTS = os.path.join(HERE, "test-lab", "reports")   # detailed per-query log lands beside the cockpit reports
# Where the NL query corpus lives (same .jsonl the host NL gates consume).
CORPUS_GLOBS = ["tools/anima/eval_*.jsonl", "tools/anima/probe*.jsonl",
                "tools/anima-host/eval_*.jsonl", "tools/anima-host/*-cases.jsonl",
                "tools/anima-host/*-phrases.jsonl"]


def get(url, timeout=30.0):
    """GET -> (status, body). status -1 = the connection itself failed (a crash signal, not an HTTP error)."""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:                      # a real HTTP status — server is ALIVE
        try: return e.code, e.read().decode("utf-8", "replace")
        except Exception: return e.code, ""
    except Exception as e:                                   # refused/reset/timeout — server unreachable
        return -1, str(e)


def load_corpus(max_len):
    """Aggregate every NL .jsonl entry that has a `q` field, de-duped by (q, lang). Skips comment lines
    (leading //) and entries whose query exceeds the device's q[160] buffer (would be truncated)."""
    seen, out, too_long = set(), [], 0
    for pat in CORPUS_GLOBS:
        for path in glob.glob(os.path.join(REPO, pat)):
            try:
                with open(path, encoding="utf-8") as f:
                    for line in f:
                        line = line.strip()
                        if not line.startswith("{"):
                            continue
                        try: o = json.loads(line)
                        except Exception: continue
                        q = (o.get("q") or "").strip()
                        if not q:
                            continue
                        lang = "en" if str(o.get("lang", "it")).lower().startswith("en") else "it"
                        if len(q.encode("utf-8")) > max_len:
                            too_long += 1
                            continue
                        key = (q, lang)
                        if key in seen:
                            continue
                        seen.add(key)
                        out.append({"q": q, "lang": lang,
                                    "expect": o.get("expect", ""), "src": os.path.basename(path)})
            except OSError:
                continue
    return out, too_long


def is_abstain(resp):
    """Heuristic: did the device decline/abstain? Empty reply or a 'none' tier counts as abstain."""
    if not resp:
        return True
    reply = (resp.get("reply") or "").strip()
    tier = (resp.get("tier") or "").lower()
    return (not reply) or tier in ("none", "", "abstain")


def main() -> int:
    ap = argparse.ArgumentParser(description="NucleoOS NL soak test (on real device)")
    ap.add_argument("--url", default="http://192.168.0.166", help="device base URL")
    ap.add_argument("--delay", type=float, default=12.0,
                    help="cadenza fra l'inizio di due query in secondi (floor 3); il tempo della richiesta vi sta dentro")
    ap.add_argument("--mode", default="off", choices=["off", "on", "only"], help="ANIMA mode (off = pure offline)")
    ap.add_argument("--limit", type=int, default=0, help="cap number of queries (0 = whole corpus)")
    ap.add_argument("--max-len", type=int, default=159, help="skip queries longer than this (device q[160])")
    ap.add_argument("--max-fails", type=int, default=5, help="abort after this many CONSECUTIVE connection failures")
    ap.add_argument("--strict", action="store_true", help="also FAIL if trap-hallucination rate is high")
    args = ap.parse_args()
    base = args.url.rstrip("/")
    delay = max(DELAY_FLOOR, args.delay)

    # --- preflight: device reachable? (else SKIP, never a false red) -------------------------------
    st, _ = get(base + "/api/status", timeout=8.0)
    if st != 200:
        print(f"[nl-soak] SKIP — {base}/api/status unreachable (no device on this run)")
        return 0

    corpus, too_long = load_corpus(args.max_len)
    if args.limit > 0:
        corpus = corpus[:args.limit]
    if not corpus:
        print("[nl-soak] SKIP — no NL queries found in corpus")
        return 0

    eta_min = len(corpus) * delay / 60.0       # cadenza fissa: ~delay per ciclo (la richiesta vi sta dentro)
    print(f"[nl-soak] target {base} | {len(corpus)} query (mode={args.mode}, cadenza {delay:.0f}s) "
          f"| {too_long} saltate (troppo lunghe) | ETA ~{eta_min:.0f} min")
    print("[nl-soak] non-associato → create_file/add_event bloccati: soak READ-ONLY (il device non muta)")

    os.makedirs(REPORTS, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    detail_path = os.path.join(REPORTS, f"nl-device-{stamp}.jsonl")
    detail = open(detail_path, "w", encoding="utf-8")

    served = crashed = errors = 0
    consec_fail = 0
    tiers = {}
    trap_total = trap_abstained = 0
    t0 = time.time()

    for i, item in enumerate(corpus):
        url = f"{base}/api/anima?q={urllib.parse.quote(item['q'])}&lang={item['lang']}&mode={args.mode}"
        cycle_t0 = time.time()                  # cadenza misurata dall'INIZIO della richiesta
        code, body = get(url, timeout=30.0)
        req_ms = int((time.time() - cycle_t0) * 1000)
        rec = {"i": i, "q": item["q"], "lang": item["lang"], "src": item["src"], "http": code}
        if code == 200:
            served += 1; consec_fail = 0
            try:
                resp = json.loads(body)
            except Exception:
                resp = None
            if resp:
                tier = (resp.get("tier") or "none")
                tiers[tier] = tiers.get(tier, 0) + 1
                rec.update(tier=tier, intent=resp.get("intent"),
                           confidence=resp.get("confidence"), ms=resp.get("ms"),
                           reply=(resp.get("reply") or "")[:200])
            if item["expect"] == "abstain":
                trap_total += 1
                if is_abstain(resp):
                    trap_abstained += 1
                else:
                    rec["TRAP_NOT_ABSTAINED"] = True
        elif code == -1:
            crashed += 1; consec_fail += 1
            rec["error"] = "connection dropped (possible OOM crash)"
        else:
            errors += 1; consec_fail += 1
            rec["error"] = f"HTTP {code}"
        detail.write(json.dumps(rec, ensure_ascii=False) + "\n"); detail.flush()

        # Una riga per OGNI query → stream "uno ad uno" leggibile live nel cockpit.
        mark = "ok " if code == 200 else ("CRASH" if code == -1 else f"H{code}")
        trapflag = "  ⚠trappola-non-astenuta" if rec.get("TRAP_NOT_ABSTAINED") else ""
        ms = rec.get("ms")
        ms = ms if isinstance(ms, (int, float)) else req_ms     # fallback: round-trip misurato
        print(f"[q] {i+1}/{len(corpus)} {mark} {item['lang']} {rec.get('tier','-'):>7} "
              f"{ms:>4}ms · {item['q'][:46]}{trapflag}", flush=True)

        if consec_fail >= args.max_fails:
            print(f"[nl-soak] ABORT — {consec_fail} fallimenti di connessione consecutivi alla query #{i} "
                  f"(device irraggiungibile/in crash)")
            detail.close()
            print(f"[nl-soak] {served} servite prima dell'abort | log: {os.path.relpath(detail_path, REPO)}")
            print("[nl-soak] FAILED")
            return 1

        if (i + 1) % 100 == 0:
            print(f"[nl-soak] — progresso {i+1}/{len(corpus)} · servite={served} crash={crashed} err={errors}", flush=True)
        # Cadenza fissa: l'intervallo fra l'inizio di due query è `delay`. Se la richiesta ha già speso
        # parte (o tutto) quel tempo, aspetto solo ciò che manca — zero se l'ha già superato.
        if i + 1 < len(corpus):
            remaining = delay - (time.time() - cycle_t0)
            if remaining > 0:
                time.sleep(remaining)

    detail.close()
    elapsed = time.time() - t0
    alive = get(base + "/api/status", timeout=8.0)[0] == 200
    trap_rate = (trap_abstained / trap_total) if trap_total else 1.0
    top = ", ".join(f"{k}:{v}" for k, v in sorted(tiers.items(), key=lambda kv: -kv[1]))

    print(f"[nl-soak] {len(corpus)} query in {elapsed/60:.1f} min | servite={served} crash={crashed} err={errors}")
    print(f"[nl-soak] tier: {top or '-'}")
    print(f"[nl-soak] trappole astenute: {trap_abstained}/{trap_total} ({trap_rate*100:.0f}%) | "
          f"log dettagliato: {os.path.relpath(detail_path, REPO)}")

    failed = False
    if crashed > 0 or not alive:
        print(f"[nl-soak] FAIL — {crashed} connessioni cadute / device irraggiungibile a fine corsa (LIVENESS rotta)")
        failed = True
    else:
        print("[nl-soak] LIVENESS ok — il device ha risposto a ogni query, zero connessioni cadute")
    if errors > 0:
        print(f"[nl-soak] WARN — {errors} risposte non-200 (non crash, ma da guardare)")
    if args.strict and trap_total and trap_rate < 0.95:
        print(f"[nl-soak] FAIL (strict) — astensione sulle trappole {trap_rate*100:.0f}% < 95%")
        failed = True

    print("[nl-soak] " + ("PASS" if not failed else "FAILED"))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[nl-soak] interrotto")
        sys.exit(1)
