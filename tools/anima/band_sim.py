#!/usr/bin/env python3
"""Simulate a 4-WAY uncertainty policy on top of the deployed encoder and measure it on the
held-out OOD set. Sharper than the 3-way band (band_eval.py): a clear-but-mid-confidence
winner is RECOVERED with a yes/no confirm instead of being thrown away as a refusal.

Per query, with top-3 distinct-card cosines s1>=s2>=s3:
  s1 >= HI                          -> ANSWER          (assert the fact; must be right on
                                                        in-scope and never fire out-of-scope)
  LO <= s1 < HI  and (s1-s2)<MARGIN -> CLARIFY  "X or Y?"   (recover if correct in {1,2})
  LO <= s1 < HI  and (s1-s2)>=MARGIN-> CONFIRM  "did you mean X?"  (recover if top-1 correct)
  s1 < LO                           -> REFUSE          (honest "I don't know")

Safety is structural: CLARIFY/CONFIRM never assert a fact, so the only way to be WRONG is an
ANSWER with a wrong top-1. We report that count separately and pick HI so it's zero (no false
facts), on both in-scope and out-of-scope. EFFECTIVE = answer-correct + clarify-recover +
confirm-correct. Compares the 4-way policy to the 3-way (no confirm) at the same thresholds.

Honest like the rest: thresholds are swept on this same OOD set, so treat the absolute numbers
as an upper-ish bound; the POINT is the 4-way vs 3-way delta at matched thresholds, which is a
policy change, not a fit.  Run:  python tools/anima/band_sim.py
"""
import os, sys, json, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))


def matches(expect, card):
    if isinstance(expect, list):       return any(matches(e, card) for e in expect)
    if expect.startswith("category:"): return card["category"] == expect.split(":", 1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=os.path.join(HERE, "eval_ood.jsonl"))
    ap.add_argument("--margin", type=float, default=0.08)
    a = ap.parse_args()

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus()
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def top3(q):
        cos = V @ encode(q); order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[i])))
            if len(out) >= 3: break
        while len(out) < 3: out.append((None, -1.0))
        return out

    qs = [json.loads(l) for l in open(a.queries, encoding="utf-8") if l.strip() and not l.startswith("//")]
    # precompute rankings once; sweep thresholds cheaply on top of them
    R = [(item, top3(item["q"])) for item in qs]

    def run(HI, LO, four_way):
        eff = wrong_fact = refuse = noise_oos = ans_oos = 0
        inscope = oos = 0
        for item, tk in R:
            (c1, s1), (c2, s2), _ = tk
            expect = item["expect"]
            is_oos = (expect == "none")
            if is_oos: oos += 1
            else:      inscope += 1
            if s1 >= HI:
                if is_oos: ans_oos += 1                       # out-of-scope asserted -> FALSE FACT
                else:
                    if matches(expect, cards[c1]): eff += 1
                    else: wrong_fact += 1                     # in-scope but wrong top-1 asserted
            elif s1 >= LO:
                competing = (s1 - s2) < a.margin
                if competing:
                    if is_oos: noise_oos += 1
                    elif matches(expect, cards[c1]) or (c2 is not None and matches(expect, cards[c2])): eff += 1
                elif four_way:                                # CONFIRM (clear mid winner)
                    if is_oos: noise_oos += 1
                    elif matches(expect, cards[c1]): eff += 1
                else:                                          # 3-way: a clear mid winner is refused
                    if not is_oos: refuse += 1
            else:
                if not is_oos: refuse += 1
        return dict(eff=eff, inscope=inscope, oos=oos, wrong=wrong_fact,
                    ans_oos=ans_oos, noise=noise_oos)

    print(f"[band-sim] {len(qs)} queries over {len(cards)} cards | margin={a.margin}\n")
    print(f"  {'HI':>5} {'LO':>5} | {'3-way EFF':>10} | {'4-way EFF':>10} | "
          f"{'false-fact':>10} | {'oos-noise(4w)':>13}")
    print("  " + "-" * 70)
    for HI, LO in [(0.66, 0.50), (0.68, 0.50), (0.70, 0.48), (0.66, 0.46)]:
        r3 = run(HI, LO, False); r4 = run(HI, LO, True)
        ff = r4["wrong"] + r4["ans_oos"]                      # any asserted-and-wrong (in or oos)
        ni, no = r3["inscope"], r3["oos"]
        print(f"  {HI:>5.2f} {LO:>5.2f} | {r3['eff']:>4}/{ni}={r3['eff']/ni:>5.1%} | "
              f"{r4['eff']:>4}/{ni}={r4['eff']/ni:>5.1%} | {ff:>3} (oos {r4['ans_oos']}) | "
              f"{r4['noise']:>3}/{no}")


if __name__ == "__main__":
    main()
