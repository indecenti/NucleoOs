#!/usr/bin/env python3
"""ANIMA offline factory — REPRESENTATION REFINER (zero device cost).

The on-device embedding is e(s) = normalize( sum_{f in feats(s)} T[f] ), a learned hashed
bag-of-n-grams (nucleo_anima_l1.c / anima_lib.py). Two classic, well-founded transforms can be
*folded entirely into the table T offline*, so the ESP32 kernel stays byte-identical — it still
just sums int8 rows and L2-normalizes:

  1. IDF FEATURE WEIGHTING.  Scaling every row by a per-feature weight w[f] is linear and
     distributes through the sum:  sum w[f]*T[f] = sum (w[f]*T[f]).  Rare, discriminative
     n-grams get amplified; ubiquitous ones ("come/cosa/che/the/how") get damped — directly
     attacking the diagnosed failure mode ("more surface frequency dominates the sum").
     w[f] = idf(f)^alpha,  idf(f) = log((N_cards+1)/(df(f)+1)) + 1.

  2. SPECTRAL DE-BIASING ("all-but-the-top", Mu & Viswanath, ICLR 2018).  Summed embeddings
     share a dominant COMMON direction (the function-word mass) that carries no class signal
     yet dominates cosine. Removing the top-r principal directions of the (pre-normalization)
     summed vectors via the linear projection  A = I - U U^T  also folds into the table:
     normalize(A * sum T_w[f]) = normalize(sum (A * T_w[f])).  We fit U on the corpus' own
     index-text embeddings, so it's the common component of THIS knowledge base.

Both compose:  T'[f] = A @ (w[f] * T[f]),  re-quantized to int8 with a single global scale
(cosine is scale-invariant, so a global scale preserves the summation semantics exactly).

This is NOT "a bigger encoder" (that was measured to not move OOD): same H, D, NGRAMS, same
ANE2 format, same ~3 MB, same kernel. It is a better-conditioned *representation* of the
features already there. We keep a config ONLY if the held-out OOD Recall@1 rises while in-dist
stays 100% / 0 false-positives — measured here, device-faithfully, on the re-quantized table.

Usage:
  python tools/anima/refine.py --search                 # grid-search alpha x r, print table
  python tools/anima/refine.py --alpha 1 --remove 2 --out models/anima-it-encoder.refined.bin
  ANIMA_ENC=models/anima-it-encoder.refined.bin python tools/anima/eval.py   # confirm
"""
import os, sys, struct, argparse, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = 0.66                                   # mirrors L1_COS_MIN
EVAL_INDIST = os.path.join(HERE, "eval_queries.jsonl")
EVAL_OOD    = os.path.join(HERE, "eval_ood.jsonl")


def load_orig():
    """Load the ORIGINAL distilled table (never the env-overridden one)."""
    return A.load_encoder(A.ENC)


def compute_idf(cards, feats, H, alpha):
    """Per-row idf^alpha over the corpus. df(row) = #cards touching that hashed row."""
    if alpha == 0.0:
        return np.ones(H, np.float64)
    df = np.zeros(H, np.float64)
    N = len(cards)
    for c in cards:
        rows = set()
        for t in A.index_texts(c):
            rows.update(feats(t))
        for r in rows:
            df[r] += 1.0
    idf = np.log((N + 1.0) / (df + 1.0)) + 1.0     # smoothed, always >= 1
    return idf ** alpha


def corpus_vectors(cards, feats, table_w, D):
    """Pre-normalization summed vectors g(s) = sum table_w[feats(s)] for every index text."""
    G = []
    for c in cards:
        for t in A.index_texts(c):
            ids = feats(t)
            G.append(table_w[ids].sum(0) if ids else np.zeros(D))
    return np.asarray(G, np.float64)


def debias_matrix(G, r):
    """A = I - U U^T, U = top-r right singular vectors of the summed vectors G (uncentered:
    the 1st captures the common 'mean' direction). r=0 -> identity (no de-biasing)."""
    D = G.shape[1]
    if r <= 0:
        return np.eye(D)
    # power of SVD on the gram matrix is overkill; G is (n x D), D small -> economy SVD.
    _, _, Vt = np.linalg.svd(G, full_matrices=False)
    U = Vt[:r].T                                   # D x r
    return np.eye(D) - U @ U.T


def build_table(table, idf, A_proj):
    """T'[f] = A @ (idf[f] * T[f]); quantize int8 with ONE global scale. Returns (q_int8, scale)."""
    Tw = table.astype(np.float64) * idf[:, None]   # row scaling (IDF)
    Tp = Tw @ A_proj.T                             # linear de-biasing per row
    gmax = float(np.abs(Tp).max())
    scale = (127.0 / gmax) if gmax > 0 else 1.0
    q = np.clip(np.round(Tp * scale), -127, 127).astype(np.int8)
    return q, scale


def write_ane2(path, q, H, D, NGRAMS, WORD_N, scale):
    with open(path, "wb") as f:
        f.write(b"ANE2")
        f.write(struct.pack("<IIII", H, D, len(NGRAMS), WORD_N))
        f.write(struct.pack("<" + "I" * len(NGRAMS), *NGRAMS))
        f.write(struct.pack("<f", float(scale)))
        f.write(q.astype(np.int8).tobytes())


# ---- device-faithful evaluation on a (re-quantized) table -------------------------------
def load_eval(path):
    return [json.loads(l) for l in open(path, encoding="utf-8") if l.strip() and not l.startswith("//")]


def matches(expect, card):
    if isinstance(expect, list):       return any(matches(e, card) for e in expect)
    if expect.startswith("category:"): return card["category"] == expect.split(":", 1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect


def evaluate(q_table, H, D, NGRAMS, cards, qs):
    """Rank queries against index-text vectors, collapse to cards. Returns (r1,r3,inscope,
    mean_cos, fp, oos). q_table is the int8 table cast to int32 (what the device holds)."""
    encode = A.make_encoder(q_table.astype(np.int32), H, D, NGRAMS)
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def rank(qtext):
        qv = encode(qtext); cos = V @ qv; order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[i])))
            if len(out) >= 3: break
        return out

    r1 = r3 = inscope = fp = oos = 0; cs = 0.0
    for item in qs:
        expect = item["expect"]; rk = rank(item["q"]); (c1, s1) = rk[0]
        if expect == "none":
            oos += 1; fp += (s1 >= GATE); continue
        inscope += 1; cs += s1
        r1 += matches(expect, cards[c1])
        r3 += any(matches(expect, cards[ci]) for ci, _ in rk[:3])
    return r1, r3, inscope, (cs / inscope if inscope else 0), fp, oos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--search", action="store_true", help="grid-search alpha x remove, print table")
    ap.add_argument("--alpha", type=float, default=1.0, help="IDF exponent (0 = off)")
    ap.add_argument("--remove", type=int, default=2, help="# common directions to remove (0 = off)")
    ap.add_argument("--out", default=None, help="write the refined ANE2 encoder here")
    a = ap.parse_args()

    table, H, D, NGRAMS, WORD_N = load_orig()
    feats = A.make_feats(H, NGRAMS)
    cards, _ = A.load_corpus()
    qs_in  = load_eval(EVAL_INDIST)
    qs_ood = load_eval(EVAL_OOD)
    print(f"[refine] encoder {H}x{D} ngrams={NGRAMS} | {len(cards)} cards | "
          f"{len(qs_in)} in-dist, {len(qs_ood)} OOD queries")

    def corpus_vectors_from_Tw(Tw):
        G = []
        for c in cards:
            for t in A.index_texts(c):
                ids = feats(t); G.append(Tw[ids].sum(0) if ids else np.zeros(D))
        return np.asarray(G, np.float64)

    # idf + summed vectors depend only on alpha (not r); cache per alpha so the grid is cheap.
    _cache = {}
    def prep(alpha):
        if alpha not in _cache:
            idf = compute_idf(cards, feats, H, alpha)
            Tw = table.astype(np.float64) * idf[:, None]
            _cache[alpha] = (idf, Tw, corpus_vectors_from_Tw(Tw))
        return _cache[alpha]

    def run(alpha, r):
        idf, Tw, G = prep(alpha)
        Aproj = debias_matrix(G, r)
        q, scale = build_table(table, idf, Aproj)
        din = evaluate(q, H, D, NGRAMS, cards, qs_in)
        doo = evaluate(q, H, D, NGRAMS, cards, qs_ood)
        return q, scale, din, doo

    if a.search:
        # Honest hyperparameter selection: split OOD into DEV (tune on) and TEST (report on),
        # deterministic 50/50 by index parity so selection never sees the reported half.
        ood_dev  = [x for i, x in enumerate(qs_ood) if i % 2 == 0]
        ood_test = [x for i, x in enumerate(qs_ood) if i % 2 == 1]
        alphas = [0.0, 0.5, 1.0, 1.5]
        removes = [0, 1, 2, 4, 6, 8, 10, 12]
        print(f"\n  DEV={len(ood_dev)} TEST={len(ood_test)} (held-out OOD halves)\n")
        print(f"  {'alpha':>5} {'rm':>3} | {'devR@1':>7} {'testR@1':>8} {'OODcos':>7} | "
              f"{'IN R@1':>7} {'FP':>3}   keep?")
        print("  " + "-" * 72)
        base_dev = base_test = None
        best = None              # (dev_r1, alpha, r, test_r1) under strict in-dist==100% & FP==0
        for alpha in alphas:
            for r in removes:
                q, scale, din, _ = run(alpha, r)
                r1i, _, ni, _, fpi, _ = din
                d1, _, nd, _, _, _ = evaluate(q, H, D, NGRAMS, cards, ood_dev)
                t1, _, nt, _, _, _ = evaluate(q, H, D, NGRAMS, cards, ood_test)
                _, _, _, co, _, _ = evaluate(q, H, D, NGRAMS, cards, qs_ood)
                devr, testr, indr = d1 / nd, t1 / nt, r1i / ni
                ok = (abs(indr - 1.0) < 1e-9) and (fpi == 0)
                if alpha == 0.0 and r == 0: base_dev, base_test = devr, testr
                if ok and (best is None or devr > best[0]): best = (devr, alpha, r, testr)
                print(f"  {alpha:>5.1f} {r:>3d} | {devr:>6.1%} {testr:>7.1%} {co:>7.3f} | "
                      f"{indr:>6.1%} {fpi:>3d}   {'ok' if ok else 'no'}")
        print("  " + "-" * 72)
        print(f"  baseline:  dev {base_dev:.1%}  test {base_test:.1%}")
        if best:
            print(f"  SELECTED on dev: alpha={best[1]} remove={best[2]} -> dev {best[0]:.1%} "
                  f"(+{(best[0]-base_dev)*100:.1f})  |  TEST {best[3]:.1%} (+{(best[3]-base_test)*100:.1f} pts, held-out)")
        return

    q, scale, din, doo = run(a.alpha, a.remove)
    r1i, r3i, ni, ci, fpi, _ = din
    r1o, r3o, no, co, _, _ = doo
    print(f"  config alpha={a.alpha} remove={a.remove}  scale={scale:.4f}")
    print(f"  in-dist : R@1 {r1i}/{ni}={r1i/ni:.1%}  R@3 {r3i/ni:.1%}  FP {fpi}")
    print(f"  OOD     : R@1 {r1o}/{no}={r1o/no:.1%}  R@3 {r3o/no:.1%}  mean cos {co:.3f}")
    if a.out:
        write_ane2(a.out, q, H, D, NGRAMS, WORD_N, scale)
        print(f"  wrote refined encoder -> {a.out} ({os.path.getsize(a.out)/1024:.1f} KB)")


if __name__ == "__main__":
    main()
