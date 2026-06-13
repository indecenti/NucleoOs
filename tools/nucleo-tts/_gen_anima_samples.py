# Assembla RISPOSTE REALI di ANIMA dai clip mirati (_wav), come fara' il device: composizione per i
# template (ore/batteria/data/apro), clip-INTERA per le risposte fisse (identita'/fallback). Scrive un
# player per ascoltare la coerenza. Solo verifica, non e' il pack.
import os, sys, wave
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_voice import slugify
RATE = 24000
OUT = os.path.join(HERE, "_samples_anima")

# (lang, testo risposta, tokens) — token ("c",slug) clip | ("p",ms) pausa. None = clip-intera (slug=slugify).
REPLIES = [
 ("it","Sono le 9 e 30.",                [("c","sono_le"),("c","n9"),("c","e"),("c","n30"),("p",300)]),
 ("it","La batteria è al 90 per cento.", [("c","la_batteria_e_al"),("c","n90"),("c","per_cento"),("p",300)]),
 ("it","Apro musica.",                   [("c","apro"),("c","musica"),("p",300)]),
 ("it","Oggi è lunedì 10 giugno.",       [("c","oggi_e"),("c","lunedi"),("c","n10"),("c","giugno"),("p",300)]),
 ("it","Siamo nel 2026.",                [("c","siamo_nel"),("c","duemila"),("c","n26"),("p",300)]),
 ("it","Sono ANIMA, l'assistente offline di NucleoOS. Funziono senza internet, sul dispositivo.", None),
 ("it","A dire il vero non lo sapevo, non mi fiderei.", None),
 ("en","The battery is at 90 percent.", [("c","the_battery_is_at"),("c","n90"),("c","percent"),("p",300)]),
 ("en","Opening music.",                [("c","opening"),("c","music"),("p",300)]),
 ("en","It is 9 30.",                   [("c","it_is"),("c","n9"),("c","n30"),("p",300)]),
 ("en","I'm ANIMA, NucleoOS's offline assistant. I work with no internet, on the device.", None),
]

def clip_path(lang, slug): return os.path.join(HERE, "_wav", lang, slug + ".wav")

def assemble(dst, lang, tokens):
    missing = []
    w = wave.open(dst, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    for kind, val in tokens:
        if kind == "p":
            w.writeframes(b"\x00\x00" * int(RATE * val / 1000))
        else:
            p = clip_path(lang, val)
            if not os.path.exists(p): missing.append(val); continue
            c = wave.open(p, "rb"); w.writeframes(c.readframes(c.getnframes())); c.close()
    w.close()
    return missing

def main():
    os.makedirs(OUT, exist_ok=True)
    rows = {"it": [], "en": []}
    allmiss = 0
    for i, (lang, text, toks) in enumerate(REPLIES):
        if toks is None:
            toks = [("c", slugify(text)), ("p", 300)]      # risposta fissa -> clip intera
        name = "r%02d.wav" % i
        miss = assemble(os.path.join(OUT, name), lang, toks)
        if miss: allmiss += len(miss); print("  MANCANTI in [%s]: %s" % (text, miss))
        rows[lang].append((name, text, "intera" if REPLIES[i][2] is None else "composta"))
    # player
    def block(lang, title):
        items = "".join(
          '<div class=ex><div class=lab>%s <span class=k>(%s)</span></div>'
          '<audio controls preload=none src="%s"></audio></div>' % (t, kind, n)
          for (n, t, kind) in rows[lang])
        return "<section><h2>%s</h2>%s</section>" % (title, items)
    html = ("<!doctype html><meta charset=utf-8><title>Risposte ANIMA - voce</title>"
      "<style>body{font:15px system-ui;background:#0b1020;color:#e8eefc;margin:0;padding:20px;max-width:820px;margin:auto}"
      "h1{font-size:18px}h2{font-size:13px;color:#6aa8ff;text-transform:uppercase;letter-spacing:1px;margin-top:22px}"
      ".ex{display:flex;align-items:center;gap:14px;padding:9px;border-bottom:1px solid #22304f}"
      ".lab{flex:1}.k{color:#8aa0c8;font-size:12px}audio{height:34px}</style>"
      "<h1>Risposte reali di ANIMA - voce mirata (Edge 24kHz)</h1>"
      "<p style='color:#8aa0c8'>Le \"composte\" assemblano pezzi (template+numeri); le \"intere\" sono una clip unica (prosodia naturale).</p>"
      + block("it","Italiano") + block("en","English"))
    open(os.path.join(OUT, "index.html"), "w", encoding="utf-8").write(html)
    print("Assemblate %d risposte, clip mancanti: %d -> %s" % (len(REPLIES), allmiss, OUT))

if __name__ == "__main__":
    main()
