#!/usr/bin/env python3
"""ANIMA closed-loop learner: turn the device's REAL failures into a corpus work-list.

The firmware logs every knowledge hit and honest miss to /data/anima/telemetry.ndjson
(nucleo_anima.c telemetry_log) — cheap L0 commands are skipped, so the file is exactly the
"interesting" traffic: what the assistant retrieved, and what it could NOT answer. Until now
nothing consumed it. This does.

It triages each logged query OFFLINE with the same encoder + corpus the device uses:

  MISS (tier=none) and the nearest card is in the [lo, gate) band   -> NEAR-MISS:
        a card almost certainly covers it; the query is a real phrasing to ADD to that card's
        `ask` (the single highest-value, non-cheating enrichment: real user words that just
        missed the gate).
  MISS and the nearest card is far (< lo)                            -> GAP:
        nothing covers it; candidate for a NEW card (or genuinely out-of-scope).
  FRAGILE (tier=fact, conf just over the gate)                       -> WEAK CARD:
        it answered, but barely; reinforce that card so it isn't one paraphrase from failing.

Near-duplicate queries are collapsed (same normalized text) and ranked by frequency x value.
Output is a REVIEW work-list (knowledge/_suggestions.jsonl) — never auto-applied. You read it,
keep the good phrasings, run build_akb2 + eval. eval_ood.jsonl stays untouched (no cheating).

Usage:
  python tools/anima/learn.py --telemetry path/to/telemetry.ndjson
  python tools/anima/learn.py --host 192.168.0.166 --pin 689614   # pull it off the device first
"""
import os, sys, json, argparse, urllib.request
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = 0.66          # L1_COS_MIN
LO   = 0.50          # clarify/near-miss floor (mirrors the dialogic band)
FRAGILE_HI = 0.74    # a fact hit below this barely cleared the gate


def pull_from_device(host, pin, out):
    """Best-effort fetch of the telemetry file via the firmware fs API. Auth varies by build;
    if it fails, pull it yourself (it's just /data/anima/telemetry.ndjson on the SD)."""
    url = f"http://{host}/api/fs/read?path=/data/anima/telemetry.ndjson"
    req = urllib.request.Request(url, headers={"X-Auth-Pin": str(pin)} if pin else {})
    with urllib.request.urlopen(req, timeout=8) as r, open(out, "wb") as f:
        f.write(r.read())
    print(f"[learn] pulled telemetry -> {out}")


def read_telemetry(path):
    """Parse NDJSON tolerantly: the device rotates the file at 64 KB, so the first line may be
    a fragment. Skip anything that isn't a complete JSON object."""
    rows = []
    if not os.path.exists(path):
        sys.exit(f"[learn] no telemetry at {path} (use --host to pull it, or point --telemetry at a copy)")
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or not line.startswith("{") or not line.endswith("}"):
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "q" in r and "tier" in r:
            rows.append(r)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--telemetry", default=os.path.join(HERE, ".cache", "telemetry.ndjson"))
    ap.add_argument("--host", default=None, help="pull telemetry off the device first")
    ap.add_argument("--pin", default=None)
    # NB: NOT under knowledge/ — load_corpus globs knowledge/*.jsonl and would try to parse the
    # work-list as cards and fail the build. Keep it a sibling for human review instead.
    ap.add_argument("--out", default=os.path.join(HERE, "suggestions.jsonl"))
    a = ap.parse_args()

    if a.host:
        os.makedirs(os.path.dirname(a.telemetry), exist_ok=True)
        pull_from_device(a.host, a.pin, a.telemetry)

    rows = read_telemetry(a.telemetry)
    if not rows:
        print("[learn] telemetry is empty — nothing to learn yet (the device hasn't logged misses/hits).")
        return

    # Collapse near-duplicate queries (same normalized text), keeping count + worst tier seen.
    agg = {}
    for r in rows:
        key = A.norm_text(r["q"])
        if not key:
            continue
        e = agg.setdefault(key, {"q": r["q"], "n": 0, "miss": 0, "fact": 0, "conf": []})
        e["n"] += 1
        if r["tier"] == "none": e["miss"] += 1
        else:                   e["fact"] += 1
        e["conf"].append(int(r.get("conf", 0)))

    # Offline triage with the device encoder + corpus.
    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus()
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def nearest(q):
        cos = V @ encode(q); i = int(np.argmax(cos)); return cards[owner[i]], float(cos[i])

    near_miss, gap, fragile = [], [], []
    for key, e in agg.items():
        card, cos = nearest(e["q"])
        if e["miss"] >= e["fact"]:                       # predominantly a miss
            (near_miss if cos >= LO else gap).append((e, card, cos))
        elif e["conf"] and max(e["conf"]) <= int(FRAGILE_HI * 100):
            fragile.append((e, card, cos))

    near_miss.sort(key=lambda x: (-x[0]["n"], -x[2]))
    gap.sort(key=lambda x: -x[0]["n"])
    fragile.sort(key=lambda x: (-x[0]["n"], x[2]))

    def show(title, items, kind):
        print(f"\n  {title}: {len(items)}")
        for e, card, cos in items[:25]:
            print(f"    x{e['n']:<2} cos={cos:.2f}  {e['q'][:52]:52} -> {card['id']}  [{kind}]")

    print(f"[learn] {len(rows)} log lines, {len(agg)} distinct queries")
    show("NEAR-MISS (add as `ask` to the named card)", near_miss, "add-ask")
    show("WEAK CARD (reinforce; it barely answered)", fragile, "reinforce")
    show("GAP (no close card -> new card or out-of-scope)", gap, "new-card")

    # Machine-readable work-list for review (NOT auto-applied to the corpus).
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        for kind, items in (("add-ask", near_miss), ("reinforce", fragile), ("new-card", gap)):
            for e, card, cos in items:
                f.write(json.dumps({"action": kind, "query": e["q"], "count": e["n"],
                                    "nearest_card": card["id"], "cosine": round(cos, 3)},
                                   ensure_ascii=False) + "\n")
    print(f"\n  work-list -> {a.out}  (review, then fold good phrasings into knowledge/*.jsonl)")
    print("  loop: learn.py -> edit cards -> build_akb2.py -> eval.py  (eval_ood stays held-out)")


if __name__ == "__main__":
    main()
