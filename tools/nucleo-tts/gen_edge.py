#!/usr/bin/env python3
# gen_edge.py — generatore clip voce con EDGE-TTS (voci neurali Microsoft, gratis, qualita' alta).
# Online SOLO durante la generazione sul PC; le clip restano offline sul device. Async + concorrenza
# per reggere decine di migliaia di clip in tempi ragionevoli.
#
# Qualita' massima per la CONCATENAZIONE:
#   - 24 kHz mono 16-bit (nativo Edge, niente downsample lossy a 16k);
#   - TRIM del silenzio iniziale/finale di ogni clip (silenceremove ai due bordi): cosi' le parole
#     si attaccano senza buchi e le pause le decide il planner;
#   - loudnorm: volume uniforme tra clip diverse (niente parole piu' forti/piu' deboli).
#
# Usa il vocabolario di build_voice.py (pacchetto numeri/lettere + lexicon + dizionario).
# Esempi:
#   python gen_edge.py --langs it,en                         # tutto (pacchetto+lexicon+dizionario)
#   python gen_edge.py --langs it,en --no-dict               # solo pacchetto+frasi (veloce)
#   python gen_edge.py --langs it --limit 500 --skip-existing
import asyncio, os, sys, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_voice import mandatory, read_lines   # vocabolario condiviso

import edge_tts

RATE = 24000
VOICES = {"it": "it-IT-ElsaNeural", "en": "en-US-AriaNeural"}
DICT   = {"it": "dict-it-en.tsv",   "en": "dict-en-it.tsv"}

# Trim silenzio ai due bordi (reverse trick) + loudnorm. -50dB = togli solo vero silenzio, lascia 20ms.
def af_chain(fast):
    trim = ("silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.02,areverse,"
            "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.02,areverse")
    return trim if fast else trim + ",loudnorm=I=-16:TP=-1.5:LRA=11"

async def ffmpeg_norm(src, dst, rate, fast):
    p = await asyncio.create_subprocess_exec(
        "ffmpeg", "-y", "-i", src, "-ar", str(rate), "-ac", "1", "-sample_fmt", "s16",
        "-af", af_chain(fast), dst,
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
    return await p.wait()

async def synth_clip(text, voice, dst, rate=RATE, fast=False, retries=2):
    """Sintetizza `text` con Edge-TTS -> mp3 temp -> ffmpeg (trim+norm) -> `dst` (wav). True se ok."""
    mp3 = dst + ".tmp.mp3"
    for attempt in range(retries + 1):
        try:
            # TIMEOUT per-richiesta: senza, una connessione Edge appesa (throttle/rate-limit) blocca il
            # worker PER SEMPRE (era lo stallo a meta' EN). 30s -> fallisce, ritenta, e il giro prosegue.
            await asyncio.wait_for(edge_tts.Communicate(text, voice).save(mp3), timeout=30)
            break
        except Exception as e:
            if attempt == retries:
                print("  ! edge fail '%s': %s" % (text, e), flush=True)
                return False
            await asyncio.sleep(1.0 * (attempt + 1))
    ok = (await ffmpeg_norm(mp3, dst, rate, fast)) == 0
    try: os.remove(mp3)
    except OSError: pass
    return ok

async def run_lang(lang, voice, entries, outdir, conc, rate, fast, skip):
    os.makedirs(outdir, exist_ok=True)
    items = sorted(entries.items())
    total = len(items)
    st = {"ok": 0, "skip": 0, "err": 0}
    idx = 0
    lock = asyncio.Lock()

    async def worker():
        nonlocal idx
        while True:
            async with lock:
                if idx >= total: return
                slug, text = items[idx]; idx += 1
            dst = os.path.join(outdir, slug + ".wav")
            if skip and os.path.exists(dst) and os.path.getsize(dst) > 44:
                st["skip"] += 1
            else:
                st["ok" if await synth_clip(text, voice, dst, rate, fast) else "err"] += 1
            n = st["ok"] + st["skip"] + st["err"]
            if n % 200 == 0:
                print("  [%s] %d/%d (ok %d skip %d err %d)" % (lang, n, total, st["ok"], st["skip"], st["err"]), flush=True)

    print("[%s] %d clip, voce %s -> %s" % (lang, total, voice, outdir), flush=True)
    await asyncio.gather(*[worker() for _ in range(conc)])
    print("[%s] FATTO: ok %d, gia' presenti %d, errori %d -> %s" % (lang, st["ok"], st["skip"], st["err"], outdir), flush=True)
    return st

def build_entries(lang, here, dict_dir, no_dict, limit):
    entries = mandatory(lang)
    entries.update(read_lines(os.path.join(here, "lexicon.%s.txt" % lang), col0_only=False))
    # lessico autorato dal workflow tts-coverage (grounded sul parlato reale, verificato)
    entries.update(read_lines(os.path.join(here, "lexicon.wf.%s.txt" % lang), col0_only=False))
    # flywheel: parole raccolte dai MISS reali del device (harvest_misses.py)
    entries.update(read_lines(os.path.join(here, "lexicon.harvest.%s.txt" % lang), col0_only=False))
    # strato "parole utili": le piu' FREQUENTI (coniugazioni per uso reale). Il curato (mandatory+lexicon)
    # ha precedenza; le freq riempiono solo gli slug nuovi. File opzionale freq.<lang>.txt (uno/riga).
    for k, v in read_lines(os.path.join(here, "freq.%s.txt" % lang), col0_only=False).items():
        entries.setdefault(k, v)
    if not no_dict:
        d = read_lines(os.path.join(dict_dir, DICT[lang]), col0_only=True)
        if limit: d = dict(list(d.items())[:limit])
        for k, v in d.items(): entries.setdefault(k, v)
    return entries

async def main():
    ap = argparse.ArgumentParser(description="Genera le clip voce con Edge-TTS (qualita' alta).")
    ap.add_argument("--langs", default="it,en")
    ap.add_argument("--voice-it", default=VOICES["it"]); ap.add_argument("--voice-en", default=VOICES["en"])
    ap.add_argument("--dict-dir", default=os.path.join(os.path.dirname(__file__), "..", "..", "deploy", "sd", "data", "anima"))
    ap.add_argument("--no-dict", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..", "deploy", "sd-safe", "data", "tts"))
    ap.add_argument("--conc", type=int, default=10, help="richieste concorrenti (abbassa se throttling)")
    ap.add_argument("--rate", type=int, default=RATE)
    ap.add_argument("--fast", action="store_true", help="salta loudnorm (piu' veloce)")
    ap.add_argument("--skip-existing", action="store_true")
    args = ap.parse_args()

    import shutil
    if not shutil.which("ffmpeg"): sys.exit("ffmpeg non trovato nel PATH.")
    here = os.path.dirname(os.path.abspath(__file__))
    voice = {"it": args.voice_it, "en": args.voice_en}
    for lang in [x.strip() for x in args.langs.split(",") if x.strip()]:
        entries = build_entries(lang, here, os.path.abspath(args.dict_dir), args.no_dict, args.limit)
        outdir = os.path.abspath(os.path.join(args.out, lang))
        await run_lang(lang, voice.get(lang, VOICES.get(lang)), entries, outdir, args.conc, args.rate, args.fast, args.skip_existing)

if __name__ == "__main__":
    asyncio.run(main())
