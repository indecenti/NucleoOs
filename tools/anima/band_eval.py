#!/usr/bin/env python3
"""Measure the DIALOGIC AMBIGUITY BAND: instead of a binary answer/refuse at the gate, add a
middle band where ANIMA asks a clarifying question ("did you mean X or Y?") using the top-2
candidates. This converts honest-but-dead refusals into a recoverable dialogue WITHOUT ever
asserting a wrong fact (safe by construction).

Three-way decision on the top-1 cosine:
  cos >= GATE_HI            -> ANSWER directly
  GATE_LO <= cos < GATE_HI  -> CLARIFY with top-2 (only if the two are genuinely competing)
  cos < GATE_LO             -> REFUSE ("I don't know")

We report, over the held-out OOD set:
  - direct answers (correct)
  - clarifications where the CORRECT card is among the offered top-2 (=> recoverable)
  - clarifications that miss (correct not in top-2)  -> still safe, just unhelpful
  - refusals
  - out-of-scope handling: a clarify on an out-of-scope query is NOISE (not a false fact);
    we count how often that happens to keep the band from being annoying.
"""
import os, sys, json, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=os.path.join(os.path.dirname(__file__), "eval_ood.jsonl"))
    ap.add_argument("--kdir", default=os.path.join(os.path.dirname(__file__), ".snap"))
    ap.add_argument("--hi", type=float, default=0.66)   # answer gate (current L1_COS_MIN)
    ap.add_argument("--lo", type=float, default=0.50)   # clarify floor
    ap.add_argument("--margin", type=float, default=0.08)  # top1-top2 < margin => genuinely competing
    a = ap.parse_args()

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus(kdir=a.kdir)
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

    def match(expect, card):
        if isinstance(expect, list): return any(match(e, card) for e in expect)
        if expect.startswith("category:"): return card["category"] == expect.split(":",1)[1]
        if expect.endswith("*"): return card["id"].startswith(expect[:-1])
        return card["id"] == expect

    qs = [json.loads(l) for l in open(a.queries, encoding="utf-8") if l.strip() and not l.startswith("//")]
    answered_ok = clarify_recover = clarify_miss = refused = 0
    oos_total = oos_clarify_noise = oos_clean = 0
    inscope = 0
    transcript = []
    for item in qs:
        q, expect = item["q"], item["expect"]
        tk = topk(q); (c1, s1) = tk[0]; (c2, s2) = tk[1] if len(tk) > 1 else (None, 0.0)
        competing = (s1 - s2) < a.margin
        if expect == "none":
            oos_total += 1
            if a.lo <= s1 < a.hi and competing: oos_clarify_noise += 1
            else: oos_clean += 1
            continue
        inscope += 1
        if s1 >= a.hi:
            ok = match(expect, cards[c1]); answered_ok += ok
            transcript.append(("ANSWER", q, cards[c1]["id"], s1, ok))
        elif s1 >= a.lo and competing:
            in_top2 = match(expect, cards[c1]) or (c2 is not None and match(expect, cards[c2]))
            clarify_recover += in_top2; clarify_miss += (not in_top2)
            transcript.append(("CLARIFY", q, f"{cards[c1]['id']}|{cards[c2]['id'] if c2 is not None else '-'}", s1, in_top2))
        else:
            refused += 1
            transcript.append(("REFUSE", q, cards[c1]["id"], s1, False))

    print(f"[band] OOD held-out: {inscope} in-scope, {oos_total} out-of-scope | gate_hi={a.hi} gate_lo={a.lo} margin={a.margin}\n")
    print(f"  ANSWER (direct, correct)      : {answered_ok}/{inscope}")
    print(f"  CLARIFY -> correct in top-2   : {clarify_recover}/{inscope}   <-- recovered from refusal")
    print(f"  CLARIFY -> miss (still safe)   : {clarify_miss}/{inscope}")
    print(f"  REFUSE                         : {refused}/{inscope}")
    eff = answered_ok + clarify_recover
    print(f"\n  EFFECTIVE success (answer + recoverable clarify): {eff}/{inscope} = {eff/inscope:.1%}")
    print(f"  (vs answer-only baseline: {answered_ok}/{inscope} = {answered_ok/inscope:.1%})")
    print(f"\n  out-of-scope: clean {oos_clean}/{oos_total}, clarify-noise {oos_clarify_noise}/{oos_total} (no false facts either way)\n")
    for kind, q, ids, s, ok in transcript:
        flag = "OK " if ok else "   "
        print(f"  {flag}{kind:8} cos={s:.2f}  {q[:48]:48}  -> {ids}")

if __name__ == "__main__":
    main()
