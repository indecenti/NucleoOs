#!/usr/bin/env python3
"""holo_probe — measure whether a BINARY ("holographic") L1 representation is migliorativa.

ANIMA's L1 today: int8 vectors on SD, cosine rerank feeds the 0.85 gate. The AKB4 trailer adds a
SYMMETRIC sign-bit Hamming PREFILTER (sign(query) vs sign(db)) -> top-M survivors -> exact int8 rerank.

This script controls one variable — the SCORING/QUANTIZATION scheme — over the REAL encoder + corpus,
and answers three questions with numbers, no flashing:

  1. END-TASK RECALL: can a binary-ONLY ranking replace int8 cosine?           (Option B: 8x smaller SD)
  2. PREFILTER RETENTION: does the cosine-winner survive the top-M prefilter?   (the device's real metric)
       symmetric  = sign(q) . sign(db)        (what AKB4 does today)
       asymmetric = int8(q) . sign(db)        (full-precision query, 1-bit db) <- the proposed change
  3. Does asymmetric retain the winner at a SMALLER M (fewer int8 rerank reads)?

Asymmetric keeps the int8 rerank, so the GATE still sees exact cosine -> zero recalibration, the safe win.
"""
import os, sys, json, struct, time
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
    t0 = time.time()
    for ci, c in enumerate(cards):
        for txt in A.index_texts(c):
            vecs.append(enc(txt)); labs.append(c["id"])
        if ci % 200 == 0:
            print(f"  encoding card {ci}/{len(cards)}  ({time.time()-t0:.1f}s)", file=sys.stderr)
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
        if e == "none" or e.startswith("category:"):  # abstention / category -> not a recall target
            continue
        items.append((c["q"], e))
    return items

def to_int8(unit):
    return np.clip(np.round(unit * 127.0), -127, 127).astype(np.int32)

def main():
    vec_f, labs = build_db()                      # (N,D) float unit vectors
    labs = np.array(labs, object)
    N, D = vec_f.shape
    db_i8 = to_int8(vec_f)                         # (N,D) int8-as-int32 (device DB)
    db_sign = np.where(db_i8 >= 0, 1, -1).astype(np.int32)   # tie 0 -> +1 (matches device)
    db_i8_norm = np.linalg.norm(db_i8, axis=1) + 1e-9
    print(f"DB: N={N} vectors, D={D}, from {os.path.basename(ENC)}", file=sys.stderr)

    table, H, Dd, NGRAMS, WORD_N = A.load_encoder(ENC)
    enc = A.make_encoder(table, H, Dd, NGRAMS)

    def expect_hit(idx_sorted, expect, k):
        # expect may be exact id or "prefix*"
        topk = labs[idx_sorted[:k]]
        if expect.endswith("*"):
            p = expect[:-1]
            return any(str(x).startswith(p) for x in topk)
        return expect in [str(x) for x in topk]

    Ms = [8, 16, 32, 64]
    sets = [("in-dist", os.path.join(ROOT, "tools", "anima", "eval_queries.jsonl")),
            ("OOD",     os.path.join(ROOT, "tools", "anima", "eval_ood.jsonl"))]

    for name, fn in sets:
        items = load_eval(fn)
        n = len(items)
        # end-task recall@1/@3
        rc = {"cos1":0,"cos3":0,"sym1":0,"sym3":0,"asym1":0,"asym3":0}
        # prefilter retention of the COSINE winner, by M
        ret_sym = {m:0 for m in Ms}; ret_asym = {m:0 for m in Ms}
        for q, expect in items:
            qf = enc(q); qi = to_int8(qf)
            qn = np.linalg.norm(qi) + 1e-9
            s_cos  = (db_i8 @ qi) / (db_i8_norm * qn)        # exact device cosine
            s_sym  = db_sign @ np.where(qi >= 0, 1, -1)       # symmetric: sign(q).sign(db)
            s_asym = db_sign @ qi                             # asymmetric: int8(q).sign(db)
            o_cos  = np.argsort(-s_cos)
            o_sym  = np.argsort(-s_sym)
            o_asym = np.argsort(-s_asym)
            rc["cos1"]  += expect_hit(o_cos, expect, 1);  rc["cos3"]  += expect_hit(o_cos, expect, 3)
            rc["sym1"]  += expect_hit(o_sym, expect, 1);  rc["sym3"]  += expect_hit(o_sym, expect, 3)
            rc["asym1"] += expect_hit(o_asym, expect, 1); rc["asym3"] += expect_hit(o_asym, expect, 3)
            gold = o_cos[0]                                  # the vector the device int8 rerank would pick
            sym_rank  = np.where(o_sym  == gold)[0][0]
            asym_rank = np.where(o_asym == gold)[0][0]
            for m in Ms:
                ret_sym[m]  += (sym_rank  < m)
                ret_asym[m] += (asym_rank < m)
        pct = lambda x: f"{100.0*x/n:5.1f}%"
        print(f"\n=== {name}  (n={n}) ===")
        print("  END-TASK recall    R@1     R@3     bytes/vec")
        print(f"    int8 cosine     {pct(rc['cos1'])}  {pct(rc['cos3'])}   {D}")
        print(f"    binary symmetric{pct(rc['sym1'])}  {pct(rc['sym3'])}   {D//8}")
        print(f"    binary asymmetr.{pct(rc['asym1'])}  {pct(rc['asym3'])}   {D//8}")
        print("  PREFILTER retention of the cosine-winner (device correctness vs exact):")
        print("     M        symmetric   asymmetric")
        for m in Ms:
            print(f"     {m:<3}      {pct(ret_sym[m])}      {pct(ret_asym[m])}")

if __name__ == "__main__":
    main()
