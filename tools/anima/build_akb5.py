#!/usr/bin/env python3
"""AKB5 — category-SHARDED, scalable offline index (the keystone for 10GB+ certain knowledge).

WHY: the flat AKB4 index re-clusters EVERYTHING on any corpus change (adding 21 cards flipped unrelated
gate cases) and caps K by the device's 18 KB centroid RAM, so recall collapses past tens of thousands of
vectors. AKB5 fixes both:
  * each CATEGORY -> its own self-contained AKB4 shard file (models/akb5/<cat>.bin). Adding/!rebuilding a
    category touches ONLY its shard — others stay byte-identical (no perturbation, no gate whack-a-mole).
  * a tiny MANIFEST (models/anima-it-akb5.bin) holds one routing CENTROID per shard. The device STREAMS
    the manifest (a few KB, sequential) per query, picks the top shard(s), then loads+searches ONLY that
    shard's AKB4 (<=18 KB centroids resident). RAM stays flat (one shard at a time) at any #shards/GB, and
    per-query SD reads are bounded to one shard. Shards are separate files -> no FAT32 4 GB/file ceiling.

Reuses the EXACT AKB4 encoding of build_akb2.py + augment_akb4.py (byte-identical to the firmware reader).
Run: python tools/anima/build_akb5.py   (uses models/anima-it-encoder.bin, i.e. the device D)
"""
import os, sys, struct
import numpy as np
from sklearn.cluster import KMeans
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

table, H, D, NGRAMS, WORD_N = A.load_encoder()
encode_unit = A.make_encoder(table, H, D, NGRAMS)
cards, counts = A.load_corpus()
# ANIMA_EXTRA=path1,path2 — extra JSONL card files to include (e.g. staged imports). Lets us prove the
# ISOLATION property: adding cards to a category rebuilds ONLY that shard, others stay byte-identical.
import json as _json
for _p in filter(None, os.environ.get("ANIMA_EXTRA", "").split(",")):
    for _l in open(_p, encoding="utf-8"):
        _l = _l.strip()
        if not _l or _l.startswith("//"): continue
        try: cards.append(_json.loads(_l))
        except Exception: pass
print(f"[akb5] encoder {H}x{D}  {len(cards)} cards")

OUTDIR = os.environ.get("ANIMA_AKB5_DIR", os.path.join(A.ROOT, "models", "akb5"))
os.makedirs(OUTDIR, exist_ok=True)
MANIFEST = os.path.join(os.path.dirname(OUTDIR.rstrip("/\\")) or ".", "anima-it-akb5.bin") \
    if "ANIMA_AKB5_DIR" in os.environ else os.path.join(A.ROOT, "models", "anima-it-akb5.bin")
RAM_BUDGET = 18000          # per-shard centroid malloc cap (one shard resident at a time)
DEDUP_COS = 0.97

def q8(v): return np.clip(np.round(v * 127), -127, 127).astype(np.int8)

def cstr(s, cap=360):       # clip to a clean boundary, len-prefixed (identical to build_akb2.cstr)
    b = s.encode("utf-8")
    if len(b) > cap:
        b = b[:cap]
        while b and (b[-1] & 0xC0) == 0x80: b = b[:-1]
        t = b.decode("utf-8", "ignore")
        cut = max(t.rfind(". "), t.rfind("! "), t.rfind("? "))
        t = t[:cut + 1] if cut >= len(t) // 2 else ((t[:t.rfind(" ")].rstrip() + "…") if t.rfind(" ") > 0 else t)
        b = t.encode("utf-8")
        while len(b) > cap: b = b[:-1]
        while b and (b[-1] & 0xC0) == 0x80: b = b[:-1]
    return struct.pack("<H", len(b)) + b

def build_shard(ccards):
    """Build one AKB4 shard body (bytes) + its routing centroid (int8[D]) from a category's cards."""
    LABELS = A.to_labels(ccards)
    ans_off, blob = [], bytearray()
    for act, arg, rit, ren, dit, den, _ in LABELS:
        ans_off.append(len(blob))
        blob += struct.pack("<B", A.ACT[act]) + cstr(arg) + cstr(rit) + cstr(ren) + cstr(dit) + cstr(den)
    vecs, vlab = [], []
    for li, (_, _, _, _, _, _, phrs) in enumerate(LABELS):
        kept = []
        for p in phrs:
            v = encode_unit(p)
            if any(float(v @ k) > DEDUP_COS for k in kept): continue
            kept.append(v); vecs.append(v); vlab.append(li)
    if not vecs: return None
    vecs = np.stack(vecs).astype(np.float32); N = len(vecs)
    # K-cap MEASURED, kept at 93. Streaming removed the RAM bound, so K could go higher — but raising it to
    # 256 was tried and REVERTED: with L1_TOPC=6 fixed, finer clusters search FEWER total vectors per query,
    # so person-surname recall (chi è nixon/einstein) DROPPED (akb5-content 22->20). K and nprobe are coupled
    # — raising K needs more probes (more SD/latency) to keep coverage. Net: no recall win, a real loss on the
    # class we care about. The true recall ceiling is the encoder hash table (H), not K. Do NOT raise without
    # also coupling nprobe and re-measuring on BOTH dims.
    K = max(1, min(93, RAM_BUDGET // D, max(1, N // 22)))
    if K > 1:
        labels = KMeans(n_clusters=K, n_init=4, random_state=0).fit(vecs).labels_
    else:
        labels = np.zeros(N, int)
    order = np.argsort(labels); cl = labels[order]
    centroids = np.zeros((K, D), np.float32)
    for c in range(K):
        m = vecs[labels == c]
        if len(m): centroids[c] = m.mean(0)
    cn = np.linalg.norm(centroids, axis=1, keepdims=True); centroids /= np.where(cn > 0, cn, 1)
    dir_off = np.zeros(K, np.uint32); dir_cnt = np.zeros(K, np.uint32); pos = 0
    for c in range(K):
        n = int((cl == c).sum()); dir_off[c] = pos; dir_cnt[c] = n; pos += n
    answers_start = 16 + K * D + K * 8 + N * (D + 4)
    body = bytearray()
    body += b"AKB3" + struct.pack("<III", D, K, N)
    body += q8(centroids).tobytes()
    for c in range(K): body += struct.pack("<II", int(dir_off[c]), int(dir_cnt[c]))
    stored = q8(vecs[order])
    for j, i in enumerate(order):
        body += stored[j].tobytes() + struct.pack("<I", answers_start + ans_off[vlab[i]])
    body += blob
    # AKB4 ASIG sign-signature trailer (sign of each STORED int8 vector, vector-index order)
    sig_off = len(body)
    sigs = np.packbits(stored.astype(np.int16) >= 0, axis=1, bitorder="little")
    body += sigs.tobytes() + b"ASIG" + struct.pack("<III", sig_off, D, 1)
    # ROUTER: return ALL the shard's k-means centroids (not just the mean). The manifest is the union of
    # every shard's centroids -> a fine 2-level IVF whose coarse level lives on SD (streamed), so routing
    # is per-cluster (recall ~= flat) instead of per-category-mean (which mis-routed badly).
    return bytes(body), q8(centroids), N        # centroids: (K, D) int8

by_cat = defaultdict(list)
for c in cards: by_cat[c.get("category", "general")].append(c)

# SHARD SIZE CAP: AKB5 within-shard recall holds only while a shard's clusters stay fine (K capped at 93, a
# fixed fraction of clusters searched). Past ~SHARD_CAP cards the clusters grow coarse and recall drops (e.g.
# Einstein lost in a 99k-vector person-science slab). So any oversized category is SPLIT into ceil(n/CAP)
# coherent sub-shards (cat-0, cat-1, …); the router (which scores every sub-shard's clusters) + an adaptive
# probe pick them all up. This keeps each shard in the recall sweet spot at any corpus size.
SHARD_CAP = int(os.environ.get("ANIMA_SHARD_CAP", "3500"))
units = []     # (shard_name, cards)
for cat in sorted(by_cat):
    cc = by_cat[cat]
    if len(cc) <= SHARD_CAP:
        units.append((cat, cc))
    else:
        nparts = (len(cc) + SHARD_CAP - 1) // SHARD_CAP
        for i in range(nparts):
            units.append((f"{cat}-{i}", cc[i::nparts]))   # strided -> each sub-shard a representative slice

shards = []
for name, cc in units:
    res = build_shard(cc)
    if not res: continue
    body, cents, N = res
    fn = name.replace("/", "_").replace(" ", "_") + ".bin"
    with open(os.path.join(OUTDIR, fn), "wb") as f: f.write(body)
    shards.append((name, fn, cents, len(cc), N, len(body)))

# MANIFEST: AKB5 | u32 D | u32 n_shards | per-shard table {u8 namelen, name, u32 ncards, u32 N, u16 K}
# | then a CENTROID block: every shard's K centroids as 1-BIT SIGNS (ceil(D/8) bytes each, little-bitorder)
# concatenated in shard order. The device streams the block and scores each centroid with the SAME asymmetric
# holographic score L1 uses (int8 query x 1-bit sign) -> 8x smaller manifest (~25 KB vs ~200 KB), 8x faster
# routing read, with no recall change at the routing granularity. O(1) RAM (streaming scan; top-n ids kept).
nbytes = (D + 7) // 8
with open(MANIFEST, "wb") as f:
    f.write(b"AKB5"); f.write(struct.pack("<II", D, len(shards)))
    for cat, fn, cents, nc, N, bl in shards:
        nb = fn.encode("utf-8")[:31]
        f.write(struct.pack("<B", len(nb))); f.write(nb)
        f.write(struct.pack("<IIH", nc, N, cents.shape[0]))
    for cat, fn, cents, nc, N, bl in shards:
        # cents are already L2-normalized float32 (K,D); the SIGN per dim is the 1-bit holographic centroid.
        f.write(np.packbits(cents >= 0, axis=1, bitorder="little").tobytes())

tot_body = sum(s[5] for s in shards)
tot_cent = sum(s[2].shape[0] for s in shards)
print(f"[akb5] {len(shards)} shards, {sum(s[4] for s in shards)} vectors total, {tot_body/1024:.0f} KB bodies")
print(f"[akb5] manifest {os.path.getsize(MANIFEST)} B ({tot_cent} router centroids, 1-bit, streamed/query ~ {tot_cent*nbytes/1024:.0f} KB seq read)")
for cat, fn, cents, nc, N, bl in sorted(shards, key=lambda s: -s[4])[:8]:
    print(f"    {cat:22} {nc:4} cards  {N:5} vec  K={cents.shape[0]:3}  {bl/1024:6.1f} KB")
print(f"[akb5] RAM: stream router (O(1), top-n shard ids) + ONE shard's centroids (<=18 KB) — flat vs #shards/GB")
