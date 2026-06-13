#!/usr/bin/env python3
# Rimuove dallo staging _wav/<lang> le clip NON appartenenti all'insieme MIRATO (no-dict): mandatory +
# lexicon + lexicon.wf + freq. Serve a ripulire un'eventuale generazione che ha incluso il dizionario.
import sys, os
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from gen_edge import build_entries
dict_dir = os.path.abspath(os.path.join(here, "..", "..", "deploy", "sd", "data", "anima"))
for lang in ["it", "en"]:
    intended = set(build_entries(lang, here, dict_dir, True, 0).keys())   # no_dict=True
    wavdir = os.path.join(here, "_wav", lang)
    if not os.path.isdir(wavdir):
        print("[%s] niente _wav" % lang); continue
    removed = kept = 0
    for fn in os.listdir(wavdir):
        if not fn.endswith(".wav"):
            continue
        slug = fn[:-4]
        if slug in intended:
            kept += 1
        else:
            os.remove(os.path.join(wavdir, fn)); removed += 1
    print("[%s] mirati=%d  tenuti=%d  rimossi(dict)=%d" % (lang, len(intended), kept, removed), flush=True)
