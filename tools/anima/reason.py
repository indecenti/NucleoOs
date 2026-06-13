#!/usr/bin/env python3
"""ANIMA symbolic reasoning — prototype of "reasoning without hallucination".

The retriever picks ONE frozen card. This layer adds the smallest, safest form of reasoning:
COMPOSE frozen cards. It never writes a new claim — it only selects, contrasts and chains
human-verified replies, so it cannot hallucinate by construction.

Primitive #1 implemented here: COMPARISON.
  "che differenza c'e tra X e Y" / "X vs Y" / "meglio X o Y" / "difference between X and Y"
  -> retrieve the best card for X and the best card for Y -> contrast their frozen replies.
  Guards: if either side is below the confidence floor -> don't assert (ask/refuse);
          if both sides resolve to the SAME card -> just return that card (it already covers it).

This is a Python reference for the firmware FSM controller (deterministic C, no generation),
the same way band_eval.py was the reference for the clarify band. Run:
  python tools/anima/reason.py "che differenza c'e tra un intero e un float"
  python tools/anima/reason.py            # demo battery
"""
import os, sys, re, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

GATE_HI, FLOOR = 0.66, 0.55   # assert above HI; for a comparison side accept down to FLOOR

# Comparison patterns -> (X, Y). Ordered: most specific first.
PATTERNS = [
    r"(?:che |qual\s*['e]?\s*la )?differenz[ae].* (?:tra|fra) (.+?) (?:e|ed) (.+)",
    r"difference between (.+?) and (.+)",
    r"(?:meglio|preferisci|scelgo|uso) (.+?) o (?:uno |una |un )?(.+)",
    r"(.+?) (?:vs\.?|versus|contro) (.+)",
    r"compare (.+?) (?:and|with|to) (.+)",
]
STOP = re.compile(r"^(un|uno|una|il|lo|la|gli|i|le|l'|a|an|the|del|della|di|dei|le)\s+", re.I)


def clean(s):
    s = s.strip(" ?.!,;:").strip()
    prev = None
    while prev != s:                      # strip leading articles/preps, possibly stacked
        prev = s; s = STOP.sub("", s).strip()
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("queries", nargs="*")
    ap.add_argument("--lang", default="it")
    ap.add_argument("--kdir", default=None)
    a = ap.parse_args()

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus(kdir=a.kdir) if a.kdir else A.load_corpus()
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def best(q):
        qv = encode(q); cos = V @ qv; i = int(np.argmax(cos))
        return cards[owner[i]], float(cos[i])

    def best_entity(x, lang):
        # A bare entity ("i2c") embeds weakly; framing it like a card question ("cos'e i2c")
        # lifts it toward the card's own phrasings. Take the strongest framing. Still frozen.
        frames = [x, f"cos'e {x}", f"che cos'e {x}"] if lang != "en" else [x, f"what is {x}", f"what is a {x}"]
        out = [best(f) for f in frames]
        return max(out, key=lambda t: t[1])

    def reply(card, lang):
        return (card.get("reply") or {}).get(lang) or (card.get("reply") or {}).get("it", "")

    def split_comparison(q):
        ql = q.lower()
        for pat in PATTERNS:
            m = re.search(pat, ql)
            if m:
                x, y = clean(m.group(1)), clean(m.group(2))
                if x and y and len(x) > 1 and len(y) > 1:
                    return x, y
        return None

    def answer(q, lang):
        comp = split_comparison(q)
        if comp:
            x, y = comp
            cx, sx = best_entity(x, lang); cy, sy = best_entity(y, lang)
            if cx["id"] == cy["id"]:                       # one card already covers the pair
                return f"[REASON: confronto -> 1 card]\n {reply(cx, lang)}   [{cx['id']} {sx:.2f}]"
            if sx >= FLOOR and sy >= FLOOR:                # contrast two frozen facts
                lead = "Here's the difference:" if lang == "en" else "Ecco la differenza:"
                return (f"[REASON: confronto -> 2 card, fatti congelati]\n {lead}\n"
                        f"  - {x} — {reply(cx, lang)}   [{cx['id']} {sx:.2f}]\n"
                        f"  - {y} — {reply(cy, lang)}   [{cy['id']} {sy:.2f}]")
            weak = x if sx < sy else y
            return f"[REASON: confronto -> lato debole '{weak}' sotto soglia, non invento]\n Non sono sicuro su «{weak}»."
        c, s = best(q)                                     # not a comparison -> plain retrieval
        if s >= GATE_HI:
            return f"[retrieval diretto]\n {reply(c, lang)}   [{c['id']} {s:.2f}]"
        return f"[sotto gate -> rifiuto onesto]\n Non lo so.   [best {c['id']} {s:.2f}]"

    DEMO = [
        ("it", "che differenza c'e tra un intero e un float"),
        ("it", "differenza tra i2c e spi"),
        ("it", "meglio il wifi o il bluetooth"),
        ("it", "che differenza c'e tra una variabile e una costante"),
        ("en", "what is the difference between a function and a loop"),
        ("it", "stack vs heap"),
        ("it", "differenza tra un diodo e un transistor"),
        ("it", "che differenza c'e tra la fotosintesi e la gravita"),  # unrelated -> still 2 frozen facts, no lie
    ]
    items = [(a.lang, q) for q in a.queries] if a.queries else DEMO
    print(f"ANIMA reason  ({len(cards)} cards, gate {GATE_HI}/floor {FLOOR})\n")
    for lang, q in items:
        print(f"  YOU> {q}")
        print(f" ANIMA> {answer(q, lang)}\n")


if __name__ == "__main__":
    main()
