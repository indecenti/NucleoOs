#!/usr/bin/env python3
"""Apply cross-lingual enrichment to the knowledge cards from tools/anima/xling_enrich.json.

For each card id in the work-list: ADD the listed English ask phrasings (deduped, case-insensitive,
appended in order) and, when given, set reply.en IF the card has no distinct English reply yet. Every
other line in every file is preserved byte-for-byte — only the touched cards are rewritten — so this is
safe to run repeatedly and easy to review in a diff. After running, rebuild the index: build_akb2.py.

The work-list is the scalable lever: as xling-check surfaces more EN-recall gaps, add ids to the JSON and
re-run. Each enrichment is a translation of the card's OWN Italian source (the same grounded fact in
English), never a new claim — so the zero-hallucination property is preserved.

  python tools/anima/enrich_xling.py            # apply
  python tools/anima/enrich_xling.py --check    # exit 1 if any card still needs enrichment (CI)
"""
import os, sys, json, glob

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KDIR = os.path.join(ROOT, "tools", "anima", "knowledge")
WL = os.path.join(ROOT, "tools", "anima", "xling_enrich.json")


def load_worklist():
    wl = json.loads(open(WL, encoding="utf-8").read())
    return {k: v for k, v in wl.items() if not k.startswith("_")}


def merge_ask_en(card, add):
    cur = card.setdefault("ask", {}).setdefault("en", [])
    have = {a.strip().lower() for a in cur}
    added = 0
    for a in add:
        if a.strip().lower() not in have:
            cur.append(a); have.add(a.strip().lower()); added += 1
    return added


def needs(card, spec):
    """True if applying spec would change the card (so --check can flag pending work)."""
    cur = {a.strip().lower() for a in card.get("ask", {}).get("en", [])}
    if any(a.strip().lower() not in cur for a in spec.get("ask_en", [])):
        return True
    rep = card.get("reply") or {}
    ren = (rep.get("en") or "").strip(); rit = (rep.get("it") or "").strip()
    if spec.get("reply_en") and (not ren or ren == rit):
        return True
    return False


def revert_card(card, spec):
    """Undo this card's enrichment: drop the worklist's ask.en phrasings and a reply.en equal to the
    worklist value. Returns True if anything changed. Used by --revert to restore the pre-enrichment state."""
    changed = False
    add = {a.strip().lower() for a in spec.get("ask_en", [])}
    cur = card.get("ask", {}).get("en", [])
    new = [a for a in cur if a.strip().lower() not in add]
    if len(new) != len(cur):
        card["ask"]["en"] = new; changed = True
    rep = card.get("reply") or {}
    if spec.get("reply_en") and (rep.get("en") or "").strip() == spec["reply_en"].strip():
        rep.pop("en", None); changed = True
    return changed


def main():
    check = "--check" in sys.argv
    revert = "--revert" in sys.argv
    wl = load_worklist()
    seen, pending, edits = set(), [], 0
    files_changed = 0

    for path in sorted(glob.glob(os.path.join(KDIR, "*.jsonl"))):
        lines = open(path, encoding="utf-8").read().split("\n")
        changed = False
        for i, line in enumerate(lines):
            t = line.strip()
            if not t or t.startswith("//"):
                continue
            try:
                c = json.loads(t)
            except json.JSONDecodeError:
                continue
            cid = c.get("id")
            if cid not in wl:
                continue
            seen.add(cid)
            spec = wl[cid]
            if check:
                if needs(c, spec):
                    pending.append(cid)
                continue
            if revert:
                if revert_card(c, spec):
                    lines[i] = json.dumps(c, ensure_ascii=False)
                    changed = True; edits += 1
                    print(f"  reverted {cid} ({os.path.basename(path)})")
                continue
            n = merge_ask_en(c, spec.get("ask_en", []))
            rep = c.setdefault("reply", {})
            ren = (rep.get("en") or "").strip(); rit = (rep.get("it") or "").strip()
            set_reply = bool(spec.get("reply_en")) and (not ren or ren == rit)
            if set_reply:
                rep["en"] = spec["reply_en"]
            if n or set_reply:
                lines[i] = json.dumps(c, ensure_ascii=False)
                changed = True; edits += 1
                print(f"  enriched {cid}: +{n} ask.en{' +reply.en' if set_reply else ''} ({os.path.basename(path)})")
        if changed and (not check):
            open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
            files_changed += 1

    missing = [cid for cid in wl if cid not in seen]
    if missing:
        print(f"  WARNING: {len(missing)} work-list id(s) not found in the corpus: {', '.join(missing)}", file=sys.stderr)

    if check:
        if pending:
            print(f"[enrich_xling] STALE: {len(pending)} card(s) need enrichment: {', '.join(sorted(set(pending)))}", file=sys.stderr)
            sys.exit(1)
        print(f"[enrich_xling] OK: all {len(wl)} work-list cards already enriched.")
        return
    print(f"[enrich_xling] applied {edits} edit(s) across {files_changed} file(s) for {len(seen)}/{len(wl)} cards.")


if __name__ == "__main__":
    main()
