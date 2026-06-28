# -*- coding: utf-8 -*-
"""
Registro dei tool della NucleoOS Toolkit — bilingue IT/EN.
NucleoOS Toolkit tool registry — bilingual IT/EN.

Fonte UNICA usata sia dal launcher (launcher.py) sia dal build .exe
(NucleoSuite.spec). I percorsi `script` sono relativi alla radice del repo e
vengono rispecchiati 1:1 dentro il bundle PyInstaller.

I campi testuali localizzabili (title, tagline, description, features, requires,
e le label dei params) sono dizionari {"it": ..., "en": ...}. Il launcher sceglie
la lingua con L(). I campi NON testuali (id, cat, glyph, accent, script, kind,
frozen_ok, needs_repo) restano semplici.

cat = chiave categoria STABILE ("video" | "system" | "diag"): il filtro lavora su
questa, i nomi visualizzati arrivano da CATS (localizzati).
"""

TOOLS = [
    {
        "id": "nfv-converter",
        "cat": "video",
        "glyph": "film",
        "accent": "#ff7a59",
        "script": "tools/nfv/studio_gui.py",
        "kind": "gui",
        "frozen_ok": True,
        "needs_repo": False,
        "title": {"it": "NFV Video Converter", "en": "NFV Video Converter"},
        "tagline": {
            "it": "Trasforma qualsiasi video in una clip riproducibile sul Cardputer",
            "en": "Turn any video into a clip playable on the Cardputer",
        },
        "description": {
            "it": ("Studio drag-and-drop che converte un filmato qualsiasi nel formato video "
                   "nativo di NucleoOS. Produce un file .nfv (MJPEG 240×135) con audio "
                   "integrato, calibrato per lo schermo da 1.14\" e per l'ESP32-S3 senza PSRAM."),
            "en": ("Drag-and-drop studio that converts any movie into NucleoOS' native video "
                   "format. It outputs a .nfv file (MJPEG 240×135) with embedded audio, tuned "
                   "for the 1.14\" panel and the PSRAM-less ESP32-S3."),
        },
        "features": {
            "it": [
                "NFV v3 tile-delta: redraw solo dei riquadri che cambiano, 3-5× più piccolo",
                "Compatibilità NFV v2 (full-frame + .mp3) per le build più vecchie",
                "Trim, crop/pad, FPS e qualità regolabili; decodifica GPU NVIDIA opzionale",
            ],
            "en": [
                "NFV v3 tile-delta: redraws only the tiles that change, 3-5× smaller",
                "NFV v2 compatibility (full-frame + .mp3) for older builds",
                "Trim, crop/pad, FPS and quality tunable; optional NVIDIA GPU decode",
            ],
        },
        "requires": {
            "it": ["ffmpeg sul PATH", "pillow", "numpy", "(opz.) tkinterdnd2"],
            "en": ["ffmpeg on PATH", "pillow", "numpy", "(opt.) tkinterdnd2"],
        },
    },
    {
        "id": "nfv-reindex",
        "cat": "video",
        "glyph": "reindex",
        "accent": "#ffb454",
        "script": "tools/nfv/reindex.py",
        "kind": "cli",
        "frozen_ok": True,
        "needs_repo": False,
        "title": {"it": "NFV Reindex", "en": "NFV Reindex"},
        "tagline": {
            "it": "Aggiunge l'indice di seek alle vecchie clip .nfv (resume istantaneo)",
            "en": "Adds a seek index to old .nfv clips (instant resume)",
        },
        "description": {
            "it": ("Aggiorna sul posto le clip NFV v1 create prima dell'indice di ricerca: "
                   "evita la lenta scansione \"Preparing...\" sul device. Non ri-codifica, "
                   "accoda solo la tabella offset e ritocca l'header (crash-safe)."),
            "en": ("Upgrades in place the NFV v1 clips made before the seek index existed: it "
                   "avoids the slow on-device \"Preparing...\" scan. It does not re-encode — it "
                   "only appends the offset table and patches the header (crash-safe)."),
        },
        "features": {
            "it": [
                "Opera su un singolo .nfv o su un'intera cartella (es. la SD in un lettore)",
                "Salta le clip già indicizzate; verifica la catena fino a EOF prima di toccare",
                "Modalità simulazione (--dry-run) per vedere cosa farebbe",
            ],
            "en": [
                "Works on a single .nfv or a whole folder (e.g. the SD in a reader)",
                "Skips already-indexed clips; verifies the chain to EOF before touching anything",
                "Dry-run mode to preview what it would do",
            ],
        },
        "requires": {"it": ["solo stdlib Python"], "en": ["Python stdlib only"]},
        "params": [
            {"name": "target", "type": "path", "mode": "any", "default": "", "required": True,
             "label": {"it": "Cartella o file .nfv", "en": "Folder or .nfv file"}},
            {"name": "dry", "type": "bool", "default": False, "flag": "--dry-run",
             "label": {"it": "Solo simulazione (non scrive)", "en": "Dry run (no writes)"}},
        ],
    },
    {
        "id": "sd-deploy",
        "cat": "system",
        "glyph": "sd",
        "accent": "#54c7ec",
        "script": "tools/nucleo-sd-deploy/sd_deploy.py",
        "kind": "gui",
        "frozen_ok": False,
        "needs_repo": True,
        "title": {"it": "SD Deploy", "en": "SD Deploy"},
        "tagline": {
            "it": "Prepara e aggiorna la scheda SD del Cardputer",
            "en": "Provision and update the Cardputer's SD card",
        },
        "description": {
            "it": ("Provisioning della SD con tre operazioni: PROVISION (SD nuova, payload "
                   "completo dal master del repo), UPDATE (solo file di sistema, preserva "
                   "chiave/card imparate/dati utente) e VERIFY (confronto hash col master)."),
            "en": ("SD provisioning with three operations: PROVISION (fresh SD, full payload "
                   "from the repo master), UPDATE (system files only, preserves API key / learned "
                   "cards / user data) and VERIFY (hash compare against the master)."),
        },
        "features": {
            "it": [
                "Assembla il master da registry/, apps/, web/shell/ e dai pack ANIMA",
                "UPDATE non cancella mai nulla: aggiorna senza toccare lo stato del device",
                "Guardia sui dischi removibili, gz automatico, formattazione FAT32 guidata",
            ],
            "en": [
                "Assembles the master from registry/, apps/, web/shell/ and the ANIMA packs",
                "UPDATE never deletes anything: refreshes without touching device state",
                "Removable-drive guard, automatic gz, guided FAT32 format",
            ],
        },
        "requires": {
            "it": ["repo NucleoOS presente", "una SD in un lettore"],
            "en": ["NucleoOS repo present", "an SD in a reader"],
        },
    },
    {
        "id": "flasher",
        "cat": "system",
        "glyph": "bolt",
        "accent": "#b388ff",
        "script": "tools/flasher.py",
        "kind": "gui",
        "frozen_ok": False,
        "needs_repo": True,
        "title": {"it": "Flasher", "en": "Flasher"},
        "tagline": {
            "it": "Build, flash USB, deploy SD e OTA Wi-Fi del firmware",
            "en": "Build, USB flash, SD deploy and Wi-Fi OTA of the firmware",
        },
        "description": {
            "it": ("Pannello unico che orchestra gli script già presenti nel repo: build + "
                   "flash USB (flash.ps1), deploy SD incrementale (deploy.ps1), push OTA via "
                   "Wi-Fi (ota.ps1) e identificazione della board (esptool chip-id)."),
            "en": ("Single panel that orchestrates the scripts already in the repo: build + USB "
                   "flash (flash.ps1), incremental SD deploy (deploy.ps1), Wi-Fi OTA push "
                   "(ota.ps1) and board identification (esptool chip-id)."),
        },
        "features": {
            "it": [
                "Allinea GUI e script da riga di comando: una sola fonte di verità",
                "Identify per capire QUALE board e su QUALE porta",
                "Flash seriale affidabile (l'OTA resta il piano B)",
            ],
            "en": [
                "Keeps the GUI and the command-line scripts in sync: one source of truth",
                "Identify to tell WHICH board on WHICH port",
                "Reliable serial flash (OTA stays the plan B)",
            ],
        },
        "requires": {
            "it": ["repo NucleoOS", "ESP-IDF installato", "pyserial"],
            "en": ["NucleoOS repo", "ESP-IDF installed", "pyserial"],
        },
    },
    {
        "id": "boot-log",
        "cat": "diag",
        "glyph": "terminal",
        "accent": "#7ee787",
        "script": "tools/serial_boot.py",
        "kind": "cli",
        "frozen_ok": True,
        "needs_repo": False,
        "title": {"it": "Boot Log", "en": "Boot Log"},
        "tagline": {
            "it": "Cattura il log di avvio del Cardputer via USB-Serial/JTAG",
            "en": "Capture the Cardputer's boot log over USB-Serial/JTAG",
        },
        "description": {
            "it": ("Apre la porta, resetta il chip in RUN e legge il boot per qualche secondo. "
                   "Utile per diagnosticare panic, reset e problemi di avvio senza ricompilare."),
            "en": ("Opens the port, resets the chip to RUN and reads the boot for a few seconds. "
                   "Useful to diagnose panics, resets and boot issues without recompiling."),
        },
        "features": {
            "it": [
                "Pulsa la linea di reset (DTR) tenendo il boot pin rilasciato (RTS)",
                "Durata regolabile; output a schermo nel pannello del launcher",
            ],
            "en": [
                "Pulses the reset line (DTR) while keeping the boot pin released (RTS)",
                "Adjustable duration; output shown in the launcher panel",
            ],
        },
        "requires": {"it": ["pyserial", "Cardputer su porta COM"],
                     "en": ["pyserial", "Cardputer on a COM port"]},
        "params": [
            {"name": "port", "type": "text", "default": "COM3",
             "label": {"it": "Porta COM", "en": "COM port"}},
            {"name": "secs", "type": "int", "default": 12,
             "label": {"it": "Durata (secondi)", "en": "Duration (seconds)"}},
        ],
    },
    {
        "id": "serial-monitor",
        "cat": "diag",
        "glyph": "wave",
        "accent": "#56d364",
        "script": "tools/serial_capture.py",
        "kind": "cli",
        "frozen_ok": True,
        "needs_repo": False,
        "title": {"it": "Serial Monitor", "en": "Serial Monitor"},
        "tagline": {
            "it": "Registra il log seriale su file SENZA resettare il device",
            "en": "Logs the serial output to a file WITHOUT resetting the device",
        },
        "description": {
            "it": ("Apre la porta senza toccare DTR/RTS (USB-Serial/JTAG li ignora comunque) "
                   "così da non riavviare il Cardputer, e registra il log su file per la "
                   "durata scelta. Ideale per catturare crash che avvengono a regime."),
            "en": ("Opens the port without touching DTR/RTS (USB-Serial/JTAG ignores them "
                   "anyway) so it won't reboot the Cardputer, and logs to a file for the chosen "
                   "duration. Ideal to catch crashes that happen at runtime."),
        },
        "features": {
            "it": [
                "Non resetta il device: cattura runtime \"a caldo\"",
                "Scrive su file con flush continuo (niente perdite in caso di stacco)",
            ],
            "en": [
                "Does not reset the device: captures runtime \"hot\"",
                "Writes to file with continuous flush (no loss if unplugged)",
            ],
        },
        "requires": {"it": ["pyserial", "Cardputer su porta COM"],
                     "en": ["pyserial", "Cardputer on a COM port"]},
        "params": [
            {"name": "port", "type": "text", "default": "COM3",
             "label": {"it": "Porta COM", "en": "COM port"}},
            {"name": "secs", "type": "int", "default": 80,
             "label": {"it": "Durata (secondi)", "en": "Duration (seconds)"}},
            {"name": "out", "type": "savepath", "default": "firmware/serial_cap.log",
             "label": {"it": "File di output", "en": "Output file"}},
        ],
    },
]

TOOLS_BY_ID = {t["id"]: t for t in TOOLS}

# Categorie: chiave stabile + nome localizzato.
CATS = [
    ("all",    {"it": "Tutti",       "en": "All"}),
    ("video",  {"it": "Video",       "en": "Video"}),
    ("system", {"it": "Sistema",     "en": "System"}),
    ("diag",   {"it": "Diagnostica", "en": "Diagnostics"}),
]
