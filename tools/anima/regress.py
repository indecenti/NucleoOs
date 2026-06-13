#!/usr/bin/env python3
"""ANIMA regression suite — ONE command, clear PASS/FAIL, future-proof.

Runs every offline check that doesn't need the device, with hard GATES so a regression
fails loudly (use it before any deploy / after editing the corpus or the encoder):

  - in-distribution recall   (eval_queries.jsonl)  GATE: Recall@1 == 100%, 0 false-positives
  - out-of-distribution recall (eval_ood.jsonl)     track Recall@1/@3; GATE: oos false-pos <= --max-ood-fp
  - dialogic band              (the [0.50,0.66) clarify layer)  reports effective success

The orchestrator/routing checks (calc, app launch, system, tool) run end-to-end over HTTP
against the mock or device -> use eval_routing.py for those (this file is L1 + band only).

  python tools/anima/regress.py                 # live corpus
  python tools/anima/regress.py --kdir .snap    # against a snapshot (avoid races while enriching)
Exit code != 0 if any GATE fails.
"""
import os, sys, json, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))
# Mirror the SHIPPED firmware gate (nucleo_anima_l1.c): answer at cos >= 0.85, evidential rescue at
# cos >= 0.72 with a >= 0.12 margin to the distinct runner-up, and a clarify band in [0.82, 0.85).
# The earlier flat 0.66/0.50 here was STALE (an old firmware value): it over-counted out-of-scope
# false-positives the device actually refuses. Verified device-accurate against the real compiled
# exe by tools/anima-host/ood-check.mjs (0 FP). Recall@1/@3 stay rank-based (gate-independent).
ANSWER, RESCUE_ABS, RESCUE_MARGIN = 0.85, 0.72, 0.12
BAND_LO, BAND_MARGIN = 0.82, 0.08


def answered(s1, s2):
    """The device's answer/refuse decision: clear the 0.85 gate, OR clear 0.72 with a decisive margin
    over the runner-up (CRAG-style evidential rescue). Anything else refuses."""
    return s1 >= ANSWER or (s1 >= RESCUE_ABS and (s1 - s2) >= RESCUE_MARGIN)


def matches(expect, card):
    if isinstance(expect, list): return any(matches(e, card) for e in expect)
    if expect.startswith("category:"): return card["category"] == expect.split(":", 1)[1]
    if expect.endswith("*"): return card["id"].startswith(expect[:-1])
    return card["id"] == expect


def load_queries(path):
    return [json.loads(l) for l in open(path, encoding="utf-8") if l.strip() and not l.startswith("//")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kdir", default=None, help="knowledge dir (default: live)")
    ap.add_argument("--max-ood-fp", type=int, default=1, help="allowed out-of-scope false-positives in OOD set")
    ap.add_argument("--min-ood-recall1", type=float, default=None, help="optional floor on OOD Recall@1")
    a = ap.parse_args()

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus(kdir=a.kdir) if a.kdir else A.load_corpus()
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def topk(q, k=3):
        qv = encode(q); cos = V @ qv; order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[i])))
            if len(out) >= k: break
        return out

    def eval_set(path):
        qs = load_queries(path)
        ins = oos = r1 = r3 = ans = ans_ok = fp = 0
        band_recover = 0; cos_sum = 0.0
        for it in qs:
            tk = topk(it["q"]); (c1, s1) = tk[0]; (c2, s2) = tk[1] if len(tk) > 1 else (None, 0.0)
            if it["expect"] == "none":
                oos += 1
                if answered(s1, s2): fp += 1
                continue
            ins += 1; cos_sum += s1
            hit1 = matches(it["expect"], cards[c1])
            hit3 = any(matches(it["expect"], cards[ci]) for ci, _ in tk[:3])
            r1 += hit1; r3 += hit3
            if answered(s1, s2):
                ans += 1; ans_ok += hit1
            elif BAND_LO <= s1 < ANSWER and c2 is not None and (s1 - s2) < BAND_MARGIN:  # clarify band
                if hit1 or matches(it["expect"], cards[c2]): band_recover += 1
        return dict(ins=ins, oos=oos, r1=r1, r3=r3, ans=ans, ans_ok=ans_ok, fp=fp,
                    band=band_recover, cos=cos_sum / ins if ins else 0)

    gates = []
    print(f"ANIMA regression  ({len(cards)} cards, {len(V)} phrasings, device gate {ANSWER}/rescue {RESCUE_ABS}+{RESCUE_MARGIN})\n")

    # 0) data hygiene: NO ask phrasing may belong to >1 card (cross-card duplicate = the small
    # model's contamination signal; generate.py's guard prevents new ones, this proves it held).
    from collections import defaultdict as _dd
    holders = _dd(set)
    for c in cards:
        for lang in ("it", "en"):
            for ask in c["ask"].get(lang, []):
                holders[ask.strip().lower()].add(c["id"])
    dups = {k: v for k, v in holders.items() if len(v) > 1}
    ok_dup = len(dups) == 0
    gates.append(("0 cross-card duplicate asks", ok_dup))
    print(f"  [DEDUP]    cross-card duplicate asks: {len(dups)}   {'PASS' if ok_dup else 'FAIL'}")
    for k, v in list(dups.items())[:5]:
        print(f"             '{k[:42]}' on {sorted(v)}")

    # 1) in-distribution -- hard regression gate
    d = eval_set(os.path.join(HERE, "eval_queries.jsonl"))
    r1p = d["r1"] / d["ins"] if d["ins"] else 0
    ok_ind = (r1p >= 0.999) and d["fp"] == 0
    gates.append(("in-dist Recall@1 100% & 0 FP", ok_ind))
    print(f"  [IN-DIST]  Recall@1 {d['r1']}/{d['ins']}={r1p:.1%}  Recall@3 {d['r3']}/{d['ins']}  "
          f"out-of-scope FP {d['fp']}/{d['oos']}   {'PASS' if ok_ind else 'FAIL <-- REGRESSION'}")

    # 2) out-of-distribution -- tracked; FP gated
    o = eval_set(os.path.join(HERE, "eval_ood.jsonl"))
    eff = o["ans_ok"] + o["band"]
    or1 = o["r1"] / o["ins"] if o["ins"] else 0
    ok_fp = o["fp"] <= a.max_ood_fp
    gates.append((f"OOD out-of-scope FP <= {a.max_ood_fp}", ok_fp))
    print(f"  [OOD]      Recall@1 {o['r1']}/{o['ins']}={or1:.1%}  Recall@3 {o['r3']}/{o['ins']}={o['r3']/o['ins']:.1%}  "
          f"mean cos {o['cos']:.3f}")
    print(f"  [OOD+BAND] effective {eff}/{o['ins']}={eff/o['ins']:.1%}  "
          f"(direct {o['ans_ok']} + clarify-recover {o['band']})   answered@gate correct {o['ans_ok']}/{o['ans']}")
    print(f"  [SAFETY]   OOD out-of-scope FP {o['fp']}/{o['oos']}   {'PASS' if ok_fp else 'FAIL'}")
    if a.min_ood_recall1 is not None:
        ok_or = or1 >= a.min_ood_recall1
        gates.append((f"OOD Recall@1 >= {a.min_ood_recall1:.0%}", ok_or))
        print(f"  [OOD floor] {or1:.1%} >= {a.min_ood_recall1:.0%}   {'PASS' if ok_or else 'FAIL'}")

    # 3) calc engine -- exact arithmetic must never regress (real math, not retrieval)
    try:
        import calc as _calc
        cok = sum(1 for q, exp in _calc.TESTS if _calc.calc(q)[1] == exp)
        ctot = len(_calc.TESTS)
        ok_calc = cok == ctot
        gates.append(("calc engine exact", ok_calc))
        print(f"  [CALC]     {cok}/{ctot} expressions exact   {'PASS' if ok_calc else 'FAIL'}")
    except Exception as e:
        print(f"  [CALC]     skipped ({e})")

    # 4) unified math agent -- calc + solver (percent/Ohm/units/powers/roots/abs/modulo)
    try:
        import re as _re, math_agent as _ma
        mok = sum(1 for q, exp in _ma.TESTS
                  if (_ma.solve(q, "en" if _re.search(r"power of|root of|% of", q) else "it") or {}).get("reply", "").lower().find(exp.lower()) >= 0)
        mtot = len(_ma.TESTS)
        ok_ma = mok == mtot
        gates.append(("math agent exact", ok_ma))
        print(f"  [MATH]     {mok}/{mtot} agent (calc+percent+ohm+units+abs/mod)   {'PASS' if ok_ma else 'FAIL'}")
    except Exception as e:
        print(f"  [MATH]     skipped ({e})")

    failed = [name for name, ok in gates if not ok]
    print("\n" + ("ALL GATES PASS" if not failed else "GATES FAILED: " + "; ".join(failed)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
