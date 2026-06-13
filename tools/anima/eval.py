#!/usr/bin/env python3
"""ANIMA retrieval benchmark. Measures whether the corpus actually answers real queries.

Loads the encoder + JSONL corpus, encodes every `ask` phrasing, then for each eval query
finds the best-matching card by cosine (the retrieval ceiling — full scan, no clustering)
and reports Recall@1, Recall@3, mean top-1 cosine, and the gate behaviour at cosine 0.66.

Eval set: tools/anima/eval_queries.jsonl — {"q","lang","expect"} where expect is a card id
(or an "id-prefix*" / "category:<cat>" matcher). Deliberately includes paraphrases and
cross-language queries NOT present verbatim in the cards, to stress real generalization.

Run:  python tools/anima/eval.py            # report
      python tools/anima/eval.py --min-recall1 0.8   # also exit non-zero if below (CI gate)
"""
import os, sys, json, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

GATE = 0.66  # mirrors L1_COS_MIN in nucleo_anima_l1.c (tuned via sweep: cuts out-of-scope
             # false positives 10->1 while keeping ~95% in-scope; in-scope mean cos ~0.85)

def matches(expect, card):
    # expect may be a single matcher or a LIST of acceptable ones (multi-valid: some questions
    # have more than one genuinely correct card, e.g. "can it go online?" -> esp.wifi OR os.wifi).
    if isinstance(expect, list):
        return any(matches(e, card) for e in expect)
    if expect.startswith("category:"): return card["category"] == expect.split(":", 1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect

def main():
    global GATE
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=os.path.join(os.path.dirname(__file__), "eval_queries.jsonl"))
    ap.add_argument("--kdir", default=None, help="knowledge dir (default: live; use a snapshot to avoid races)")
    ap.add_argument("--min-recall1", type=float, default=None, help="exit non-zero if Recall@1 below this")
    ap.add_argument("--max-fp", type=int, default=None, help="exit non-zero if out-of-scope false positives exceed this (0 = none allowed)")
    ap.add_argument("--gate", type=float, default=GATE, help="answer/refuse cosine threshold (default mirrors L1_COS_MIN)")
    ap.add_argument("--show-fails", action="store_true", default=True)
    a = ap.parse_args()
    GATE = a.gate

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus(kdir=a.kdir) if a.kdir else A.load_corpus()

    # one vector per index text (ask phrasings + answer cards' reply/detail) -> owning card
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def rank(q):
        qv = encode(q)
        cos = V @ qv
        order = np.argsort(cos)[::-1]
        ranked, seen = [], set()
        for i in order:                       # collapse phrasings -> distinct cards, best-first
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); ranked.append((ci, float(cos[i])))
            if len(ranked) >= 5: break
        return ranked

    qs = [json.loads(l) for l in open(a.queries, encoding="utf-8") if l.strip() and not l.startswith("//")]
    # expect == "none" => out-of-scope: the system MUST refuse (top-1 below the gate). A
    # confident hit there is a FALSE POSITIVE — the "confidently wrong" failure that adding
    # cards to a weak encoder can introduce. Recall is measured over in-scope queries only.
    inscope = oos = 0
    r1 = r3 = answered = correct_answered = oos_refused = oos_fp = 0
    cos_sum = 0.0; fails = []
    for item in qs:
        q, expect, lang = item["q"], item["expect"], item.get("lang", "?")
        top1_ci, top1_cos = rank(q)[0]
        hit = top1_cos >= GATE
        if expect == "none":
            oos += 1
            if hit: oos_fp += 1; fails.append(("FALSE+", lang, q, "none", cards[top1_ci]["id"], top1_cos))
            else:   oos_refused += 1
            continue
        inscope += 1; cos_sum += top1_cos
        ranked = rank(q)
        hit1 = matches(expect, cards[top1_ci])
        hit3 = any(matches(expect, cards[ci]) for ci, _ in ranked[:3])
        r1 += hit1; r3 += hit3
        if hit: answered += 1; correct_answered += hit1
        if not hit1: fails.append(("MISS", lang, q, expect, cards[top1_ci]["id"], top1_cos))

    print(f"[eval] {len(qs)} queries over {len(cards)} cards ({len(V)} phrasings)  gate={GATE}")
    if inscope:
        print(f"  in-scope ({inscope}):  Recall@1 {r1}/{inscope}={r1/inscope:.1%}  Recall@3 {r3}/{inscope}={r3/inscope:.1%}  mean cos {cos_sum/inscope:.3f}")
        print(f"    answered@gate: {answered}/{inscope}, correct {correct_answered}/{answered or 1}")
    if oos:
        print(f"  out-of-scope ({oos}):  refused(good) {oos_refused}  FALSE-POSITIVE(bad) {oos_fp}")
    if a.show_fails and fails:
        print(f"  issues ({len(fails)}):")
        for kind, lang, q, exp, got, cs in fails:
            print(f"    [{kind}] [{lang}] {q!r}  expected {exp} -> {got} ({cs:.2f})")
    gate_fails = []
    if a.min_recall1 is not None and inscope and r1/inscope < a.min_recall1:
        gate_fails.append(f"Recall@1 {r1/inscope:.1%} < target {a.min_recall1:.1%}")
    if a.max_fp is not None and oos_fp > a.max_fp:
        gate_fails.append(f"out-of-scope false-positives {oos_fp} > max {a.max_fp}")
    if gate_fails:
        sys.exit("[eval] FAIL: " + "; ".join(gate_fails))

if __name__ == "__main__":
    main()
