#!/usr/bin/env python3
# FLYWHEEL di copertura voce: raccoglie le parole che il device NON ha saputo pronunciare (le righe
# `clip scoperte [...]` che nucleo_tts.c gia' logga su /api/logs) e le accoda in lexicon.harvest.<lang>.txt.
# Poi gen_all.ps1 genera SOLO quelle (stessa voce Elsa/Aria, --skip-existing). Cosi' il pool voce cresce
# ESATTAMENTE sull'uso REALE — niente dump del dizionario (blob enorme + seek lenti). Intelligente e mirato.
#
#   python harvest_misses.py --lang it                       # raccoglie da http://192.168.0.166/api/logs
#   python harvest_misses.py --lang it --file _misslog.txt   # da un log salvato
#   python harvest_misses.py --lang it --seed traduco traduci # + parole a mano
# Poi:  .\gen_all.ps1   (genera + repack)   e   .\gen_all.ps1 -CopyToSd   (sulla SD)
import sys, os, re, argparse, urllib.request
here = os.path.dirname(os.path.abspath(__file__))

def fetch_logs(ip):
    try:
        with urllib.request.urlopen("http://%s/api/logs" % ip, timeout=8) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        print("  ! /api/logs non raggiungibile (%s)" % e, flush=True); return ""

def extract_misses(text):
    out = set()
    for m in re.finditer(r"clip scoperte \[([^\]]*)\]", text):
        for w in m.group(1).split(","):
            w = w.strip().lower()
            if re.fullmatch(r"[a-z]{3,24}", w):   # slug gia' foldato; >=3 char (no lettere isolate/sigle), no cifre
                out.add(w)
    return out

def main():
    ap = argparse.ArgumentParser(description="Flywheel: accoda i miss voce reali del device per la generazione.")
    ap.add_argument("--ip", default="192.168.0.166")
    ap.add_argument("--lang", default="it", choices=["it", "en"])
    ap.add_argument("--file", help="leggi i log da file invece che da /api/logs")
    ap.add_argument("--seed", nargs="*", default=[], help="parole extra da aggiungere a mano")
    args = ap.parse_args()

    text = open(args.file, encoding="utf-8", errors="replace").read() if args.file else fetch_logs(args.ip)
    miss = extract_misses(text)
    for w in args.seed:
        w = w.strip().lower()
        if re.fullmatch(r"[a-zàèéìíîòóùú]{2,24}", w):
            miss.add(w)

    path = os.path.join(here, "lexicon.harvest.%s.txt" % args.lang)
    existing = set()
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            s = line.strip()
            if s and not s.startswith("#"):
                existing.add(s.lower())
    new = sorted(miss - existing)
    if not new:
        print("[%s] niente di nuovo da accodare (%d miss, gia' tutte in coda/coperte)." % (args.lang, len(miss)), flush=True)
        return
    with open(path, "a", encoding="utf-8") as f:
        if not existing:
            f.write("# Parole raccolte dai MISS reali del device (flywheel harvest). gen_all.ps1 le genera.\n")
        for w in new:
            f.write(w + "\n")
    print("[%s] +%d parole accodate -> %s" % (args.lang, len(new), path), flush=True)
    print("  poi:  .\\gen_all.ps1   (genera + repack)   e   .\\gen_all.ps1 -CopyToSd   (sulla SD)", flush=True)

if __name__ == "__main__":
    main()
