#!/usr/bin/env python3
"""
ANIMA L1 fast-path research bench (HOST, real vectors).

Goal: prove that a two-stage retrieval — a 1-bit SimHash *binary prefilter* (XOR+popcount)
followed by an *exact* int8 rerank on the few survivors — preserves the answer the current
brute-force int8 cosine would pick, while doing far fewer exact dot products and far less I/O.

This is the algorithmic core; if it holds on the real index we port the kernel to firmware.

Reads the REAL device index  tools/anima-host/sd/data/anima/anima-it-index.bin  (AKB3),
replicates nucleo_anima_l1_query()'s flow byte-for-byte (centroid pass -> adaptive nprobe ->
rerank), and measures fidelity + cost vs the exact baseline on realistic paraphrase queries.
"""
import os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
IDX  = os.path.join(HERE, "..", "..", "anima-host", "sd", "data", "anima", "anima-it-index.bin")

# ---------------------------------------------------------------- load AKB3 (mirror load_index)
def load_index(path):
    with open(path, "rb") as f:
        buf = f.read()
    assert buf[:4] == b"AKB3", buf[:4]
    D, K, N = struct.unpack_from("<III", buf, 4)
    off = 16
    centroids = np.frombuffer(buf, np.int8, K * D, off).reshape(K, D).copy(); off += K * D
    cdir = np.frombuffer(buf, np.uint32, K * 2, off).reshape(K, 2).copy(); off += K * 2 * 4
    vec_base = off
    rec = D + 4
    # vectors section: N records of (int8[D] vec, u32 ansoff)
    raw = np.frombuffer(buf, np.uint8, N * rec, vec_base).reshape(N, rec)
    vecs = raw[:, :D].view(np.int8).astype(np.int8)
    ans  = raw[:, D:].copy().view(np.uint32).reshape(N)
    return D, K, N, centroids, cdir, vecs, ans

D, K, N, CENTROIDS, CDIR, VECS, ANS = load_index(IDX)
print(f"[idx] AKB3 D={D} K={K} N={N}  (~{N//K} vec/cluster)")

VF   = VECS.astype(np.int32)
VNRM = np.sqrt((VF * VF).sum(1)).astype(np.float64)          # exact per-vector L2 norm
CF   = CENTROIDS.astype(np.int32)
CNRM = np.sqrt((CF * CF).sum(1)).astype(np.float64)

# ---- FREE WIN #1: is the vss recompute (firmware l1.c:606) actually wasteful? -----------------
print(f"[norm] card-vector L2 norm: mean={VNRM.mean():.1f} std={VNRM.std():.2f} "
      f"min={VNRM.min():.0f} max={VNRM.max():.0f}  (stored as unit*127 -> ~127 by construction)")

# ---------------------------------------------------------------- SimHash signatures (1 bit/dim)
# sign bit per dimension; D=256 -> 4x uint64 = 32 bytes/vector. Hamming via popcount.
def signatures(mat_i32):
    bits = (mat_i32 >= 0)                                    # >=0 -> 1  (ties to 1, like the device will)
    packed = np.packbits(bits, axis=1, bitorder="little")   # (n, D/8) uint8
    return packed.view(np.uint64).reshape(mat_i32.shape[0], D // 64)
SIG_V = signatures(VF)
SIG_C = signatures(CF)

POPC = np.array([bin(i).count("1") for i in range(256)], np.uint8)
def hamming(sig_q, sig_block):                              # sig_q: (W,)  sig_block: (m, W) uint64
    x = np.bitwise_xor(sig_block, sig_q)                    # (m, W)
    xb = x.view(np.uint8).reshape(x.shape[0], -1)
    return POPC[xb].sum(1).astype(np.int32)                # (m,) Hamming distance in [0, D]

# ---------------------------------------------------------------- replicate firmware retrieval
def probe_clusters(q_i32):
    qn = np.sqrt((q_i32 * q_i32).sum()) + 1e-6
    cos = (CF @ q_i32) / (qn * CNRM + 1e-6)
    order = np.argsort(-cos)
    topc = order[:6]
    margin = cos[topc[0]] - cos[topc[1]]
    nprobe = 2 if margin >= 0.28 else 3 if margin >= 0.10 else 6
    return topc[:nprobe], qn

def candidate_ids(topc):
    ids = []
    for c in topc:
        off, cnt = int(CDIR[c, 0]), int(CDIR[c, 1])
        if cnt: ids.append(np.arange(off, off + cnt))
    return np.concatenate(ids) if ids else np.empty(0, np.int64)

def exact_argmax(q_i32, ids, qn):                          # the BASELINE the firmware returns
    dot = VF[ids] @ q_i32
    cos = dot / (qn * VNRM[ids] + 1e-6)
    return ids[int(cos.argmax())], cos

def fastpath_argmax(q_i32, sig_q, ids, qn, M):             # prefilter by Hamming, rerank M exact
    h = hamming(sig_q, SIG_V[ids])
    keep = ids[np.argpartition(h, min(M, len(ids) - 1))[:M]] if len(ids) > M else ids
    dot = VF[keep] @ q_i32
    cos = dot / (qn * VNRM[keep] + 1e-6)
    return keep[int(cos.argmax())], len(keep)

# ---------------------------------------------------------------- realistic paraphrase queries
# A real query is a paraphrase: its encoder vector sits NEAR a stored card vector but not on it.
# Model: pick a random card, rotate it to a target cosine c in [0.75,0.97] (the gate's live range),
# requantize to int8. The "right answer" is whatever EXACT rerank picks over the probed clusters;
# we measure whether the fast path returns the SAME id (fidelity to the exact baseline).
def make_query(rng, src_unit, c):
    g = rng.standard_normal(D)
    g -= (g @ src_unit) * src_unit                          # component orthogonal to the source
    g /= (np.linalg.norm(g) + 1e-12)
    u = c * src_unit + np.sqrt(max(0.0, 1 - c * c)) * g
    q = np.clip(np.round(u * 127), -127, 127).astype(np.int32)
    return q

def run(trials=4000, seed=1, Ms=(16, 32, 64), cos_lo=0.75, cos_hi=0.97):
    rng = np.random.default_rng(seed)
    VUNIT = VF / (VNRM[:, None] + 1e-9)
    srcs = rng.integers(0, N, trials)
    cs   = rng.uniform(cos_lo, cos_hi, trials)
    stats = {M: dict(hit1=0, hit5=0, exact_dots=0) for M in Ms}
    base_dots = 0; ncand = 0
    for t in range(trials):
        q = make_query(rng, VUNIT[srcs[t]], cs[t])
        topc, qn = probe_clusters(q)
        ids = candidate_ids(topc)
        if len(ids) == 0: continue
        base_id, base_cos = exact_argmax(q, ids, qn)
        base_dots += len(ids); ncand += len(ids)
        # exact top-5 for recall@5 reference
        top5 = set(ids[np.argsort(-base_cos)[:5]].tolist())
        sig_q = signatures(q[None, :])[0]
        for M in Ms:
            fid, kept = fastpath_argmax(q, sig_q, ids, qn, M)
            stats[M]["hit1"] += (fid == base_id)
            stats[M]["hit5"] += (fid in top5)
            stats[M]["exact_dots"] += kept
    print(f"\n[bench] {trials} paraphrase queries (cos 0.75-0.97), adaptive nprobe (firmware-exact)")
    print(f"[bench] avg candidates reranked by BASELINE (exact, every probed vector): {base_dots/trials:.0f}/query")
    print(f"{'M':>4} {'recall@1':>9} {'recall@5':>9} {'exact dots/q':>13} {'speedup(dots)':>14} {'I/O coarse':>11}")
    for M in Ms:
        s = stats[M]
        r1 = s["hit1"]/trials*100; r5 = s["hit5"]/trials*100
        ed = s["exact_dots"]/trials
        speed = (base_dots/trials)/ed
        io = (base_dots/trials*(D+4)) / (base_dots/trials*(D//8) + ed*(D+4))  # sig-only coarse + full for M
        print(f"{M:>4} {r1:>8.2f}% {r5:>8.2f}% {ed:>13.0f} {speed:>13.1f}x {io:>10.1f}x")

# ---------------------------------------------------------------- ablations / safety proofs
def norm_constant_invariance(trials=6000, seed=3):
    """FREE WIN #1 proof: does replacing exact per-vector norm with the constant 127 ever change
    the argmax the firmware returns? If ~0 disagreements, the per-candidate sqrt(vss) is pure waste."""
    rng = np.random.default_rng(seed)
    VUNIT = VF / (VNRM[:, None] + 1e-9)
    disagree = 0; total = 0
    for t in range(trials):
        s = int(rng.integers(0, N)); c = float(rng.uniform(0.72, 0.98))
        q = make_query(rng, VUNIT[s], c)
        topc, qn = probe_clusters(q); ids = candidate_ids(topc)
        if len(ids) == 0: continue
        dot = VF[ids] @ q
        a_exact = ids[int((dot / (qn * VNRM[ids] + 1e-6)).argmax())]   # exact norm
        a_const = ids[int((dot / (qn * 127.0 + 1e-6)).argmax())]       # constant norm
        disagree += (a_exact != a_const); total += 1
    print(f"\n[free-win] norm=127 vs exact-norm argmax disagreements: {disagree}/{total} "
          f"({disagree/total*100:.3f}%)  -> per-candidate sqrt(vss) is removable")

def slack_eval(trials=8000, seed=7, M=64, CAP=128, slacks=(0, 1, 2, 3, 4)):
    """IMPROVEMENT: keep top-M by Hamming PLUS any near-tie within `slack` of the M-th distance
    (capped at CAP), then exact-rerank. Misses happen when the true NN sits just past the M-th
    Hamming; the slack band recovers them adaptively — costs extra reranks only when the boundary
    is genuinely ambiguous. Reports recall@1 and the AVG vectors reranked (the real device cost)."""
    rng = np.random.default_rng(seed)
    VUNIT = VF / (VNRM[:, None] + 1e-9)
    res = {s: dict(hit=0, rer=0) for s in slacks}
    tot = 0
    for t in range(trials):
        s0 = int(rng.integers(0, N)); c = float(rng.uniform(0.72, 0.97))
        q = make_query(rng, VUNIT[s0], c)
        topc, qn = probe_clusters(q); ids = candidate_ids(topc)
        if len(ids) == 0: continue
        base = ids[int((VF[ids] @ q / (qn * VNRM[ids] + 1e-6)).argmax())]
        ham = hamming(signatures(q[None, :])[0], SIG_V[ids])
        order = np.argsort(ham, kind="stable")
        ids_s, ham_s = ids[order], ham[order]
        thr = ham_s[min(M, len(ids_s)) - 1]                  # the M-th smallest Hamming
        for s in slacks:
            cut = thr + s
            n = int(np.searchsorted(ham_s, cut, side="right"))   # keep all with ham <= cut
            n = min(max(n, M), CAP, len(ids_s))
            keep = ids_s[:n]
            win = keep[int((VF[keep] @ q / (qn * VNRM[keep] + 1e-6)).argmax())]
            res[s]["hit"] += (win == base); res[s]["rer"] += n
        tot += 1
    print(f"\n[slack-eval] M={M} CAP={CAP}, {tot} queries cos 0.72-0.97")
    print(f"{'slack':>6} {'recall@1':>9} {'avg rerank':>11}")
    for s in slacks:
        print(f"{s:>6} {res[s]['hit']/tot*100:>8.3f}% {res[s]['rer']/tot:>10.1f}")

def _fwht(x):
    """In-place fast Walsh-Hadamard transform along axis 1 (D a power of two). Vectorized over rows."""
    x = x.astype(np.float32).copy(); n = x.shape[1]; h = 1
    while h < n:
        x = x.reshape(x.shape[0], -1, 2 * h)
        a = x[:, :, :h].copy(); b = x[:, :, h:].copy()
        x[:, :, :h] = a + b; x[:, :, h:] = a - b
        x = x.reshape(x.shape[0], n); h *= 2
    return x

def rotation_eval(trials=8000, seed=13, Ms=(16, 32, 48, 64)):
    """IMPROVEMENT test: structured random rotation before signing. SimHash on raw LEARNED dims is
    suboptimal (dims correlated/anisotropic); rotating by FWHT(±random) decorrelates them so Hamming
    tracks the angle better -> same recall at a SMALLER M (=cheaper). Same fixed rotation for index &
    query (stored once). Compares raw-sign vs rotated-sign recall@1 across M."""
    rng = np.random.default_rng(seed)
    rs = (rng.integers(0, 2, D).astype(np.int8) * 2 - 1).astype(np.float32)   # fixed ±1 diagonal
    def rot_sig(mat_i32):
        r = _fwht(mat_i32.astype(np.float32) * rs)
        return np.packbits(r >= 0, axis=1, bitorder="little").view(np.uint64).reshape(mat_i32.shape[0], D // 64)
    SIG_VR = rot_sig(VF)
    VUNIT = VF / (VNRM[:, None] + 1e-9)
    raw = {M: 0 for M in Ms}; rot = {M: 0 for M in Ms}; tot = 0
    for t in range(trials):
        s0 = int(rng.integers(0, N)); c = float(rng.uniform(0.72, 0.97))
        q = make_query(rng, VUNIT[s0], c)
        topc, qn = probe_clusters(q); ids = candidate_ids(topc)
        if len(ids) == 0: continue
        base = ids[int((VF[ids] @ q / (qn * VNRM[ids] + 1e-6)).argmax())]
        hr = hamming(signatures(q[None, :])[0], SIG_V[ids])
        ht = hamming(rot_sig(q[None, :])[0], SIG_VR[ids])
        for M in Ms:
            for tag, h, acc in (("raw", hr, raw), ("rot", ht, rot)):
                keep = ids[np.argpartition(h, min(M, len(ids)-1))[:M]] if len(ids) > M else ids
                acc[M] += (keep[int((VF[keep] @ q / (qn*VNRM[keep]+1e-6)).argmax())] == base)
        tot += 1
    print(f"\n[rotation] {tot} queries cos 0.72-0.97 — recall@1 raw-sign vs FWHT-rotated-sign")
    print(f"{'M':>4} {'raw':>9} {'rotated':>9}")
    for M in Ms: print(f"{M:>4} {raw[M]/tot*100:>8.3f}% {rot[M]/tot*100:>8.3f}%")

def adaptive_m_eval(trials=8000, seed=11):
    """IMPROVEMENT (governor, mirrors nprobe): scale M to query ambiguity. The centroid margin
    already chooses nprobe (2=clear .. 6=ambiguous); reuse it for M too — a clear query needs few
    survivors, an ambiguous one more. Compare fixed M=64 vs adaptive {nprobe2:32, 3:48, 6:96}."""
    rng = np.random.default_rng(seed)
    VUNIT = VF / (VNRM[:, None] + 1e-9)
    MMAP = {2: 32, 3: 48, 6: 96}
    fixed = dict(hit=0, rer=0); adap = dict(hit=0, rer=0); tot = 0
    for t in range(trials):
        s0 = int(rng.integers(0, N)); c = float(rng.uniform(0.72, 0.97))
        q = make_query(rng, VUNIT[s0], c)
        # replicate nprobe governor
        qn = np.sqrt((q * q).sum()) + 1e-6
        cos = (CF @ q) / (qn * CNRM + 1e-6); order = np.argsort(-cos)
        margin = cos[order[0]] - cos[order[1]]
        nprobe = 2 if margin >= 0.28 else 3 if margin >= 0.10 else 6
        ids = candidate_ids(order[:nprobe])
        if len(ids) == 0: continue
        base = ids[int((VF[ids] @ q / (qn * VNRM[ids] + 1e-6)).argmax())]
        ham = hamming(signatures(q[None, :])[0], SIG_V[ids]); ordh = np.argsort(ham, kind="stable")
        ids_s = ids[ordh]
        for tag, M, acc in (("fixed", 64, fixed), ("adap", MMAP[nprobe], adap)):
            keep = ids_s[:min(M, len(ids_s))]
            win = keep[int((VF[keep] @ q / (qn * VNRM[keep] + 1e-6)).argmax())]
            acc["hit"] += (win == base); acc["rer"] += len(keep)
        tot += 1
    print(f"\n[adaptive-M] {tot} queries cos 0.72-0.97  (nprobe->M: 2:32 3:48 6:96)")
    print(f"  fixed  M=64 : recall@1 {fixed['hit']/tot*100:.3f}%   avg rerank {fixed['rer']/tot:.1f}")
    print(f"  adaptive    : recall@1 {adap['hit']/tot*100:.3f}%   avg rerank {adap['rer']/tot:.1f}")

def sig_ablation(trials=4000, seed=5, M=64):
    """Signature-width ablation: 256-bit (full sign) vs 128-bit (folded sign d[i]^d[i+128])."""
    rng = np.random.default_rng(seed)
    VUNIT = VF / (VNRM[:, None] + 1e-9)
    def sig128(mat):                              # fold to 128 bits: sign-agreement of halves
        b = (mat[:, :128] >= 0) ^ (mat[:, 128:] >= 0)
        return np.packbits(b, axis=1, bitorder="little").view(np.uint64).reshape(mat.shape[0], 2)
    SIG_V128 = sig128(VF)
    def ham128(sq, blk):
        x = np.bitwise_xor(blk, sq).view(np.uint8).reshape(blk.shape[0], -1)
        return POPC[x].sum(1)
    hit256 = hit128 = tot = 0
    for t in range(trials):
        s = int(rng.integers(0, N)); c = float(rng.uniform(0.75, 0.97))
        q = make_query(rng, VUNIT[s], c)
        topc, qn = probe_clusters(q); ids = candidate_ids(topc)
        if len(ids) == 0: continue
        base = ids[int((VF[ids] @ q / (qn * VNRM[ids] + 1e-6)).argmax())]
        # 256-bit
        sq = signatures(q[None, :])[0]
        k = ids[np.argpartition(hamming(sq, SIG_V[ids]), min(M, len(ids)-1))[:M]] if len(ids) > M else ids
        hit256 += (k[int((VF[k] @ q / (qn*VNRM[k]+1e-6)).argmax())] == base)
        # 128-bit folded
        sq2 = sig128(q[None, :])[0]
        k2 = ids[np.argpartition(ham128(sq2, SIG_V128[ids]), min(M, len(ids)-1))[:M]] if len(ids) > M else ids
        hit128 += (k2[int((VF[k2] @ q / (qn*VNRM[k2]+1e-6)).argmax())] == base)
        tot += 1
    print(f"\n[sig-ablation] M={M}  recall@1: 256-bit={hit256/tot*100:.2f}%  128-bit(folded)={hit128/tot*100:.2f}%  "
          f"(sig bytes/vec: 32 vs 16)")

if __name__ == "__main__":
    run()                                                   # main regime cos 0.75-0.97
    print("\n--- WORST-CASE regime cos 0.70-0.80 (hardest paraphrases) ---")
    run(trials=4000, seed=2, Ms=(32, 64, 96), cos_lo=0.70, cos_hi=0.80)
    norm_constant_invariance()
    sig_ablation()
    slack_eval()
    adaptive_m_eval()
    rotation_eval()
