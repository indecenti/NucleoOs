#!/usr/bin/env python3
"""Talk to ANIMA from the PC — faithful to the device cascade (L1 retrieval).

Loads the SAME encoder + corpus the device uses, builds the in-RAM full-scan index, and for
each natural-language question prints what ANIMA WOULD reply, the cosine confidence, and
whether it is ABOVE the gate (she answers) or BELOW (she honestly refuses). This is the
quickest way to feel the conversation quality on the freshly-enriched index, before deploy.

Usage:
  python tools/anima/ask.py "cos'e un puntatore" "what is a diode"   # ask specific things
  python tools/anima/ask.py                                          # run the demo battery
  python tools/anima/ask.py --lang en "how do i read a varying voltage"
"""
import os, sys, json, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

GATE = 0.66  # mirrors L1_COS_MIN in nucleo_anima_l1.c

# Demo battery: mixes paraphrased questions on ENRICHED packs (c-lang, esp32, electronics),
# NOT-yet-enriched packs (programming, nucleoos), and clearly out-of-scope (must refuse).
DEMO = [
    ("it", "come faccio a leggere una tensione che cambia"),     # esp.adc (enriched)
    ("en", "the component that lets current flow only one way"), # elec.diode (enriched)
    ("it", "come chiedo memoria mentre il programma gira"),       # c.malloc (enriched)
    ("en", "how do i stop a value from changing once set"),       # c.const (enriched)
    ("it", "cos'e un puntatore"),                                  # c.pointer (enriched)
    ("en", "what makes a program repeat the same step many times"),# prog.for-loop (NOT yet)
    ("it", "come funzionano le automazioni"),                     # os.* (NOT yet)
    ("en", "tell me a joke"),                                      # out-of-scope -> refuse
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("queries", nargs="*", help="questions; empty = demo battery")
    ap.add_argument("--lang", default="it", help="reply language for ad-hoc queries (it/en)")
    ap.add_argument("--kdir", default=None, help="knowledge dir (default: live; use a snapshot to avoid races)")
    a = ap.parse_args()

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus(kdir=a.kdir) if a.kdir else A.load_corpus()

    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    GATE_LO, MARGIN = 0.50, 0.08   # dialogic ambiguity band (mirrors band_eval / firmware spec)

    def top2(q):
        qv = encode(q); cos = V @ qv; order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((cards[ci], float(cos[i])))
            if len(out) >= 2: break
        return out

    def short(card, lang):
        return (card.get("reply") or {}).get(lang) or (card.get("reply") or {}).get("it", "")

    items = [(a.lang, q) for q in a.queries] if a.queries else DEMO
    print(f"ANIMA  (encoder {H}x{D}, {len(cards)} cards, {len(V)} phrasings, gate {GATE}/{GATE_LO})\n")
    for lang, q in items:
        t = top2(q); (c1, s1) = t[0]; (c2, s2) = t[1] if len(t) > 1 else (None, 0.0)
        print(f"  YOU> {q}")
        if s1 >= GATE:                                   # confident -> assert
            print(f" ANIMA> {short(c1, lang)}   [{c1['id']} cos={s1:.2f}]\n")
        elif s1 >= GATE_LO and c2 is not None and (s1 - s2) < MARGIN:   # ambiguous -> ask
            a1 = "Did you mean" if lang == "en" else "Intendi"
            print(f" ANIMA> {a1}: «{short(c1, lang)}»  {'or' if lang=='en' else 'oppure'}  «{short(c2, lang)}» ?"
                  f"   [CLARIFY {c1['id']} {s1:.2f} / {c2['id']} {s2:.2f}]\n")
        elif s1 >= GATE_LO:                              # one weak lead -> tentative confirm
            a1 = "Maybe you mean" if lang == "en" else "Forse intendi"
            print(f" ANIMA> {a1}: «{short(c1, lang)}» ?   [CONFIRM {c1['id']} cos={s1:.2f}]\n")
        else:
            print(f" ANIMA> Non lo so / I don't know.   [best={c1['id']} cos={s1:.2f} -> refuses]\n")


if __name__ == "__main__":
    main()
