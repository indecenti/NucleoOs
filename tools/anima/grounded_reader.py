#!/usr/bin/env python3
"""ANIMA GROUNDED READER — measure a retrieve-then-select hybrid that NEVER invents facts.

The offline encoder retrieves the top-K candidate cards (R@5 is high even when the absolute
cosine is below the answer gate). The LLM ("Grok"/Groq) then acts ONLY as a strict ROUTER: it
picks the ONE candidate whose stored answer genuinely addresses the question, or 0 (none). ANIMA
would then speak that card's HUMAN-VERIFIED reply verbatim — the LLM never writes the answer, so
it cannot hallucinate a fact. A junk/out-of-scope query yields 0 -> honest refuse.

This script MEASURES that design end to end against the held-out OOD eval, with real Groq:
  - in-scope: did the router pick the card whose verified reply == the expected one? (correct)
  - out-of-scope (expect "none"): did it correctly pick 0? (a non-zero pick = FALSE POSITIVE)
It reuses the EXACT device encoder (anima_lib) for retrieval, so the candidate set is what the
device would actually produce. The LLM never sees the eval's expected answer.

Usage:
  python tools/anima/grounded_reader.py                 # full eval_ood.jsonl
  python tools/anima/grounded_reader.py --limit 15      # quick smoke
  python tools/anima/grounded_reader.py --k 5 --model llama-3.3-70b-versatile --fails
"""
import os, sys, json, argparse, urllib.request, urllib.error, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def load_teacher():
    for p in ["H:/data/anima/teacher.json",
              os.path.join(ROOT, "tools", "sd-sim", "data", "anima", "teacher.json")]:
        if os.path.exists(p):
            return json.loads(open(p, encoding="utf-8").read().lstrip("﻿"))
    sys.exit("teacher.json not found")

def matches(expect, card):
    if isinstance(expect, list):
        return any(matches(e, card) for e in expect)
    if expect == "none": return False
    if expect.startswith("category:"): return card["category"] == expect.split(":", 1)[1]
    if expect.endswith("*"):           return card["id"].startswith(expect[:-1])
    return card["id"] == expect

SYS = (
 "Sei un ROUTER severo per un assistente offline. Ricevi una DOMANDA utente e una lista di "
 "VOCI di conoscenza numerate, ognuna col suo testo di risposta gia' verificato. Il tuo UNICO "
 "compito: scegliere il NUMERO della voce la cui risposta risponde DAVVERO e direttamente alla "
 "domanda. REGOLE FERREE: (1) scegli una voce SOLO se la sua risposta soddisfa pienamente la "
 "domanda; (2) se nessuna voce risponde davvero, rispondi 0; (3) NON inventare, NON usare "
 "conoscenza esterna, NON spiegare: scegli solo tra le voci date; (4) in caso di dubbio, 0. "
 "Una scelta SBAGLIATA e' molto peggio di uno 0: preferisci SEMPRE 0 se non sei certo. "
 "Rispondi SOLO JSON: {\"pick\": <numero della voce, oppure 0>}"
)

def ask_groq(cfg, question, cands, retries=4):
    lines = [f"{i+1}) {c['reply']}" for i, c in enumerate(cands)]
    user = f"DOMANDA: {question}\n\nVOCI:\n" + "\n".join(lines) + "\n\nQuale voce risponde? (numero, o 0)"
    body = json.dumps({"model": cfg["model"], "temperature": 0,
                       "response_format": {"type": "json_object"},
                       "messages": [{"role": "system", "content": SYS},
                                    {"role": "user", "content": user}]}).encode("utf-8")
    req = urllib.request.Request(f"{cfg['base']}/chat/completions", data=body,
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {cfg['key']}",
                 "User-Agent": "Mozilla/5.0 (anima-grounded-reader)"})
    for a in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                j = json.loads(r.read().decode("utf-8"))
            c = j.get("choices", [{}])[0].get("message", {}).get("content", "")
            pick = int(json.loads(c).get("pick", 0))
            return pick if 0 <= pick <= len(cands) else 0
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503): time.sleep(1.5 * (a + 1)); continue
            print("  groq HTTP", e.code, e.read()[:120].decode("utf-8", "ignore")); return 0
        except Exception as e:
            time.sleep(1.0 * (a + 1))
    return 0

VSYS = ("Sei un verificatore severo. Ti do una DOMANDA e una RISPOSTA candidata. Stabilisci se la "
        "risposta risponde in modo CORRETTO, DIRETTO e COMPLETO alla domanda. Sii rigoroso: se la "
        "risposta e' su un argomento solo affine, parziale o diverso, e' NO. Rispondi SOLO JSON: "
        "{\"ok\": true|false}")

def verify_groq(cfg, question, reply, retries=3):
    body = json.dumps({"model": cfg["model"], "temperature": 0,
                       "response_format": {"type": "json_object"},
                       "messages": [{"role": "system", "content": VSYS},
                                    {"role": "user", "content": f"DOMANDA: {question}\nRISPOSTA: {reply}\nRisponde davvero?"}]}).encode("utf-8")
    req = urllib.request.Request(f"{cfg['base']}/chat/completions", data=body,
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {cfg['key']}",
                 "User-Agent": "Mozilla/5.0 (anima-grounded-reader)"})
    for a in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                j = json.loads(r.read().decode("utf-8"))
            c = j.get("choices", [{}])[0].get("message", {}).get("content", "")
            return bool(json.loads(c).get("ok", False))
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503): time.sleep(1.5 * (a + 1)); continue
            return False
        except Exception:
            time.sleep(1.0 * (a + 1))
    return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=os.path.join(HERE, "eval_ood.jsonl"))
    ap.add_argument("--k", type=int, default=5, help="candidates retrieved per query")
    ap.add_argument("--model", default=None)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--verify", action="store_true", help="second-pass yes/no confirmation; refuse if not confident")
    ap.add_argument("--fails", action="store_true")
    a = ap.parse_args()

    cfg = load_teacher()
    if a.model: cfg["model"] = a.model
    print(f"[grounded] model={cfg['model']}  K={a.k}")

    table, H, D, NGRAMS, _ = A.load_encoder()
    encode = A.make_encoder(table, H, D, NGRAMS)
    cards, _ = A.load_corpus()
    vecs, owner = [], []
    for ci, c in enumerate(cards):
        for p in A.index_texts(c):
            vecs.append(encode(p)); owner.append(ci)
    V = np.stack(vecs).astype(np.float32)

    def rank(q, k):
        cos = V @ encode(q)
        order = np.argsort(cos)[::-1]
        out, seen = [], set()
        for i in order:
            ci = owner[i]
            if ci in seen: continue
            seen.add(ci); out.append((ci, float(cos[i])))
            if len(out) >= k: break
        return out

    qs = [json.loads(l) for l in open(a.queries, encoding="utf-8") if l.strip() and not l.startswith("//")]
    if a.limit: qs = qs[:a.limit]

    inscope = oos = 0
    correct = wrong = refused = oos_ok = oos_fp = 0
    fails = []
    for n, item in enumerate(qs):
        q, expect = item["q"], item["expect"]
        lang = "en" if item.get("lang") == "en" else "it"
        ranked = rank(q, a.k)
        cands = [{"ci": ci, "id": cards[ci]["id"],
                  "reply": (cards[ci].get("reply", {}).get(lang) or cards[ci].get("reply", {}).get("it")
                            or cards[ci].get("reply", {}).get("en") or "")} for ci, _ in ranked]
        pick = ask_groq(cfg, q, cands)
        chosen = cands[pick - 1] if pick >= 1 else None
        if chosen is not None and a.verify and not verify_groq(cfg, q, chosen["reply"]):
            chosen = None        # second pass not confident -> refuse rather than risk a wrong answer
        if expect == "none":
            oos += 1
            if chosen is None: oos_ok += 1
            else: oos_fp += 1; fails.append(("FALSE+", q, "none", chosen["id"]))
        else:
            inscope += 1
            if chosen is None:
                refused += 1; fails.append(("REFUSED", q, expect, "(0)"))
            elif matches(expect, cards[chosen["ci"]]):
                correct += 1
            else:
                wrong += 1; fails.append(("WRONG", q, expect, chosen["id"]))
        if (n + 1) % 10 == 0: print(f"  ...{n+1}/{len(qs)}", file=sys.stderr)
        time.sleep(0.05)

    pct = lambda x: f"{100*x/inscope:.1f}%" if inscope else "0"
    print(f"\n[grounded] {len(qs)} queries over {len(cards)} cards")
    print(f"  in-scope ({inscope}): CORRECT {correct} ({pct(correct)})  wrong {wrong}  refused {refused}")
    if oos: print(f"  out-of-scope ({oos}): refused(good) {oos_ok}  FALSE-POSITIVE(bad) {oos_fp}")
    if a.fails:
        for f in fails: print(f"    [{f[0]}] {f[1]!r} exp={f[2]} got={f[3]}")

if __name__ == "__main__":
    main()
