# NucleoOS Toolkit

A single launcher (Tkinter GUI, carousel-style) for the **PC-side** tools of the
NucleoOS Cardputer. It gathers the tools that are useful to the user in one place
and starts each of them in its own process; it is designed to be packaged as a
distributable **`.exe`**.

```
python tools/nucleo-suite/launcher.py        # start the launcher
python tools/nucleo-suite/launcher.py --selftest   # check the wiring, without a GUI
```

## Included tools

| Tool | Cat. | Type | From .exe | What it does |
|------|------|------|:------:|---------|
| **NFV Video Converter** | Video | GUI | ✅ | Converts any video into `.nfv` clips for the device |
| **NFV Reindex** | Video | CLI | ✅ | Adds the seek index to old `.nfv` clips |
| **SD Deploy** | System | GUI | ⛔ | Provisions/updates the Cardputer's SD card |
| **Flasher** | System | GUI | ⛔ | Build / USB flash / SD deploy / OTA |
| **Boot Log** | Diagnostics | CLI | ✅ | Captures the boot log over serial |
| **Serial Monitor** | Diagnostics | CLI | ✅ | Records the serial log without resetting the device |

The **⛔** tools depend on the repo and on the toolchain (ESP-IDF, the SD master):
they work from source, but make no sense in a standalone `.exe` — in the package
they appear disabled with a note.

What is deliberately **not** included: the internal development/AI tools
(`tools/anima/*`, gates, evals, test-lab, `npx_gen`, `train_wiki`, voice and
manifest generators, NFV diagnostics).

## Architecture (why it also works from the `.exe`)

- `tools_registry.py` is the **single source of truth**: it describes each tool and
  its `.py` path relative to the repo root.
- From **source**, the launcher runs the real repo script directly
  (GUIs with `pythonw`, CLIs with their output in the panel).
- From the **`.exe`**, the launcher re-launches itself with `--run <id>`: the frozen
  executable contains the scripts (mirrored in the same `tools/...` structure)
  and runs them via `runpy`. Same path string, two modes.

## Building the `.exe`

PyInstaller is required (`pip install pyinstaller`). Then:

```powershell
powershell -ExecutionPolicy Bypass -File tools\nucleo-suite\build_exe.ps1
```

Output: `tools/nucleo-suite/dist/NucleoSuite/NucleoSuite.exe` — a **single
folder** to zip and distribute.

> ⚠️ **ffmpeg** is not included in the bundle: it must be on the end user's
> `PATH` for the video converter to work.

## Adding a new tool

Add a dictionary to `TOOLS` in `tools_registry.py` (see the fields documented
there). If it must also work from the `.exe`, set `frozen_ok: True` and add its
`.py` (and any dependencies) to `NucleoSuite.spec`.
