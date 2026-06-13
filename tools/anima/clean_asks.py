#!/usr/bin/env python3
"""Purge contaminated `ask` phrasings on the CONVERSATIONAL cards — the "ask-spazzatura" the
qwen2.5-coder:7b enrichment left behind.

The enrichment was meant to add paraphrases of each card's own question. On the conversational
cards (assistant.*, self.*) it instead dumped long, off-topic generated sentences: assistant.thanks
("grazie") collected vector-geometry questions, assistant.howareyou collected ten angle-between-vectors
questions, assistant.greeting collected const/array/if-else questions, assistant.capabilities collected
meta "How can I ask the assistant to ..." sentences. These mislabeled phrasings make "grazie" retrieve a
math answer and pollute the encoder index.

Signature (verified): on these cards EVERY legitimate ask is short — "ciao", "grazie", "cosa puoi fare",
"what can you do" (<=5 words). EVERY contaminated ask is a long generated sentence (>=6 words). So the
rule is simply: on conversational cards, keep asks with <= --max-words words, drop the rest. The card
always retains its short curated core. We deliberately do NOT touch knowledge/person cards — there a long
ask is usually a legitimate domain paraphrase, and cosine-to-reply cannot tell them apart (a question
rarely shares surface words with its own answer).

Also blanks any `detail` that is a leaked prompt placeholder ("1-2 frasi piu profonde con un esempio...").

   python tools/anima/clean_asks.py            # dry-run
   python tools/anima/clean_asks.py --apply
"""
import os, sys, glob, json, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anima_lib import KDIR

CONV_CATS = {"assistant"}
CONV_PREFIX = ("self.",)
PLACEHOLDER_MARKERS = (
    "frasi piu profonde", "frasi più profonde", "deeper sentences",
    "con un esempio concreto", "with a concrete example",
)

def is_conv(card):
    return card.get("category") in CONV_CATS or str(card.get("id", "")).startswith(CONV_PREFIX)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--max-words", type=int, default=6, help="conversational asks longer than this are junk")
    args = ap.parse_args()

    import unicodedata
    STOP = {"come", "cosa", "quale", "quali", "puoi", "posso", "vuoi", "tuo", "tua", "miei", "this",
            "what", "which", "your", "with", "that", "have", "does", "about", "sapere", "fare", "essere"}
    def content_words(s):
        s = unicodedata.normalize("NFD", s.lower())
        s = "".join(ch for ch in s if unicodedata.category(ch) != "Mn")
        return {w for w in "".join(c if c.isalnum() else " " for c in s).split() if len(w) >= 4 and w not in STOP}

    files = sorted(glob.glob(os.path.join(KDIR, "*.jsonl")))
    n_removed = n_detail = 0
    samples = []
    file_rows = {}
    for path in files:
        rows = []
        for line in open(path, encoding="utf-8"):
            s = line.strip()
            if not s or s.startswith("//"):
                rows.append(("raw", line.rstrip("\n"))); continue
            try:
                c = json.loads(s)
            except json.JSONDecodeError:
                rows.append(("raw", line.rstrip("\n"))); continue
            if is_conv(c):
                a = c.get("ask") or {}
                for lang in ("it", "en"):
                    kept = []
                    for q in (a.get(lang) or []):
                        if len(q.split()) > args.max_words:
                            n_removed += 1
                            if len(samples) < 30:
                                samples.append((c.get("id"), lang, q))
                        else:
                            kept.append(q)
                    if lang in a:
                        a[lang] = kept
                c["ask"] = a
            det = c.get("detail") or {}
            if det:
                lo = (det.get("it") or "").lower() + " " + (det.get("en") or "").lower()
                blank = any(m in lo for m in PLACEHOLDER_MARKERS)
                # On conversational cards a detail that shares NO content word with the reply is
                # off-topic enrichment (e.g. assistant.thanks detail = "angolo tra due vettori") and
                # gets indexed — so it makes "grazie" retrievable by geometry queries. Blank it.
                if not blank and is_conv(c):
                    rep = c.get("reply") or {}
                    anchor = content_words((rep.get("it") or "") + " " + (rep.get("en") or ""))
                    dwords = content_words(lo)
                    if dwords and anchor and not (dwords & anchor):
                        blank = True
                if blank:
                    c["detail"] = {"it": "", "en": ""}
                    n_detail += 1
            rows.append(("card", c))
        file_rows[path] = rows

    print(f"[clean] conversational ask-spazzatura removed: {n_removed}  |  placeholder details blanked: {n_detail}\n")
    for cid, lang, q in samples[:24]:
        print(f"    [{lang}] {cid:24s} {q[:62]!r}")

    if not args.apply:
        print("\n[dry-run] nothing written. Re-run with --apply.")
        return
    # guard: no conversational card may end up with zero asks
    for path, rows in file_rows.items():
        for kind, c in rows:
            if kind == "card" and is_conv(c):
                a = c.get("ask") or {}
                if not ((a.get("it") or []) + (a.get("en") or [])):
                    sys.exit(f"[clean] ABORT: {c.get('id')} would have no asks — adjust --max-words")
    touched = 0
    for path, rows in file_rows.items():
        out = [val if kind == "raw" else json.dumps(val, ensure_ascii=False) for kind, val in rows]
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out) + "\n")
        touched += 1
    print(f"\n[apply] rewrote {touched} files.")

if __name__ == "__main__":
    main()
