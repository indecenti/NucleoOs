#!/usr/bin/env python3
"""Part B PROTOTYPE — a card-relation graph ("mini network"), measured before any commit.

Builds, offline, a kNN graph over cards: each card -> its nearest neighbours by reply+detail
similarity (the device encoder, so it's faithful to what could ship). This is a SYMBOLIC graph
(inspectable, can't hallucinate), not a neural net. We then MEASURE whether it actually buys us
anything, so we ship it only if the numbers justify it:

  1. neighbour coherence   -> are a card's top neighbours sensible (same category)?
  2. disambiguation value  -> are the band's "did you mean X or Y?" pairs real neighbours?
  3. reasoning value        -> are "difference between X and Y" cards neighbours?
  4. contamination reach    -> would the graph have flagged the known off-topic asks?

  python tools/anima/relation_graph.py
"""
import os, sys, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

K = 4          # neighbours kept per card
GATE_HI, GATE_LO, MARGIN = 0.66, 0.50, 0.08


def main():
    table, H, D, NG, _ = A.load_encoder(); enc = A.make_encoder(table, H, D, NG)
    cards, _ = A.load_corpus()
    n = len(cards)

    # card MEANING vector = normalized mean of reply(it/en)+detail(it/en)
    M = np.zeros((n, D), np.float32)
    for i, c in enumerate(cards):
        ts = [c["reply"]["it"], c["reply"]["en"], c["detail"].get("it", ""), c["detail"].get("en", "")]
        vs = [enc(t) for t in ts if t]
        v = np.mean(vs, 0) if vs else np.zeros(D, np.float32)
        nrm = np.linalg.norm(v); M[i] = v / nrm if nrm > 0 else v
    S = M @ M.T
    np.fill_diagonal(S, -1)
    nbr = [list(np.argsort(S[i])[::-1][:K]) for i in range(n)]      # top-K neighbours per card

    cat = [c["category"] for c in cards]
    id2i = {c["id"]: i for i, c in enumerate(cards)}

    # 1) neighbour coherence: fraction of top-K neighbours in the same category
    same = sum(1 for i in range(n) for j in nbr[i] if cat[j] == cat[i])
    print(f"1) NEIGHBOUR COHERENCE: {same}/{n*K} = {same/(n*K):.0%} of top-{K} neighbours share the card's category")
    print("   samples:")
    for cid in ["prog.for-loop", "c.malloc", "elec.diode", "sci.gravity", "math.fraction", "geo.capital.italia"]:
        if cid in id2i:
            i = id2i[cid]; print(f"     {cid:20} -> {[cards[j]['id'] for j in nbr[i]]}")

    # full index for retrieval (ask+reply+detail) -> owner, to reproduce the band's top-2
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(enc(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def top2(q):
        cos = V @ enc(q); order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for k in order:
            ci = owner[k]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[k])))
            if len(out) >= 2: break
        return out

    def are_neighbours(i, j):
        return j in nbr[i] or i in nbr[j]

    # 2) disambiguation value: of the OOD clarify-band pairs, how many are graph-neighbours?
    ood = [json.loads(l) for l in open(os.path.join(os.path.dirname(__file__), "eval_ood.jsonl"), encoding="utf-8")
           if l.strip() and not l.startswith("//")]
    pairs = nb = 0
    for it in ood:
        if it["expect"] == "none": continue
        t = top2(it["q"]); (c1, s1), (c2, s2) = t[0], t[1]
        if GATE_LO <= s1 < GATE_HI and (s1 - s2) < MARGIN:        # clarify fires
            pairs += 1; nb += are_neighbours(c1, c2)
    print(f"\n2) DISAMBIGUATION: {nb}/{pairs} clarify pairs are graph-neighbours "
          f"({nb/pairs:.0%} sensible)" if pairs else "\n2) DISAMBIGUATION: no clarify pairs in OOD")

    # 3) reasoning value: are compared cards neighbours?
    comps = [("prog.integer", "prog.float"), ("esp.i2c", "esp.spi"), ("c.stack-heap", "c.stack-heap"),
             ("elec.diode", "elec.transistor"), ("prog.function", "prog.loop"), ("os.wifi", "esp.ble")]
    ok = tot = 0
    print("\n3) REASONING (comparison pairs that are neighbours):")
    for a, b in comps:
        if a in id2i and b in id2i and a != b:
            tot += 1; r = are_neighbours(id2i[a], id2i[b]); ok += r
            print(f"     {a:16} vs {b:16} -> {'neighbour' if r else 'NOT neighbour'}")
    print(f"   => {ok}/{tot} comparison pairs are neighbours")

    # 4) contamination reach: re-inject known off-topic asks; would the graph flag them?
    # rule: an ask is off-topic for card C if its best retrieved card is NOT C and NOT a neighbour of C.
    inj = [("geo.capital.giappone", "what is the capital of Russia"),     # cross-CARD same-category
           ("math.week-days", "qual e la formula per l'angolo tra due vettori"),  # cross-category junk
           ("geo.capital.italia", "come si calcola l'area di un cerchio"),         # cross-category
           ("prog.loop", "how do i repeat a block of code")]               # legit (should NOT flag)
    print("\n4) CONTAMINATION REACH (would the graph flag these?):")
    for cid, ask in inj:
        if cid not in id2i: continue
        i = id2i[cid]; best = top2(ask)[0][0]
        flagged = best != i and not are_neighbours(i, best)
        print(f"     [{('FLAG' if flagged else 'pass')}] {cid:20} <- {ask[:42]:42} (best={cards[best]['id']})")


if __name__ == "__main__":
    main()
