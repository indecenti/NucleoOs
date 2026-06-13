# Forge vendored runtimes

Offline-first runtime libs for the M4 (local-LLM) engines, served from the SD by webfs.
Resolution is **SD-first → CDN fallback** via `forge/lib-resolver.js` (a HEAD probe decides per file,
so a partially-provisioned SD still works online). Host-gated by `forge-lib-resolver.test.mjs`.

- **web-llm.js** — `@mlc-ai/web-llm` flattened ESM bundle (jsdelivr `+esm`). `forge-demo.html` →
  `importWebLLM` imports this LOCALLY first, CDN only if absent. Removes the 6 MB lib CDN dependency.
- **Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm** — the per-model WebLLM `model_lib` for
  `Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC` (the cs1k base lib, v0_2_84). `createWebLLM` builds the
  appConfig through `webllmAppConfig()` which overrides `model_lib` to this local file when present.

## Offline status (honest)
- **Weights**: fully on the SD (`apps/anima/www/forge/models/<id>/`), delivery proven
  (`forge-delivery.test.mjs`); downloaded on demand by the Models panel (CDN→SD, SHA-verified).
- **WebLLM runtime lib**: vendored (`web-llm.js`) → offline.
- **WebLLM per-model `model_lib` .wasm**: vendored (above) → a fully air-gapped WebGPU run is now
  possible (lib + weights both on SD). `webllmAppConfig()` falls back to the
  `raw.githubusercontent.com/mlc-ai/binary-mlc-llm-libs/...` URL if the local file is ever missing.
- **wllama (no-GPU / WASM path)**: `vendor/wllama.mjs` (esbuild flat bundle of `@wllama/wllama@1.17.1
  esm/index.js`) + `vendor/wllama/{single,multi}-thread/wllama.wasm` — all vendored → fully offline.
  The loader is still SD-first → CDN fallback, but the SD files are now present.

The loader degrades gracefully: SD → CDN → honest error. The Demo engine (scripted MockEngine)
always runs offline with no model.
