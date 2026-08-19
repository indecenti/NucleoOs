# Third-party notice — EmulatorJS and libretro cores

This directory is **vendored third-party software**, not NucleoOS code. It is redistributed here so
the Arcade app can run with no internet: the device serves the emulator and its cores off its own SD
card. Nothing in this directory was written by, or is owned by, the NucleoOS authors.

## EmulatorJS

- Upstream: <https://github.com/EmulatorJS/EmulatorJS>
- Version: **4.2.3** (`version.json`)
- Licence: **GNU General Public License v3.0** — full text in `LICENSE` in this directory.
- Files: `loader.js`, `emulator.min.js`, `emulator.min.css`, `emulator.css`, `localization/`,
  `compression/`, `version.json`.

## libretro cores (`cores/*-wasm.data`)

Each core is a separate upstream project compiled to WebAssembly by the EmulatorJS build bot, and
each carries **its own licence**, which is the licence of that emulator — not of EmulatorJS and not
of NucleoOS:

| Core file | Emulator | Upstream licence |
|---|---|---|
| `fceumm` | FCEUmm (NES) | GPL-2.0 |
| `snes9x` | Snes9x (SNES) | Snes9x non-commercial licence |
| `gambatte` | Gambatte (Game Boy / Color) | GPL-2.0 |
| `mgba` | mGBA (Game Boy Advance) | MPL-2.0 |
| `genesis_plus_gx` | Genesis Plus GX (Mega Drive / Game Gear) | non-commercial, see upstream |
| `smsplus` | SMS Plus (Master System) | GPL-2.0 |
| `mednafen_pce` | Beetle PCE (PC Engine) | GPL-2.0 |
| `mednafen_ngp` | Beetle NeoPop (Neo Geo Pocket) | GPL-2.0 |
| `handy` | Handy (Atari Lynx) | zlib / GPL-2.0 |
| `fbneo` | FinalBurn Neo (arcade, Neo Geo) | FBNeo non-commercial licence |
| `fbalpha2012_cps1` | FB Alpha 2012 (CPS-1) | FB Alpha non-commercial licence |
| `fbalpha2012_cps2` | FB Alpha 2012 (CPS-2) | FB Alpha non-commercial licence |

Consult each project for its exact terms before redistributing this bundle.

## Relationship to the NucleoOS licence

NucleoOS itself is under **PolyForm Noncommercial 1.0.0** (see the repository `LICENSE`). That is a
*different* licence from the ones above and does not extend to this directory: the files here remain
under their own terms, and this notice exists so that is unambiguous to anyone who receives a build.

The Arcade app loads `loader.js` into its own page and configures it through `EJS_*` globals. If you
redistribute NucleoOS with this bundle, treat the GPL-3.0 obligations as applying to EmulatorJS —
including making its corresponding source available, which upstream already does at the URL above.

## No ROMs

**No game ROMs are distributed with NucleoOS.** `data/ROMs/` ships as an empty folder tree with a
README; whatever is in it was put there by the device's owner.
