# deploy/sd-safe — payload SD sicuro (pronto alla copia)

Questa cartella contiene **solo** gli asset statici di sistema di NucleoOS, quelli che
si possono ricopiare sulla SD del Cardputer **in qualsiasi momento senza distruggere
nulla**:

```
apps/                 app web (UI)
www/                  shell PWA
system/registry/      registry app, manuali, web-api-spec
data/anima/           pack del modello ANIMA:
                        anima-it-encoder.bin   (3.1 MB, magic ANE2)
                        anima-it-encoder.json
                        anima-it-index.bin     (1.6 MB, magic AKB3)
                        commands.it.json
```

## Cosa NON c'è qui (di proposito)

Stato runtime/utente che vive **solo sul device** e non va mai sovrascritto:

- `data/anima/teacher.json` — chiave Groq / config online
- `data/anima/learned/**` — card imparate da ANIMA (+ `.vec`)
- `data/anima/telemetry.ndjson`, `session.txt`, `.httptrace`
- `system/config/**` — impostazioni utente (create a runtime)
- `config/**`, `backups/`, `journal/`
- media utente (`data/Documents`, `Music`, `Pictures`, `ROMs`, `DOS`, `Videos`, ...)
  → questi stanno nell'immagine completa `deploy/sd/` solo per il **primo**
    provisioning; non vanno ricopiati sopra ai file dell'utente.

## Come copiare in sicurezza

Usa lo script (non cancella mai, protegge lo stato del device):

```powershell
pwsh tools/sd-sync.ps1 -Target H:\ -WhatIf   # anteprima
pwsh tools/sd-sync.ps1 -Target H:\           # copia reale
```

I pack ANIMA (`encoder`/`index`) devono restare allineati al firmware:
il loro schema di hashing deve combaciare byte-per-byte con `tools/anima/distill.py`
e `build_index.py`. Se rigeneri i modelli, aggiorna anche questa cartella.
