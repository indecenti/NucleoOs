#!/usr/bin/env python3
"""holo_cascade — find the BEST way to apply the asymmetric "holographic" prefilter on-device.

holo_probe.py proved asymmetric (int8 query x 1-bit DB) retains the cosine-winner far better than
symmetric popcount at equal SD cost. But asymmetric is PER-DIM (256 ±adds), not a 4-word popcount,
so it can't be the cheap bulk scan over thousands of candidates. This script measures whether a
3-STAGE in-RAM cascade recovers pure-asymmetric quality cheaply:

    Stage 0  popcount(sym) over ALL probed cands     -> keep wide pool W      (cheap, today's scan)
    Stage 1  asymmetric over the W survivors         -> keep M                (RAM only, no new I/O)
    Stage 2  exact int8 cosine over the M            -> gate                  (M SD reads, was 64)

We answer:
  - RETENTION of the exact cosine-winner: pure-asym@M vs cascade(W->M) vs today's sym@64.
  - END-TASK recall@1/@3 through the real pipeline (exact rerank over the M survivors picks the card).
  - The (W,M) knee: smallest M (fewest SD reads) that matches/beats today's correctness.
Cascade collapses to today's behavior at W==M (so the change is provably non-regressive by a knob).
"""
import os, sys, json, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

ROOT = A.ROOT
ENC  = os.environ.get("ANIMA_ENC", os.path.join(ROOT, "tools", "anima-host", "sd", "data", "anima", "anima-it-encoder.bin"))
CACHE = os.path.join(ROOT, "tools", "anima", ".holo_db.npz")

def build_db():
    if os.path.exists(CACHE):
        z = np.load(CACHE, allow_pickle=True)
        if str(z["enc"]) == ENC:
            return z["vec"].astype(np.float32), list(z["lab"])
    table, H, D, NGRAMS, WORD_N = A.load_encoder(ENC)
    enc = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus(strict=False)
    vecs, labs = [], []
    for c in cards:
        for txt in A.index_texts(c):
            vecs.append(enc(txt)); labs.append(c["id"])
    vec = np.asarray(vecs, np.float32)
    np.savez(CACHE, vec=vec, lab=np.array(labs, object), enc=ENC)
    return vec, labs

def load_eval(fn):
    items = []
    for line in open(fn, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("//"): continue
        c = json.loads(line)
        e = str(c.get("expect", ""))
        if e == "none" or e.startswith("category:"): continue
        items.append((c["q"], e))
    return items

def to_int8(unit):
    return np.clip(np.round(unit * 127.0), -127, 127).astype(np.int32)

def main():
    vec_f, labs = build_db()
    labs = np.array(labs, object)
    N, D = vec_f.shape
    db_i8 = to_int8(vec_f)
    db_sign = np.where(db_i8 >= 0, 1, -1).astype(np.int32)
    db_norm = np.linalg.norm(db_i8, axis=1) + 1e-9
    table, H, Dd, NGRAMS, WORD_N = A.load_encoder(ENC)
    enc = A.make_encoder(table, H, Dd, NGRAMS)
    print(f"DB: N={N} D={D}", file=sys.stderr)

    def hit(idx, expect, k):
        topk = labs[idx[:k]]
        if expect.endswith("*"):
            p = expect[:-1]
            return any(str(x).startswith(p) for x in topk)
        return expect in [str(x) for x in topk]

    Ws = [32, 64, 128, 256, 512]
    Ms = [8, 12, 16, 24, 32]
    sets = [("in-dist", os.path.join(ROOT, "tools", "anima", "eval_queries.jsonl")),
            ("OOD",     os.path.join(ROOT, "tools", "anima", "eval_ood.jsonl"))]

    for name, fn in sets:
        items = load_eval(fn); n = len(items)
        ret_sym64 = 0                                   # today's baseline: popcount M=64
        ret_pure = {m:0 for m in Ms}                    # pure asymmetric @M
        ret_casc = {(w,m):0 for w in Ws for m in Ms if m <= w}
        end_casc = {(w,m):[0,0] for w in Ws for m in Ms if m <= w}   # [R@1, R@3] via exact rerank of M
        end_base = [0,0]                                 # today: exact rerank of popcount-64
        for q, expect in items:
            qf = enc(q); qi = to_int8(qf); qn = np.linalg.norm(qi) + 1e-9
            s_cos  = (db_i8 @ qi) / (db_norm * qn)
            s_sym  = db_sign @ np.where(qi >= 0, 1, -1)
            s_asym = db_sign @ qi
            o_cos  = np.argsort(-s_cos); gold = o_cos[0]
            o_sym  = np.argsort(-s_sym)
            o_asym = np.argsort(-s_asym)
            ret_sym64 += (np.where(o_sym == gold)[0][0] < 64)
            # baseline end-task: exact cosine over the popcount-64 survivors
            surv = o_sym[:64]
            re = surv[np.argsort(-s_cos[surv])]
            end_base[0] += hit(re, expect, 1); end_base[1] += hit(re, expect, 3)
            for m in Ms:
                ret_pure[m] += (np.where(o_asym == gold)[0][0] < m)
            for w in Ws:
                pool = o_sym[:w]                                  # stage-0 popcount cull
                casc = pool[np.argsort(-s_asym[pool])]           # stage-1 asymmetric rerank
                grank = np.where(casc == gold)[0]
                grank = grank[0] if len(grank) else 10**9
                for m in Ms:
                    if m > w: continue
                    ret_casc[(w,m)] += (grank < m)
                    surv = casc[:m]                               # stage-2 exact rerank of M
                    re = surv[np.argsort(-s_cos[surv])]
                    e = end_casc[(w,m)]
                    e[0] += hit(re, expect, 1); e[1] += hit(re, expect, 3)
        pct = lambda x: f"{100.0*x/n:5.1f}%"
        print(f"\n=== {name} (n={n}) ===")
        print(f"  BASELINE today  popcount->exact M=64 : retention {pct(ret_sym64)}   end R@1 {pct(end_base[0])} R@3 {pct(end_base[1])}")
        print("  PURE asymmetric retention of cosine-winner by M:")
        print("    " + "  ".join(f"M={m}:{pct(ret_pure[m])}" for m in Ms))
        print("  CASCADE popcount(W)->asym->exact(M)  end-task R@1  [retention in brackets]:")
        hdr = "    W\\M  " + "".join(f"{m:>10}" for m in Ms)
        print(hdr)
        for w in Ws:
            cells = []
            for m in Ms:
                if m > w: cells.append(f"{'-':>10}"); continue
                r1 = 100.0*end_casc[(w,m)][0]/n
                rt = 100.0*ret_casc[(w,m)]/n
                cells.append(f"{r1:5.1f}[{rt:4.1f}]")
            print(f"    {w:<4}" + "".join(cells))

if __name__ == "__main__":
    main()
