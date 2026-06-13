#!/usr/bin/env python3
"""ANIMA card SEEDER — draft NEW knowledge cards from a curated topic list, with a local LLM.

Unlike generate.py (which only ENRICHES existing cards' asks, never the facts), this DRAFTS
whole new cards — including the reply, which is a FACT the model could get wrong. So output goes
to tools/anima/drafts/ (NOT knowledge/), flagged source="llm-draft", and MUST be fact-checked
by a human before being promoted into a real pack. The build never sees drafts/.

Topics: tools/anima/seed_topics.jsonl — {"id","category","it","en"} (a clear, unambiguous name
anchors the model and minimizes drift/hallucination).

  python tools/anima/seed.py --model qwen2.5-coder:7b
"""
import os, sys, json, time, argparse, urllib.request

OLLAMA = os.environ.get("OLLAMA_HOST", "http://localhost:11434") + "/api/generate"
HERE = os.path.dirname(os.path.abspath(__file__))
DRAFTS = os.path.join(HERE, "drafts")

PROMPT = """Sei un curatore di enciclopedia per un assistente OFFLINE su microcontrollore.
Scrivi UNA card di conoscenza sull'argomento: "{it}" (in inglese: "{en}"), categoria {cat}.

Genera SOLO JSON valido (niente altro testo):
{{"reply": {{"it": "1 frase FATTUALE, precisa e neutra che definisce l'argomento", "en": "1 factual precise sentence"}},
  "detail": {{"it": "1-2 frasi piu profonde con un esempio o dato concreto", "en": "1-2 deeper sentences with a concrete example"}},
  "ask": {{"it": ["10-12 modi diversi in cui un utente reale chiederebbe questo: meta diretti, meta descrizioni indirette che non nominano il termine"],
           "en": ["10-12 varied ways in English"]}}}}

Regole TASSATIVE:
- ACCURATEZZA ASSOLUTA. Se non sei sicuro di una data/numero, NON inventarlo: resta generale.
- reply neutro e conciso (no opinioni, no "secondo me"). detail con un esempio concreto.
- ask con vocabolario VARIO (colloquiale e tecnico, sinonimi, forme indirette).
- Solo JSON, nessun markdown, nessun commento."""


def ollama(model, prompt, timeout):
    req = urllib.request.Request(OLLAMA, headers={"Content-Type": "application/json"},
        data=json.dumps({"model": model, "prompt": prompt, "stream": False, "format": "json",
                         "options": {"temperature": 0.3, "num_ctx": 2048}}).encode())
    return json.loads(json.load(urllib.request.urlopen(req, timeout=timeout))["response"])


def as_str(x):
    if isinstance(x, str): return x
    if isinstance(x, list): return " ".join(as_str(i) for i in x)
    if isinstance(x, dict): return " ".join(as_str(v) for v in x.values())
    return "" if x is None else str(x)


def as_list(x):
    if isinstance(x, list): return [as_str(i).strip() for i in x if as_str(i).strip()]
    return [as_str(x).strip()] if as_str(x).strip() else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="qwen2.5-coder:7b")
    ap.add_argument("--topics", default=os.path.join(HERE, "seed_topics.jsonl"))
    ap.add_argument("--timeout", type=float, default=180)
    a = ap.parse_args()
    os.makedirs(DRAFTS, exist_ok=True)

    topics = [json.loads(l) for l in open(a.topics, encoding="utf-8") if l.strip() and not l.startswith("//")]
    # resume: skip ids already drafted
    have = set()
    for p in (os.path.join(DRAFTS, f) for f in os.listdir(DRAFTS) if f.endswith(".jsonl")):
        for l in open(p, encoding="utf-8"):
            try: have.add(json.loads(l)["id"])
            except Exception: pass

    done = 0
    for t in topics:
        cid = t["id"]
        if cid in have:
            continue
        try:
            t0 = time.time()
            g = ollama(a.model, PROMPT.format(it=t["it"], en=t["en"], cat=t["category"]), a.timeout)
        except Exception as e:
            print(f"  ! {cid}: LLM error ({e})", flush=True); continue
        rep = g.get("reply") if isinstance(g.get("reply"), dict) else {}
        det = g.get("detail") if isinstance(g.get("detail"), dict) else {}
        ask = g.get("ask") if isinstance(g.get("ask"), dict) else {}
        rit, ren = as_str(rep.get("it")).strip(), as_str(rep.get("en")).strip()
        if not rit and not ren:
            print(f"  ! {cid}: empty reply, skipped", flush=True); continue
        card = {"id": cid, "category": t["category"], "action": "answer", "arg": "",
                "reply": {"it": rit or ren, "en": ren or rit},
                "detail": {"it": as_str(det.get("it")).strip(), "en": as_str(det.get("en")).strip()},
                "ask": {"it": as_list(ask.get("it")), "en": as_list(ask.get("en"))},
                "source": "llm-draft:" + a.model}
        outp = os.path.join(DRAFTS, t["category"] + ".jsonl")
        with open(outp, "a", encoding="utf-8") as f:
            f.write(json.dumps(card, ensure_ascii=False) + "\n")
        done += 1
        print(f"  + {cid}: reply «{(rit or ren)[:50]}» ask {len(card['ask']['it'])}/{len(card['ask']['en'])} ({time.time()-t0:.0f}s)", flush=True)
    print(f"[seed] drafted {done} new card(s) into {DRAFTS}/ — REVIEW before promoting", flush=True)


if __name__ == "__main__":
    main()
