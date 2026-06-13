#!/usr/bin/env python3
# Costruisce freq.<lang>.txt = top-N forme PIU' FREQUENTI (wordfreq) per lingua. Espansione "intelligente"
# (Zipf: poche migliaia di forme coprono ~96% del testo reale) per dare al concatenatore un pool ricco con
# cui "incrociare" parole nelle risposte MOSAICO. Le curate (mandatory/lexicon/lexicon.wf) restano prioritarie
# (gen_edge usa setdefault). Pronuncia corretta: salvo la forma con accenti, lo slug se lo ricava il build.
import re, os
from wordfreq import top_n_list
here = os.path.dirname(os.path.abspath(__file__))
N = 12000
IT_RE = re.compile(r"^[a-zàèéìíîòóùú]{2,24}$")
EN_RE = re.compile(r"^[a-z]{2,24}$")
for lang in ("it", "en"):
    rx = IT_RE if lang == "it" else EN_RE
    raw = top_n_list(lang, N + 6000)                 # extra: ne scartiamo (punteggiatura/lettere isolate)
    seen, out = set(), []
    for w in raw:
        wl = w.strip().lower()
        if not rx.match(wl) or wl in seen:
            continue
        seen.add(wl); out.append(wl)
        if len(out) >= N:
            break
    path = os.path.join(here, "freq.%s.txt" % lang)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("[%s] %d forme -> %s" % (lang, len(out), path), flush=True)
