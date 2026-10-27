# -*- coding: utf-8 -*-
"""
NucleoOS Toolkit tool registry — bilingual IT/EN.

SINGLE source used by both the launcher (launcher.py) and the .exe build
(NucleoSuite.spec). The `script` paths are relative to the repo root and
are mirrored 1:1 inside the PyInstaller bundle.

The localizable text fields (title, tagline, description, features, requires,
and the params labels) are dicts {"it": ..., "en": ...}. The launcher picks
the language with L(). The NON-text fields (id, cat, glyph, accent, script, kind,
frozen_ok, needs_repo) stay plain.

cat = STABLE category key ("video" | "system" | "diag"): the filter works on
it, the displayed names come from CATS (localized).
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
                   "nativo di NucleoOS: un file .nfv v4 (frame JPEG interi + audio IMA ADPCM) "
                   "a 320×240 per lo schermo esterno o 240×135 per il solo Cardputer."),
            "en": ("Drag-and-drop studio that converts any movie into NucleoOS' native video "
                   "format: one .nfv v4 file (whole-frame JPEGs + IMA ADPCM sound), 320×240 for "
                   "the external panel or 240×135 for the Cardputer alone."),
        },
        "features": {
            "it": [
                "Volume normalizzato a -14 LUFS: tutti i video suonano allo stesso livello",
                "Coda di più video, taglio inizio/durata, crop o bande nere, FPS e qualità",
                "Maschera delle strisce cambiate: il Cardputer invia al display solo ciò che si muove",
            ],
            "en": [
                "Loudness normalised to -14 LUFS: every video plays at the same level",
                "Queue of several videos, start/duration trim, crop or pad, FPS and quality",
                "Changed-strip mask: the Cardputer only sends what moves to the panel",
            ],
        },
        "requires": {
            "it": ["ffmpeg sul PATH", "pillow", "numpy", "(opz.) tkinterdnd2"],
            "en": ["ffmpeg on PATH", "pillow", "numpy", "(opt.) tkinterdnd2"],
        },
    },
    {
        "id": "nfv-migrate",
        "cat": "video",
        "glyph": "reindex",
        "accent": "#ffb454",
        "script": "tools/nfv/migrate.py",
        "kind": "cli",
        "frozen_ok": True,
        "needs_repo": False,
        "title": {"it": "NFV Migrate", "en": "NFV Migrate"},
        "tagline": {
            "it": "Converte una vecchia clip .nfv (v1/v2 + .mp3) nel formato v4",
            "en": "Converts an old .nfv clip (v1/v2 + .mp3) to the v4 format",
        },
        "description": {
            "it": ("Il Cardputer riproduce solo .nfv v4. Questa utility porta in v4 una clip vecchia "
                   "anche senza il video originale: usa i suoi frame JPEG e la .mp3 accanto, con l'audio "
                   "normalizzato come ogni altra clip. Scrive '<nome> (v4).nfv' e non tocca l'originale."),
            "en": ("The Cardputer plays .nfv v4 only. This utility carries an old clip over even without "
                   "its source video: it uses its JPEG frames and the sibling .mp3, with the sound normalised "
                   "like every other clip. Writes '<name> (v4).nfv' and leaves the original alone."),
        },
        "features": {
            "it": ["Nessuna sorgente necessaria: bastano .nfv e .mp3", "Resta a 240×135 (niente ingrandimenti sfocati)"],
            "en": ["No source needed: the .nfv and .mp3 are enough", "Stays at 240×135 (no blurry upscaling)"],
        },
        "requires": {"it": ["ffmpeg sul PATH", "pillow", "numpy"], "en": ["ffmpeg on PATH", "pillow", "numpy"]},
        "params": [
            {"name": "input", "type": "path", "mode": "file", "default": "", "required": True,
             "label": {"it": "Clip .nfv vecchia", "en": "Old .nfv clip"}},
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

# Categories: stable key + localized name.
CATS = [
    ("all",    {"it": "Tutti",       "en": "All"}),
    ("video",  {"it": "Video",       "en": "Video"}),
    ("system", {"it": "Sistema",     "en": "System"}),
    ("diag",   {"it": "Diagnostica", "en": "Diagnostics"}),
]
