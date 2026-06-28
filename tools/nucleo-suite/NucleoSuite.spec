# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec per la NucleoOS Toolkit.

Build (da tools/nucleo-suite/):   pyinstaller NucleoSuite.spec
Output:                           dist/NucleoSuite/NucleoSuite.exe  (cartella unica)

Cosa entra nel bundle:
  • launcher.py + tools_registry.py  (l'app)
  • gli script dei tool "frozen_ok" rispecchiati nella STESSA struttura di percorsi
    del repo (tools/...), così tools_registry["script"] risolve identico via _MEIPASS.
  • le dipendenze runtime di quegli script (Pillow, numpy, pyserial, tkinterdnd2).

Cosa NON entra: ffmpeg (deve stare sul PATH) e i tool repo-only (SD Deploy,
Flasher), che restano disponibili solo da sorgente.
"""
import os
from PyInstaller.utils.hooks import collect_submodules

HERE = os.path.dirname(os.path.abspath(SPEC))           # tools/nucleo-suite
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))  # radice repo

def repo(*parts):
    return os.path.join(REPO, *parts)

# Script dei tool frozen_ok -> (sorgente, sottocartella di destinazione nel bundle).
# La destinazione DEVE combaciare con tools_registry["script"].
TOOL_DATAS = [
    (repo("tools", "nfv", "studio_gui.py"),          "tools/nfv"),
    (repo("tools", "nfv", "nfv3.py"),                "tools/nfv"),
    (repo("tools", "nfv", "encode.py"),              "tools/nfv"),
    (repo("tools", "nfv", "reindex.py"),             "tools/nfv"),
    (repo("tools", "serial_boot.py"),                "tools"),
    (repo("tools", "serial_capture.py"),             "tools"),
]

datas = list(TOOL_DATAS)

# tkinterdnd2 è ESCLUSO di proposito: la sua libreria nativa tkdnd non si carica
# in modo affidabile dentro un .exe PyInstaller e fa crashare il converter. Il
# drag&drop è opzionale by-design (studio_gui ripiega su tk.Tk() + file picker).
hiddenimports = [
    "PIL", "PIL.Image", "PIL.ImageOps", "PIL.ImageFilter", "PIL.ImageTk",
    "numpy", "serial", "serial.tools", "serial.tools.list_ports",
    # I tool sono inclusi come DATAS (non analizzati): i loro import tkinter vanno
    # forzati a mano, altrimenti ttk/scrolledtext/simpledialog mancano dal bundle.
    "tkinter", "tkinter.ttk", "tkinter.scrolledtext", "tkinter.filedialog",
    "tkinter.messagebox", "tkinter.simpledialog", "tkinter.font", "tkinter.colorchooser",
]
hiddenimports += collect_submodules("numpy")
hiddenimports += collect_submodules("PIL")
hiddenimports += collect_submodules("tkinter")

a = Analysis(
    ["launcher.py"],
    pathex=[HERE],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    runtime_hooks=[],
    excludes=["matplotlib", "scipy", "pytest", "pandas", "numba", "tkinterdnd2"],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="NucleoSuite",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,          # app GUI: niente console
    disable_windowed_traceback=False,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    name="NucleoSuite",
)
