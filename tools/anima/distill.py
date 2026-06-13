#!/usr/bin/env python3
"""ANIMA offline factory — STEP 1: distill a BILINGUAL (IT+EN) on-device encoder.

Distills multilingual-e5 into a tiny hashed char+word n-gram EmbeddingBag (int8) that runs
on the ESP32. Trained on Italian + English with a CROSS-LINGUAL alignment term from Tatoeba
IT<->EN pairs, so a query in either language lands in one shared space (EN query can hit IT
knowledge and vice-versa). Same ANE2 format/size as before -> zero extra device RAM.

Outputs:  models/anima-it-encoder.bin  +  models/anima-it-encoder.json   (now bilingual)
Run:  python tools/anima/distill.py
"""
import os, bz2, io, zipfile, urllib.request, random, struct, json, time, unicodedata
import numpy as np
import torch, torch.nn as nn
from transformers import AutoTokenizer, AutoModel
from scipy.stats import spearmanr

random.seed(0); np.random.seed(0); torch.manual_seed(0)
ROOT   = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUTDIR = os.path.join(ROOT, "models"); os.makedirs(OUTDIR, exist_ok=True)
CACHE  = os.path.join(ROOT, "tools", "anima", ".cache"); os.makedirs(CACHE, exist_ok=True)

DEV     = "cuda" if torch.cuda.is_available() else "cpu"
TEACHER = "intfloat/multilingual-e5-small"
D, H    = 256, 65536              # bigger embedding dim (= device L1_MAXDIM) + 4x hash table:
NGRAMS, WORD_N = (3, 4, 5), 2     # more capacity = far fewer n-gram collisions = truer semantics
N_IT, N_EN = 100000, 100000       # more data = better generalisation of the n-gram->meaning map
EPOCHS = 24                       # (encoder lives on SD, read row-by-row -> no extra device RAM)
print(f"[anima] device={DEV}  teacher={TEACHER}  table={H}x{D}  bilingual IT+EN")

# ---- features (identical to the C device encoder; language-agnostic) ---------------------
def fnv1a(b):
    h = 0x811c9dc5
    for x in b: h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h
def norm_text(s):
    s = unicodedata.normalize("NFD", s.lower())
    s = "".join(c for c in s if unicodedata.category(c) != "Mn")
    return " ".join("".join(c if c.isalnum() else " " for c in s).split())
def feats(s):
    w = norm_text(s).split(); ids = []; t = "^" + "^".join(w) + "$"
    for n in NGRAMS:
        for i in range(len(t) - n + 1): ids.append(fnv1a(b"\x01" + t[i:i+n].encode()) % H)
    for x in w: ids.append(fnv1a(b"\x02" + x.encode()) % H)
    for i in range(len(w) - 1): ids.append(fnv1a(b"\x02" + (w[i] + " " + w[i+1]).encode()) % H)
    return ids or [fnv1a(b"\x01^$") % H]

# ---- data: IT corpus (Tatoeba) + IT/EN pairs (manythings/Tatoeba-derived) ----------------
def italian_sentences(n):
    path = os.path.join(CACHE, "ita_sentences.tsv.bz2")
    if not os.path.exists(path):
        urllib.request.urlretrieve("https://downloads.tatoeba.org/exports/per_language/ita/ita_sentences.tsv.bz2", path)
    out = []
    with bz2.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) == 3 and 8 <= len(p[2]) <= 90: out.append(p[2])
    random.shuffle(out); return out[:n]

def english_sentences(n):
    path = os.path.join(CACHE, "eng_sentences.tsv.bz2")
    if not os.path.exists(path):
        urllib.request.urlretrieve("https://downloads.tatoeba.org/exports/per_language/eng/eng_sentences.tsv.bz2", path)
    out = []
    with bz2.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) == 3 and 8 <= len(p[2]) <= 90: out.append(p[2])
    random.shuffle(out); return out[:n]

# Hand-picked IT/EN translation pairs to MEASURE cross-lingual alignment (not for training).
PROBES = [
    ("apri le foto", "open the photos"), ("vorrei ascoltare musica", "i want to listen to music"),
    ("che ora e", "what time is it"), ("quanto spazio sulla scheda", "how much space on the card"),
    ("crea un file di testo", "create a text file"), ("qual e la capitale della francia", "what is the capital of france"),
    ("chi sei", "who are you"), ("apri le impostazioni", "open the settings"),
    ("mostrami i file", "show me the files"), ("quanta batteria", "how much battery"),
    ("apri la calcolatrice", "open the calculator"), ("aiutami", "help me"),
]

# ---- teacher embeddings (e5 mean-pooling) -----------------------------------------------
@torch.no_grad()
def teacher_embed(sents, bs=256):
    tok = AutoTokenizer.from_pretrained(TEACHER); mdl = AutoModel.from_pretrained(TEACHER).to(DEV).eval()
    out = []
    for i in range(0, len(sents), bs):
        enc = tok(["query: " + s for s in sents[i:i+bs]], padding=True, truncation=True, max_length=64, return_tensors="pt").to(DEV)
        h = mdl(**enc).last_hidden_state; m = enc["attention_mask"].unsqueeze(-1).float()
        e = (h * m).sum(1) / m.sum(1).clamp(min=1e-9)
        out.append(torch.nn.functional.normalize(e, dim=1).cpu())
        if i % (bs*20) == 0: print(f"  teacher {i}/{len(sents)}")
    return torch.cat(out)

class Student(nn.Module):
    def __init__(self):
        super().__init__(); self.bag = nn.EmbeddingBag(H, D, mode="sum"); nn.init.normal_(self.bag.weight, std=0.05)
    def forward(self, ids, off): return torch.nn.functional.normalize(self.bag(ids, off), dim=1)

def main():
    t0 = time.time()
    it = italian_sentences(N_IT)
    en = english_sentences(N_EN)
    sents = it + en
    lang = np.array([0]*len(it) + [1]*len(en))   # 0=IT, 1=EN
    print(f"[anima] corpus: {len(it)} IT + {len(en)} EN = {len(sents)} sentences")

    T = teacher_embed(sents); print(f"[anima] teacher done {time.time()-t0:.0f}s")
    Tc = T - T.mean(0, keepdim=True)
    _, _, V = torch.pca_lowrank(Tc, q=D, niter=4)
    target = torch.nn.functional.normalize(Tc @ V[:, :D], dim=1).to(DEV)

    # Pre-hash every sentence once.
    flat, offs, o = [], [0], 0
    for s in sents:
        f = feats(s); flat.extend(f); o += len(f); offs.append(o)
    flat = torch.tensor(flat, dtype=torch.long)
    offs = torch.tensor(offs, dtype=torch.long)

    def fwd(model, indices):                       # forward a list of sentence indices
        ids, boff, oo = [], [], 0
        for j in indices:
            a, b = offs[j].item(), offs[j+1].item(); ids.append(flat[a:b]); boff.append(oo); oo += b - a
        return model(torch.cat(ids).to(DEV), torch.tensor(boff, dtype=torch.long).to(DEV))

    model = Student().to(DEV); opt = torch.optim.Adam(model.parameters(), lr=2e-3)
    N = len(sents); tr = int(N*0.97); order = list(range(tr))
    for ep in range(EPOCHS):
        model.train(); random.shuffle(order); dl = 0.0; nb = 0
        for i in range(0, tr, 1024):
            b = order[i:i+1024]
            pred = fwd(model, b); loss = (1 - (pred * target[torch.tensor(b)]).sum(1)).mean()
            opt.zero_grad(); loss.backward(); opt.step(); dl += loss.item(); nb += 1
        print(f"[anima] epoch {ep+1}/{EPOCHS}  distill={dl/nb:.4f}")

    # ---- eval ---------------------------------------------------------------------------
    model.eval()
    @torch.no_grad()
    def emb(indices): return fwd(model, indices).cpu()
    @torch.no_grad()
    def enc_str(s):
        f = feats(s); return model(torch.tensor(f).to(DEV), torch.tensor([0]).to(DEV)).cpu()[0]
    te = list(range(tr, N)); E = emb(te)
    for L, name in [(0, "IT"), (1, "EN")]:
        sub = [k for k, j in enumerate(te) if lang[j] == L]
        if len(sub) < 50: continue
        a = [sub[x] for x in np.random.randint(0, len(sub), 2000)]; b = [sub[x] for x in np.random.randint(0, len(sub), 2000)]
        st = (E[a]*E[b]).sum(1).numpy(); tt = (T[[te[x] for x in a]]*T[[te[x] for x in b]]).sum(1).numpy()
        print(f"[anima] EVAL Spearman(student vs e5) {name}: {spearmanr(tt, st).correlation:.3f}")
    # cross-lingual: each EN probe should retrieve its own IT translation among the IT probes
    itp = torch.nn.functional.normalize(torch.stack([enc_str(a) for a, _ in PROBES]), dim=1)
    enp = torch.nn.functional.normalize(torch.stack([enc_str(b) for _, b in PROBES]), dim=1)
    sims = enp @ itp.T
    r1 = (sims.argmax(1) == torch.arange(len(PROBES))).float().mean().item()
    diag = sims.diag().mean().item()
    print(f"[anima] EVAL cross-lingual recall@1 (EN->IT probes n={len(PROBES)}): {r1*100:.0f}%  mean cos(translation)={diag:.2f}")
    for k in range(min(4, len(PROBES))):
        print(f"    '{PROBES[k][1]}' -> '{PROBES[sims[k].argmax()][0]}'  (cos {sims[k].max():.2f})")

    # ---- export ANE2 (int8) -------------------------------------------------------------
    W = model.bag.weight.detach().cpu().numpy(); scale = float(np.abs(W).max())/127.0
    q = np.clip(np.round(W/scale), -127, 127).astype(np.int8)
    binp = os.path.join(OUTDIR, "anima-it-encoder.bin")
    with open(binp, "wb") as f:
        f.write(b"ANE2"); f.write(struct.pack("<IIII", H, D, len(NGRAMS), WORD_N))
        f.write(struct.pack("<"+"I"*len(NGRAMS), *NGRAMS)); f.write(struct.pack("<f", scale)); f.write(q.tobytes())
    json.dump({"format":"ANE2","rows":H,"dim":D,"ngrams":list(NGRAMS),"word_n":WORD_N,"scale":scale,
               "teacher":TEACHER,"bilingual":True,"crosslingual_recall1":round(r1,3),"bytes":os.path.getsize(binp)},
              open(os.path.join(OUTDIR,"anima-it-encoder.json"),"w"), indent=2)
    print(f"[anima] exported {binp} ({os.path.getsize(binp)/1024:.0f} KB int8, bilingual)")

if __name__ == "__main__":
    main()
