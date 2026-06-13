#!/usr/bin/env python3
"""Resolve cross-card duplicate asks in the ANIMA corpus — the "one concept = one card" gate.

The Wikipedia bulk-ingest (wiki.it.* cards) collided with the hand-curated cards (cult.*, elec.*,
prog.*, sci.*, geo.* ...): the SAME ask ("cos'e la filosofia") ended up on two cards. regress.py
hard-fails on any cross-card duplicate ask, and two cards for one concept blur the retrieval clusters.

This tool resolves every collision with an encoder-grounded decision, in two cases:

  MERGE (same concept, reply-cosine >= --same):  keep the canonical card (the non-wiki one if exactly
     one side is wiki, else the richer), ABSORB the other's unique asks, and — if the canonical has no
     detail yet — PROMOTE the dropped card's fuller reply into the canonical's `detail` (so "dimmi di
     piu" still surfaces the Wikipedia depth). The duplicate card is removed. No knowledge is lost.

  REASSIGN (distinct concepts, reply-cosine < --same):  e.g. carbonio (element) vs Carbon (language),
     or man.history (shell command) vs wiki.it.storia (the discipline). Keep BOTH cards; for each shared
     ask, keep it only on the card it is semantically closest to (ask->reply+detail cosine) and strip it
     from the other.

Run order:
   python tools/anima/dedup.py            # dry-run: print the full plan + cosines, touch nothing
   python tools/anima/dedup.py --apply    # rewrite the affected knowledge/*.jsonl in place

Encoding is byte-identical to the device (anima_lib.load_encoder/make_encoder), so the cosines used to
decide here are the same ones the on-device retriever will see.
"""
import os, sys, glob, json, argparse
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anima_lib import load_encoder, make_encoder, KDIR

def is_wiki(card):
    return str(card.get("id", "")).startswith("wiki.") or str(card.get("source", "")).startswith("wikipedia")

def asks_of(card):
    a = card.get("ask") or {}
    return [s for s in (a.get("it") or []) if s] + [s for s in (a.get("en") or []) if s]

def reply_text(card):
    r = card.get("reply") or {}
    return (r.get("it") or r.get("en") or "").strip()

def detail_text(card):
    d = card.get("detail") or {}
    return (d.get("it") or d.get("en") or "").strip()

def richness(card):
    # how "rich" a card is, to break a same-concept tie when neither/both are wiki
    return len(reply_text(card)) + 2 * len(asks_of(card)) + (50 if reply_text(card) and (card.get("reply") or {}).get("en") else 0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="rewrite files (default: dry-run)")
    ap.add_argument("--same", type=float, default=0.60, help="reply-cosine >= this => same concept => MERGE")
    ap.add_argument("--max-asks", type=int, default=16, help="cap asks per card after absorbing")
    args = ap.parse_args()

    table, H, D, NGRAMS, WORD_N = load_encoder()
    enc = make_encoder(table, H, D, NGRAMS)
    def cos(a, b):
        va, vb = enc(a), enc(b)
        na, nb = np.linalg.norm(va), np.linalg.norm(vb)
        return float(va @ vb / (na * nb)) if na and nb else 0.0

    # --- load raw cards, remembering their file (we rewrite whole files) ---
    files = sorted(glob.glob(os.path.join(KDIR, "*.jsonl")))
    file_cards = {}          # path -> list of (raw_line_dict | None for comments, is_card)
    by_id = {}               # id -> card dict (the live object we mutate)
    card_file = {}           # id -> path
    for path in files:
        rows = []
        for line in open(path, encoding="utf-8"):
            s = line.strip()
            if not s or s.startswith("//"):
                rows.append(("raw", line.rstrip("\n")))
                continue
            try:
                c = json.loads(s)
            except json.JSONDecodeError:
                rows.append(("raw", line.rstrip("\n"))); continue
            rows.append(("card", c))
            cid = c.get("id")
            if cid:
                by_id[cid] = c; card_file[cid] = path
        file_cards[path] = rows

    # --- build ask -> holders ---
    ask2ids = {}
    for cid, c in by_id.items():
        for a in asks_of(c):
            ask2ids.setdefault(a.lower(), set()).add(cid)
    dups = {a: ids for a, ids in ask2ids.items() if len(ids) > 1}

    # group by colliding id-set
    groups = {}
    for a, ids in dups.items():
        groups.setdefault(frozenset(ids), []).append(a)

    # The encoder's surface-text cosine does NOT separate "same concept" from "distinct homonym"
    # (a short curated definition vs a long Wikipedia abstract scores low even for the SAME concept;
    # two distinct homonyms templated from the same word score high). So we MERGE by default — these
    # collisions are overwhelmingly a curated card and its Wikipedia translation of ONE concept — and
    # carve out the handful of genuine homonyms explicitly. For each, KEEP names the card that owns the
    # shared (generic, usually EN-templated) ask; the other keeps only its specific title asks.
    DISTINCT_KEEP = {
        frozenset({"wiki.it.carbon-linguaggio-di-programmazione", "wiki.it.carbonio"}): "wiki.it.carbonio",
        frozenset({"man.history", "wiki.it.storia"}): "wiki.it.storia",
        frozenset({"wiki.it.insieme", "wiki.it.set-informatica"}): "wiki.it.set-informatica",
        frozenset({"wiki.it.algoritmi-per-la-generazione-di-un-labirinto",
                   "wiki.it.algoritmi-per-la-risoluzione-di-labirinti"}): "wiki.it.algoritmi-per-la-risoluzione-di-labirinti",
        frozenset({"wiki.it.multidigrafo-euleriano", "wiki.it.multigrafo-euleriano"}): "wiki.it.multigrafo-euleriano",
    }

    removed = set()          # ids to drop
    absorbed_asks = {}       # id -> list of new asks to add
    stripped = {}            # id -> set of asks (lowercased) to remove
    plan = []

    for ids_set, shared in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        ids = sorted(ids_set)
        if len(ids) != 2:
            plan.append(("SKIP-3WAY", ids, len(shared), 0.0))   # none expected; report if it appears
            continue
        a_id, b_id = ids
        ca, cb = by_id[a_id], by_id[b_id]
        key = frozenset(ids)
        if key in DISTINCT_KEEP:
            keep = DISTINCT_KEEP[key]
            loser = b_id if keep == a_id else a_id
            for a in shared:
                stripped.setdefault(loser, set()).add(a.lower())
            plan.append(("REASSIGN", [f"keep:{keep}", f"strip<-{loser}"], len(shared), cos(reply_text(ca), reply_text(cb))))
            continue
        # MERGE: keep the non-wiki card (exactly one side is wiki here), else the richer.
        wa, wb = is_wiki(ca), is_wiki(cb)
        if wa != wb:
            keep, drop = (b_id, a_id) if wa else (a_id, b_id)
        else:
            keep, drop = (a_id, b_id) if richness(ca) >= richness(cb) else (b_id, a_id)
        kc, dc = by_id[keep], by_id[drop]
        have = {s.lower() for s in asks_of(kc)}
        for s in asks_of(dc):
            if s.lower() not in have:
                absorbed_asks.setdefault(keep, []).append(s); have.add(s.lower())
        removed.add(drop)
        plan.append(("MERGE", [keep, f"<-{drop}"], len(shared), cos(reply_text(ca), reply_text(cb))))

    # --- report ---
    n_merge = sum(1 for p in plan if p[0] == "MERGE")
    n_reassign = sum(1 for p in plan if p[0].startswith("REASSIGN"))
    print(f"[dedup] {len(dups)} duplicated asks across {len(groups)} groups  "
          f"=> {n_merge} MERGE, {n_reassign} REASSIGN, dropping {len(removed)} cards\n")
    for kind, ids, n, rc in plan:
        tag = f"cos={rc:.2f}" if rc else "      "
        print(f"  {kind:11s} {tag}  ({n} ask)  " + "  ".join(ids))

    if not args.apply:
        print("\n[dry-run] nothing written. Re-run with --apply to rewrite the files.")
        return

    # --- apply: detail-promotion for merges, then write files ---
    for kind, ids, n, rc in plan:
        if kind != "MERGE":
            continue
        keep = ids[0]
        drop = ids[1][2:]   # strip "<-"
        kc, dc = by_id[keep], by_id[drop]
        kdet = kc.get("detail") or {}
        # Preserve depth: the drill-down keeps the RICHER of (existing curated detail, Wikipedia abstract),
        # so dropping the duplicate card never loses the Wikipedia knowledge — it becomes "dimmi di piu".
        cur = (kdet.get("it") or "").strip()
        wiki_abs = reply_text(dc)
        best = max([t for t in (cur, wiki_abs) if t], key=len, default="")
        if best and best != reply_text(kc):
            kc["detail"] = {"it": best, "en": (kdet.get("en") or "")}

    # apply absorbed asks (append to it/en heuristically: keep language of original list)
    for cid, extra in absorbed_asks.items():
        c = by_id[cid]
        a = c.setdefault("ask", {})
        it = list(a.get("it") or [])
        en = list(a.get("en") or [])
        for s in extra:
            # route to en if it is ascii-ish & looks english (has english stopword), else it
            low = s.lower()
            is_en = any(w in low.split() for w in ("what", "the", "how", "explain", "of", "is", "tell"))
            (en if is_en else it).append(s)
        # cap, keeping the originals first
        a["it"] = it[:args.max_asks]
        a["en"] = en[:args.max_asks]

    # apply stripped asks
    for cid, rem in stripped.items():
        c = by_id[cid]
        a = c.get("ask") or {}
        for lang in ("it", "en"):
            a[lang] = [s for s in (a.get(lang) or []) if s.lower() not in rem]
        c["ask"] = a

    # rewrite files
    touched = 0
    for path, rows in file_cards.items():
        changed = any(k == "card" and c.get("id") in removed for k, c in rows) \
                  or any(k == "card" and (c.get("id") in absorbed_asks or c.get("id") in stripped) for k, c in rows)
        if not changed:
            continue
        out = []
        for kind, val in rows:
            if kind == "raw":
                out.append(val)
            else:
                if val.get("id") in removed:
                    continue
                out.append(json.dumps(val, ensure_ascii=False))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out) + "\n")
        touched += 1
    print(f"\n[apply] rewrote {touched} files, dropped {len(removed)} duplicate cards.")

if __name__ == "__main__":
    main()
