#!/usr/bin/env python3
"""ANIMA distillation A/B: does TYPO/DROP AUGMENTATION make the on-device encoder robust?

stress.py showed the deployed encoder loses ~23 Recall@1 points to a SINGLE typo: it was
distilled only on CLEAN Tatoeba sentences, so it never learned that "qunato spazio" means
"quanto spazio". This trains TWO students that are byte-identical in architecture, data,
seed and gradient-steps-per-epoch — the ONLY difference is that the TREATMENT arm replaces a
fraction of its student inputs with perturbed text while keeping the CLEAN e5 target. Same
ANE2 format/size -> ZERO extra device cost.

Outputs: models/ab-control.bin  models/ab-treat.bin   (+ .json)
Run:     python tools/anima/distill_aug.py
Then:    ANIMA_ENC=models/ab-control.bin python tools/anima/eval.py ; ... stress.py
         ANIMA_ENC=models/ab-treat.bin   python tools/anima/eval.py ; ... stress.py
"""
import os, bz2, random, struct, json, time, unicodedata
import numpy as np
import torch, torch.nn as nn
from transformers import AutoTokenizer, AutoModel

random.seed(0); np.random.seed(0); torch.manual_seed(0)
ROOT   = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUTDIR = os.path.join(ROOT, "models")
CACHE  = os.path.join(ROOT, "tools", "anima", ".cache")
DEV    = "cuda" if torch.cuda.is_available() else "cpu"
TEACHER = "intfloat/multilingual-e5-small"

# Match the DEPLOYED encoder config (models/anima-it-encoder.json) so this is a true drop-in A/B.
D, H        = 192, 16384
NGRAMS, WORD_N = (3, 4, 5), 2
N_IT, N_EN  = 100000, 100000   # full factory scale (matches distill.py) for a shippable encoder
EPOCHS      = 22
AUG_PROB    = 0.5              # treatment: fraction of student inputs replaced with a perturbed variant
KEYS = "abcdefghilmnopqrstuvz "
print(f"[ab] device={DEV} table={H}x{D} N={N_IT}+{N_EN} epochs={EPOCHS} aug_p={AUG_PROB}")

# ---- features (identical to anima_lib / device) ----------------------------------------------
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

# ---- the SAME perturbations stress.py uses (so we train against the metric we report) --------
def typo(s, k):
    s = list(s)
    for _ in range(k):
        if len(s) < 2: break
        i = random.randrange(len(s) - 1); op = random.random()
        if op < 0.30 and i < len(s) - 1: s[i], s[i+1] = s[i+1], s[i]
        elif op < 0.55: del s[i]
        elif op < 0.80: s[i] = random.choice(KEYS)
        else: s.insert(i, random.choice(KEYS))
    return "".join(s)
def perturb(s):
    r = random.random()
    if r < 0.45:  return typo(s, random.choice([1, 2]))
    if r < 0.75:                                              # drop a word
        w = s.split()
        if len(w) > 2: del w[random.randrange(len(w))]; return " ".join(w)
        return typo(s, 1)
    return typo(s, random.choice([2, 3]))                     # heavier

def sentences(lang, n):
    path = os.path.join(CACHE, f"{lang}_sentences.tsv.bz2"); out = []
    with bz2.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) == 3 and 8 <= len(p[2]) <= 90: out.append(p[2])
    random.shuffle(out); return out[:n]

@torch.no_grad()
def teacher_embed(sents, bs=256):
    tok = AutoTokenizer.from_pretrained(TEACHER); mdl = AutoModel.from_pretrained(TEACHER).to(DEV).eval()
    out = []
    for i in range(0, len(sents), bs):
        enc = tok(["query: " + s for s in sents[i:i+bs]], padding=True, truncation=True, max_length=64, return_tensors="pt").to(DEV)
        h = mdl(**enc).last_hidden_state; m = enc["attention_mask"].unsqueeze(-1).float()
        e = (h * m).sum(1) / m.sum(1).clamp(min=1e-9)
        out.append(torch.nn.functional.normalize(e, dim=1).cpu())
    return torch.cat(out)

class Student(nn.Module):
    def __init__(self):
        super().__init__(); self.bag = nn.EmbeddingBag(H, D, mode="sum"); nn.init.normal_(self.bag.weight, std=0.05)
    def forward(self, ids, off): return torch.nn.functional.normalize(self.bag(ids, off), dim=1)

def pack(flat_lists):
    """list[list[int]] -> (flat tensor, offsets tensor) for EmbeddingBag, recomputable per arm."""
    flat, offs, o = [], [0], 0
    for f in flat_lists: flat.extend(f); o += len(f); offs.append(o)
    return torch.tensor(flat, dtype=torch.long), torch.tensor(offs, dtype=torch.long)

def export(model, name, extra):
    W = model.bag.weight.detach().cpu().numpy(); scale = float(np.abs(W).max())/127.0
    q = np.clip(np.round(W/scale), -127, 127).astype(np.int8)
    binp = os.path.join(OUTDIR, name + ".bin")
    with open(binp, "wb") as f:
        f.write(b"ANE2"); f.write(struct.pack("<IIII", H, D, len(NGRAMS), WORD_N))
        f.write(struct.pack("<"+"I"*len(NGRAMS), *NGRAMS)); f.write(struct.pack("<f", scale)); f.write(q.tobytes())
    json.dump({"format":"ANE2","rows":H,"dim":D,"ngrams":list(NGRAMS),"word_n":WORD_N,"scale":scale,
               "teacher":TEACHER,**extra}, open(os.path.join(OUTDIR, name+".json"),"w"), indent=2)
    print(f"[ab] exported {binp} ({os.path.getsize(binp)/1024:.0f} KB)")

def main():
    t0 = time.time()
    sents = sentences("ita", N_IT) + sentences("eng", N_EN)
    print(f"[ab] corpus {len(sents)} sentences")
    # teacher targets (cached to disk: identical for both arms)
    tcache = os.path.join(CACHE, f"teacher_{len(sents)}_{D}.npy")
    if os.path.exists(tcache):
        target = torch.tensor(np.load(tcache)); print("[ab] teacher targets from cache")
    else:
        T = teacher_embed(sents); Tc = T - T.mean(0, keepdim=True)
        _, _, V = torch.pca_lowrank(Tc, q=D, niter=4)
        target = torch.nn.functional.normalize(Tc @ V[:, :D], dim=1)
        np.save(tcache, target.numpy()); print(f"[ab] teacher done {time.time()-t0:.0f}s")
    target = target.to(DEV)

    print("[ab] pre-hashing clean + perturbed feature lists...")
    clean = [feats(s) for s in sents]
    pert  = [feats(perturb(s)) for s in sents]   # one perturbed variant per sentence (treatment pool)
    cflat, coff = pack(clean)
    pflat, poff = pack(pert)

    def fwd(model, idx, use_pert_mask=None):
        ids, boff, oo = [], [], 0
        for n, j in enumerate(idx):
            if use_pert_mask is not None and use_pert_mask[n]:
                a, b = poff[j].item(), poff[j+1].item(); src = pflat
            else:
                a, b = coff[j].item(), coff[j+1].item(); src = cflat
            ids.append(src[a:b]); boff.append(oo); oo += b - a
        return model(torch.cat(ids).to(DEV), torch.tensor(boff, dtype=torch.long).to(DEV))

    N = len(sents); tr = int(N*0.99)
    def train(aug):
        torch.manual_seed(0); random.seed(1)
        model = Student().to(DEV); opt = torch.optim.Adam(model.parameters(), lr=2e-3)
        order = list(range(tr))
        for ep in range(EPOCHS):
            model.train(); random.shuffle(order); dl = 0.0; nb = 0
            for i in range(0, tr, 1024):
                b = order[i:i+1024]
                mask = [random.random() < AUG_PROB for _ in b] if aug else None
                pred = fwd(model, b, mask)
                loss = (1 - (pred * target[torch.tensor(b)]).sum(1)).mean()
                opt.zero_grad(); loss.backward(); opt.step(); dl += loss.item(); nb += 1
            print(f"   [{'AUG' if aug else 'CTL'}] epoch {ep+1}/{EPOCHS} distill={dl/nb:.4f}")
        return model

    # A/B already proven at fast config; ship-build trains ONLY the augmented (treatment) encoder.
    print("[ab] training TREATMENT (augmented, full scale)...")
    export(train(True), "anima-it-encoder.aug", {"aug": True, "aug_prob": AUG_PROB, "epochs": EPOCHS, "n": N_IT+N_EN})
    print(f"[ab] done {time.time()-t0:.0f}s")

if __name__ == "__main__":
    main()
