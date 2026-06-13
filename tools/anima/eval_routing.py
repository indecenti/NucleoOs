#!/usr/bin/env python3
"""ANIMA routing benchmark — does each query reach the RIGHT mechanism?

The retrieval eval (eval.py) only asks "is the best card correct?". This asks the other
half: is the query even routed to the right place — calc, system value, app launch, file
tool, knowledge retrieval, or an honest refusal? Routing is where a retrieval assistant
goes wrong by treating everything as semantic search (the "coverage illusion").

It drives the live cascade over HTTP (GET /api/anima?q=&lang=), so it exercises the real
router code, not a reimplementation. Point it at the mock (L0 only) or the device (full).

Eval set: tools/anima/eval_routing.jsonl — {"q","lang","domain"[,"tier":"l1"]}.
Lines tagged "tier":"l1" need the AKB2 pack (device only); --no-l1 skips them.

Run:  python tools/anima/eval_routing.py --base-url http://localhost:5599 --no-l1   # mock
      python tools/anima/eval_routing.py --base-url http://192.168.0.166           # device
      python tools/anima/eval_routing.py --min-accuracy 0.9                         # CI gate
"""
import os, sys, json, argparse, urllib.parse, urllib.request

def query(base, q, lang, timeout):
    url = base.rstrip("/") + "/api/anima?q=" + urllib.parse.quote(q) + "&lang=" + lang
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://192.168.0.166")
    ap.add_argument("--queries", default=os.path.join(os.path.dirname(__file__), "eval_routing.jsonl"))
    ap.add_argument("--no-l1", action="store_true", help="skip knowledge queries (mock is L0-only)")
    ap.add_argument("--min-accuracy", type=float, default=None, help="exit non-zero if routing accuracy below this")
    ap.add_argument("--timeout", type=float, default=5.0)
    a = ap.parse_args()

    rows = [json.loads(l) for l in open(a.queries, encoding="utf-8") if l.strip() and not l.startswith("//")]
    if a.no_l1:
        rows = [r for r in rows if r.get("tier") != "l1"]

    total = correct = refused = 0
    per = {}                                   # domain -> [correct, total]
    fails = []
    for item in rows:
        q, lang, want = item["q"], item.get("lang", "it"), item["domain"]
        try:
            res = query(a.base_url, q, lang, a.timeout)
        except Exception as e:
            sys.exit(f"[routing] cannot reach {a.base_url} ({e})\n  start the mock (node tools/serve-shell.mjs) or point --base-url at the device.")
        got = res.get("domain", "none")
        total += 1
        per.setdefault(want, [0, 0])[1] += 1
        if got == "none":
            refused += 1
        if got == want:
            correct += 1; per[want][0] += 1
        else:
            fails.append((lang, q, want, got, res.get("reply", "")[:48]))

    acc = correct / total if total else 0.0
    print(f"[routing] {total} queries -> {a.base_url}")
    print(f"  accuracy {correct}/{total} = {acc:.1%}   non-so rate {refused}/{total} = {refused/total:.1%}" if total else "  (no queries)")
    print("  by domain: " + "  ".join(f"{d} {c}/{t}" for d, (c, t) in sorted(per.items())))
    if fails:
        print(f"  mis-routed ({len(fails)}):")
        for lang, q, want, got, rep in fails:
            print(f"    [{lang}] {q!r}  want {want} -> got {got}   ({rep!r})")
    if a.min_accuracy is not None and acc < a.min_accuracy:
        sys.exit(f"[routing] FAIL: accuracy {acc:.1%} < target {a.min_accuracy:.1%}")

if __name__ == "__main__":
    main()
