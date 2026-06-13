"""
Training runner — feed ANIMA a list of Wikipedia-style knowledge questions and see which it ANSWERS
(grounded from its wiki-derived corpus) vs which it leaves EMPTY (abstains). The empty ones are the
training targets: topics to add to the knowledge corpus.

Reads a list of requests from --file and sends each to GET /api/anima on a flashed device, ONE at a
time with a fixed cadence (so the PSRAM-less single-task server keeps up). Read-only: it does not pair,
so no side-effecting tool can fire — it only asks questions.

Input formats accepted by --file (auto-detected):
  • JSON array of objects:   [{"q": "chi è Dante", "lang": "it"}, ...]
  • JSON array of strings:    ["chi è Dante", "what is photosynthesis"]
  • JSONL (one object/line) or plain text (one question per line; // comments skipped)

  python tools/train_wiki.py --file requests.json [--url http://192.168.0.166] [--delay 8] [--mode off]
  python tools/train_wiki.py --example tools/anima/train_requests.example.json   # write a sample & exit

Outputs (in tools/test-lab/reports/): train-<stamp>.jsonl (full per-query log) and
train-gaps-<stamp>.json (just the unanswered questions = what to train next). Exit 0 always (data run),
SKIP if device unreachable.
"""

import sys
import os
import json
import time
import argparse
import urllib.request
import urllib.parse
import urllib.error
from datetime import datetime

DELAY_FLOOR = 3
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
REPORTS = os.path.join(HERE, "test-lab", "reports")

# A ready-to-run sample: varied Wikipedia-style knowledge questions, IT + EN. Used by --example and by
# the cockpit's "Scarica esempio" button. Keep questions short (<150 chars) and side-effect free.
# Ogni caso punta a una VOCE Wikipedia reale: "subject" = titolo canonico (IT) = chiave per ingerire
# l'articolo; "q" = domanda che contiene il nome completo, nelle forme delle ask-phrasing del corpus.
EXAMPLE = [
    {"q": "chi è Dante Alighieri", "lang": "it", "subject": "Dante Alighieri"},
    {"q": "chi era Marie Curie", "lang": "it", "subject": "Marie Curie"},
    {"q": "chi era Leonardo da Vinci", "lang": "it", "subject": "Leonardo da Vinci"},
    {"q": "cos'è la fotosintesi clorofilliana", "lang": "it", "subject": "Fotosintesi clorofilliana"},
    {"q": "parlami della Seconda guerra mondiale", "lang": "it", "subject": "Seconda guerra mondiale"},
    {"q": "cos'è la relatività generale", "lang": "it", "subject": "Relatività generale"},
    {"q": "spiegami la Torre Eiffel", "lang": "it", "subject": "Torre Eiffel"},
    {"q": "cos'è il buco nero", "lang": "it", "subject": "Buco nero"},
    {"q": "chi era Alan Turing", "lang": "it", "subject": "Alan Turing"},
    {"q": "cos'è la Rivoluzione francese", "lang": "it", "subject": "Rivoluzione francese"},
    {"q": "chi è Galileo Galilei", "lang": "it", "subject": "Galileo Galilei"},
    {"q": "parlami del Colosseo", "lang": "it", "subject": "Colosseo"},
    {"q": "who was Albert Einstein", "lang": "en", "subject": "Albert Einstein"},
    {"q": "who was Isaac Newton", "lang": "en", "subject": "Isaac Newton"},
    {"q": "what is photosynthesis", "lang": "en", "subject": "Fotosintesi"},
    {"q": "what is the theory of relativity", "lang": "en", "subject": "Relatività"},
    {"q": "who was Leonardo da Vinci", "lang": "en", "subject": "Leonardo da Vinci"},
    {"q": "what is DNA", "lang": "en", "subject": "DNA"},
    {"q": "who was Marie Curie", "lang": "en", "subject": "Marie Curie"},
    {"q": "what is the French Revolution", "lang": "en", "subject": "Rivoluzione francese"},
    {"q": "who was Nikola Tesla", "lang": "en", "subject": "Nikola Tesla"},
    {"q": "what is a black hole", "lang": "en", "subject": "Buco nero"},
    {"q": "who was Charles Darwin", "lang": "en", "subject": "Charles Darwin"},
    {"q": "what is the Roman Empire", "lang": "en", "subject": "Impero romano"},
    {"q": "who was Galileo Galilei", "lang": "en", "subject": "Galileo Galilei"},
]

# Prompt template the user copies into an LLM to mass-generate NEW cases in the same format.
# Reusable: run it again and again; {count} is filled from the UI box. The cockpit also appends the
# questions already in the list as a "do not repeat" block, so each batch stays fresh.
GEN_PROMPT = """Sei un generatore di dati di ADDESTRAMENTO per ANIMA, un assistente di conoscenza OFFLINE.
La FONTE della conoscenza di ANIMA è WIKIPEDIA: ogni risposta nasce dall'abstract di una voce Wikipedia reale. Genera quindi esattamente {count} richieste che puntino a VOCI WIKIPEDIA ESISTENTI, così che ogni "buco" trovato si possa colmare ingerendo quell'articolo.

REGOLE DI CONTENUTO:
- Ogni elemento riguarda UN soggetto che ha una PROPRIA voce su Wikipedia in ITALIANO: persone reali, luoghi, opere, eventi storici, concetti scientifici/tecnici, animali, ecc. Niente soggetti inventati, vaghi o composti ("la capitale del Giappone" NO → usa "Tokyo").
- Usa il NOME COMPLETO e CANONICO, esatto come Wikipedia titola la voce: persone con nome e cognome ("Dante Alighieri", non "Dante"; "Marie Curie"; "Leonardo da Vinci"); luoghi/opere/eventi col nome ufficiale ("Seconda guerra mondiale", "Torre Eiffel", "Impero romano"). Se serve la disambiguazione, mettila come Wikipedia ("Condensatore (elettrotecnica)").
- "subject" = il TITOLO ESATTO della voce di Wikipedia in italiano (è la chiave per ingerire l'articolo); anche per le domande in inglese il subject resta il titolo ITALIANO.
- "q" = la domanda in linguaggio naturale che CONTIENE il nome canonico completo, nelle forme tipiche del corpus: "chi è <Nome completo>", "chi era <Nome completo>", "cos'è <Titolo>", "che cos'è <Titolo>", "parlami di <Titolo>", "spiegami <Titolo>" (in inglese: "who is/was <Name>", "what is <Title>").
- Mescola italiano ("lang":"it") e inglese ("lang":"en"), circa metà e metà.
- Varia MOLTO i soggetti: epoche, discipline, paesi, ambiti. SOLO domande di conoscenza: niente comandi/azioni, niente calcoli, niente date-evento sciolte.

NON RIPETERE (importante):
- Non riproporre soggetti già generati in risposte PRECEDENTI di questa conversazione.
- Se ti viene fornito un elenco "DA NON USARE", non generare nessun soggetto uguale o equivalente a quelli.
- Ogni elemento del batch deve avere un "subject" DIVERSO da tutti gli altri.

FORMATO DI USCITA (tassativo):
- Restituisci SOLO un array JSON valido, nient'altro: niente spiegazioni, niente markdown, niente testo prima o dopo.
- Ogni elemento esattamente: {{"q": "<domanda con nome canonico>", "lang": "it"|"en", "subject": "<Titolo esatto voce Wikipedia IT>"}}.

Esempio:
[{{"q": "chi è Dante Alighieri", "lang": "it", "subject": "Dante Alighieri"}}, {{"q": "what is photosynthesis", "lang": "en", "subject": "Fotosintesi"}}, {{"q": "parlami della Seconda guerra mondiale", "lang": "it", "subject": "Seconda guerra mondiale"}}]"""


def get(url, timeout=30.0):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        try: return e.code, e.read().decode("utf-8", "replace")
        except Exception: return e.code, ""
    except Exception as e:
        return -1, str(e)


def is_answered(resp):
    """A question is ANSWERED when the device returned a non-empty reply with a real tier."""
    if not resp:
        return False
    reply = (resp.get("reply") or "").strip()
    tier = (resp.get("tier") or "").lower()
    return bool(reply) and tier not in ("none", "", "abstain")


def load_requests(path, max_len):
    """Parse the request file (JSON array / JSONL / plain lines), de-dupe, skip over-long queries."""
    raw = open(path, encoding="utf-8").read().strip()
    items, seen, too_long = [], set(), 0

    def add(q, lang, subject=""):
        nonlocal too_long
        q = (q or "").strip()
        if not q:
            return
        lang = "en" if str(lang or "it").lower().startswith("en") else "it"
        if len(q.encode("utf-8")) > max_len:
            too_long += 1
            return
        key = (q, lang)
        if key not in seen:
            seen.add(key)
            items.append({"q": q, "lang": lang, "subject": (subject or "").strip()})

    parsed = None
    try:
        parsed = json.loads(raw)
    except Exception:
        parsed = None
    if isinstance(parsed, list):
        for o in parsed:
            if isinstance(o, dict):
                add(o.get("q"), o.get("lang", "it"), o.get("subject", ""))
            elif isinstance(o, str):
                add(o, "it")
    else:
        for line in raw.splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            if line.startswith("{"):
                try:
                    o = json.loads(line); add(o.get("q"), o.get("lang", "it"), o.get("subject", "")); continue
                except Exception:
                    pass
            add(line, "it")
    return items, too_long


def main() -> int:
    ap = argparse.ArgumentParser(description="ANIMA wiki-knowledge training runner")
    ap.add_argument("--file", help="file with the requests (JSON array / JSONL / plain text)")
    ap.add_argument("--example", help="write the sample request file to this path and exit")
    ap.add_argument("--print-prompt", action="store_true", help="print the LLM generator prompt and exit")
    ap.add_argument("--count", type=int, default=200, help="quante richieste chiedere nel prompt generatore")
    ap.add_argument("--url", default="http://192.168.0.166", help="device base URL")
    ap.add_argument("--delay", type=float, default=8.0, help="cadence between request-starts (s, floor 3)")
    ap.add_argument("--mode", default="off", choices=["off", "on", "only"],
                    help="off = corpus wiki offline; on = ibrida (può andare online); only = solo online")
    ap.add_argument("--max-len", type=int, default=159, help="skip queries longer than this")
    ap.add_argument("--max-fails", type=int, default=5, help="abort after N consecutive connection failures")
    args = ap.parse_args()

    if args.print_prompt:
        print(GEN_PROMPT.format(count=max(1, args.count)))
        return 0
    if args.example:
        with open(args.example, "w", encoding="utf-8") as f:
            json.dump(EXAMPLE, f, ensure_ascii=False, indent=2)
        print(f"[train] esempio scritto: {args.example} ({len(EXAMPLE)} casi)")
        return 0
    if not args.file:
        print("[train] SKIP — nessun --file di richieste"); return 0

    base = args.url.rstrip("/")
    delay = max(DELAY_FLOOR, args.delay)

    st, _ = get(base + "/api/status", timeout=8.0)
    if st != 200:
        print(f"[train] SKIP — {base}/api/status irraggiungibile (nessun device in questo run)")
        return 0
    try:
        reqs, too_long = load_requests(args.file, args.max_len)
    except OSError as e:
        print(f"[train] SKIP — non riesco a leggere {args.file}: {e}"); return 0
    if not reqs:
        print("[train] SKIP — nessuna richiesta valida nel file"); return 0

    print(f"[train] target {base} | {len(reqs)} richieste (mode={args.mode}, cadenza {delay:.0f}s) "
          f"| {too_long} saltate (troppo lunghe) | ETA ~{len(reqs)*delay/60:.0f} min")
    print("[train] non-associato → READ-ONLY (nessuna azione eseguita, solo domande)")

    os.makedirs(REPORTS, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    detail_path = os.path.join(REPORTS, f"train-{stamp}.jsonl")
    detail = open(detail_path, "w", encoding="utf-8")

    answered = crashed = errors = 0
    consec_fail = 0
    tiers, gaps = {}, []

    for i, item in enumerate(reqs):
        url = f"{base}/api/anima?q={urllib.parse.quote(item['q'])}&lang={item['lang']}&mode={args.mode}"
        c0 = time.time()
        code, body = get(url, timeout=30.0)
        req_ms = int((time.time() - c0) * 1000)
        rec = {"i": i, "q": item["q"], "lang": item["lang"], "subject": item.get("subject", ""), "http": code}
        ans = False
        if code == 200:
            consec_fail = 0
            try: resp = json.loads(body)
            except Exception: resp = None
            ans = is_answered(resp)
            if resp:
                tier = resp.get("tier") or "none"
                tiers[tier] = tiers.get(tier, 0) + 1
                rec.update(tier=tier, answered=ans, ms=resp.get("ms"),
                           reply=(resp.get("reply") or "")[:300])
            if ans:
                answered += 1
            else:
                gaps.append({"q": item["q"], "lang": item["lang"], "subject": item.get("subject", "")})
        elif code == -1:
            crashed += 1; consec_fail += 1; rec["error"] = "connection dropped"
        else:
            errors += 1; consec_fail += 1; rec["error"] = f"HTTP {code}"
        detail.write(json.dumps(rec, ensure_ascii=False) + "\n"); detail.flush()

        mark = "ok " if code == 200 else ("CRASH" if code == -1 else f"H{code}")
        ms = rec.get("ms"); ms = ms if isinstance(ms, (int, float)) else req_ms
        verdict = "→ risposta" if ans else ("→ —" if code == 200 else "→ errore")
        print(f"[q] {i+1}/{len(reqs)} {mark} {item['lang']} {rec.get('tier','-'):>7} {ms:>4}ms · "
              f"{item['q'][:42]} {verdict}", flush=True)

        if consec_fail >= args.max_fails:
            print(f"[train] ABORT — {consec_fail} fallimenti di connessione consecutivi (device giù)")
            detail.close(); print("[train] FAILED"); return 1
        if i + 1 < len(reqs):
            remaining = delay - (time.time() - c0)
            if remaining > 0:
                time.sleep(remaining)

    detail.close()
    gaps_path = os.path.join(REPORTS, f"train-gaps-{stamp}.json")
    with open(gaps_path, "w", encoding="utf-8") as f:
        json.dump(gaps, f, ensure_ascii=False, indent=2)

    top = ", ".join(f"{k}:{v}" for k, v in sorted(tiers.items(), key=lambda kv: -kv[1]))
    rate = (answered / len(reqs) * 100) if reqs else 0
    print(f"[train] {len(reqs)} richieste | RISPOSTE {answered} ({rate:.0f}%) · "
          f"BUCHI {len(gaps)} · crash {crashed} · err {errors}")
    print(f"[train] tier: {top or '-'}")
    subj_gaps = [g["subject"] for g in gaps if g.get("subject")]
    if subj_gaps:
        print(f"[train] titoli Wikipedia da ingerire ({len(subj_gaps)}): " + ", ".join(subj_gaps[:8])
              + (" …" if len(subj_gaps) > 8 else ""))
    print(f"[train] buchi → {os.path.relpath(gaps_path, REPO)} (campo subject = titolo da dare a ingest_wiki)  |  "
          f"log → {os.path.relpath(detail_path, REPO)}")
    if crashed:
        print(f"[train] FAIL — {crashed} connessioni cadute (device instabile sotto carico)")
        return 1
    print("[train] PASS — esecuzione completata")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[train] interrotto"); sys.exit(1)
