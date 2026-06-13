#!/usr/bin/env python3
"""C-NATIVE gate sweep. Runs the REAL device cascade (host harness, AKB2 retrieval) with the gate
forced to 0 + ANIMA_TRACE, in ONE pass per recall setting, capturing each query's true C-native
top1/top2 cosine + winning card. Then simulates ANY (gate, rescue_abs, rescue_margin) policy OFFLINE
from those captures — exact, since the cosines are the device's own. Measures, per policy:
  correct  = L1-reached in-scope query whose top1 card == expected (answered right)
  wrong    = L1-reached in-scope query answered with the WRONG card (the thing the gate must prevent)
  FP       = junk query (expect==none) that the policy would answer at all
The device's current policy (0.85 / rescue 0.72+0.12) is marked. Goal: max correct + max-recovered
at wrong==0 AND FP==0 — the only safe move. Compares device-nprobe vs full-recall to size the
recall lever (costs CPU/SD) against the gate lever (free).
"""
import os, sys, json, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXE_NODE = ["node", os.path.join(ROOT, "tools", "anima-host", "anima.mjs")]

def load_jsonl(path):
    out = []
    for l in open(path, encoding="utf-8"):
        l = l.strip()
        if not l or l.startswith("//"): continue
        out.append(json.loads(l))
    return out

def matches(expect, card):
    if isinstance(expect, list): return any(matches(e, card) for e in expect)
    if expect == "none": return False
    if expect.startswith("category:"): return card["category"] == expect.split(":",1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect

# corpus -> map a reply back to its card via a NORMALIZED ascii prefix (robust to the harness's
# occasional non-utf8 byte on exotic glyphs like the minus sign, and to whitespace differences).
cards, _ = A.load_corpus()
def nk(s):
    s = (s or "").lower()
    s = "".join(ch if ("a" <= ch <= "z" or "0" <= ch <= "9" or ch == " ") else " " for ch in s)
    return " ".join(s.split())[:48]
reply2card = {}
for c in cards:
    for lang in ("it","en"):
        t = c.get("reply",{}).get(lang) or ""
        k = nk(t)
        if k: reply2card.setdefault(k, c)

def run_pass(queries, extra_env):
    """One harness pass with gate forced to 0; capture per-query (tier, top1, top2, reply)."""
    qfile = os.path.join(HERE, ".csweep_q.txt")
    open(qfile, "w", encoding="utf-8").write("\n".join(queries) + "\n")
    env = dict(os.environ, ANIMA_TRACE="1", L1_GATE="0", L1_RABS="0", L1_RMARG="0", **extra_env)
    out = subprocess.run(EXE_NODE + ["--file", qfile], capture_output=True, encoding="utf-8", errors="replace", env=env).stdout
    rows, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"\s*Q:\s*(.*)$", line)
        if m: cur = m.group(1).strip(); rows[cur] = {"tier":"?","t1":-2.0,"t2":-2.0,"reply":""}
        elif cur:
            mt = re.search(r"tier=(\S+)", line)
            if mt: rows[cur]["tier"] = mt.group(1)
            mc = re.search(r"top1=([\-\d.]+)\s+top2=([\-\d.]+)", line)
            if mc: rows[cur]["t1"] = float(mc.group(1)); rows[cur]["t2"] = float(mc.group(2))
            mr = re.match(r"\s*reply:\s*(.*)$", line)
            if mr: rows[cur]["reply"] = mr.group(1).strip()
    return rows

def classify(item, row):
    """Return ('correct'|'wrong'|'l0'|'miss', is_l1) for an in-scope query under the gate=0 pass."""
    tier = row["tier"]; reply = row["reply"]
    if tier == "L1/fact":
        card = reply2card.get(nk(reply))
        ok = card is not None and matches(item["expect"], card)
        return ("correct" if ok else "wrong"), True
    if tier in ("none","?") or reply in ("(vuoto)",""):
        return "miss", False           # even gate=0 found nothing -> true retrieval miss / empty
    return "l0", False                 # answered earlier by an L0 tool/HDC -> gate-independent

def main():
    inscope = load_jsonl(os.path.join(HERE, "eval_ood.jsonl"))
    junkfiles = sys.argv[1:] or []
    junk = [j for j in inscope if j["expect"] == "none"]
    inscope = [j for j in inscope if j["expect"] != "none"]
    for jf in junkfiles:
        for l in open(jf, encoding="utf-8"):
            l = l.strip()
            if not l or l.startswith("//"): continue
            try:
                o = json.loads(l); q = o["q"] if isinstance(o, dict) else o
                if isinstance(o, dict) and o.get("expect") not in (None, "none"): continue  # skip in-scope rows
            except Exception:
                q = l
            junk.append({"q": q, "expect": "none"})

    allq = [x["q"] for x in inscope] + [x["q"] for x in junk]
    for mode, env in (("device(probe2-6)", {}), ("full-recall(70)", {"L1_KEEP":"70","L1_NPROBE":"70"})):
        rows = run_pass(allq, env)
        isc = [(it, rows.get(it["q"], {"tier":"?","t1":-2,"t2":-2,"reply":""})) for it in inscope]
        jnk = [rows.get(it["q"], {"tier":"?","t1":-2,"t2":-2,"reply":""}) for it in junk]
        # gate=0 baseline classification (path each query takes)
        cls = [classify(it, r) for it,r in isc]
        n_l0   = sum(1 for c,_ in cls if c=="l0")
        n_miss = sum(1 for c,_ in cls if c=="miss")
        l1rows = [(c, r["t1"], r["t2"]) for (c,is1),(it,r) in zip(cls, isc) if is1]   # L1-reached only
        jl1 = [(r["t1"], r["t2"]) for r in jnk if r["tier"]=="L1/fact"]                # junk reaching the L1 gate
        l0fp = [(it["q"], r) for it,r in zip(junk, jnk) if r["tier"] not in ("L1/fact","none","?") and r["reply"] not in ("(vuoto)","")]

        def acc(t1,t2,gate,rabs,rmarg): return t1>=gate or (t1>=rabs and (t1-t2)>=rmarg)
        CUR=(0.85,0.72,0.12)
        cW = sum(1 for c,t1,t2 in l1rows if c=="wrong" and acc(t1,t2,*CUR))
        print(f"\n===== MODE {mode} =====  in-scope={len(inscope)} junk={len(junk)}")
        print(f"  L0/tool-answered (gate-independent): {n_l0}   L1-reached in-scope: {len(l1rows)}   junk reaching L1: {len(jl1)}   L0-FP floor: {len(l0fp)}")
        print(f"\n   gate  rabs  rmarg | correct  wrong | gate-FP   (L0-correct {n_l0}, L0-FP floor {len(l0fp)})")
        grid = []
        for gate in (0.85,0.82,0.80,0.78,0.76,0.74,0.72):
            for rabs in (0.72,0.68,0.64,0.60):
                for rmarg in (0.12,0.10,0.08,0.06):
                    cor = sum(1 for c,t1,t2 in l1rows if c=="correct" and acc(t1,t2,gate,rabs,rmarg))
                    wr  = sum(1 for c,t1,t2 in l1rows if c=="wrong"   and acc(t1,t2,gate,rabs,rmarg))
                    gfp = sum(1 for t1,t2 in jl1 if acc(t1,t2,gate,rabs,rmarg))
                    grid.append((gate,rabs,rmarg,cor,wr,gfp))
        cur = next(g for g in grid if (g[0],g[1],g[2])==CUR)
        print(f"   {cur[0]:.2f} {cur[1]:.2f} {cur[2]:.2f}  |   {cur[3]:3d}    {cur[4]:3d}  |   {cur[5]:3d}     <-- CURRENT DEVICE")
        # SAFE = recovers more correct, with NO extra wrong and NO extra gate-FP vs current
        safe = sorted([g for g in grid if g[4]<=cur[4] and g[5]<=cur[5] and g[3]>cur[3]], key=lambda g:-g[3])
        print("   --- configs that recover correct with wrong<=cur AND gate-FP<=cur ---")
        for g in safe[:8]: print(f"   {g[0]:.2f} {g[1]:.2f} {g[2]:.2f}  |   {g[3]:3d}    {g[4]:3d}  |   {g[5]:3d}")
        if not safe: print("   (NONE — no loosening recovers correct without adding a wrong or a gate-FP)")
        if mode.startswith("device"):
            print("\n   >>> WRONG in-scope at CURRENT config (confident mis-answers — validate):")
            for (c,is1),(it,r) in zip(cls, isc):
                if is1 and c=="wrong" and acc(r["t1"],r["t2"],*CUR):
                    print(f"       t1={r['t1']:.3f} t2={r['t2']:.3f} exp={str(it['expect'])[:22]:<22} got={nk(r['reply'])[:40]!r} | {it['q']!r}")
            print("   >>> L0-FP junk (answered by a tool, gate-independent):")
            for q,r in l0fp: print(f"       tier={r['tier']} reply={r['reply'][:50]!r} | {q!r}")

if __name__ == "__main__":
    main()
