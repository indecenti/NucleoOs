#!/usr/bin/env python3
"""Audit (and optionally fix) LLM `ask` phrasings that drifted onto the WRONG card.

A small model sometimes copies a phrasing onto a card it doesn't belong to ("what is the
capital of Russia" on the Japan card; a hallucinated "angle between two vectors" on a calendar
card). These are bad retrieval keys (harmless to facts — reply/detail are frozen — but they
mis-route queries).

Detection that actually works: an `ask` trivially matches its own card (it's in that card's
index), so self-retrieval hides single drifts; the reliable signal is a CROSS-CARD DUPLICATE
ask. For each ask held by >1 card, measure how well the ask matches each holder's frozen
reply+detail (the card's MEANING). Keep it only on holders it's actually about (reply cos >=
--thresh); drop it from holders where it's off-topic. Legitimate overlaps (a loop phrasing on
both `loop` and `for-loop`) stay on both (both replies match). Single asks whose own reply cos
is very low are REPORTED (manual review) but never auto-removed (would hit valid indirect asks).

  python tools/anima/audit_asks.py                       # report
  python tools/anima/audit_asks.py --fix                 # drop off-topic duplicate asks
  python tools/anima/audit_asks.py --fix --skip geography  # leave a file being enriched alone
"""
import os, sys, json, glob, argparse
from collections import defaultdict
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

KDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "knowledge")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--thresh", type=float, default=0.45, help="min ask->reply cos to keep an ask on a card")
    ap.add_argument("--skip", default="")
    a = ap.parse_args()
    skip = {s.strip() for s in a.skip.split(",") if s.strip()}

    table, H, D, NG, _ = A.load_encoder(); enc = A.make_encoder(table, H, D, NG)
    cards, _ = A.load_corpus()

    # per-card MEANING anchor = reply(it/en)+detail(it/en) vectors
    anchor = []
    for c in cards:
        texts = [c["reply"]["it"], c["reply"]["en"], c["detail"].get("it", ""), c["detail"].get("en", "")]
        vs = [enc(t) for t in texts if t]
        anchor.append(np.stack(vs) if vs else None)

    def reply_cos(ask, ci):
        if anchor[ci] is None: return 0.0
        return float((anchor[ci] @ enc(ask)).max())

    # cross-card duplicate asks (same string on >1 card), enriched cards only
    holders = defaultdict(set)
    for ci, c in enumerate(cards):
        if "llm" not in c.get("source", ""): continue
        for lang in ("it", "en"):
            for ask in c["ask"].get(lang, []):
                holders[(lang, ask)].add(ci)

    drops = defaultdict(lambda: defaultdict(set))   # card_id -> lang -> {asks to drop}
    print("[audit] cross-card duplicate asks:")
    for (lang, ask), cis in holders.items():
        if len(cis) < 2: continue
        scored = sorted(((reply_cos(ask, ci), ci) for ci in cis), reverse=True)
        keep = [ci for s, ci in scored if s >= a.thresh] or [scored[0][1]]  # keep best if none pass
        for s, ci in scored:
            tag = "KEEP" if ci in keep else "DROP"
            if ci not in keep:
                drops[cards[ci]["id"]][lang].add(ask)
            print(f"   {tag} cos={s:.2f} [{lang}] {cards[ci]['id']:22} | {ask[:50]}")

    # single off-topic candidates (own reply cos very low) -> report only
    low = []
    for ci, c in enumerate(cards):
        if "llm" not in c.get("source", ""): continue
        for lang in ("it", "en"):
            for ask in c["ask"].get(lang, []):
                if len(holders[(lang, ask)]) == 1 and reply_cos(ask, ci) < 0.25:
                    low.append((reply_cos(ask, ci), c["id"], lang, ask))
    if low:
        print(f"\n[audit] single asks with very low reply-cos (<0.25) — REVIEW manually ({len(low)}):")
        for s, cid, lang, ask in sorted(low)[:20]:
            print(f"   cos={s:.2f} [{lang}] {cid:22} | {ask[:50]}")

    ndrop = sum(len(s) for v in drops.values() for s in v.values())
    print(f"\n[audit] off-topic duplicate asks to drop: {ndrop}")
    if not a.fix or not drops:
        return
    changed = 0
    for p in glob.glob(os.path.join(KDIR, "*.jsonl")):
        if os.path.basename(p)[:-6] in skip: continue
        lines = open(p, encoding="utf-8").read().splitlines(); out = []; touched = False
        for ln in lines:
            s = ln.strip()
            if not s or s.startswith("//"): out.append(ln); continue
            d = json.loads(s); bad = drops.get(d.get("id"))
            if bad:
                for lang, asks in bad.items():
                    b = len(d["ask"].get(lang, []))
                    d["ask"][lang] = [x for x in d["ask"].get(lang, []) if x not in asks]
                    changed += b - len(d["ask"][lang]); touched = touched or b != len(d["ask"][lang])
                d["source"] = d.get("source", "") + "+decontam"
                out.append(json.dumps(d, ensure_ascii=False))
            else:
                out.append(ln)
        if touched:
            open(p, "w", encoding="utf-8").write("\n".join(out) + "\n"); print(f"  fixed {os.path.basename(p)}")
    print(f"[audit] removed {changed} off-topic asks")


if __name__ == "__main__":
    main()
