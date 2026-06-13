#!/usr/bin/env python3
"""ANIMA calc engine — "answer ANY calculation", exactly and without hallucination.

A safe recursive-descent arithmetic evaluator (NO eval/exec): the device computes the number,
it never "guesses" it. This is the reference engine for the firmware tool (nucleo_anima.c
a_cexpr/term/prim) and the mock (serve-shell.mjs tryCalc) — mirror the operators/folds here.

Supports, IT + EN:
  + - * /            piu/plus, meno/minus, per|x|times|moltiplicato, diviso|divided by
  ^ (power)          elevato a, alla, to the power of      (right-assoc)
  % (modulo)         modulo, resto di
  X% di Y / X% of Y  percentage of               |  N%  ->  N/100
  sqrt / radice      radice (quadrata) di N, square root of N, √N
  abs                valore assoluto, abs
  ( )                parentheses, unary minus, decimal comma or point, pi/π
Division/modulo by zero is reported honestly (never a fake number).

  python tools/anima/calc.py "quanto fa il 15% di 240 piu radice di 81"
  python tools/anima/calc.py --test      # run the self-checking battery
"""
import sys, re, math, argparse

PI = math.pi


def fold(text):
    """Natural-language math -> a pure symbolic expression string."""
    s = " " + text.lower().strip() + " "
    s = re.sub(r"(\d),(\d)", r"\1.\2", s)                         # decimal comma -> point
    s = s.replace("×", " * ").replace("÷", " / ").replace("√", " radice di ")
    # 'resto/modulo di A per B' -> A % B  (BEFORE 'per' becomes '*')
    s = re.sub(r"\b(?:resto\s+(?:della\s+divisione\s+)?(?:di\s+)?|modulo\s+di\s+)(\d+(?:\.\d+)?)\s+per\s+(\d+(?:\.\d+)?)",
               r" \1 % \2 ", s)
    # percentage: 'N% di M' -> (N/100)*M ; bare 'N%' -> (N/100) but NOT 'N % M' (that's modulo)
    s = re.sub(r"(\d+(?:\.\d+)?)\s*(?:%|per\s*cento|percento|percent)\s*(?:di|del|dello|della|dei|delle|of)\s+",
               r"(\1/100)*", s)
    s = re.sub(r"(\d+(?:\.\d+)?)\s*(?:per\s*cento|percento|percent)\b", r"(\1/100)", s)
    s = re.sub(r"(\d+(?:\.\d+)?)\s*%(?!\s*\d)", r"(\1/100)", s)   # '%' not followed by a number
    # functions
    s = re.sub(r"\b(?:radice\s+quadrata\s+di|radice\s+di|square\s+root\s+of|sqrt\s+of|sqrt)\b\s*", " sqrt ", s)
    s = re.sub(r"\b(?:valore\s+assoluto\s+di|valore\s+assoluto|abs\s+of|abs)\b\s*", " abs ", s)
    # power
    s = re.sub(r"\b(?:elevato\s+(?:a|alla)|alla\s+potenza\s+di|to\s+the\s+power\s+of)\b", " ^ ", s)
    s = re.sub(r"\bmodulo\b", " % ", s)
    # four ops (multi-word first)
    s = re.sub(r"\b(?:diviso\s+per|diviso|divided\s+by|fratto|over)\b", " / ", s)
    s = re.sub(r"\b(?:moltiplicato\s+per|times|multiplied\s+by|per)\b", " * ", s)
    s = re.sub(r"(?<=\d)\s*x\s*(?=\d)", " * ", s)               # 3x4 / 3 x 4
    s = re.sub(r"\b(?:piu|più|plus)\b", " + ", s)
    s = re.sub(r"\b(?:meno|minus)\b", " - ", s)
    s = re.sub(r"\b(?:pi\s*greco|pigreco|pi|π)\b", f"({PI})", s)
    # drop leftover filler words ('quanto fa', 'il', 'what is', 'di', ...) — keep sqrt/abs
    s = re.sub(r"\b(?!sqrt\b)(?!abs\b)[a-z]+\b", " ", s)
    return s


_TOK = re.compile(r"\s*(sqrt|abs|\d+\.?\d*|\.\d+|[+\-*/%^()])")


def tokenize(s):
    toks, i = [], 0
    while i < len(s):
        m = _TOK.match(s, i)
        if not m:
            if s[i].isspace():
                i += 1; continue
            raise ValueError(f"carattere non valido: {s[i]!r}")
        toks.append(m.group(1)); i = m.end()
    return toks


class P:
    def __init__(self, toks): self.t = toks; self.i = 0
    def peek(self): return self.t[self.i] if self.i < len(self.t) else None
    def eat(self): tok = self.t[self.i]; self.i += 1; return tok

    def expr(self):
        v = self.term()
        while self.peek() in ("+", "-"):
            v = v + self.term() if self.eat() == "+" else v - self.term()
        return v

    def term(self):
        v = self.unary()
        while self.peek() in ("*", "/", "%"):
            op = self.eat(); r = self.unary()
            if op == "*": v *= r
            elif op == "/":
                if r == 0: raise ZeroDivisionError
                v /= r
            else:
                if r == 0: raise ZeroDivisionError
                v = math.fmod(v, r)
        return v

    def unary(self):
        if self.peek() == "-": self.eat(); return -self.unary()
        if self.peek() == "+": self.eat(); return self.unary()
        return self.power()

    def power(self):
        v = self.atom()
        if self.peek() == "^":
            self.eat(); v = v ** self.unary()      # right-assoc, allows 2^-1
        return v

    def atom(self):
        tok = self.peek()
        if tok is None: raise ValueError("espressione incompleta")
        if tok == "(":
            self.eat(); v = self.expr()
            if self.peek() != ")": raise ValueError("parentesi non chiusa")
            self.eat(); return v
        if tok == "sqrt":
            self.eat(); x = self.atom()
            if x < 0: raise ValueError("radice di un numero negativo")
            return math.sqrt(x)
        if tok == "abs":
            self.eat(); return abs(self.unary())
        if re.match(r"^\d|\.", tok): self.eat(); return float(tok)
        raise ValueError(f"token inatteso: {tok!r}")


def looks_like_calc(text):
    """Route to calc only if there's something to compute (a digit + an operator/keyword)."""
    s = fold(text)
    return bool(re.search(r"\d", s) and re.search(r"[+\-*/%^]|sqrt|abs", s))


def fmt(n):
    if abs(n - round(n)) < 1e-9: return str(int(round(n)))
    return f"{n:.4f}".rstrip("0").rstrip(".")


def calc(text):
    """Return (ok, result_str). ok=False with a reason for non-expressions / div-by-zero."""
    if not looks_like_calc(text): return False, "non e un calcolo"
    try:
        toks = tokenize(fold(text))
        if not toks: return False, "vuoto"
        p = P(toks); v = p.expr()
        if p.peek() is not None: return False, "sintassi"
        return True, fmt(v)
    except ZeroDivisionError:
        return False, "divisione per zero"
    except (ValueError, OverflowError) as e:
        return False, str(e)


TESTS = [
    ("quanto fa 7 per 8", "56"), ("12 diviso 3", "4"), ("(2+3)*4", "20"),
    ("2^10", "1024"), ("2 elevato a 8", "256"), ("radice di 144", "12"),
    ("il 15% di 240", "36"), ("100 meno 37", "63"), ("3,5 + 1,5", "5"),
    ("10 % 3", "1"), ("resto della divisione di 17 per 5", "2"),
    ("sqrt(81) + 2^3", "17"), ("pi greco per 2", "6.2832"),
    ("valore assoluto di -8", "8"), ("(10+5)/3", "5"), ("9 x 9", "81"),
    ("what is 15 plus 27", "42"), ("8 times 9", "72"), ("144 divided by 12", "12"),
    ("20% of 50", "10"), ("2 to the power of 5", "32"), ("square root of 49", "7"),
    ("il 15% di 240 piu radice di 81", "45"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("expr", nargs="*")
    ap.add_argument("--test", action="store_true")
    a = ap.parse_args()
    if a.test:
        ok = bad = 0
        for q, exp in TESTS:
            got = calc(q)
            good = got[0] and got[1] == exp
            ok += good; bad += not good
            print(f"  {'OK ' if good else 'BAD'}  {q:42} = {got[1]:>10}   (atteso {exp})")
        print(f"\n{ok}/{ok+bad} corretti" + ("" if not bad else f"  <-- {bad} DA SISTEMARE"))
        sys.exit(1 if bad else 0)
    if a.expr:
        ok, r = calc(" ".join(a.expr))
        print(f"Fa {r}." if ok else f"(non calcolo: {r})")


if __name__ == "__main__":
    main()
