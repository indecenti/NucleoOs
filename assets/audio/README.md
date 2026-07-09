# Audio library (CC0) — device-ready WAV

All packs by **Kenney, CC0** (public domain, commercial OK, no attribution). Source `*.ogg`
kept; converted to the device format under each pack's `wav/`.

## Device format (nucleo_audio.c WAV path)

- **16-bit PCM, mono, 16000 Hz**, canonical 44-byte header (`data` chunk at offset 36).
- The parser rejects non-16-bit and mis-parses extra chunks, so WAVs are converted with
  `ffmpeg -map_metadata -1 -bitexact -ar 16000 -ac 1 -sample_fmt s16` (no LIST/INFO chunk).
- Deploy as a `game_sfx` **arcade pack**: `<gamedir>/pack/<name>.wav` → sampled playback, zero
  synth CPU (the 16-bit "sampled SFX" model).

## Packs

| Pack | Count | Use |
|---|---|---|
| `digital-audio/wav` | 63 | **Chiptune / 16-bit** — `laser1..9` (shots), `phaserUp/Down`, `pepSound1..5` (pickup), `highUp/Down` (level-up), `phaseJump` |
| `impact-sounds/wav` | 130 | **Combat impacts** — `impactMetal/Wood/Glass/Plank/Punch/Generic_{light,medium,heavy}` (enemy hit/death), plus footsteps |
| `interface-sounds/wav` | 100 | **UI** — `click`, `confirmation`, `back`, `error`, `glass`, `drop` (menus, HUD, hero carousel) |
| `rpg-audio/wav` | 52 | **Foley** — `knifeSlice/chop/drawKnife` (melee), `handleCoins` (gold pickup), doors/cloth/books |

## Cue → event map (mini vampire-survivors)

| Event | Candidate WAV |
|---|---|
| Shot / magic bolt | `digital-audio/laser1..9` |
| Whip / melee | `rpg-audio/knifeSlice1..2`, `chop` |
| Enemy hit | `impact-sounds/impactGeneric_light_00x` |
| Enemy death | `impact-sounds/impactMetal_medium_00x` / `impactWood_medium` |
| Gem / XP pickup | `digital-audio/pepSound1..5` |
| Gold pickup | `rpg-audio/handleCoins` |
| Level-up | `digital-audio/highUp` / `phaserUp1` |
| Player hurt | `impact-sounds/impactPunch_heavy_00x` |
| Boss spawn / wave | `digital-audio/lowThreeTone` / `phaseJump5` |
| UI select / confirm | `interface-sounds/click_00x`, `confirmation_00x` |
| Game over | `interface-sounds/error_00x` low, or `digital-audio/lowDown` |

## Mixer note (16-bit discipline)

Hordes fire many SFX at once. The device WAV path is not a full mixer, so cap concurrent
voices (SNES SPC700 had 8) with **priority + voice-stealing**, and **rate-limit** high-freq
cues (e.g. one enemy-hit sound per ~40 ms, not per hit). See `docs/game-mini-vs.md`.
