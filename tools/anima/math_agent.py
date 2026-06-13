#!/usr/bin/env python3
"""ANIMA math agent — ONE transversal math skill the orchestrator calls for ANY calculation.

A single entry, `solve(text, lang)`, that the cascade can treat as one dedicated tool instead
of juggling separate calc/solve paths. It dispatches, in order, the exact skills:

    ohm   -> Ohm's law (any 2 of V/I/R/P -> the rest), gated on an electrical keyword
    units -> conversions (length / mass / data / time / temperature)
    calc  -> arithmetic incl. ^ % abs sqrt and percentages (the calc.py engine)

Everything is EXACT (real arithmetic, never a guessed number); returns None when the input
isn't math so the orchestrator can fall through to retrieval. Mirrors nucleo_anima.c
anima_solve + a_try_calc; this file is the testable reference for that firmware agent.

    python tools/anima/math_agent.py "5 volt e 2 ampere quanta potenza"
    python tools/anima/math_agent.py --test
"""
import os, sys, re, math, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calc as _calc

PI = math.pi


def _fmt(n):
    return _calc.fmt(n)


def _items(text):
    norm = text.lower()
    for a, b in [("à", "a"), ("è", "e"), ("é", "e"), ("ì", "i"), ("ò", "o"), ("ù", "u")]:
        norm = norm.replace(a, b)
    norm = norm.replace(",", ".")
    out = []
    for m in re.finditer(r"(\d*\.?\d+)|([a-z]+)|(%)", norm):
        if m.group(1) is not None:
            out.append(("num", float(m.group(1))))
        elif m.group(3):
            out.append(("w", "pct"))
        else:
            out.append(("w", m.group(2)))
    return norm, out


def _ohm(norm, items, lang):
    if not re.search(r"volt|ampere|ohm|watt|tension|corrent|resisten|potenz|current|voltage|resistance|power", norm):
        return None
    V = I = R = P = 0.0; hV = hI = hR = hP = False
    for i in range(len(items) - 1):
        (k, val), (k2, u) = items[i], items[i + 1]
        if k != "num" or k2 != "w":
            continue
        if u == "v" or u.startswith("volt"):
            if not hV: V, hV = val, True
        elif u == "ma" or u == "milliampere":
            if not hI: I, hI = val * 1e-3, True
        elif u == "a" or u.startswith("amper") or u == "amp":
            if not hI: I, hI = val, True
        elif u in ("kohm", "kiloohm"):
            if not hR: R, hR = val * 1000, True
        elif u.startswith("ohm"):
            if not hR: R, hR = val, True
        elif u == "w" or u.startswith("watt"):
            if not hP: P, hP = val, True
    if hV + hI + hR + hP < 2:
        return None
    if not hV: V = I * R if (hI and hR) else (P / I if (hP and hI and I) else (math.sqrt(P * R) if (hP and hR) else V))
    if not hI: I = V / R if (hV and hR and R) else (P / V if (hP and hV and V) else (math.sqrt(P / R) if (hP and hR and R) else I))
    if not hR: R = V / I if (hV and I) else (P / (I * I) if (hP and I) else ((V * V) / P if hP and P else R))
    if not hP: P = V * I
    ci = f"{_fmt(I*1000)} mA" if abs(I) < 1 else f"{_fmt(I)} A"
    return {"kind": "ohm", "reply": f"V={_fmt(V)} V, I={ci}, R={_fmt(R)} ohm, P={_fmt(P)} W."}


_UNITS = {  # name -> (dim, factor-to-base); dim 5 = temperature (affine, handled apart)
    "mm": (1, .001), "cm": (1, .01), "dm": (1, .1), "m": (1, 1), "metro": (1, 1), "metri": (1, 1),
    "km": (1, 1000), "chilometri": (1, 1000), "inch": (1, .0254), "pollici": (1, .0254),
    "mg": (2, .001), "g": (2, 1), "grammi": (2, 1), "kg": (2, 1000), "chili": (2, 1000),
    "bit": (3, .125), "byte": (3, 1), "kb": (3, 1024), "mb": (3, 1048576), "gb": (3, 1073741824),
    "sec": (4, 1), "secondi": (4, 1), "min": (4, 60), "minuti": (4, 60),
    "ora": (4, 3600), "ore": (4, 3600), "giorni": (4, 86400),
}


def _units(items):
    num = None; found = []
    for k, v in items:
        if k == "num" and num is None:
            num = v
        elif k == "w" and v in _UNITS and len(found) < 2:
            found.append((v, _UNITS[v]))
    if num is None or len(found) < 2 or found[0][1][0] != found[1][1][0]:
        return None
    res = num * found[0][1][1] / found[1][1][1]
    return {"kind": "convert", "reply": f"{_fmt(num)} {found[0][0]} = {_fmt(res)} {found[1][0]}."}


def solve(text, lang="it"):
    """Single entry: return {kind, reply, value?} for any math, else None."""
    norm, items = _items(text)
    if not items:
        return None
    r = _ohm(norm, items, lang)
    if r: return r
    r = _units(items)
    if r: return r
    ok, val = _calc.calc(text)              # arithmetic incl. ^ % abs sqrt percent
    if ok:
        return {"kind": "calc", "reply": (f"It's {val}." if lang == "en" else f"Fa {val}."), "value": val}
    if val == "divisione per zero":
        return {"kind": "calc", "reply": ("I can't divide by zero." if lang == "en" else "Non posso dividere per zero.")}
    return None


TESTS = [
    ("quanto fa 7 per 8", "56"), ("(2+3)*4", "20"), ("2 elevato a 10", "1024"),
    ("radice di 144", "12"), ("il 15% di 240", "36"), ("valore assoluto di -8", "8"),
    ("10 modulo 3", "1"), ("17 modulo 5", "2"), ("12 diviso 0", "zero"),
    ("5 volt e 1000 ohm", "5 mA"), ("5 volt e 2 ampere quanta potenza", "10 W"),
    ("100 cm in metri", "1 metri"), ("3 km in metri", "3000"), ("1 ora in minuti", "60"),
    ("2 to the power of 5", "32"), ("square root of 49", "7"), ("20% of 50", "10"),
    ("il 15% di 240 piu radice di 81", "45"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("query", nargs="*")
    ap.add_argument("--test", action="store_true")
    a = ap.parse_args()
    if a.test:
        ok = bad = 0
        for q, exp in TESTS:
            r = solve(q, "en" if re.search(r"power of|root of|% of", q) else "it")
            rep = (r or {}).get("reply", "<none>")
            good = exp.lower() in rep.lower()
            ok += good; bad += not good
            print(f"  {'OK ' if good else 'BAD'}  {q:40} -> {rep[:44]:44} (~{exp})")
        print(f"\n{ok}/{ok+bad} corretti" + ("" if not bad else f"  <-- {bad} DA SISTEMARE"))
        sys.exit(1 if bad else 0)
    if a.query:
        r = solve(" ".join(a.query), "it")
        print(r["reply"] if r else "(non e matematica)")


if __name__ == "__main__":
    main()
