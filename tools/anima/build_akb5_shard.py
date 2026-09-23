#!/usr/bin/env python3
"""AKB5 -- SCOPED per-shard rebuild (exercises the isolation property build_akb5.py claims in its
own header comment: each category is a self-contained AKB4 file).

WHY this exists next to build_akb5.py: a full build_akb5.py run regenerates ALL shards plus the
whole manifest from the current corpus in one shot. That is correct right after a bulk import, but
for a small, scoped corpus edit (for example "add a few how-to cards to the nucleoos category") it
is unnecessary blast radius -- and on a tree where the shipped shards have already drifted from the
corpus for OTHER, unrelated categories (pre-existing staleness this tool does not try to fix), a
full rebuild would silently also change those unrelated shards. This tool changes exactly the
shards named on the command line, nothing else -- every other shard file is never even opened, and
every other manifest table entry plus centroid block is copied through byte for byte.

Implements the "Incremental / append per shard" line item noted as future work in
docs/anima-knowledge-scale.md next to build_akb5.py.

Requires the target dir/manifest to ALREADY exist (from a prior build_akb5.py run) with a table
entry for every named category -- this UPDATES shards, it does not add or remove them. If a
category would need to split across multiple shards (SHARD_CAP), this tool refuses and asks for a
full build_akb5.py run instead.

Usage:
  python tools/anima/build_akb5_shard.py --dir <akb5-dir> --manifest <manifest.bin>
      --cat nucleoos --cat science --cat computer-science [--shard-cap 3500]

Encoder: honors ANIMA_ENC like build_akb5.py / build_akb2.py (defaults to
models/anima-it-encoder.bin).

ID-OVERRIDE: like build_akb5.py, an ANIMA_EXTRA card whose "id" matches an already-loaded card (from
the tracked corpus or an earlier ANIMA_EXTRA file) REPLACES it in place instead of being appended as
a duplicate id -- the mechanism tools/anima/knowledge.staged/overrides.jsonl relies on to correct a
shipped card without touching the tracked corpus. See docs/anima-knowledge-scale.md.
"""
import argparse, json, os, struct, sys
from collections import defaultdict
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

DEDUP_COS = 0.97
RAM_BUDGET = 18000


def q8(v):
    return np.clip(np.round(v * 127), -127, 127).astype(np.int8)


def cstr(s, cap=360):
    b = s.encode("utf-8")
    if len(b) > cap:
        b = b[:cap]
        while b and (b[-1] & 0xC0) == 0x80:
            b = b[:-1]
        t = b.decode("utf-8", "ignore")
        cut = max(t.rfind(". "), t.rfind("! "), t.rfind("? "))
        t = t[:cut + 1] if cut >= len(t) // 2 else ((t[:t.rfind(" ")].rstrip() + "...") if t.rfind(" ") > 0 else t)
        b = t.encode("utf-8")
        while len(b) > cap:
            b = b[:-1]
        while b and (b[-1] & 0xC0) == 0x80:
            b = b[:-1]
    return struct.pack("<H", len(b)) + b


def build_shard(ccards, encode_unit, D):
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
            if any(float(v @ k) > DEDUP_COS for k in kept):
                continue
            kept.append(v)
            vecs.append(v)
            vlab.append(li)
    if not vecs:
        return None
    vecs = np.stack(vecs).astype(np.float32)
    N = len(vecs)
    K = max(1, min(93, RAM_BUDGET // D, max(1, N // 22)))
    if K > 1:
        from sklearn.cluster import KMeans
        labels = KMeans(n_clusters=K, n_init=4, random_state=0).fit(vecs).labels_
    else:
        labels = np.zeros(N, int)
    order = np.argsort(labels)
    cl = labels[order]
    centroids = np.zeros((K, D), np.float32)
    for c in range(K):
        m = vecs[labels == c]
        if len(m):
            centroids[c] = m.mean(0)
    cn = np.linalg.norm(centroids, axis=1, keepdims=True)
    centroids /= np.where(cn > 0, cn, 1)
    dir_off = np.zeros(K, np.uint32)
    dir_cnt = np.zeros(K, np.uint32)
    pos = 0
    for c in range(K):
        n = int((cl == c).sum())
        dir_off[c] = pos
        dir_cnt[c] = n
        pos += n
    answers_start = 16 + K * D + K * 8 + N * (D + 4)
    body = bytearray()
    body += b"AKB3" + struct.pack("<III", D, K, N)
    body += q8(centroids).tobytes()
    for c in range(K):
        body += struct.pack("<II", int(dir_off[c]), int(dir_cnt[c]))
    stored = q8(vecs[order])
    for j, i in enumerate(order):
        body += stored[j].tobytes() + struct.pack("<I", answers_start + ans_off[vlab[i]])
    body += blob
    sig_off = len(body)
    sigs = np.packbits(stored.astype(np.int16) >= 0, axis=1, bitorder="little")
    body += sigs.tobytes() + b"ASIG" + struct.pack("<III", sig_off, D, 1)
    return bytes(body), q8(centroids), N


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", required=True, help="akb5 shard directory (existing)")
    ap.add_argument("--manifest", required=True, help="anima-it-akb5.bin manifest (existing)")
    ap.add_argument("--cat", action="append", required=True, dest="cats", help="category to rebuild (repeatable)")
    ap.add_argument("--shard-cap", type=int, default=int(os.environ.get("ANIMA_SHARD_CAP", "3500")))
    args = ap.parse_args()

    if not os.path.isdir(args.dir):
        sys.exit("[akb5-shard] no such dir: " + args.dir)
    if not os.path.isfile(args.manifest):
        sys.exit("[akb5-shard] no such manifest: " + args.manifest)

    table, H, D, NGRAMS, WORD_N = A.load_encoder()
    encode_unit = A.make_encoder(table, H, D, NGRAMS)
    cards, counts = A.load_corpus()

    # ANIMA_EXTRA=path1,path2 - same convention as build_akb5.py: extra JSONL card files (typically
    # tools/anima/knowledge.staged/*) merged in on top of the tracked corpus. Several shipped shards
    # (including nucleoos and science) already carry staged cards from a prior full build_akb5.py run
    # that included ANIMA_EXTRA, so a scoped rebuild of those shards must pass the SAME extra files or
    # it would silently drop the staged cards already live in the shard (a real regression).
    #
    # ID-OVERRIDE (same rule as build_akb5.py, kept in sync on purpose): an ANIMA_EXTRA card whose
    # "id" matches an already-loaded card REPLACES it in place (same position) instead of being
    # appended as a duplicate id. This is how tools/anima/knowledge.staged/overrides.jsonl corrects a
    # shipped card (e.g. the "ram" alias collision on wiki.it.accesso-casuale) without a tracked-corpus
    # edit: the correction lands in the SAME shard slot, so it is not a second competing entry whose
    # retrieval outcome against the original is an unresolved int8-quantization tie. See
    # docs/anima-knowledge-scale.md next to the staged/ANIMA_EXTRA mechanism.
    n_extra = 0
    n_override = 0
    id_pos = {c["id"]: i for i, c in enumerate(cards) if "id" in c}
    for _p in filter(None, os.environ.get("ANIMA_EXTRA", "").split(",")):
        for _l in open(_p, encoding="utf-8"):
            _l = _l.strip()
            if not _l or _l.startswith("//"):
                continue
            try:
                _c = json.loads(_l)
            except Exception:
                continue
            n_extra += 1
            _cid = _c.get("id")
            if _cid and _cid in id_pos:
                cards[id_pos[_cid]] = _c
                n_override += 1
            else:
                if _cid:
                    id_pos[_cid] = len(cards)
                cards.append(_c)
    print("[akb5-shard] encoder %dx%d  %d cards total (%d from ANIMA_EXTRA, %d override(s) in place)"
          % (H, D, len(cards), n_extra, n_override))

    by_cat = defaultdict(list)
    for c in cards:
        by_cat[c.get("category", "general")].append(c)

    targets = {}
    for cat in args.cats:
        cc = by_cat.get(cat, [])
        if not cc:
            sys.exit("[akb5-shard] no cards found for category %r - nothing to rebuild" % (cat,))
        if len(cc) > args.shard_cap:
            sys.exit("[akb5-shard] category %r has %d cards > shard-cap %d - it would SPLIT into "
                      "multiple shards; this scoped tool refuses. Run build_akb5.py instead."
                      % (cat, len(cc), args.shard_cap))
        fn = cat.replace("/", "_").replace(" ", "_") + ".bin"
        targets[fn] = cc

    built = {}
    for fn, cc in targets.items():
        res = build_shard(cc, encode_unit, D)
        if not res:
            sys.exit("[akb5-shard] shard %s produced zero vectors" % fn)
        body, cents, N = res
        with open(os.path.join(args.dir, fn), "wb") as f:
            f.write(body)
        built[fn] = (cents, len(cc), N)
        print("[akb5-shard] wrote %s: %d cards, %d vec, K=%d" % (fn, len(cc), N, cents.shape[0]))

    with open(args.manifest, "rb") as f:
        raw = f.read()
    if raw[:4] != b"AKB5":
        sys.exit("[akb5-shard] bad manifest magic")
    Dm, ns = struct.unpack_from("<II", raw, 4)
    if Dm != D:
        sys.exit("[akb5-shard] manifest dim %d != encoder dim %d - wrong tree/encoder pairing" % (Dm, D))

    off = 12
    entries = []
    for _ in range(ns):
        nl = raw[off]
        name = raw[off + 1:off + 1 + nl].decode("utf-8")
        nc, Nv = struct.unpack_from("<II", raw, off + 1 + nl)
        K = struct.unpack_from("<H", raw, off + 1 + nl + 8)[0]
        entries.append({"name": name, "nc": nc, "N": Nv, "K": K})
        off += 1 + nl + 4 + 4 + 2
    table_end = off

    nbytes = (D + 7) // 8
    coff = table_end
    for e in entries:
        clen = e["K"] * nbytes
        e["cent"] = raw[coff:coff + clen]
        coff += clen
    if coff != len(raw):
        sys.exit("[akb5-shard] manifest size mismatch while parsing (parsed %d, file %d bytes) - refusing to patch" % (coff, len(raw)))

    touched = 0
    for e in entries:
        if e["name"] in built:
            cents, nc, Nv = built[e["name"]]
            e["nc"], e["N"], e["K"] = nc, Nv, cents.shape[0]
            e["cent"] = np.packbits(cents >= 0, axis=1, bitorder="little").tobytes()
            touched += 1
    if touched != len(targets):
        missing = set(targets) - set(e["name"] for e in entries if e["name"] in targets)
        sys.exit("[akb5-shard] %d target shard(s) not found in the manifest table: %r - this tool "
                  "updates existing shards only, run build_akb5.py to add a new one"
                  % (len(targets) - touched, missing))

    out = bytearray()
    out += b"AKB5"
    out += struct.pack("<II", D, ns)
    for e in entries:
        nb = e["name"].encode("utf-8")
        out += struct.pack("<B", len(nb)) + nb
        out += struct.pack("<IIH", e["nc"], e["N"], e["K"])
    for e in entries:
        out += e["cent"]
    with open(args.manifest, "wb") as f:
        f.write(bytes(out))
    print("[akb5-shard] manifest patched: %d shard(s) updated in place, %d total, %d bytes (was %d bytes)"
          % (touched, ns, len(out), len(raw)))


if __name__ == "__main__":
    main()
