#!/usr/bin/env python3
"""Compile the NucleoOS manual (registry/manual/*.info, bilingual) into ANIMA knowledge cards.

The .info files are rich man-page-style JSON ({id, category, it:{title,description,details},
en:{...}}) used by the Help app. ANIMA couldn't answer "come uso ls" / "cos'e il comando grep"
/ "hai un manuale" because that content lived only in the Help app, not in the retrieval corpus.

This turns each .info into one answer card: the short `description` becomes the reply, the long
`details` becomes the drill-down `detail` ("dimmi di piu"), and a handful of natural phrasings
(command name + title + "come uso X" / "how do i use X" / "manuale di X" ...) become the `ask`
keys. The pack auto-scales: drop a new .info in registry/manual, re-run this + build_akb2.

Usage:  python tools/anima/ingest_manual.py        # -> tools/anima/knowledge/manual.jsonl
"""
import os, sys, json, glob

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MANDIR = os.path.join(ROOT, "registry", "manual")
OUT = os.path.join(HERE, "knowledge", "manual.jsonl")
DETAIL_CAP = 240          # firmware rd_cstr reads <=256; keep detail comfortably under


def clip(s, n):
    s = " ".join((s or "").split())
    return s if len(s) <= n else s[:n - 1].rsplit(" ", 1)[0] + "…"


def phrasings(cid, title, lang):
    t = " ".join((title or "").split())
    if lang == "it":
        out = [cid, t, f"come uso {cid}", f"a cosa serve {cid}", f"cos'e {cid}", f"cosa fa {cid}",
               f"cosa fa il comando {cid}", f"comando {cid}", f"manuale di {cid}", f"spiegami {cid}",
               f"come funziona {cid}"]
    else:
        out = [cid, t, f"how do i use {cid}", f"what is {cid}", f"what does {cid} do",
               f"what does the {cid} command do", f"{cid} command", f"{cid} manual",
               f"explain {cid}", f"how does {cid} work"]
    seen, res = set(), []
    for p in out:
        p = " ".join((p or "").split())
        if p and p.lower() not in seen:
            seen.add(p.lower()); res.append(p)
    return res


def main():
    files = sorted(glob.glob(os.path.join(MANDIR, "*.info")))
    if not files:
        sys.exit(f"[manual] no .info files in {MANDIR}")
    cards, skipped = [], []
    for path in files:
        try:
            d = json.load(open(path, encoding="utf-8"))
        except Exception as e:
            skipped.append(f"{os.path.basename(path)}: {e}"); continue
        cid = d.get("id") or os.path.splitext(os.path.basename(path))[0]
        it, en = d.get("it") or {}, d.get("en") or {}
        rep_it, rep_en = clip(it.get("description", ""), 240), clip(en.get("description", ""), 240)
        if not rep_it and not rep_en:
            skipped.append(f"{cid}: no description"); continue
        card = {
            "id": f"man.{cid}",
            "category": "nucleoos",
            "action": "answer",
            "reply": {"it": rep_it or rep_en, "en": rep_en or rep_it},
            "detail": {"it": clip(it.get("details", ""), DETAIL_CAP),
                       "en": clip(en.get("details", ""), DETAIL_CAP)},
            "ask": {"it": phrasings(cid, it.get("title", ""), "it"),
                    "en": phrasings(cid, en.get("title", ""), "en")},
            "source": f"manual/{cid}.info",
        }
        cards.append(card)

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("// NucleoOS manual compiled to ANIMA cards by tools/anima/ingest_manual.py — do not hand-edit.\n")
        f.write("// Re-generate: python tools/anima/ingest_manual.py && python tools/anima/build_akb2.py\n")
        for c in cards:
            f.write(json.dumps(c, ensure_ascii=False) + "\n")

    print(f"[manual] {len(cards)} cards -> {os.path.relpath(OUT, ROOT)}")
    if skipped:
        print(f"[manual] skipped {len(skipped)}:")
        for s in skipped[:10]:
            print("   -", s)


if __name__ == "__main__":
    main()
