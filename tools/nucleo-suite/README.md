# NucleoOS Toolkit

Launcher unico (GUI Tkinter, a carosello) per gli strumenti **PC-side** del
Cardputer NucleoOS. Raccoglie in un solo posto i tool utili all'utente e li
avvia ciascuno nel proprio processo; pensato per essere impacchettato in un
**`.exe`** distribuibile.

```
python tools/nucleo-suite/launcher.py        # avvia il launcher
python tools/nucleo-suite/launcher.py --selftest   # verifica il wiring, senza GUI
```

## Strumenti inclusi

| Tool | Cat. | Tipo | Da .exe | Cosa fa |
|------|------|------|:------:|---------|
| **NFV Video Converter** | Video | GUI | ✅ | Converte qualsiasi video in clip `.nfv` per il device |
| **NFV Reindex** | Video | CLI | ✅ | Aggiunge l'indice di seek alle vecchie clip `.nfv` |
| **SD Deploy** | Sistema | GUI | ⛔ | Provisiona/aggiorna la SD del Cardputer |
| **Flasher** | Sistema | GUI | ⛔ | Build / flash USB / deploy SD / OTA |
| **Boot Log** | Diagnostica | CLI | ✅ | Cattura il log di avvio via seriale |
| **Serial Monitor** | Diagnostica | CLI | ✅ | Registra il log seriale senza resettare il device |

I tool **⛔** dipendono dal repo e dalla toolchain (ESP-IDF, master della SD):
funzionano da sorgente, ma non hanno senso in un `.exe` autonomo — nel pacchetto
appaiono disattivati con una nota.

Cosa **non** è incluso, di proposito: gli strumenti interni di sviluppo/AI
(`tools/anima/*`, gate, eval, test-lab, `npx_gen`, `train_wiki`, generatori voce
e manifest, diagnostica NFV).

## Architettura (perché funziona anche da `.exe`)

- `tools_registry.py` è la **fonte unica**: descrive ogni tool e il suo percorso
  `.py` relativo alla radice del repo.
- Da **sorgente** il launcher esegue direttamente lo script reale del repo
  (i GUI con `pythonw`, i CLI con output nel pannello).
- Da **`.exe`** il launcher ri-lancia sé stesso con `--run <id>`: l'eseguibile
  congelato contiene gli script (rispecchiati nella stessa struttura `tools/...`)
  e li esegue via `runpy`. Stessa stringa di percorso, due modalità.

## Creare il `.exe`

Serve PyInstaller (`pip install pyinstaller`). Poi:

```powershell
powershell -ExecutionPolicy Bypass -File tools\nucleo-suite\build_exe.ps1
```

Output: `tools/nucleo-suite/dist/NucleoSuite/NucleoSuite.exe` — una **cartella
unica** da zippare e distribuire.

> ⚠️ **ffmpeg** non è incluso nel bundle: deve stare sul `PATH` dell'utente
> finale perché il convertitore video funzioni.

## Aggiungere un nuovo tool

Aggiungi un dizionario in `TOOLS` dentro `tools_registry.py` (vedi i campi
documentati lì). Se deve funzionare anche da `.exe`, metti `frozen_ok: True` e
aggiungi il suo `.py` (e le eventuali dipendenze) a `NucleoSuite.spec`.
