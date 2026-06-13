#!/usr/bin/env python3
"""NLU lever probe — measure principled retrieval upgrades on the HELD-OUT OOD set.

The distilled char-ngram encoder is the documented ceiling (OOD Recall@1 ~22%, e5 ~50%).
Re-distilling bigger failed (more surface dependence). This probes representation/scoring
levers that are mathematically grounded AND cheap enough for the ESP, measuring each on
eval_ood.jsonl (queries phrased UNLIKE the cards — the honest generalization signal) while
checking in-distribution doesn't regress. Ship only what wins. NOT a device change by itself.

Levers:
  baseline : sum(table[f]) then L2-normalize (current encoder).
  idf      : sum(idf[f]*table[f]) — downweight ubiquitous n-grams (TF-IDF pooling). Shippable
             with ZERO device change: bake idf into the encoder rows offline + requantize.
  sif      : remove the top principal component from every embedding (Arora 2017 "SIF"). Tiny
             device change: one dot + axpy with a fixed D-vector.
  idf+sif  : both.
  hybrid   : fuse dense cosine rank with an IDF-weighted lexical-overlap rank (reciprocal rank
             fusion). Robust where a rare shared term matters but the dense vec is weak.

Run:  python tools/anima/nlu_probe.py
"""
import os, sys, json, math
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))


def matches(expect, card):
    if isinstance(expect, list):
        return any(matches(e, card) for e in expect)
    if expect.startswith("category:"): return card["category"] == expect.split(":", 1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect


def load_q(name):
    p = os.path.join(HERE, name)
    return [json.loads(l) for l in open(p, encoding="utf-8") if l.strip() and not l.startswith("//")]


def main():
    table, H, D, NGRAMS, _ = A.load_encoder()
    feats = A.make_feats(H, NGRAMS)
    cards, _ = A.load_corpus()

    # index texts (ask + reply/detail) -> owning card, with their feature id lists
    texts, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            texts.append(p); owner.append(ci)

    tablef = table.astype(np.float32)

    # ---- IDF over the index documents (each index text = one document) ----
    Ndoc = len(texts)
    df = np.zeros(H, np.float64)
    feat_cache = []
    for t in texts:
        ids = feats(t); feat_cache.append(ids)
        for f in set(ids): df[f] += 1
    idf = np.log((Ndoc + 1.0) / (df + 1.0)) + 1.0          # smoothed idf, >=1

    def emb(ids, use_idf):
        a = np.zeros(D, np.float64)
        if use_idf:
            for f in ids: a += idf[f] * tablef[f]
        else:
            for f in ids: a += tablef[f]
        n = np.linalg.norm(a)
        return (a / n) if n > 0 else a

    def build(use_idf):
        return np.stack([emb(ids, use_idf) for ids in feat_cache]).astype(np.float32)

    Vbase = build(False)
    Vidf = build(True)

    # ---- SIF: remove top principal component (computed on the index matrix) ----
    def sif_dir(V):
        # first right singular vector
        Vc = V - V.mean(0, keepdims=True)
        _, _, Vt = np.linalg.svd(Vc, full_matrices=False)
        return Vt[0]

    def sif_apply(V, u):
        P = V @ u
        Vs = V - np.outer(P, u)
        n = np.linalg.norm(Vs, axis=1, keepdims=True); n[n == 0] = 1
        return Vs / n

    u_base = sif_dir(Vbase)
    Vbase_sif = sif_apply(Vbase, u_base)
    u_idf = sif_dir(Vidf)
    Vidf_sif = sif_apply(Vidf, u_idf)

    def encq(q, use_idf, sif_u=None):
        v = emb(feats(q), use_idf).astype(np.float32)
        if sif_u is not None:
            v = v - (v @ sif_u) * sif_u
            n = np.linalg.norm(v);  v = v / n if n > 0 else v
        return v

    # lexical (sparse) score: idf-weighted shared-word overlap, query vs each index text
    def words(s): return A.norm_text(s).split()
    text_wset = [set(words(t)) for t in texts]
    widf = {}
    for ws in text_wset:
        for w in ws: widf[w] = widf.get(w, 0) + 1
    def w_idf(w): return math.log((Ndoc + 1.0) / (widf.get(w, 0) + 1.0)) + 1.0

    def ranked_dense(V, qv, topn=5):
        cos = V @ qv
        order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[i])))
            if len(out) >= topn: break
        return out

    def ranked_hybrid(qv, q, topn=5):
        cos = Vbase @ qv
        qw = set(words(q))
        lex = np.zeros(len(texts))
        for j, ws in enumerate(text_wset):
            inter = qw & ws
            if inter: lex[j] = sum(w_idf(w) for w in inter)
        # reciprocal rank fusion over the two per-text rankings, then collapse to cards
        def rrf_ranks(score):
            order = np.argsort(score)[::-1]
            rank = np.empty(len(score)); rank[order] = np.arange(len(score))
            return rank
        rd, rl = rrf_ranks(cos), rrf_ranks(lex)
        K = 30.0
        fused = 1.0 / (K + rd) + 1.0 / (K + rl)
        order = np.argsort(fused)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[i])))
            if len(out) >= topn: break
        return out

    # Conservative hybrid: keep the dense ranking when it is CONFIDENT (top-1 cos >= thr, where
    # in-distribution queries live), and only let the lexical signal re-order the top-K when the
    # dense vec is uncertain (where OOD lives). Designed to be non-regressive on in-dist.
    def ranked_hybrid_cons(qv, q, thr=0.80, topn=5):
        dense = ranked_dense(Vbase, qv, topn=8)
        if dense and dense[0][1] >= thr:
            return dense[:topn]
        return ranked_hybrid(qv, q, topn=topn)

    methods = {
        "baseline":  lambda q: ranked_dense(Vbase, encq(q, False)),
        "idf":       lambda q: ranked_dense(Vidf, encq(q, True)),
        "sif":       lambda q: ranked_dense(Vbase_sif, encq(q, False, u_base)),
        "idf+sif":   lambda q: ranked_dense(Vidf_sif, encq(q, True, u_idf)),
        "hybrid":    lambda q: ranked_hybrid(encq(q, False), q),
        "hyb_cons":  lambda q: ranked_hybrid_cons(encq(q, False), q),
    }

    ood = load_q("eval_ood.jsonl")
    ind = [x for x in load_q("eval_queries.jsonl") if x["expect"] != "none"]
    oos = [x for x in load_q("eval_queries.jsonl") if x["expect"] == "none"]

    def score(rankfn, qs):
        r1 = r3 = 0; cs = 0.0
        for it in qs:
            r = rankfn(it["q"]); top = r[0]
            cs += top[1]
            if matches(it["expect"], cards[top[0]]): r1 += 1
            if any(matches(it["expect"], cards[ci]) for ci, _ in r[:3]): r3 += 1
        n = len(qs)
        return r1 / n, r3 / n, cs / n

    def sep(rankfn):
        # gate-independent separability: mean in-scope top1 cos vs mean out-of-scope top1 cos
        ins = np.mean([rankfn(it["q"])[0][1] for it in ind])
        out = np.mean([rankfn(it["q"])[0][1] for it in oos])
        return ins, out

    print(f"cards={len(cards)} index_texts={len(texts)} D={D} H={H}")
    print(f"OOD={len(ood)} in-dist={len(ind)} out-of-scope={len(oos)}\n")
    print(f"{'method':10}  {'OOD R@1':>8} {'OOD R@3':>8}  {'IND R@1':>8} {'IND R@3':>8}  {'in-cos':>7} {'oos-cos':>7} {'margin':>7}")
    for name, fn in methods.items():
        o1, o3, _ = score(fn, ood)
        i1, i3, _ = score(fn, ind)
        ic, oc = sep(fn)
        print(f"{name:10}  {o1:8.1%} {o3:8.1%}  {i1:8.1%} {i3:8.1%}  {ic:7.3f} {oc:7.3f} {ic-oc:7.3f}")


if __name__ == "__main__":
    main()
