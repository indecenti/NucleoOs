#!/usr/bin/env python3
"""Generate the offline ANIMA translation dictionaries from the single source seed.

Source : tools/anima/dict/seed.it-en.tsv   (edit THIS, then run this script)
Targets: deploy/sd-safe/data/anima/dict-it-en.tsv   (IT key -> EN translations)
         deploy/sd-safe/data/anima/dict-en-it.tsv   (EN key -> IT translations)

The on-device translate skill (firmware) and the host harness read these files directly,
so the device and the PC simulator translate through ONE vocabulary. The skill does an
EXACT key lookup (grounded -> zero hallucination) and DECLINES on a miss; it never guesses.

Key normalization MIRRORS the firmware tokenizer a_tokenize() byte-for-byte:
  * lowercase ASCII alnum kept; Italian accented vowels fold to their base
    (a/e/i/o/u family only — exactly the switch in nucleo_anima.c);
  * every other byte is a separator; tokens are rejoined with single spaces;
  * each token is capped at A_TOK_LEN-1 (23) and at most A_MAX_TOKENS (24) tokens.
So the device normalizes a query phrase the same way and finds the key (or doesn't).

Output is SORTED by key in ASCII byte order, matching strcmp() on the device, so the
firmware can scan with an early-exit (stop once a key sorts past the query).

Usage:
  python tools/anima/gen_dicts.py            # (re)write both .tsv files
  python tools/anima/gen_dicts.py --check    # CI: exit 1 if either file is stale
  python tools/anima/gen_dicts.py --stdout   # print both blocks, write nothing
  python tools/anima/gen_dicts.py --out DIR  # write to DIR instead of the deploy path

Stdlib only, to match the other tools/anima generators.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEED = ROOT / "tools" / "anima" / "dict" / "seed.it-en.tsv"
# Primary-sense override (bare EN forms) — loaded BEFORE everything so it leads both dicts. Fixes the few
# common words FreeDict orders noun/rare-sense first (run->correre, not run->periodo).
SEED_OVR = ROOT / "tools" / "anima" / "dict" / "seed.override.it-en.tsv"
# Bulk FreeDict (build_dict.py), one seed PER DIRECTION so each dict keeps FreeDict's native sense order
# (primary first). dict-it-en uses ONLY the IT->EN seed, dict-en-it ONLY the EN->IT seed — never the other
# dict's reverse aggregation (which alphabetised rare senses to the front). Curated seed leads both.
SEED_FD_ITEN = ROOT / "tools" / "anima" / "dict" / "seed.freedict.it-en.tsv"
SEED_FD_ENIT = ROOT / "tools" / "anima" / "dict" / "seed.freedict.en-it.tsv"
# Two consumers read these verbatim: the SD payload synced to the device, and the host
# harness (tools/anima-host chdir's to its root and reads ./sd). Keep both in lockstep.
OUT_DIRS = [
    ROOT / "deploy" / "sd-safe" / "data" / "anima",      # SD payload synced to the device
    ROOT / "tools" / "anima-host" / "sd" / "data" / "anima",  # C host harness (anima.exe)
    ROOT / "tools" / "sd-sim" / "data" / "anima",         # JS twin / browser simulator (serve-shell.mjs)
]

A_TOK_LEN = 24       # mirror firmware: token buffer, so cap = 23 usable chars
A_MAX_TOKENS = 24    # mirror firmware: max tokens kept per input

# Italian accented vowels the device folds (a_tokenize switch). Anything else non-ASCII
# is a separator on the device, so we MUST treat it the same here (do NOT fold ñ/ç/ü/...).
FOLD = {
    "à": "a", "á": "a", "â": "a",
    "è": "e", "é": "e", "ê": "e",
    "ì": "i", "í": "i", "î": "i",
    "ò": "o", "ó": "o", "ô": "o",
    "ù": "u", "ú": "u", "û": "u",
}


def norm_key(s: str) -> str:
    """Exact Python port of firmware a_tokenize(): fold IT vowels, keep ASCII alnum
    (lowercased), split on everything else, cap token length and count, rejoin with ' '."""
    tokens = []
    cur = []
    for ch in s:
        out = FOLD.get(ch)
        if out is None and ch.isascii() and ch.isalnum():
            out = ch.lower()
        if out:
            if len(cur) < A_TOK_LEN - 1:
                cur.append(out)
        else:
            if cur:
                tokens.append("".join(cur))
                cur = []
                if len(tokens) >= A_MAX_TOKENS:
                    cur = []
                    break
    if cur and len(tokens) < A_MAX_TOKENS:
        tokens.append("".join(cur))
    return " ".join(tokens)


def san_val(s: str) -> str:
    """One readable translation cell: collapse tab/newline to space, trim. Accents kept."""
    return " ".join(s.replace("\t", " ").replace("\r", " ").replace("\n", " ").split()).strip()


def load_seed():
    """Return list of (it_terms, en_terms) pairs; each side a list of trimmed synonyms. The curated seed is
    loaded first (display priority on dedup); the bulk FreeDict seed (if present) fills the long tail and is
    LENIENT (skips malformed rows instead of aborting — it's machine-generated, not hand-curated)."""
    pairs = []
    for ln, raw in enumerate(SEED.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if "\t" not in line:
            sys.exit(f"error: {SEED.name}:{ln}: no TAB separator -> {line!r}")
        it_cell, en_cell = line.split("\t", 1)
        it_terms = [t.strip() for t in it_cell.split(";") if t.strip()]
        en_terms = [t.strip() for t in en_cell.split(";") if t.strip()]
        if not it_terms or not en_terms:
            sys.exit(f"error: {SEED.name}:{ln}: empty side -> {line!r}")
        pairs.append((it_terms, en_terms))
    return pairs


def load_fd(path):
    """Lenient loader for a machine-generated FreeDict seed (IT<TAB>EN). Skips malformed rows."""
    pairs = []
    if not path.exists():
        return pairs
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#") or "\t" not in line:
            continue
        it_cell, en_cell = line.split("\t", 1)
        it_terms = [t.strip() for t in it_cell.split(";") if t.strip()]
        en_terms = [t.strip() for t in en_cell.split(";") if t.strip()]
        if it_terms and en_terms:
            pairs.append((it_terms, en_terms))
    print(f"[gen_dicts] +{len(pairs)} pairs from {path.name}", file=sys.stderr)
    return pairs


def build(pairs, src_index, dst_index):
    """src_index/dst_index pick which side of each pair is the key vs the value.
    Returns {key: [readable translations]} aggregated + deduped, key normalized."""
    table = {}      # key -> list of values (insertion order, deduped case-insensitively)
    seen = {}       # key -> set(lowercased values)
    display = {}    # key -> the readable source term that produced it (for warnings)
    for terms in pairs:
        for src in terms[src_index]:
            key = norm_key(src)
            if not key:
                continue
            table.setdefault(key, [])
            seen.setdefault(key, set())
            display.setdefault(key, src)
            for dst in terms[dst_index]:
                val = san_val(dst)
                low = val.lower()
                if val and low not in seen[key]:
                    seen[key].add(low)
                    table[key].append(val)
    return table


def render(table) -> str:
    lines = []
    for key in sorted(table):                 # ASCII order == device strcmp order
        vals = ", ".join(table[key])
        lines.append(f"{key}\t{vals}")
    return "\n".join(lines) + "\n"


def validate(table, name) -> list:
    errs = []
    for key, vals in table.items():
        if not key.isascii():
            errs.append(f"{name}: key {key!r} is not ASCII (fold mismatch with device)")
        if "\t" in "".join(vals):
            errs.append(f"{name}: key {key!r} has a TAB in a value")
        if not vals:
            errs.append(f"{name}: key {key!r} has no translation")
    return errs


def main():
    ap = argparse.ArgumentParser(description="Generate offline IT<->EN translation dictionaries")
    ap.add_argument("--check", action="store_true", help="exit 1 if either .tsv is stale")
    ap.add_argument("--stdout", action="store_true", help="print both blocks, write nothing")
    ap.add_argument("--out", type=Path, default=None, action="append",
                    help="output directory (repeatable); default = both deploy + host-harness dirs")
    args = ap.parse_args()

    out_dirs = args.out if args.out else OUT_DIRS

    lead = load_fd(SEED_OVR) + load_seed()   # override first, then curated -> both LEAD over bulk FreeDict
    it_en = build(lead + load_fd(SEED_FD_ITEN), 0, 1)   # key = IT, value = EN  (ita-eng forward only)
    en_it = build(lead + load_fd(SEED_FD_ENIT), 1, 0)   # key = EN, value = IT  (eng-ita forward only)

    errs = validate(it_en, "dict-it-en") + validate(en_it, "dict-en-it")
    if errs:
        print("\n".join(f"  - {e}" for e in errs), file=sys.stderr)
        sys.exit(f"FAILED: {len(errs)} problem(s) in the generated dictionaries")

    out_it_en = render(it_en)
    out_en_it = render(en_it)

    if args.stdout:
        sys.stdout.write(f"==== dict-it-en.tsv ({len(it_en)} keys) ====\n{out_it_en}")
        sys.stdout.write(f"==== dict-en-it.tsv ({len(en_it)} keys) ====\n{out_en_it}")
        return

    targets = []
    for d in out_dirs:
        targets.append((d / "dict-it-en.tsv", out_it_en, len(it_en)))
        targets.append((d / "dict-en-it.tsv", out_en_it, len(en_it)))
    stale = False
    for path, content, n in targets:
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == content:
            print(f"OK: up to date {path.relative_to(ROOT)} ({n} keys)")
            continue
        stale = True
        if args.check:
            print(f"STALE: {path.relative_to(ROOT)} differs from seed", file=sys.stderr)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
            print(f"OK: wrote {path.relative_to(ROOT)} ({n} keys)")
    if args.check and stale:
        sys.exit("STALE: run `python tools/anima/gen_dicts.py` to regenerate the dictionaries")


if __name__ == "__main__":
    main()
