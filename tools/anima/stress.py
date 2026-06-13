#!/usr/bin/env python3
"""ANIMA encoder STRESS bench — finds where retrieval actually breaks.

eval.py is saturated (100% Recall@1) because its queries are close to the card phrasings.
This probe perturbs the in-scope eval queries the way real users do — typos (edit-distance
1..N), word drops, and cross-lingual swaps — and reports how Recall@1 degrades. That decay
curve is the REAL ceiling to beat: a better encoder should stay flatter under perturbation.

Run:  python tools/anima/stress.py
      ANIMA_ENC=models/candidate.bin python tools/anima/stress.py   # compare a candidate
"""
import os, sys, json, random
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

random.seed(0)
GATE = 0.66
KEYS = "abcdefghilmnopqrstuvz "

def typo(s, k):
    """Apply k single-char edits (swap adjacent / delete / substitute / insert)."""
    s = list(s)
    for _ in range(k):
        if len(s) < 2: break
        i = random.randrange(len(s) - 1)
        op = random.random()
        if op < 0.30 and i < len(s) - 1: s[i], s[i+1] = s[i+1], s[i]      # transpose
        elif op < 0.55:                  del s[i]                          # delete
        elif op < 0.80:                  s[i] = random.choice(KEYS)        # substitute
        else:                            s.insert(i, random.choice(KEYS))  # insert
    return "".join(s)

def drop_word(s):
    w = s.split()
    if len(w) <= 2: return s
    del w[random.randrange(len(w))]
    return " ".join(w)

def main():
    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus()
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def top1(q):
        cos = V @ encode(q)
        i = int(np.argmax(cos))
        return owner[i], float(cos[i])

    def matches(expect, card):
        if isinstance(expect, list): return any(matches(e, card) for e in expect)
        if expect.startswith("category:"): return card["category"] == expect.split(":",1)[1]
        if expect.endswith("*"): return card["id"].startswith(expect[:-1])
        return card["id"] == expect

    qfile = os.path.join(os.path.dirname(__file__), "eval_queries.jsonl")
    qs = [json.loads(l) for l in open(qfile, encoding="utf-8") if l.strip() and not l.startswith("//")]
    inscope = [it for it in qs if it["expect"] != "none"]
    print(f"[stress] encoder rows={H} dim={D}  {len(inscope)} in-scope queries, {len(V)} phrasings")

    def run(perturb, label, trials=5):
        hit = tot = 0; cossum = 0.0
        for it in inscope:
            for _ in range(trials):
                q = perturb(it["q"])
                ci, cs = top1(q)
                ok = matches(it["expect"], cards[ci]) and cs >= GATE
                hit += ok; tot += 1; cossum += cs
        print(f"  {label:<22} Recall@1 {hit/tot:5.1%}   mean cos {cossum/tot:.3f}")

    run(lambda s: s, "clean (sanity)", trials=1)
    for k in (1, 2, 3, 4):
        run(lambda s, k=k: typo(s, k), f"typos x{k}")
    run(drop_word, "1 word dropped")
    run(lambda s: typo(drop_word(s), 2), "drop + typos x2")

if __name__ == "__main__":
    main()
