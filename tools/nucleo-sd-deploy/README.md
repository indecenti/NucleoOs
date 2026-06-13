# NucleoOS — SD Deploy

App **Python + Tkinter** per il provisioning della SD del Cardputer: prepara una SD
**vuova/nuova** da zero, **aggiorna** una SD esistente senza perdere lo stato del device,
e **verifica** che tutto sia a posto.

Nasce per risolvere la frammentazione delle vecchie pipeline:
`deploy.ps1` non portava i **46 shard akb5** (la conoscenza ANIMA v5), `sd-sync.ps1`
usava un payload `deploy/sd-safe` stale, e i **wallpaper** vivevano solo sulla SD.
Qui un **unico assemblatore dichiarativo** raccoglie da tutte le sorgenti canoniche,
produce un **master verificabile** (con manifest SHA-256) e lo scrive in sicurezza.

## Uso (GUI)

```
python tools/nucleo-sd-deploy/sd_deploy.py
```

1. **Assembla master** — raccoglie il payload completo in `deploy/sd-master/` e ne calcola il manifest. Segnala in rosso ogni pezzo mancante (es. akb5 < 40 shard).
2. **Scegli l'unità** — solo unità rimovibili sono consigliate; il disco di sistema è **bloccato**. La app rileva se la SD è *vuota*, *NucleoOS* o *estranea*.
3. **Operazione**:
   - **Provisiona** (SD vuota): scrive tutto il payload + crea le cartelle utente + template di stato **puliti** (chiave Groq vuota, nessuna card imparata).
   - **Aggiorna** (SD esistente): aggiorna app/www/registry/conoscenza ANIMA **preservando** chiave, card imparate, impostazioni e dati utente. Non cancella mai.
   - **Verifica**: confronto hash master↔SD, elenca mancanti/diversi.
4. **Anteprima (dry-run)** mostra cosa farebbe senza scrivere. Consigliata prima di ogni scrittura reale.

## Uso (CLI, senza UI)

```
python sd_deploy.py assemble            # costruisci il master + manifest
python sd_deploy.py drives              # elenca le unità
python sd_deploy.py provision H:\       # SD vuota -> payload completo (+ --dry)
python sd_deploy.py update H:\          # aggiorna preservando lo stato (+ --dry)
python sd_deploy.py verify H:\          # confronto hash
```

## Cosa finisce sulla SD (source map)

| Destinazione SD | Sorgente canonica |
|---|---|
| `system/registry/` | `registry/` |
| `apps/` (+ `.gz`) | `apps/` *(sorgenti complete, incl. tour.js/nlcommand.js)* |
| `www/shell/` (+ `.gz`) | `web/shell/` |
| `data/` (seed utente + base ANIMA) | `tools/sd-sim/data/` |
| `data/anima/akb5/` (46 shard) | `deploy/sd-safe/data/anima/akb5/` |
| `data/anima/anima-it-akb5.bin` | `deploy/sd-safe/…` → fallback `models/` |
| `evilportal/`, `wallpapers/`, `README.md` | `deploy/sd-safe/` |

## Stato device — mai sovrascritto su *Aggiorna*, ricreato pulito su *Provisiona*

`data/anima/teacher.json` (chiave), `data/anima/learned/*` (card + `.vec`),
telemetry/session/workspace, `system/config/*`, `config/`, `backups/`, `journal/`,
`*.vec`, `auth.json`, `volume.json`, `settings.json`.

## Sicurezza

- Scrive **solo** su unità rimovibili (avviso forte sui dischi fissi, **blocco** sul disco di sistema).
- Scritture **atomiche** per-file (`.nctmp` + rename), skip per hash dei file invariati.
- **Mai** cancella in modalità *Aggiorna*; lo stato del device è protetto da un elenco esplicito.
- `Anteprima` per validare prima di toccare la SD.

## Note

- Richiede solo la **standard library** (tkinter, ctypes, hashlib, gzip). Niente pip.
- Windows-first (enumerazione unità via WinAPI). Su altri OS la GUI parte ma la lista unità è vuota → usa la CLI con un percorso di mount.
- Il master è in `deploy/sd-master/` (ri-assemblabile, con `.deploy-manifest.json`).
