# Text statistics: the reference implementation. Port this to stats.js for the device with identical output.
import re

TEXT = """La radio di Lia gracchiava ancora. Lia girava la manopola, piano, finche' la voce
di Marco non torno' chiara: 'Pronto? Mi senti?' Fuori nevicava, e il paese dormiva.
La radio, la neve, il paese: tutto aspettava una risposta."""

def rle(s):
    out, i = [], 0
    while i < len(s):
        j = i
        while j < len(s) and s[j] == s[i]:
            j += 1
        out.append(f"{j - i}{s[i]}" if j - i > 1 else s[i])
        i = j
    return "".join(out)

words = re.findall(r"[a-zA-Z']+", TEXT.lower())
words = [w.strip("'") for w in words if w.strip("'")]
freq = {}
for w in words:
    freq[w] = freq.get(w, 0) + 1
top = sorted(freq.items(), key=lambda kv: (-kv[1], kv[0]))[:5]
print("WORDS", len(words))
print("UNIQUE", len(freq))
for w, c in top:
    print("TOP", w, c)
longest = max(words, key=lambda w: (len(w), w))
print("LONGEST", longest, len(longest))
sentences = [s.strip() for s in re.split(r"[.!?]", TEXT) if s.strip()]
print("SENTENCES", len(sentences))
print("AVG", "%.2f" % (sum(len(w) for w in words) / len(words)))
print("RLE", rle("aaabccddddde"), rle("xyz"), rle("zzzzzzzzzzzz"))
