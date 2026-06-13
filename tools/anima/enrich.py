#!/usr/bin/env python3
"""ANIMA corpus enrichment — fully offline, no model (per the ESP-only constraint).

The single biggest lever on retrieval quality is the density and bilingual coverage of the
`ask` phrasings. This tool helps keep them dense WITHOUT any external model:

  report (default)  list cards with few phrasings or a missing language — the work-list.
  --apply           add within-language question-stem variants for thin cards, using an
                    editable template map. It NEVER invents a missing-language translation
                    (that needs real knowledge, not templates) — it only flags those, so a
                    human (or you, acting as the author) fills them in.

Run:  python tools/anima/enrich.py                 # report gaps
      python tools/anima/enrich.py --apply         # expand thin cards in place
      python tools/anima/enrich.py --min 4         # gap threshold (default 4 per language)
"""
import os, sys, json, glob, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

# Question-stem templates: from one seed phrasing, derive natural variants in the same
# language. {x} is the "topic" — the seed with any leading stem stripped.
STEMS = {
    "it": ["cos'è {x}", "che cos'è {x}", "cosa significa {x}", "spiegami {x}", "parlami di {x}", "a cosa serve {x}"],
    "en": ["what is {x}", "what does {x} mean", "explain {x}", "tell me about {x}", "what are {x} for"],
}
LEAD = {
    "it": ["cos'è", "cos è", "che cos'è", "che cos è", "cosa significa", "spiegami", "parlami di", "a cosa serve", "qual è", "cos'e"],
    "en": ["what is", "what does", "what are", "explain", "tell me about", "the"],
}


def topic(phrase, lang):
    p = phrase.strip().rstrip("?").strip()
    low = p.lower()
    for s in sorted(LEAD[lang], key=len, reverse=True):
        if low.startswith(s + " "):
            return p[len(s):].strip()
    return p


def expand(asks, lang, target):
    """Grow `asks` toward `target` count using stem templates around the shortest seed."""
    if not asks or len(asks) >= target:
        return asks
    seed = min(asks, key=len)
    x = topic(seed, lang)
    if not x:
        return asks
    out = list(asks)
    for tpl in STEMS[lang]:
        if len(out) >= target:
            break
        cand = tpl.format(x=x)
        if cand.lower() not in (a.lower() for a in out):
            out.append(cand)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=int, default=4, help="target phrasings per language")
    ap.add_argument("--apply", action="store_true", help="write expansions back into the JSONL")
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(A.KDIR, "*.jsonl")))
    thin, missing_lang, changed_files = [], [], 0
    for path in files:
        rows = [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]
        dirty = False
        for r in rows:
            ask = r.get("ask", {})
            it, en = ask.get("it", []), ask.get("en", [])
            # commands/system cards are intentionally terse; only flag 'answer' cards.
            answerish = r.get("action", "answer") == "answer"
            if answerish and (not it or not en):
                missing_lang.append((r["id"], "it" if not it else "en"))
            if answerish and (len(it) < a.min or len(en) < a.min):
                thin.append((r["id"], len(it), len(en)))
            if a.apply and answerish:
                new_it = expand(it, "it", a.min) if it else it
                new_en = expand(en, "en", a.min) if en else en
                if new_it != it or new_en != en:
                    r.setdefault("ask", {})["it"] = new_it
                    r["ask"]["en"] = new_en
                    dirty = True
        if a.apply and dirty:
            with open(path, "w", encoding="utf-8") as f:
                for r in rows:
                    f.write(json.dumps(r, ensure_ascii=False) + "\n")
            changed_files += 1

    print(f"[enrich] threshold {a.min} phrasings/language")
    print(f"  thin cards (below threshold): {len(thin)}")
    for cid, ni, ne in thin[:40]:
        print(f"    {cid}: it={ni} en={ne}")
    if missing_lang:
        print(f"  cards missing a language ({len(missing_lang)}) — fill these by hand (templates can't translate):")
        for cid, lng in missing_lang[:40]:
            print(f"    {cid}: no {lng}")
    if a.apply:
        print(f"[enrich] expanded thin cards in {changed_files} file(s). Review, then run build_akb2.py + eval.py.")
    else:
        print("[enrich] report only — re-run with --apply to expand thin cards (within-language).")


if __name__ == "__main__":
    main()
