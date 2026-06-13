#!/usr/bin/env python3
"""ANIMA knowledge factory — enrich cards with a LOCAL LLM (Ollama), offline, on the PC.

This is the design's "factory": a big model runs on the PC's GPU and produces the frozen text
the device retrieves. The LLM NEVER runs on the ESP32 — the device only does int8 cosine over
the SD index. Here the LLM ADDS, per card:
  - extra `ask` phrasings with VARIED vocabulary (synonyms, colloquial + technical) — the lever
    measured to lift out-of-distribution recall, since the char-ngram encoder matches surface words;
  - a `detail` (deeper explanation + concrete example) if the card lacks one.
It deliberately does NOT rewrite `reply` (hand-verified, terse-but-correct) — enrichment is purely
ADDITIVE, so a slightly-off phrasing only adds a retrieval key, it can't corrupt a fact.

Output cards are flagged source-wise and MUST be reviewed (LLMs can err). Then rebuild + eval:
  python tools/anima/generate.py --file tools/anima/knowledge/c-lang.jsonl --model qwen3-coder:30b
  python tools/anima/build_akb2.py && python tools/anima/eval.py --queries tools/anima/eval_ood.jsonl

Talks to Ollama's HTTP API (localhost:11434) — no extra Python deps. Pick any installed model
(`ollama list`); a coder model is great for the C/ESP/electronics packs.
"""
import os, sys, json, glob, time, argparse, urllib.request

OLLAMA = os.environ.get("OLLAMA_HOST", "http://localhost:11434") + "/api/generate"

PROMPT = """Sei un generatore di "knowledge card" per un assistente OFFLINE su microcontrollore.
Il dispositivo fa SOLO retrieval (coseno) sulle frasi-esempio: NON genera testo a runtime.

Card esistente (NON cambiare il significato del reply):
  argomento/categoria: {category}
  reply IT: {rit}
  reply EN: {ren}

Genera SOLO JSON valido con questo schema (niente altro testo):
{{"detail": {{"it": "1-2 frasi piu profonde con un esempio concreto", "en": "1-2 deeper sentences with a concrete example"}},
  "ask": {{"it": ["10-12 modi DIVERSI in cui un utente reale chiederebbe questa cosa"],
           "en": ["10-12 varied ways in English"]}}}}

Regole TASSATIVE:
- META' delle 'ask' DEVONO essere DESCRIZIONI INDIRETTE/FUNZIONALI che NON nominano mai il
  termine tecnico: descrivi COSA FA o A COSA SERVE, come lo direbbe chi non conosce la parola.
  Es. per 'diode': "il componente che fa passare corrente in un solo verso", "quel pezzo che
  blocca la corrente al contrario". Es. per 'const': "come impedisco a un valore di cambiare
  dopo averlo impostato", "rendere un dato fisso e non modificabile".
- L'ALTRA META': vocabolario vario diretto — colloquiale E tecnico, sinonimi del termine.
  Es. per 'array': lista, sequenza, insieme di valori, fila di dati.
- Mescola IT ed EN con la stessa logica. Accuratezza tecnica assoluta; coerenti col reply dato.
  Niente domande generiche, niente fuori tema, niente ripetere il termine in ogni frase.
- Solo JSON, nessun markdown, nessun commento."""


def ollama(model, prompt, timeout):
    req = urllib.request.Request(OLLAMA, headers={"Content-Type": "application/json"},
        data=json.dumps({"model": model, "prompt": prompt, "stream": False, "format": "json",
                         "options": {"temperature": 0.4, "num_ctx": 2048}}).encode())
    return json.loads(json.load(urllib.request.urlopen(req, timeout=timeout))["response"])


def as_str(x):
    """Coerce any LLM-returned value to a string. A small model sometimes emits an `ask`
    item or a `detail` as a list of sentences or a nested dict instead of a plain string;
    join those instead of crashing on .strip() (the bug that silently killed whole packs)."""
    if isinstance(x, str):  return x
    if isinstance(x, list): return " ".join(as_str(i) for i in x)
    if isinstance(x, dict): return " ".join(as_str(v) for v in x.values())
    return "" if x is None else str(x)


def as_list(x):
    """Coerce an LLM `ask.it/en` to a list of items (it may arrive as a bare string)."""
    if isinstance(x, list): return x
    if x in (None, ""):     return []
    return [x]


def merge_list(old, new, cap):
    """Append new strings to old, dedup case-insensitively, keep order, cap length.
    Robust to non-string items in either side (coerced via as_str)."""
    seen, out = set(), []
    for s in list(old) + list(new):
        s = as_str(s).strip()
        if s and s.lower() not in seen:
            seen.add(s.lower()); out.append(s)
    return out[:cap]


def load_global_asks(kdir):
    """Map every existing ask (lowercased) -> the card id that owns it, across ALL packs.
    A small model sometimes emits a phrasing that belongs to a DIFFERENT card ("what is the
    capital of Russia" on the Japan card). This lets us reject those at the source, so a
    cross-card duplicate (the only reliable contamination signal) can never enter the corpus."""
    owner = {}
    for p in glob.glob(os.path.join(kdir, "*.jsonl")):
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            cid = d.get("id"); ask = d.get("ask", {})
            if not cid or not isinstance(ask, dict):
                continue
            for lang in ("it", "en"):
                for s in ask.get(lang, []):
                    k = as_str(s).strip().lower()
                    if k:
                        owner.setdefault(k, cid)
    return owner


def guard_foreign(new_asks, card_id, owner):
    """Drop asks already owned by another card; register the rest to card_id. Returns (kept, dropped)."""
    kept, dropped = [], 0
    for s in new_asks:
        k = as_str(s).strip().lower()
        if not k:
            continue
        own = owner.get(k)
        if own is not None and own != card_id:
            dropped += 1            # belongs to another card -> contamination, skip it
            continue
        owner[k] = card_id; kept.append(s)
    return kept, dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", help="a specific knowledge/*.jsonl (default: all)")
    ap.add_argument("--model", default="qwen3-coder:30b")
    ap.add_argument("--category", help="only cards of this category")
    ap.add_argument("--limit", type=int, default=0, help="max cards to enrich (0 = all)")
    ap.add_argument("--max-ask", type=int, default=14, help="cap ask phrasings per language")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    kdir = os.path.join(os.path.dirname(__file__), "knowledge")
    files = [a.file] if a.file else sorted(glob.glob(os.path.join(kdir, "*.jsonl")))
    owner = load_global_asks(kdir)        # anti-contamination guard: ask -> owning card (all packs)
    done = 0; foreign = 0
    for path in files:
        lines = open(path, encoding="utf-8").read().splitlines()
        changed = False
        for i, line in enumerate(lines):
            s = line.strip()
            if not s or s.startswith("//"):
                continue
            c = json.loads(s)
            if c.get("action", "answer") != "answer":      # only knowledge cards carry reply/detail
                continue
            if a.category and c.get("category") != a.category:
                continue
            if a.limit and done >= a.limit:
                break
            rep = c.get("reply", {})
            try:
                t = time.time()
                g = ollama(a.model, PROMPT.format(category=c.get("category", ""),
                           rit=rep.get("it", ""), ren=rep.get("en", "")), a.timeout)
            except Exception as e:
                print(f"  ! {c['id']}: LLM error ({e})"); continue
            ask = c.setdefault("ask", {})
            n_it, n_en = len(ask.get("it", [])), len(ask.get("en", []))
            gask = g.get("ask") if isinstance(g.get("ask"), dict) else {}
            # reject any generated phrasing that already belongs to ANOTHER card (contamination)
            new_it, d_it = guard_foreign(as_list(gask.get("it")), c["id"], owner)
            new_en, d_en = guard_foreign(as_list(gask.get("en")), c["id"], owner)
            foreign += d_it + d_en
            ask["it"] = merge_list(ask.get("it", []), new_it, a.max_ask)
            ask["en"] = merge_list(ask.get("en", []), new_en, a.max_ask)
            det = c.get("detail") or {}
            gd = g.get("detail") if isinstance(g.get("detail"), dict) else {}
            if not det.get("it") and gd.get("it"):
                det["it"] = as_str(gd["it"]).strip()
            if not det.get("en") and gd.get("en"):
                det["en"] = as_str(gd["en"]).strip()
            if det:
                c["detail"] = det
            src = c.get("source", "")
            if "llm:" not in src:
                c["source"] = (src + "+" if src else "") + "llm-enriched:" + a.model
            lines[i] = json.dumps(c, ensure_ascii=False)
            changed = True; done += 1
            if not a.dry_run:                       # write after EVERY card: observable + crash-safe
                open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
            print(f"  + {c['id']}: ask {n_it}/{n_en} -> {len(ask['it'])}/{len(ask['en'])}  detail={'y' if c.get('detail',{}).get('it') else 'n'}  ({time.time()-t:.0f}s)", flush=True)
        if changed and not a.dry_run:
            print(f"[generate] {path} updated")
        elif changed:
            print(f"[generate] (dry-run) would update {path}")
    print(f"[generate] enriched {done} card(s) with {a.model}  (rejected {foreign} cross-card/foreign phrasings)")


if __name__ == "__main__":
    main()
