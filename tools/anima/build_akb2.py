#!/usr/bin/env python3
"""ANIMA offline factory — RAM-managed clustered index (AKB2), BILINGUAL, multi-domain.

EcoVector-lite: k-means centroids + cluster directory in RAM; per-cluster vectors + a
bilingual answer blob on the SD. Answers carry reply_it AND reply_en; the device picks by
the app's language. Knowledge lives in the index (RAG), not in the model — per the design.

Knowledge is authored as JSONL cards under tools/anima/knowledge/*.jsonl (schema:
schemas/anima-card.schema.json). To add knowledge you edit/ingest JSONL and re-run this —
you never touch this script. Encoder math lives in anima_lib.py. See tools/anima/README.md.

Answer record (in the blob): u8 action | cstr arg | cstr reply_it | cstr reply_en
Run:  python tools/anima/build_akb2.py   (needs models/anima-it-encoder.bin)
"""
import os, sys, struct, glob, hashlib, json, numpy as np
from sklearn.cluster import KMeans
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

# ---- provenance (anti-drift guard) -----------------------------------------------------------
# The index is a deterministic function of (corpus files, encoder). It is built/synced as a
# SEPARATE file from both, so the live failure mode is a SILENT stale fixture: the corpus is
# edited but the committed index isn't rebuilt, so the gate keeps testing an index that no
# longer matches the corpus — green now, a surprise borderline-case flip the moment someone
# finally runs `anima:packs`. To make that deterministic+actionable instead of silent, every
# built index gets a sidecar `<index>.prov` recording the exact corpus+encoder it was built
# from; check_pack.mjs recomputes both hashes and FAILS if the committed index is stale.
# The hashing MUST stay byte-identical to check_pack.mjs's recompute (sorted by basename;
# name + NUL + bytes + LF per file), or the guard would false-alarm.
def _corpus_sha():
    h = hashlib.sha256()
    for p in sorted(glob.glob(os.path.join(A.KDIR, "*.jsonl")), key=os.path.basename):
        h.update(os.path.basename(p).encode("utf-8")); h.update(b"\0")
        with open(p, "rb") as f: h.update(f.read())
        h.update(b"\n")
    return h.hexdigest()
def _file_sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
    return h.hexdigest()
def write_prov(index_path, D, K, N, n_labels):
    enc_path = os.environ.get("ANIMA_ENC", A.ENC)
    prov = {"schema": 1, "tool": "build_akb2.py",
            "corpus_sha": _corpus_sha(), "encoder_sha": _file_sha(enc_path),
            "D": int(D), "K": int(K), "N": int(N), "n_labels": int(n_labels)}
    with open(index_path + ".prov", "w", encoding="utf-8") as f:
        json.dump(prov, f, indent=0, sort_keys=True)
    return prov

# Output path is configurable so a build orchestrator (tools/anima/build_packs.mjs) can build the
# device (D=192) and host (D=256) packs to SEPARATE files without clobbering each other — the manual
# single-OUT dance is what shipped a stale-dim index next to a fresh encoder. Default is unchanged.
OUT = os.environ.get("ANIMA_INDEX_OUT", os.path.join(A.ROOT, "models", "anima-it-index.bin"))
_IS_DEFAULT_OUT = "ANIMA_INDEX_OUT" not in os.environ

table, H, D, NGRAMS, WORD_N = A.load_encoder()
encode_unit = A.make_encoder(table, H, D, NGRAMS)
print(f"[anima] encoder {H}x{D} ngrams={NGRAMS} word_n={WORD_N}")

cards, counts = A.load_corpus()
LABELS = A.to_labels(cards)
print("[anima] corpus:", ", ".join(f"{k}={v}" for k, v in counts.items()), f"(total {len(LABELS)})")

# Data hygiene: an ask phrasing on >1 card is the small model's contamination signal (e.g. a
# capital question landing on the wrong country). generate.py's guard prevents new ones; this
# surfaces any that predate it, so a contaminated index never ships silently.
from collections import defaultdict as _dd
_h = _dd(set)
for _c in cards:
    for _lang in ("it", "en"):
        for _a in _c["ask"].get(_lang, []):
            _h[_a.strip().lower()].add(_c["id"])
_dups = {k: v for k, v in _h.items() if len(v) > 1}
if _dups:
    print(f"[anima] WARNING: {len(_dups)} cross-card duplicate ask(s) — review with tools/anima/audit_asks.py:")
    for _k, _v in list(_dups.items())[:6]:
        print(f"           '{_k[:48]}' on {sorted(_v)}")

# ---- answer blob (bilingual) ------------------------------------------------------------
def cstr(s, cap=360):
    # Clip to `cap` bytes at a CLEAN boundary — never mid-word or mid-UTF-8-codepoint. Prefer a sentence
    # end, else a word break + ellipsis. (Device reply buffers are 384; cap 360 leaves headroom.)
    b = s.encode("utf-8")
    if len(b) > cap:
        b = b[:cap]
        while b and (b[-1] & 0xC0) == 0x80: b = b[:-1]              # don't split a codepoint
        t = b.decode("utf-8", "ignore")
        cut = max(t.rfind(". "), t.rfind("! "), t.rfind("? "))     # prefer a complete sentence
        if cut >= len(t) // 2:
            t = t[:cut + 1]
        else:
            sp = t.rfind(" ")
            t = (t[:sp].rstrip() + "…") if sp > 0 else t           # else a whole word + ellipsis
        b = t.encode("utf-8")
        while len(b) > cap: b = b[:-1]
        while b and (b[-1] & 0xC0) == 0x80: b = b[:-1]
    return struct.pack("<H", len(b)) + b
# AKB3 answer record: u8 action | cstr arg | cstr reply_it | cstr reply_en | cstr detail_it | cstr detail_en
ans_off, blob = [], bytearray()
for act, arg, rit, ren, dit, den, _ in LABELS:
    ans_off.append(len(blob))
    blob += struct.pack("<B", A.ACT[act]) + cstr(arg) + cstr(rit) + cstr(ren) + cstr(dit) + cstr(den)

# ---- vectors (one per phrasing) + k-means -----------------------------------------------
# Dedup within each card: drop a phrasing whose vector is ~identical (cos > 0.97) to one already
# kept for the SAME card. Diverse phrasings (the whole point of LLM enrichment) are kept; only
# true duplicates are pruned, so the index stays minimal -> fewer rerank vectors, no coverage loss.
DEDUP_COS = 0.97
vecs, vlab = [], []
dropped = 0
for li, (_, _, _, _, _, _, phrs) in enumerate(LABELS):
    kept = []
    for p in phrs:
        v = encode_unit(p)
        if any(float(v @ k) > DEDUP_COS for k in kept):
            dropped += 1; continue
        kept.append(v); vecs.append(v); vlab.append(li)
vecs = np.stack(vecs).astype(np.float32); N = len(vecs)
print(f"[anima] vectors: {N} kept, {dropped} near-duplicates pruned (cos>{DEDUP_COS})")
# K is capped for DEVICE RAM, not just quality: the centroid table is ONE contiguous malloc of
# K*D bytes on the ESP32, whose largest free block is ~31 KB (no PSRAM). Exceeding it makes L1
# silently fail to load. Cap so K*D stays well under that (~18 KB) -> L1 survives as knowledge
# scales. Fewer/bigger clusters = a few more SD reads at rerank (fine), never an OOM.
RAM_BUDGET = 18000                                  # bytes for the centroid malloc (device-proven safe < 31 KB block).
                                                    # NB at ~30k vectors K=93 makes coarse centroids blurry -> the real
                                                    # recall lever now is a better ENCODER (distill.py e5->int8), not K;
                                                    # raising K needs device-verified RAM headroom (focus-mode).
K_RAM = max(8, RAM_BUDGET // D)
K = max(8, min(256, K_RAM, N // 22))
km = KMeans(n_clusters=K, n_init=4, random_state=0).fit(vecs)
order = np.argsort(km.labels_); cl = km.labels_[order]
centroids = np.zeros((K, D), np.float32)
for c in range(K):
    m = vecs[km.labels_ == c]
    if len(m): centroids[c] = m.mean(0)
cn = np.linalg.norm(centroids, axis=1, keepdims=True); centroids /= np.where(cn > 0, cn, 1)
dir_off = np.zeros(K, np.uint32); dir_cnt = np.zeros(K, np.uint32); pos = 0
for c in range(K):
    n = int((cl == c).sum()); dir_off[c] = pos; dir_cnt[c] = n; pos += n
def q8(v): return np.clip(np.round(v*127), -127, 127).astype(np.int8)

answers_start = 16 + K*D + K*8 + N*(D+4)
with open(OUT, "wb") as f:
    f.write(b"AKB3"); f.write(struct.pack("<III", D, K, N))
    f.write(q8(centroids).tobytes())
    for c in range(K): f.write(struct.pack("<II", int(dir_off[c]), int(dir_cnt[c])))
    for i in order:
        f.write(q8(vecs[i]).tobytes()); f.write(struct.pack("<I", answers_start + ans_off[vlab[i]]))
    f.write(blob)
print(f"[anima] AKB2 bilingual: N={N} vecs, K={K} clusters, {len(LABELS)} labels -> {OUT} ({os.path.getsize(OUT)/1024:.1f} KB)")
_prov = write_prov(OUT, D, K, N, len(LABELS))
print(f"[anima] provenance -> {OUT}.prov (corpus {_prov['corpus_sha'][:12]}…, encoder {_prov['encoder_sha'][:12]}…)")
_ram = K*D + K*8
print(f"[anima] RAM on device = {_ram/1024:.1f} KB (centroid malloc {K*D} B; device largest block ~31 KB)")
if K*D > 24000:
    print(f"[anima] WARNING: centroid malloc {K*D} B approaches the device's ~31 KB largest free block — L1 may fail to load")

# Mirror into the SD staging sources so deploy.ps1 (which syncs tools/sd-sim/data) ships the freshly
# built index. ONLY on a default (device, D=192) build — and ONLY to DEVICE-dim trees. The host
# harness tree (tools/anima-host/sd, D=256) is built separately by build_packs.mjs with its own
# ANIMA_INDEX_OUT; mirroring a D-specific index across dims is exactly what disabled L1 before
# (a D=192 index landing next to a D=256 encoder → load_index rejects). Note: these copies are the
# pre-augment AKB3 body; build_packs.mjs re-augments + re-copies the ASIG version over them.
if _IS_DEFAULT_OUT:
    import shutil
    for _sd in ("tools/sd-sim/data/anima",      # JS twin / browser simulator   (D=192)
                "deploy/sd-safe/data/anima",     # SD payload synced to the device (D=192)
                "deploy/sd/data/anima"):         # primary device SD tree         (D=192)
        _dst = os.path.join(A.ROOT, *_sd.split("/"), os.path.basename(OUT))
        if os.path.isdir(os.path.dirname(_dst)):
            shutil.copy2(OUT, _dst)
            shutil.copy2(OUT + ".prov", _dst + ".prov")   # provenance travels with the index
            print(f"[anima] synced -> {_dst}")

# ---- quick search check (clustered top-2) -----------------------------------------------
cq8 = q8(centroids).astype(np.int32); vq8 = q8(vecs).astype(np.int32)
def search(q, topc=2):
    qv = q8(encode_unit(q)).astype(np.int32)
    cs = cq8 @ qv / (np.linalg.norm(cq8,axis=1)+1e-9)
    best=-1; bc=-9
    for c in np.argsort(cs)[::-1][:topc]:
        for i in np.where(km.labels_==c)[0]:
            cos=float(vq8[i]@qv/((np.linalg.norm(vq8[i])*np.linalg.norm(qv))+1e-9))
            if cos>bc: bc=cos; best=i
    return LABELS[vlab[best]], bc
print("[anima] search check:")
for q in ["cos e una variabile","what is a for loop","qual e la capitale del giappone","come funzionano le automazioni","what is nucleoos","open the music"]:
    lab,c = search(q); print(f"    {c:.2f}  {q!r} -> IT:{lab[2]}")
