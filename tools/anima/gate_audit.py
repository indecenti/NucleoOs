#!/usr/bin/env python3
"""Audit the L1 0.85 answer gate: find in-scope queries the DEVICE refuses (or answers wrong)
even though the correct card exists and is retrievable. These are the gate/AKB2 victims — the
systemic version of the ohm fix. Behavioural truth comes from the real C harness; the rank/cosine
of the expected card (via the shared encoder) explains WHY.
"""
import os, sys, json, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def matches(expect, card):
    if isinstance(expect, list): return any(matches(e, card) for e in expect)
    if expect == "none": return False
    if expect.startswith("category:"): return card["category"] == expect.split(":",1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect

# 1) load eval in-scope queries
qs = []
for l in open(os.path.join(HERE, "eval_ood.jsonl"), encoding="utf-8"):
    l = l.strip()
    if not l or l.startswith("//"): continue
    o = json.loads(l)
    if o["expect"] != "none": qs.append(o)

# 2) run the REAL C harness over all queries in one batch
qfile = os.path.join(HERE, ".audit_q.txt")
open(qfile, "w", encoding="utf-8").write("\n".join(q["q"] for q in qs) + "\n")
out = subprocess.run(["node", os.path.join(ROOT,"tools","anima-host","anima.mjs"), "--file", qfile],
                     capture_output=True, text=True, encoding="utf-8").stdout
# parse per-query blocks
beh = {}  # query -> (tier, intent, reply)
cur = None
for line in out.splitlines():
    m = re.match(r"\s*Q:\s*(.*)$", line)
    if m: cur = m.group(1).strip(); beh[cur] = ["?","?",""]
    elif cur:
        mt = re.search(r"tier=(\S+).*intent=(\S*)", line)
        if mt: beh[cur][0] = mt.group(1); beh[cur][1] = mt.group(2)
        mr = re.match(r"\s*reply:\s*(.*)$", line)
        if mr: beh[cur][2] = mr.group(1).strip()

# 3) encoder ranking: rank + cosine of the best-matching EXPECTED card
table,H,D,NG,_ = A.load_encoder(); enc = A.make_encoder(table,H,D,NG)
cards,_ = A.load_corpus()
vecs, owner = [], []
for ci,c in enumerate(cards):
    for p in A.index_texts(c): vecs.append(enc(p)); owner.append(ci)
V = np.stack(vecs).astype(np.float32)
def rank_expected(q, expect):
    cos = V @ enc(q); order = np.argsort(cos)[::-1]
    seen=set(); rnk=0
    top1 = None
    for i in order:
        ci = owner[i]
        if ci in seen: continue
        seen.add(ci); rnk += 1
        if top1 is None: top1 = (cards[ci]["id"], float(cos[i]))
        if matches(expect, cards[ci]): return rnk, float(cos[i]), cards[ci]["id"], top1
    return 999, 0.0, "?", top1

GATE = 0.85
victims, misses, wrong, ok = [], [], [], 0
for q in qs:
    tier, intent, reply = beh.get(q["q"], ["?","?",""])
    refused = (tier in ("none","?")) or reply in ("(vuoto)","")
    rnk, cos, gotid, top1 = rank_expected(q["q"], q["expect"])
    row = (q["q"], q["expect"], tier, rnk, cos, gotid, top1)
    if refused:
        if rnk <= 5: victims.append(row)      # device said nothing, but correct card IS retrievable
        else: misses.append(row)              # genuinely not retrievable -> corpus/encoder gap
    else:
        # answered: did it answer with the right thing? (heuristic: expected card is top-1 retrieval)
        if rnk == 1: ok += 1
        else: wrong.append(row)

print(f"\n=== GATE AUDIT: {len(qs)} in-scope queries, device behaviour vs retrievability ===")
print(f"  answered & expected=top1 : {ok}")
print(f"  REFUSED but card in top-5 (GATE/AKB2 VICTIMS): {len(victims)}")
print(f"  refused & card not in top-5 (true miss)       : {len(misses)}")
print(f"  answered but expected!=top1 (check)           : {len(wrong)}")
print(f"\n--- VICTIMS (refused despite knowing) sorted by cosine desc — closest to gate first ---")
for q,e,t,r,c,g,t1 in sorted(victims, key=lambda x:-x[4]):
    print(f"  cos={c:.3f} rank={r}  exp={str(e):<24} | {q!r}")
print(f"\n--- TRUE MISSES (no retrievable card) ---")
for q,e,t,r,c,g,t1 in misses:
    print(f"  exp={str(e):<24} top1={t1[0]}({t1[1]:.2f}) | {q!r}")
