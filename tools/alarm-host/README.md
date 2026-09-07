# alarm-host — host harness for the native Allarme app

Runs the REAL firmware app on the PC. `run.mjs` compiles a byte-identical copy of
`firmware/components/nucleo_app/app_alarm.cpp` against the stubs in `stubs/` (fake panel, fake mic,
fake IMU, simulated clock, SD sandbox) and drives it through scripted scenarios: opening, arming,
a silent hit with ambient recording, the auto re-arm, a second hit while the tape rolls, the PIN
challenge, the NDJSON log and its PIN-gated wipe, siren mode, a board with no IMU, and a mic that
fails to open.

```
npm run alarm:test
```

The copy exists only so the stub headers win the `""` include search over the real firmware headers
sitting next to the source; `run.mjs` fails if the copy differs from the original by a single byte.

Two things it asserts that are easy to get wrong by eye:

* **Layout** — every string must sit inside 240x121 (the hint bar is the framework's) and no two
  strings in one frame may overlap. That is how the "Sens. movimento"/"20/20" collision was found.
* **Recording continuity** — the WAV must keep growing across a re-arm and a further hit, and its
  header must stay truthful while it is being written (crash safety), which is read back from the
  file rather than from the directory size (Windows does not refresh that for an open handle).

Requires g++ from msys64 (same toolchain the other host gates use). `build/` and `sdroot/` are
scratch and are wiped on every run.
