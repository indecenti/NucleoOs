#!/usr/bin/env python3
"""ANIMA offline factory — build the on-device semantic index (RAG over labelled examples).

Encodes Italian example phrasings with the EXACT int8 device encoder (numpy reimplementation
of FNV-1a + EmbeddingBag-sum from anima-it-encoder.bin), so the index and the ESP32 agree
bit-for-bit. At runtime the device encodes the query the same way and returns the nearest
example's action/answer -> it understands paraphrases/synonyms, not keywords.

Outputs:  models/anima-it-index.bin   (loaded into RAM on the device, ~tens of KB)
Run:  python tools/anima/build_index.py   (needs models/anima-it-encoder.bin from distill.py)
"""
import os, struct, unicodedata, numpy as np

ROOT  = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENC   = os.path.join(ROOT, "models", "anima-it-encoder.bin")
OUT   = os.path.join(ROOT, "models", "anima-it-index.bin")

# ---- load the int8 device encoder -------------------------------------------------------
with open(ENC, "rb") as f:
    assert f.read(4) == b"ANE2"
    H, D, nn, WORD_N = struct.unpack("<IIII", f.read(16))
    NGRAMS = struct.unpack("<" + "I"*nn, f.read(4*nn))
    (scale,) = struct.unpack("<f", f.read(4))
    table = np.frombuffer(f.read(H*D), dtype=np.int8).reshape(H, D).astype(np.int32)
print(f"[anima] encoder {H}x{D} ngrams={NGRAMS} word_n={WORD_N}")

def fnv1a(b):
    h = 0x811c9dc5
    for x in b: h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h

def norm_text(s):
    s = unicodedata.normalize("NFD", s.lower())
    s = "".join(c for c in s if unicodedata.category(c) != "Mn")
    s = "".join(c if c.isalnum() else " " for c in s)
    return " ".join(s.split())

def feats(s):   # MUST match tools/anima/distill.py feats()
    words = norm_text(s).split()
    ids = []
    t = "^" + "^".join(words) + "$"
    for n in NGRAMS:
        for i in range(len(t)-n+1):
            ids.append(fnv1a(b"\x01" + t[i:i+n].encode()) % H)
    for w in words:
        ids.append(fnv1a(b"\x02" + w.encode()) % H)
    for i in range(len(words)-1):
        ids.append(fnv1a(b"\x02" + (words[i] + " " + words[i+1]).encode()) % H)
    return ids or [fnv1a(b"\x01^$") % H]

def encode_i8(s):
    acc = np.zeros(D, dtype=np.int32)
    for fid in feats(s):
        acc += table[fid]
    v = acc.astype(np.float32)
    nrm = np.linalg.norm(v)
    if nrm > 0: v /= nrm
    return np.clip(np.round(v*127), -127, 127).astype(np.int8)

# ---- labelled examples (Italian paraphrases -> action). action: launch|system|answer ----
# Each label: (action, arg, reply). Phrasings are deliberately varied to test generalization.
LABELS = [
    ("launch", "photo-viewer",  "Apro le foto.",            ["apri le foto","mostrami le immagini","fammi vedere le fotografie","voglio guardare le foto","apri la galleria","fammi vedere le immagini"]),
    ("launch", "media-player",  "Apro la musica.",          ["apri la musica","voglio ascoltare musica","metti un po' di musica","fammi sentire una canzone","riproduci musica","apri il lettore musicale"]),
    ("launch", "file-commander","Apro i file.",             ["apri i file","mostrami i documenti","esplora la scheda","voglio vedere le cartelle","apri il gestore file"]),
    ("launch", "calculator",    "Apro la calcolatrice.",    ["apri la calcolatrice","devo fare un calcolo","fammi fare due conti","voglio calcolare qualcosa"]),
    ("launch", "clock",         "Apro l'orologio.",         ["apri l'orologio","mostrami il cronometro","voglio un timer","apri la sveglia"]),
    ("launch", "calendar",      "Apro il calendario.",      ["apri il calendario","mostrami l'agenda","che appuntamenti ho","apri gli eventi"]),
    ("launch", "settings",      "Apro le impostazioni.",    ["apri le impostazioni","voglio cambiare le opzioni","apri la configurazione","modifica i settaggi"]),
    ("launch", "recorder",      "Apro il registratore.",    ["apri il registratore","voglio registrare la voce","registra un memo","usa il microfono"]),
    ("system", "time",          "{value}.",                 ["che ora e","che ore sono","mi dici l'ora","dimmi l'orario","sai che ora e adesso"]),
    ("system", "storage",       "Spazio SD: {value}.",      ["quanto spazio ho","quanta memoria resta sulla scheda","spazio libero sulla sd","quanto e piena la scheda"]),
    ("answer", "",              "Sono ANIMA, l'assistente offline di NucleoOS. Funziono senza internet, sul dispositivo.",
                                                            ["chi sei","come ti chiami","cosa sei","presentati"]),
    ("answer", "",              "Posso aprire le app (foto, musica, file, calcolatrice...), dirti l'ora e lo spazio. Chiedimi quello che vuoi.",
                                                            ["cosa sai fare","aiutami","quali comandi conosci","cosa puoi fare per me","elenca le funzioni"]),
]

# ---- knowledge base (RAG): facts ANIMA can answer. action=answer, 1 phrasing each. -------
# The distilled encoder generalizes phrasing, so one canonical question per fact suffices.
def gen_knowledge():
    kb = []
    CAPITALS = {
        "Francia":"Parigi","Italia":"Roma","Spagna":"Madrid","Germania":"Berlino","Portogallo":"Lisbona",
        "Regno Unito":"Londra","Irlanda":"Dublino","Paesi Bassi":"Amsterdam","Belgio":"Bruxelles","Svizzera":"Berna",
        "Austria":"Vienna","Grecia":"Atene","Polonia":"Varsavia","Svezia":"Stoccolma","Norvegia":"Oslo",
        "Danimarca":"Copenaghen","Finlandia":"Helsinki","Russia":"Mosca","Ucraina":"Kiev","Turchia":"Ankara",
        "Stati Uniti":"Washington","Canada":"Ottawa","Messico":"Citta del Messico","Brasile":"Brasilia","Argentina":"Buenos Aires",
        "Cina":"Pechino","Giappone":"Tokyo","India":"Nuova Delhi","Australia":"Canberra","Egitto":"Il Cairo",
        "Marocco":"Rabat","Sudafrica":"Pretoria","Tunisia":"Tunisi","Croazia":"Zagabria","Romania":"Bucarest",
    }
    for paese, cap in CAPITALS.items():
        kb.append((f"La capitale di {paese} e {cap}.", [f"qual e la capitale di {paese}", f"capitale {paese}"]))
    REGIONI = {
        "Lombardia":"Milano","Lazio":"Roma","Campania":"Napoli","Sicilia":"Palermo","Veneto":"Venezia",
        "Piemonte":"Torino","Toscana":"Firenze","Puglia":"Bari","Liguria":"Genova","Sardegna":"Cagliari",
        "Emilia-Romagna":"Bologna","Calabria":"Catanzaro",
    }
    for reg, cap in REGIONI.items():
        kb.append((f"Il capoluogo della {reg} e {cap}.", [f"qual e il capoluogo della {reg}", f"capoluogo {reg}"]))
    FACTS = [
        ("Il sole e una stella attorno a cui orbita la Terra.", ["cos e il sole","che cos e il sole"]),
        ("La Terra impiega circa 365 giorni per orbitare il sole.", ["quanti giorni dura un anno","durata di un anno"]),
        ("L'acqua e composta da idrogeno e ossigeno (H2O).", ["di cosa e fatta l'acqua","composizione dell'acqua"]),
        ("Un chilometro equivale a 1000 metri.", ["quanti metri in un chilometro","quanti metri ha un km"]),
        ("Un'ora ha 60 minuti.", ["quanti minuti in un'ora","minuti in un ora"]),
        ("Il monte piu alto del mondo e l'Everest.", ["qual e la montagna piu alta","monte piu alto del mondo"]),
        ("L'oceano piu grande e il Pacifico.", ["qual e l'oceano piu grande","oceano piu grande"]),
        ("Il pianeta piu grande del sistema solare e Giove.", ["qual e il pianeta piu grande","pianeta piu grande"]),
    ]
    kb += FACTS
    OS = [
        ("NucleoOS gira su un M5Stack Cardputer (ESP32-S3) e funziona offline.", ["cos e nucleoos","che sistema sei"]),
        ("Per cambiare rete apri Impostazioni o l'app Network sul dispositivo.", ["come cambio rete wifi","come mi collego al wifi"]),
        ("Per liberare spazio usa il Cestino o il gestore file.", ["come libero spazio","come cancello i file"]),
    ]
    kb += OS
    # -> LABELS entries: (action, arg, reply, phrasings)
    return [("answer", "", ans, phr) for ans, phr in kb]

LABELS += gen_knowledge()
print(f"[anima] labels: {len(LABELS)} (commands + knowledge)")

# ---- build vectors, hold out one phrasing per label for the eval -------------------------
vecs, vlabel, train_phr, eval_set = [], [], [], []
for li, (act, arg, reply, phrs) in enumerate(LABELS):
    for k, p in enumerate(phrs):
        if k == len(phrs)-1 and len(phrs) > 3:   # hold out the last phrasing
            eval_set.append((p, li)); continue
        vecs.append(encode_i8(p)); vlabel.append(li); train_phr.append(p)
vecs = np.stack(vecs)

# ---- eval: do held-out paraphrases retrieve the right label? ----------------------------
def nearest(s):
    q = encode_i8(s).astype(np.int32)
    sims = vecs.astype(np.int32) @ q
    return int(sims.argmax()), sims
ok = 0
print("\n[anima] held-out paraphrase -> predicted label:")
for p, li in eval_set:
    bi, sims = nearest(p)
    pred = vlabel[bi]; good = pred == li; ok += good
    print(f"  [{'OK ' if good else 'XX '}] '{p}'  ->  {LABELS[pred][0]}:{LABELS[pred][1]}  (via '{train_phr[bi]}')")
print(f"[anima] held-out accuracy: {ok}/{len(eval_set)} = {ok/len(eval_set)*100:.0f}%")

# ---- export device index: AKB1 | D,count,nlabels | labels[] | (int8[D] vec, u16 label)[] -
def cstr(s): b = s.encode("utf-8")[:250]; return struct.pack("<H", len(b)) + b
with open(OUT, "wb") as f:
    f.write(b"AKB1"); f.write(struct.pack("<III", D, len(vecs), len(LABELS)))
    for act, arg, reply, _ in LABELS:
        f.write(struct.pack("<B", {"launch":1,"system":2,"answer":3}[act]))
        f.write(cstr(arg)); f.write(cstr(reply))
    for v, li in zip(vecs, vlabel):
        f.write(v.tobytes()); f.write(struct.pack("<H", li))
print(f"[anima] exported {OUT}  ({os.path.getsize(OUT)/1024:.1f} KB, {len(vecs)} vectors, RAM-resident)")
