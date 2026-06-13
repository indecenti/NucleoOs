#!/usr/bin/env python3
# build_voice.py — pre-vocalizza (OFFLINE, sul PC) il "pacchetto voce" per il TTS concatenativo di
# NucleoOS, in ITALIANO e/o INGLESE. Genera clip WAV canoniche 16kHz/mono/16-bit + le copia (poi via
# tools/sd-sync.ps1) in /data/tts/<lang>/. Il device le concatena a runtime (firmware nucleo_tts):
# nessuna sintesi a bordo, voce naturale, RAM ~zero (esistenza clip = stat su SD, niente manifest).
#
# Tre strati per lingua:
#   1) OBBLIGATORIO (generato qui, sempre completo e corretto): n0..n99 (cardinali con elisione IT
#      ventuno/ventitré/ventotto; EN twenty-three), connettori numerici (cento.. / hundred,thousand),
#      virgola|point, meno|minus, lett_a..lett_z (nomi lettere).
#   2) DIZIONARIO: la colonna 0 dei .tsv (dict-it-en = parole/frasi IT; dict-en-it = EN) -> copre il
#      vocabolario reale, quasi azzera lo spelling di fallback.
#   3) LEXICON editabile: tools/nucleo-tts/lexicon.<lang>.txt (frasi comuni dell'assistente). Le frasi
#      multi-parola diventano slug con "_" e il planner le PREFERISCE alle singole parole (match greedy).
#
# Motori (offline, --engine): piper (--model *.onnx, qualita' migliore) | espeak (espeak-ng) | pico
# (pico2wave, gia' 16kHz). Normalizzazione finale via ffmpeg (nel PATH).
#
# Esempi:
#   python build_voice.py --langs it,en --engine espeak --no-dict     # giro veloce (solo pacchetto+lexicon)
#   python build_voice.py --langs it --engine piper --model it.onnx --limit 500
#   python build_voice.py --langs it,en --engine pico                 # tutto (lungo: ~77k clip)
import argparse, os, subprocess, sys, tempfile, shutil, re, unicodedata

# ---- ITALIANO ---------------------------------------------------------------------------------
IT_U   = ["zero","uno","due","tre","quattro","cinque","sei","sette","otto","nove"]
IT_T   = ["dieci","undici","dodici","tredici","quattordici","quindici","sedici",
          "diciassette","diciotto","diciannove"]
IT_TENS= ["","","venti","trenta","quaranta","cinquanta","sessanta","settanta","ottanta","novanta"]
IT_HUND= ["","cento","duecento","trecento","quattrocento","cinquecento","seicento",
          "settecento","ottocento","novecento"]
IT_THOU= ["","mille","duemila","tremila","quattromila","cinquemila","seimila",
          "settemila","ottomila","novemila"]

def cardinal_it(n):
    if n < 10: return IT_U[n]
    if n < 20: return IT_T[n - 10]
    t, u = n // 10, n % 10
    base = IT_TENS[t]
    if u == 0:      return base
    if u in (1, 8): return base[:-1] + IT_U[u]      # ventuno, ventotto, trentuno...
    if u == 3:      return base + "tré"             # ventitré, trentatré...
    return base + IT_U[u]

# ---- INGLESE ----------------------------------------------------------------------------------
EN_U   = ["zero","one","two","three","four","five","six","seven","eight","nine"]
EN_T   = ["ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen",
          "seventeen","eighteen","nineteen"]
EN_TENS= ["","","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"]

def cardinal_en(n):
    if n < 10: return EN_U[n]
    if n < 20: return EN_T[n - 10]
    t, u = n // 10, n % 10
    return EN_TENS[t] + ("-" + EN_U[u] if u else "")    # twenty-three

# ---- nomi delle lettere (per sillabare gli acronimi: USB -> "u esse bi") -----------------------
# slug lett_<a..z>; il testo e' COME si pronuncia la lettera nella lingua (Edge la dice naturale).
IT_LETT = {"a":"a","b":"bi","c":"ci","d":"di","e":"e","f":"effe","g":"gi","h":"acca","i":"i",
           "j":"i lunga","k":"cappa","l":"elle","m":"emme","n":"enne","o":"o","p":"pi","q":"cu",
           "r":"erre","s":"esse","t":"ti","u":"u","v":"vu","w":"doppia vu","x":"ics","y":"ipsilon","z":"zeta"}
EN_LETT = {"a":"ay","b":"bee","c":"see","d":"dee","e":"ee","f":"ef","g":"gee","h":"aitch","i":"eye",
           "j":"jay","k":"kay","l":"el","m":"em","n":"en","o":"oh","p":"pee","q":"cue","r":"ar",
           "s":"ess","t":"tee","u":"you","v":"vee","w":"double u","x":"ex","y":"why","z":"zee"}

# ---- pacchetto obbligatorio per lingua --------------------------------------------------------
def mandatory(lang):
    m = {}
    if lang == "en":
        for n in range(100): m["n%d" % n] = cardinal_en(n)
        m["hundred"]="hundred"; m["thousand"]="thousand"; m["million"]="million"
        m["point"]="point"; m["minus"]="minus"
        for c, name in EN_LETT.items(): m["lett_%s" % c] = name   # sillabazione acronimi
        m["read_it"] = "I can't say this out loud, please read it on the screen."
    else:
        for n in range(100): m["n%d" % n] = cardinal_it(n)
        for h in range(1,10): m[IT_HUND[h]] = IT_HUND[h]
        for t in range(1,10): m[IT_THOU[t]] = IT_THOU[t]
        m["mila"]="mila"; m["virgola"]="virgola"; m["meno"]="meno"
        for c, name in IT_LETT.items(): m["lett_%s" % c] = name   # sillabazione acronimi
        m["read_it"] = "Non posso dirtelo a voce, leggi la risposta sullo schermo."
    return m

def slugify(text):
    # DEVE combaciare con la normalizzazione del firmware (nucleo_tts_plan.c): fold accenti -> ASCII,
    # minuscolo, tieni [a-z0-9], parole unite da '_'. Altrimenti la clip non viene trovata sul device.
    t = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii").lower()
    out, prev = [], False
    for ch in t:
        if ch.isalnum():        out.append(ch); prev = False
        elif not prev and out:  out.append("_"); prev = True
    return "".join(out).strip("_")

def sanitize_text(t):
    # Testo PRONUNCIATO (non lo slug): togli caratteri/punteggiatura che il TTS leggerebbe male, tieni
    # lettere (anche accentate, per la pronuncia), cifre, spazi, apostrofo, trattino. Scrivile bene.
    t = (t.replace("’","'").replace("‘","'").replace("`","'")
          .replace("–","-").replace("—","-").replace(" "," "))
    t = re.sub(r"[^0-9A-Za-zÀ-ſ'\- ]+", " ", t)   # solo lettere(+accenti)/cifre/'/-/spazio
    return re.sub(r"\s+", " ", t).strip()

def read_lines(path, col0_only):
    """Una voce per riga: 'slug<TAB>testo' o 'testo'; col0_only -> prende solo la 1a colonna TSV.
    Lo slug e' il fold del testo (== device); il testo pronunciato e' sanitizzato."""
    m = {}
    if not path or not os.path.exists(path): return m
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"): continue
            if col0_only:
                raw = line.split("\t", 1)[0].strip(); slug = slugify(raw); text = sanitize_text(raw)
            elif "\t" in line:
                a, b = line.split("\t", 1); slug = a.strip(); text = sanitize_text(b)
            else:
                raw = line.strip(); slug = slugify(raw); text = sanitize_text(raw)
            if slug and text and slug not in m: m[slug] = text
    return m

def synth_lang(engine, espeak_bin, model, lang, text, raw):
    """Sintetizza `text` per `lang` col motore scelto -> WAV grezzo `raw`."""
    if engine == "espeak":
        p = subprocess.run([espeak_bin,"-v","en" if lang=="en" else "it","-w",raw,text], capture_output=True)
    elif engine == "pico":
        p = subprocess.run(["pico2wave","-l","en-US" if lang=="en" else "it-IT","-w",raw,text], capture_output=True)
    elif engine == "piper":
        if not model: sys.exit("piper: serve --model (o --model-it/--model-en) per la lingua %s" % lang)
        p = subprocess.run(["piper","--model",model,"--output_file",raw],
                           input=text.encode("utf-8"), capture_output=True)
    else:
        sys.exit("engine sconosciuto: %s" % engine)
    if p.returncode: sys.exit("%s fallito: %s" % (engine, p.stderr.decode('utf-8','replace')))

def normalize(raw, out_wav, fast=False):
    # 16kHz mono 16-bit PCM canonico. loudnorm uniforma il volume tra clip (importante per la
    # concatenazione) ma raddoppia il tempo: --fast lo salta (eSpeak ha gia' volume costante).
    cmd = ["ffmpeg","-y","-i",raw,"-ar","16000","-ac","1","-sample_fmt","s16"]
    if not fast: cmd += ["-af","loudnorm=I=-16:TP=-1.5:LRA=11"]
    cmd.append(out_wav)
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode: sys.exit("ffmpeg fallito: %s" % p.stderr.decode('utf-8','replace'))

def resolve_espeak(explicit):
    """Trova l'eseguibile espeak-ng (PATH o installazione Windows tipica)."""
    if explicit: return explicit
    p = shutil.which("espeak-ng") or shutil.which("espeak")
    if p: return p
    for c in [r"C:\Program Files\eSpeak NG\espeak-ng.exe",
              r"C:\Program Files (x86)\eSpeak NG\espeak-ng.exe"]:
        if os.path.exists(c): return c
    return "espeak-ng"   # ultima spiaggia: lascia fallire con messaggio chiaro

def main():
    ap = argparse.ArgumentParser(description="Pacchetto voce IT/EN per NucleoOS TTS concatenativo.")
    ap.add_argument("--langs", default="it", help="lingue separate da virgola: it,en")
    ap.add_argument("--engine", choices=["piper","espeak","pico"], default="espeak")
    ap.add_argument("--model", help="modello .onnx (piper). Per piper bilingue usa --model-it/--model-en")
    ap.add_argument("--model-it"); ap.add_argument("--model-en")
    ap.add_argument("--dict-dir", default=os.path.join(os.path.dirname(__file__),
                    "..", "..", "deploy", "sd", "data", "anima"))
    ap.add_argument("--no-dict", action="store_true", help="salta il dizionario (solo pacchetto+lexicon)")
    ap.add_argument("--limit", type=int, default=0, help="max voci di dizionario per lingua (0=tutte)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__),
                    "..", "..", "deploy", "sd-safe", "data", "tts"))
    ap.add_argument("--espeak-bin", help="percorso di espeak-ng.exe (autodetect se omesso)")
    ap.add_argument("--skip-existing", action="store_true", help="non rigenerare clip gia' presenti (resume)")
    ap.add_argument("--fast", action="store_true", help="salta la normalizzazione loudnorm (molto piu' veloce)")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"): sys.exit("ffmpeg non trovato nel PATH (serve a normalizzare le clip).")
    espeak_bin = resolve_espeak(args.espeak_bin) if args.engine == "espeak" else None
    if args.engine == "espeak" and not (shutil.which(espeak_bin) or os.path.exists(espeak_bin)):
        sys.exit("espeak-ng non trovato. Installa (winget install eSpeak-NG.eSpeak-NG) o passa --espeak-bin <path>.")
    here = os.path.dirname(__file__)
    dict_for = {"it": "dict-it-en.tsv", "en": "dict-en-it.tsv"}
    model_for = {"it": args.model_it or args.model, "en": args.model_en or args.model}

    for lang in [x.strip() for x in args.langs.split(",") if x.strip()]:
        out = os.path.abspath(os.path.join(args.out, lang))
        os.makedirs(out, exist_ok=True)
        entries = mandatory(lang)
        entries.update(read_lines(os.path.join(here, "lexicon.%s.txt" % lang), col0_only=False))
        if not args.no_dict:
            d = read_lines(os.path.join(args.dict_dir, dict_for[lang]), col0_only=True)
            if args.limit: d = dict(list(d.items())[:args.limit])
            for k, v in d.items(): entries.setdefault(k, v)
        print("[%s] %d clip -> %s" % (lang, len(entries), out), flush=True)

        tmp = tempfile.mkdtemp(prefix="nucleotts_"); raw = os.path.join(tmp, "raw.wav"); done = 0; skip = 0
        try:
            for slug, text in sorted(entries.items()):
                dst = os.path.join(out, slug + ".wav")
                if args.skip_existing and os.path.exists(dst) and os.path.getsize(dst) > 44: skip += 1; continue
                synth_lang(args.engine, espeak_bin, model_for[lang], lang, text, raw)
                normalize(raw, dst, args.fast)
                done += 1
                if done % 200 == 0: print("  [%s] %d generate (%d saltate) / %d ..." % (lang, done, skip, len(entries)), flush=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        print("[%s] OK: %d generate, %d gia' presenti -> %s" % (lang, done, skip, out), flush=True)

if __name__ == "__main__":
    main()
