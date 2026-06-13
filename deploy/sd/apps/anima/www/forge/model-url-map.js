// model-url-map.js — map a model-weight request URL → the ANIMA Forge install-cache key '/fc/<id>/<file>'.
//
// The Forge installer (model-store.js) downloads each verified shard/aux into caches['anima-forge-models']
// under '/fc/<id>/<file>'. When an offline model loads, WebLLM/wllama request those very files — from the
// device URL (/apps/anima/forge/models/<id>/<file> — webfs maps it onto www/) or the Hugging Face CDN
// (huggingface.co/<org>/<id>/resolve/<rev>/<file>). This pure mapper is what the shell service worker uses
// to serve a loaded model's files FROM that verified cache, so an installed model needs neither the network
// nor a heavy whole-file read off the single-task, no-PSRAM device (the read-storm the bounded-range
// installer exists to avoid). Returns null for anything that is not a model weight → the SW leaves it alone.
//
// IMPORTANT: web/shell/sw.js inlines a byte-identical copy of forgeModelKey (a classic service worker can't
// import this ES module). Keep the two in sync; tools/anima-host/forge-model-url-map.test.mjs pins BOTH
// against the REAL registry URLs so a drift fails the gate.
export function forgeModelKey(url) {
  const u = String(url).split('?')[0].split('#')[0];                          // ignore cache-busting query/hash
  let m = /\/forge\/models\/([^/]+)\/(.+)$/.exec(u);                          // device SD path
  if (m) return '/fc/' + m[1] + '/' + m[2];
  m = /huggingface\.co\/[^/]+\/([^/]+)\/resolve\/[^/]+\/(.+)$/.exec(u);        // HF CDN (mlc-ai / Qwen / …)
  if (m) return '/fc/' + m[1] + '/' + m[2];
  return null;
}
