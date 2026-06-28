# Tanks SFX — real CC0 samples (impact set)

The Tanks pack (`deploy/sd/data/tanks/pack/`) is a **hybrid**: most cues are synthesized by
`gen_arcade_sfx.py`, but the high-impact ones (explosions, the nuke, launches, lasers, win/lose)
were replaced with **real recorded CC0 samples** for punch — synthesized booms sounded thin.

## ⚠️ Do NOT blindly re-run the generator for tanks
`python gen_arcade_sfx.py --game tanks --out ../../deploy/sd/data/tanks/pack` would **overwrite
these real WAVs with synth**. If you regenerate, re-apply the conversions below afterwards.

## Source (all Creative Commons CC0 — public domain, MIT-repo safe)
- Kenney "Sci-Fi Sounds"  — https://kenney.nl/assets/sci-fi-sounds  (explosions, lasers, thrusters, impacts)
- Kenney "Music Jingles"  — https://kenney.nl/assets/music-jingles (8-bit NES jingles for win/lose)

Converted to the device format: `ffmpeg -i SRC -ar 22050 -ac 1 -sample_fmt s16 OUT.wav`.

## Mapping (pack name ← Kenney source)
| pack | source | note |
|------|--------|------|
| h_nuk | lowFrequency_explosion_000 (2.0s) | deep nuke boom |
| f_nuk | lowFrequency_explosion_001 | nuke charge/launch |
| h_bmb | explosionCrunch_004 | big bomb |
| boom  | explosionCrunch_003 | generic big |
| h_std | explosionCrunch_000 | standard hit |
| h_rol | explosionCrunch_001 | roller |
| h_swm | explosionCrunch_002 | swarm |
| small | impactMetal_000 | light impact |
| hit   | impactMetal_001 | generic hit |
| beam  | laserLarge_000 | laser |
| tele, h_tel | laserRetro_000 | teleport zap |
| fire, f_std, f_bmb, jump | thrusterFire_000..003 (trimmed ~0.4s + fade) | launch whoosh |
| snipe | laserSmall_000 | sniper sharp crack |
| lock  | laserRetro_001 | homing lock-on zap |
| win   | jingles_NES00 (1.76s) | **blind pick — swap by changing the NES index** |
| lose  | jingles_NES05 | **blind pick — swap by changing the NES index** |

**Variety:** `play_hit_sfx()` in app_tanks.cpp rotates generic blast hits through 3 explosion variants
(ids 30/6/35) so the same gun never sounds identical twice; signature weapons (nuke/bomb/sniper/homing)
keep their own cue.

Still synthesized (fit fine / multi-pop sequences): nav, sel, back, turn, theme, fly, h_dig,
h_clu, h_rai, f_dig, f_clu, f_rai, f_swm, f_rol, f_tel, shld, shldhit, heal, drill, build, burn.
