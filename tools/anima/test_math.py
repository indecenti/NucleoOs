#!/usr/bin/env python3
"""ANIMA math regression — checks the exact VALUES the device computes (not just routing).

Drives the L0 cascade through the mock (serve-shell.mjs, which mirrors nucleo_anima.c's
a_try_calc + anima_solve: arithmetic, percent, Ohm's law, unit conversion, powers/roots).
Every expected result is the true number — so a regression that makes the device miscompute
(or stop answering) fails loudly. The math must be EXACT and never hallucinated.

  python tools/anima/test_math.py        # starts the mock, runs the battery, tears it down
Exit code != 0 on any mismatch.
"""
import os, sys, time, subprocess, urllib.request, urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MOCK = os.path.join(ROOT, "tools", "serve-shell.mjs")
BASE = "http://localhost:5599/api/anima"

# (query, lang, must-appear-in-reply)
BATTERY = [
    ("quanto fa 7 per 8", "it", "56"),
    ("(2+3)*4", "it", "20"),
    ("2 elevato a 10", "it", "1024"),
    ("2 to the power of 5", "en", "32"),
    ("radice di 144", "it", "12"),
    ("square root of 49", "en", "7"),
    ("il 15% di 240", "it", "36"),
    ("20% of 50", "en", "10"),
    ("5 volt e 1000 ohm", "it", "5 mA"),
    ("5 volt e 2 ampere quanta potenza", "it", "10 W"),
    ("100 cm in metri", "it", "1 metri"),
    ("3 km in metri", "it", "3000"),
    ("1 ora in minuti", "it", "60"),
    ("12 diviso 0", "it", "zero"),          # honest div-by-zero, not a fake number
    ("valore assoluto di -8", "it", "8"),   # abs (gap closed)
    ("10 modulo 3", "it", "1"),             # modulo (gap closed)
    ("17 modulo 5", "it", "2"),
]


def ask(q, lang):
    url = f"{BASE}?q={urllib.parse.quote(q)}&lang={lang}"
    with urllib.request.urlopen(url, timeout=5) as r:
        import json
        return json.load(r).get("reply", "")


def main():
    proc = subprocess.Popen(["node", MOCK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(40):                 # wait for the mock to come up
            try:
                ask("1 piu 1", "it"); break
            except Exception:
                time.sleep(0.25)
        ok = bad = 0
        for q, lang, exp in BATTERY:
            try:
                rep = ask(q, lang)
            except Exception as e:
                rep = f"<no reply: {e}>"
            good = exp.lower() in rep.lower()
            ok += good; bad += not good
            print(f"  {'OK ' if good else 'BAD'}  [{lang}] {q:42} -> {rep[:46]:46} (atteso ~{exp!r})")
        print(f"\n{ok}/{ok+bad} valori corretti" + ("" if not bad else f"   <-- {bad} REGRESSIONE"))
        sys.exit(1 if bad else 0)
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except Exception: proc.kill()


if __name__ == "__main__":
    main()
