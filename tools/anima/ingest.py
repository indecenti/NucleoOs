#!/usr/bin/env python3
"""ANIMA knowledge ingestor: turn a book / document into draft RAG cards (JSONL).

The device can't read a book live (it retrieves, it doesn't generate — see docs/anima.md),
so the book must be compiled, offline, into answer cards. This splits a text into focused
chunks and emits one card per chunk in the standard schema (schemas/anima-card.schema.json).

Two modes:
  default  deterministic, fully offline — the chunk becomes the reply, the section heading
           and first sentence become the `ask` phrasings. Good raw draft; review before use.
  --llm    pipe each chunk through an LLM command that returns better Q&A. The command is
           yours (no provider hard-coded): it receives the prompt on stdin and must print
           JSON {"reply_it","reply_en","ask_it":[...],"ask_en":[...]}.

Usage:
  python tools/anima/ingest.py book.txt --category general --id-prefix atlante --lang it
  python tools/anima/ingest.py notes.md --category programming \\
         --llm "ollama run llama3"            # or any command that reads stdin -> JSON

Output goes to tools/anima/knowledge/<name>.jsonl (review it!), then run build_akb2.py.
PDF input works if `pypdf` is installed; otherwise convert to .txt first.
"""
import argparse, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KDIR = os.path.join(ROOT, "tools", "anima", "knowledge")


def read_text(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".pdf":
        try:
            from pypdf import PdfReader
        except ImportError:
            sys.exit("[ingest] PDF needs pypdf (`pip install pypdf`), or convert to .txt first")
        return "\n\n".join((pg.extract_text() or "") for pg in PdfReader(path).pages)
    return open(path, encoding="utf-8", errors="replace").read()


def chunk_text(text, min_chars, max_chars):
    """Split into focused chunks, remembering the nearest Markdown heading for each."""
    chunks, heading, buf = [], "", []
    def flush():
        if buf:
            body = " ".join(" ".join(buf).split())
            if body: chunks.append((heading, body))
            buf.clear()
    for raw in text.splitlines():
        line = raw.strip()
        m = re.match(r"^#{1,6}\s+(.*)", line)
        if m:                                   # markdown heading -> chunk boundary + label
            flush(); heading = m.group(1).strip().rstrip("#").strip(); continue
        if not line:                            # blank line -> paragraph boundary
            if sum(len(b) for b in buf) >= min_chars: flush()
            continue
        buf.append(line)
        if sum(len(b) for b in buf) >= max_chars: flush()
    flush()
    # split over-long chunks on sentence boundaries
    out = []
    for h, body in chunks:
        if len(body) <= max_chars: out.append((h, body)); continue
        cur = ""
        for sent in re.split(r"(?<=[.!?])\s+", body):
            if len(cur) + len(sent) > max_chars and cur:
                out.append((h, cur.strip())); cur = ""
            cur += sent + " "
        if cur.strip(): out.append((h, cur.strip()))
    return out


def first_sentence(body, n=120):
    s = re.split(r"(?<=[.!?])\s+", body)[0].strip()
    return s[:n].rstrip()


def deterministic_card(heading, body, lang):
    reply = body[:240].rstrip()
    asks = []
    if heading:
        asks.append(heading)
        asks += ([f"cos'e {heading}", f"parlami di {heading}"] if lang == "it"
                 else [f"what is {heading}", f"tell me about {heading}"])
    fs = first_sentence(body)
    if fs and fs.lower() != heading.lower():
        asks.append(fs[:80])
    asks = list(dict.fromkeys(a for a in asks if a)) or [first_sentence(body, 80)]
    return {"it": reply, "en": ""} if lang == "it" else {"it": "", "en": reply}, {lang: asks}


LLM_PROMPT = """You convert a passage into ONE knowledge card for an offline voice assistant.
Return ONLY compact JSON, no prose, with exactly these keys:
{{"reply_it": "...", "reply_en": "...", "ask_it": ["..."], "ask_en": ["..."]}}
- reply_it / reply_en: ONE concise factual sentence (<=220 chars) stating what the passage
  teaches, in Italian and English respectively.
- ask_it / ask_en: 3-5 short, varied questions a user might ask to get that answer, per language.
Passage{ctx}:
\"\"\"{chunk}\"\"\""""


def llm_card(cmd, heading, body):
    ctx = f" (section: {heading})" if heading else ""
    prompt = LLM_PROMPT.format(ctx=ctx, chunk=body[:1500])
    try:
        r = subprocess.run(cmd, shell=True, input=prompt, capture_output=True, text=True, timeout=180)
        out = r.stdout
        m = re.search(r"\{.*\}", out, re.S)
        if not m: raise ValueError("no JSON in LLM output")
        j = json.loads(m.group(0))
        reply = {"it": (j.get("reply_it") or "").strip(), "en": (j.get("reply_en") or "").strip()}
        ask = {}
        if j.get("ask_it"): ask["it"] = [s.strip() for s in j["ask_it"] if s.strip()]
        if j.get("ask_en"): ask["en"] = [s.strip() for s in j["ask_en"] if s.strip()]
        if (reply["it"] or reply["en"]) and (ask.get("it") or ask.get("en")):
            return reply, ask
        raise ValueError("LLM JSON missing reply/ask")
    except Exception as e:
        print(f"   ! LLM chunk failed ({e}); using deterministic draft", file=sys.stderr)
        return None


def main():
    ap = argparse.ArgumentParser(description="Ingest a document into ANIMA knowledge cards (JSONL).")
    ap.add_argument("input", help="source .txt/.md (.pdf if pypdf installed)")
    ap.add_argument("--category", default="general", help="card category (general/programming/...)")
    ap.add_argument("--id-prefix", default=None, help="id prefix (default: file stem)")
    ap.add_argument("--lang", choices=["it", "en"], default="it", help="language of the source text")
    ap.add_argument("--out", default=None, help="output JSONL (default: knowledge/<stem>.jsonl)")
    ap.add_argument("--llm", default=None, help="shell command: reads prompt on stdin, prints JSON")
    ap.add_argument("--min-chars", type=int, default=120)
    ap.add_argument("--max-chars", type=int, default=480)
    ap.add_argument("--max-chunks", type=int, default=0, help="cap number of cards (0 = no cap)")
    ap.add_argument("--append", action="store_true", help="append to the output file instead of refusing")
    ap.add_argument("--dry-run", action="store_true", help="print cards, do not write")
    a = ap.parse_args()

    stem = re.sub(r"[^a-z0-9]+", "-", os.path.splitext(os.path.basename(a.input))[0].lower()).strip("-")
    prefix = a.id_prefix or stem or "doc"
    out = a.out or os.path.join(KDIR, f"{stem or 'ingest'}.jsonl")

    chunks = chunk_text(read_text(a.input), a.min_chars, a.max_chars)
    if a.max_chunks: chunks = chunks[:a.max_chunks]
    if not chunks: sys.exit("[ingest] no usable text found")

    cards = []
    for i, (heading, body) in enumerate(chunks):
        reply = ask = None
        if a.llm:
            res = llm_card(a.llm, heading, body)
            if res: reply, ask = res
        if reply is None:
            reply, ask = deterministic_card(heading, body, a.lang)
        reply = {k: v for k, v in reply.items() if v}      # drop empty language fields
        src = f"book:{os.path.basename(a.input)}" + (f"#{heading}" if heading else f"#chunk{i}")
        cards.append({"id": f"{prefix}.{i:04d}", "category": a.category,
                      "action": "answer", "arg": "", "reply": reply, "ask": ask, "source": src})

    if a.dry_run:
        for c in cards: print(json.dumps(c, ensure_ascii=False))
        print(f"\n[ingest] {len(cards)} draft cards (dry-run, nothing written)", file=sys.stderr)
        return

    if os.path.exists(out) and not a.append:
        sys.exit(f"[ingest] {out} exists — use --append, --out <other>, or remove it first")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "a" if a.append else "w", encoding="utf-8") as f:
        for c in cards: f.write(json.dumps(c, ensure_ascii=False) + "\n")
    print(f"[ingest] wrote {len(cards)} cards -> {out}")
    print(f"[ingest] mode: {'LLM (' + a.llm + ')' if a.llm else 'deterministic'} | lang={a.lang} | category={a.category}")
    print("[ingest] NEXT: review the cards, then  python tools/anima/build_akb2.py  and deploy the SD.")


if __name__ == "__main__":
    main()
