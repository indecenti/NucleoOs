#!/usr/bin/env python3
"""Precision/recall sweep for the L1 answer gate. The audit found 59 in-scope queries refused
despite the correct card being rank-1-below-gate. Question: can a lower threshold and/or a
top1-top2 MARGIN rule recover them WITHOUT answering junk (the 0-FP promise)? This simulates
policies offline on the shared encoder (in-scope eval = should-answer; expect==none = junk =
must-refuse), so we pick a policy with evidence before touching C and re-flashing.
"""
import os, sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

def matches(expect, card):
    if isinstance(expect, list): return any(matches(e, card) for e in expect)
    if expect == "none": return False
    if expect.startswith("category:"): return card["category"] == expect.split(":",1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect

inscope, junk = [], []
for l in open(os.path.join(HERE, "eval_ood.jsonl"), encoding="utf-8"):
    l = l.strip()
    if not l or l.startswith("//"): continue
    o = json.loads(l)
    (junk if o["expect"] == "none" else inscope).append(o)

table,H,D,NG,_ = A.load_encoder(); enc = A.make_encoder(table,H,D,NG)
cards,_ = A.load_corpus()
vecs, owner = [], []
for ci,c in enumerate(cards):
    for p in A.index_texts(c): vecs.append(enc(p)); owner.append(ci)
V = np.stack(vecs).astype(np.float32)

def top2(q):
    cos = V @ enc(q); order = np.argsort(cos)[::-1]
    seen=set(); out=[]
    for i in order:
        ci = owner[i]
        if ci in seen: continue
        seen.add(ci); out.append((ci, float(cos[i])))
        if len(out) >= 2: break
    return out

# Precompute per query: (top1cos, margin, top1_is_correct)
isc = []
for o in inscope:
    t = top2(o["q"]); c1,m = t[0][1], t[0][1]-t[1][1]
    isc.append((c1, m, matches(o["expect"], cards[t[0][0]])))
jnk = []
for o in junk:
    t = top2(o["q"]); jnk.append((t[0][1], t[0][1]-t[1][1]))

N = len(isc)
print(f"in-scope={N}  junk={len(jnk)}")
print(f"current gate 0.85 -> correct {sum(1 for c,m,ok in isc if ok and c>=0.85)}/{N}  FP {sum(1 for c,m in jnk if c>=0.85)}/{len(jnk)}")
print("\n  T     M    correct(recall)   FP(junk)")
for M in (0.0, 0.04, 0.07):
    for T in (0.85,0.80,0.76,0.72,0.68,0.64,0.60):
        cor = sum(1 for c,m,ok in isc if ok and c>=T and m>=M)
        fp  = sum(1 for c,mg in jnk if c>=T and mg>=M)
        print(f"  {T:.2f}  {M:.2f}   {cor:3d} ({100*cor//N:3d}%)        {fp}/{len(jnk)}")
    print()
