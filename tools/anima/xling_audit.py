#!/usr/bin/env python3
"""Cross-lingual coverage audit of the knowledge corpus.

For RETRIEVAL, an 'answer' card indexes ask.it + ask.en + reply.it + reply.en (anima_lib.index_texts).
A card with NO English ask AND no distinct English reply has ZERO English text indexed, so an English
query can only reach it via the bilingual encoder against Italian text — weaker, and it misses outright
below the rescue band (measured: "what is encryption" -> cos 0.58, abstain, while the IT card answers).

This audit quantifies the gap per file: how many answer-cards lack English coverage (the enrichment
work-list). Launch/system cards are templated (no reply text) so they're reported separately, not as gaps.

  python tools/anima/xling_audit.py            # summary per file + totals
  python tools/anima/xling_audit.py --list     # also print every gap card id (the work-list)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

def main():
    show = "--list" in sys.argv
    cards, _ = A.load_corpus()
    per_file = {}
    gaps = []
    for c in cards:
        if c["action"] != "answer":
            continue
        cat = c["category"]
        st = per_file.setdefault(cat, {"total": 0, "en_ask": 0, "en_reply": 0, "gap": 0})
        st["total"] += 1
        has_en_ask = len(c["ask"]["en"]) > 0
        # reply.en is a real English reply only if it differs from reply.it (load_corpus mirrors it->en otherwise)
        has_en_reply = bool(c["reply"]["en"]) and c["reply"]["en"] != c["reply"]["it"]
        if has_en_ask:
            st["en_ask"] += 1
        if has_en_reply:
            st["en_reply"] += 1
        if not has_en_ask and not has_en_reply:        # zero English text indexed -> EN query can't reach it well
            st["gap"] += 1
            gaps.append(c["id"])

    tot = {"total": 0, "en_ask": 0, "en_reply": 0, "gap": 0}
    print(f"{'file':<28} {'cards':>6} {'en_ask':>7} {'en_reply':>9} {'GAP':>5}")
    for cat in sorted(per_file):
        s = per_file[cat]
        for k in tot: tot[k] += s[k]
        flag = "  <--" if s["gap"] else ""
        print(f"{cat:<28} {s['total']:>6} {s['en_ask']:>7} {s['en_reply']:>9} {s['gap']:>5}{flag}")
    print("-" * 60)
    print(f"{'TOTAL':<28} {tot['total']:>6} {tot['en_ask']:>7} {tot['en_reply']:>9} {tot['gap']:>5}")
    pct = 100 * tot["gap"] / tot["total"] if tot["total"] else 0
    print(f"\nanswer-cards with NO English text indexed (the gap): {tot['gap']}/{tot['total']} ({pct:.1f}%)")
    if show:
        print("\ngap card ids:")
        for g in gaps:
            print("  ", g)

if __name__ == "__main__":
    main()
