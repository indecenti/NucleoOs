# Third-party components

NucleoOS's own source code is licensed under [PolyForm Noncommercial 1.0.0](LICENSE). It builds on
third-party components that keep **their own licenses** — the project's noncommercial terms do
**not** restrict or override them. Each upstream is authoritative for its exact terms; the list
below is a convenience summary of the major dependencies.

| Component | Used for | License (see upstream for authoritative terms) |
|---|---|---|
| ESP-IDF | ESP32-S3 firmware framework / drivers | Apache-2.0 |
| Lua | on-device VM app runtime | MIT |
| Vosk | on-device speech transcription (WASM) | Apache-2.0 |
| WebLLM / MLC | in-browser LLM (Qwen2, WebGPU) | Apache-2.0 |
| wllama (llama.cpp) | in-browser LLM inference | MIT |
| ONNX Runtime Web | in-browser diffusion (Paint/Atelier) | MIT |
| **ffmpeg.wasm** | Video Studio (in-browser transcode) | **LGPL-2.1+ / GPL** (build-dependent) |
| **DOSBox** | in-browser DOS emulator app | **GPL-2.0** |

> ⚠️ **Copyleft components.** `ffmpeg.wasm` and `DOSBox` are LGPL/GPL. They run **in the browser**
> and are shipped as separate, unmodified vendor bundles, but if you redistribute NucleoOS —
> especially commercially — you are responsible for complying with their copyleft terms
> (source availability, notices). When in doubt, remove them from your build.

Notes:
- Fonts, icons and small snippets carry their own upstream licenses; keep their notices.
- If you add a new third-party dependency, list it here with its license and keep its notices.
- This file is informational, not legal advice. When in doubt, consult each component's LICENSE.
