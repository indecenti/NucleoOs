#!/usr/bin/env python3
"""Shared ANIMA factory helpers: the encoder math + the JSONL corpus loader.

Imported by build_akb2.py (compiler), eval.py (benchmark) and enrich.py so the encoding
is defined ONCE. The encoder math (fnv1a / norm_text / feats / encode_unit) MUST stay
byte-identical to firmware/components/nucleo_app/../nucleo_anima/nucleo_anima_l1.c — if it
drifts, the PC-built index and the on-device embeddings diverge and retrieval breaks.
"""
import os, glob, json, struct, unicodedata, sys
import numpy as np

ROOT  = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENC   = os.path.join(ROOT, "models", "anima-it-encoder.bin")
KDIR  = os.path.join(ROOT, "tools", "anima", "knowledge")
ACT   = {"answer": 3, "launch": 1, "system": 2}


def load_encoder(path=None):
    """Return (table[H,D] int32, H, D, NGRAMS, WORD_N) from the ANE2 encoder file.

    With no explicit path, honor the ANIMA_ENC env var (so eval/build can be pointed at a
    refined encoder without overwriting the deployed one), falling back to the default ENC."""
    if path is None:
        path = os.environ.get("ANIMA_ENC", ENC)
    with open(path, "rb") as f:
        assert f.read(4) == b"ANE2", "not an ANE2 encoder"
        H, D, nn, WORD_N = struct.unpack("<IIII", f.read(16))
        NGRAMS = struct.unpack("<" + "I" * nn, f.read(4 * nn)); f.read(4)
        table = np.frombuffer(f.read(H * D), dtype=np.int8).reshape(H, D).astype(np.int32)
    return table, H, D, NGRAMS, WORD_N


def fnv1a(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def norm_text(s):
    s = unicodedata.normalize("NFD", s.lower())
    s = "".join(c for c in s if unicodedata.category(c) != "Mn")
    return " ".join("".join(c if c.isalnum() else " " for c in s).split())


def make_feats(H, NGRAMS):
    def feats(s):
        w = norm_text(s).split(); ids = []; t = "^" + "^".join(w) + "$"
        for n in NGRAMS:
            for i in range(len(t) - n + 1):
                ids.append(fnv1a(b"\x01" + t[i:i + n].encode()) % H)
        for x in w:
            ids.append(fnv1a(b"\x02" + x.encode()) % H)
        for i in range(len(w) - 1):
            ids.append(fnv1a(b"\x02" + (w[i] + " " + w[i + 1]).encode()) % H)
        return ids or [fnv1a(b"\x01^$") % H]
    return feats


def make_encoder(table, H, D, NGRAMS):
    feats = make_feats(H, NGRAMS)
    def encode_unit(s):
        a = np.zeros(D, np.int32)
        for fid in feats(s):
            a += table[fid]
        v = a.astype(np.float32); nrm = np.linalg.norm(v)
        return v / nrm if nrm > 0 else v
    return encode_unit


def load_corpus(kdir=KDIR, strict=True):
    """Load + validate all knowledge/*.jsonl. Returns (cards, counts).

    cards: list of normalized dicts {id, category, action, arg, reply{it,en}, ask{it,en}, ...}.
    On validation errors, prints them and (strict) exits non-zero.
    """
    files = sorted(glob.glob(os.path.join(kdir, "*.jsonl")))
    if not files:
        sys.exit(f"[anima] no knowledge files in {kdir}")
    cards, seen, errors, counts = [], {}, [], {}
    for path in files:
        name = os.path.basename(path); n_file = 0
        for ln, line in enumerate(open(path, encoding="utf-8"), 1):
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            where = f"{name}:{ln}"
            try:
                c = json.loads(line)
            except json.JSONDecodeError as e:
                errors.append(f"{where}: invalid JSON ({e})"); continue
            cid = c.get("id")
            if not cid:
                errors.append(f"{where}: missing 'id'"); continue
            if cid in seen:
                errors.append(f"{where}: duplicate id '{cid}' (also {seen[cid]})"); continue
            seen[cid] = where
            action = c.get("action", "answer")
            if action not in ACT:
                errors.append(f"{where}: bad action '{action}'"); continue
            rep = c.get("reply") or {}
            rit, ren = (rep.get("it") or "").strip(), (rep.get("en") or "").strip()
            if not rit and not ren:
                errors.append(f"{where}: reply needs 'it' and/or 'en'"); continue
            ask = c.get("ask") or {}
            ait = [s.strip() for s in ask.get("it", []) if s.strip()]
            aen = [s.strip() for s in ask.get("en", []) if s.strip()]
            if not (ait or aen):
                errors.append(f"{where}: 'ask' has no phrasings"); continue
            det = c.get("detail") or {}
            dit, den = (det.get("it") or "").strip(), (det.get("en") or "").strip()
            cards.append({"id": cid, "category": c.get("category", name[:-6]), "action": action,
                          "arg": c.get("arg", ""), "reply": {"it": rit or ren, "en": ren or rit},
                          "detail": {"it": dit, "en": den},
                          "ask": {"it": ait, "en": aen}, "source": c.get("source", "")})
            n_file += 1
        counts[name] = n_file
    if errors:
        print("[anima] corpus errors:")
        for e in errors[:60]:
            print("   -", e)
        if strict:
            sys.exit(f"[anima] {len(errors)} invalid card(s)")
    return cards, counts


def index_texts(card):
    """Retrieval KEYS to embed for a card. Beyond the `ask` phrasings, an 'answer' card also
    indexes its reply (and detail): those carry the discriminative concepts users paraphrase
    ("a sequence of values" for an array card), which `ask` alone often omits. Measured to lift
    e5's out-of-distribution Recall@1 from 32% to 50% — at ~zero device cost (just more vectors
    in the SD-resident clustered index; RAM stays O(centroids)). Launch/system replies are
    templated ("Apro %s", "{value}") so only answer cards contribute reply/detail keys."""
    out = list(card["ask"]["it"]) + list(card["ask"]["en"])
    if card.get("action", "answer") == "answer":
        d = card.get("detail", {"it": "", "en": ""})
        out += [card["reply"]["it"], card["reply"]["en"], d.get("it", ""), d.get("en", "")]
    seen, res = set(), []
    for t in out:
        if t and t not in seen:
            seen.add(t); res.append(t)
    return res


def to_labels(cards):
    """Flatten cards to the builder's LABEL tuples:
    (action, arg, reply_it, reply_en, detail_it, detail_en, [index_texts])."""
    out = []
    for c in cards:
        d = c.get("detail", {"it": "", "en": ""})
        out.append((c["action"], c["arg"], c["reply"]["it"], c["reply"]["en"], d["it"], d["en"], index_texts(c)))
    return out


def det_kmeans(X, K, iters=80, seed=0):
    """REPRODUCIBLE k-means → cluster label per row (np.int64).

    sklearn's KMeans(random_state=0) is only deterministic for ONE sklearn version on ONE thread layout:
    the gate fixture built on another PC (other sklearn / OpenMP) clustered differently and flipped every
    borderline routing golden. This is plain numpy, float64, a seeded k-means++ init, first-index
    tie-breaking and distances rounded to 1e-9, so the same (vectors, K) gives the same labels on any
    machine and in CI. Empty clusters keep their previous centre; stops early once centres are stable.
    """
    X = np.asarray(X, dtype=np.float64)
    n = len(X)
    rng = np.random.default_rng(seed)
    xx = (X * X).sum(1)
    centers = np.empty((K, X.shape[1]), dtype=np.float64)
    centers[0] = X[int(rng.integers(n))]
    d2 = np.maximum(xx - 2 * X @ centers[0] + centers[0] @ centers[0], 0.0)
    for k in range(1, K):
        tot = d2.sum()
        if tot <= 0:
            idx = k % n
        else:
            idx = int(np.searchsorted(np.cumsum(d2 / tot), rng.random(), side="right"))
            idx = min(idx, n - 1)
        centers[k] = X[idx]
        d2 = np.minimum(d2, np.maximum(xx - 2 * X @ centers[k] + centers[k] @ centers[k], 0.0))
    lab = np.zeros(n, dtype=np.int64)
    for _ in range(iters):
        dist = np.round(xx[:, None] - 2 * (X @ centers.T) + (centers * centers).sum(1)[None, :], 9)
        lab = np.argmin(dist, axis=1)                       # first index wins a tie
        cnt = np.bincount(lab, minlength=K)
        new = np.zeros_like(centers)
        np.add.at(new, lab, X)
        keep = cnt > 0
        new[keep] /= cnt[keep, None]
        new[~keep] = centers[~keep]
        if np.array_equal(np.round(new, 12), np.round(centers, 12)):
            break
        centers = new
    return lab
