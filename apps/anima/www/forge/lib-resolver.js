// lib-resolver.js — "offline-first, online-fallback" for M4 RUNTIME libs (same policy the model
// weights use). Given a LOCAL (device SD) URL and a CDN URL, prefer the local one when it's actually
// present (a HEAD probe), else fall back to the CDN. So a fully-provisioned SD runs with no network;
// a partially-provisioned one still works online. The HEAD probe is injected → host-testable.

export async function resolveLib(localUrl, cdnUrl, probe) {
  try { if (localUrl && (await probe(localUrl))) return { url: localUrl, source: 'sd' }; } catch { /* fall through */ }
  return { url: cdnUrl, source: 'cdn' };
}

// A HEAD-request presence probe (browser). Returns false on any error/offline so we fall back cleanly.
export function makeHeadProbe(fetchFn) {
  const f = fetchFn || (typeof fetch !== 'undefined' ? fetch : null);
  return async (url) => { if (!f) return false; try { const r = await f(url, { method: 'HEAD', cache: 'no-store' }); return !!r.ok; } catch { return false; } };
}

// makeRangeProbe — the THREE-STATE presence+integrity probe for device SD libs. Returns
//   { present, ok, size?, want?, reason, status? }   reason ∈ null | 'absent' | 'truncated' | 'oversize'
//                                                            | 'empty' | 'retry' | 'size-unknown'
// so the UI can say "re-sync (truncated)" vs "never synced" vs "device busy, checking…" — a bare bool
// can't. A partially-synced SD (interrupted robocopy / out-of-space) leaves a SHORT file that would
// crash the runtime mid-load; this catches it and lets resolution fall back to CDN (online) or warn loudly.
//
// CRITICAL — it uses a RANGED GET (Range: bytes=0-0), NEVER {method:'HEAD'}. Verified against the
// firmware (nucleo_webfs.c):
//   1. webfs registers ONLY HTTP_GET — esp_http_server does not synthesize HEAD, so a HEAD 404s and
//      would mark EVERY SD lib absent (a fully-provisioned air-gapped box would falsely read network-once
//      and silently drop to CDN). A 1-byte Range gives a real 206.
//   2. A ranged request BYPASSES the ".gz" sibling (gz is served only when !want_range), so the probe
//      always measures the RAW vendored file. A plain GET could be size-poisoned by web-llm.js.gz.
//   3. webfs answers 206 with "Content-Range: bytes 0-0/<total>", <total>=true file size — authoritative.
//      A 0-byte placeholder → 416 (empty). The low-heap breaker can still 503 a big file → TRANSIENT.
//   Do NOT "simplify" this back to HEAD. (Preview python http.server ignores Range → 200 + full
//   Content-Length, which this also handles.) `expectedBytesOf(url)` → expected bytes, or null = presence-only.
export function makeRangeProbe(fetchFn, expectedBytesOf) {
  const f = fetchFn || (typeof fetch !== 'undefined' ? fetch : null);
  const want = typeof expectedBytesOf === 'function' ? expectedBytesOf : (() => null);
  return async (url) => {
    if (!f) return { present: false, ok: false, reason: 'absent' };
    let r;
    try { r = await f(url, { headers: { Range: 'bytes=0-0' }, cache: 'no-store' }); }
    catch { return { present: false, ok: false, reason: 'absent' }; }
    if (!r) return { present: false, ok: false, reason: 'absent' };
    const exp = want(url);
    const g = r.headers && r.headers.get ? (k) => r.headers.get(k) : () => null;
    if (r.status === 416) return { present: true, ok: false, reason: 'empty', size: 0, want: exp, status: 416 };
    if (r.status === 503) return { present: false, ok: false, reason: 'retry', status: 503 };  // device busy — NOT absent
    if (r.status === 404 || r.status === 405) return { present: false, ok: false, reason: 'absent', status: r.status };
    if (!r.ok && r.status !== 206) return { present: false, ok: false, reason: 'absent', status: r.status };
    // present (206, or a 200 because the server ignored Range). Measure the true size.
    let size = NaN;
    const cr = g('content-range');                                   // "bytes 0-0/<total>"
    if (cr) { const m = /\/(\d+)\s*$/.exec(cr); if (m) size = Number(m[1]); }
    if (!Number.isFinite(size)) size = Number(g('content-length') ?? NaN);
    if (exp == null) return { present: true, ok: true, reason: null, ...(Number.isFinite(size) ? { size } : {}) };
    if (!Number.isFinite(size)) return { present: true, ok: false, reason: 'size-unknown', want: exp, status: r.status }; // SD with no measurable size → fail-closed
    return { present: true, ok: size === Number(exp), reason: size === Number(exp) ? null : (size < Number(exp) ? 'truncated' : 'oversize'), size, want: exp, status: r.status };
  };
}

// makeIntegrityProbe — back-compat BOOL probe (true = present AND whole). Thin wrapper over the
// three-state makeRangeProbe so the real transport is the firmware-correct ranged GET. Used by
// resolveLib's SD-first decision: a corrupt/absent SD file → false → fall back to the CDN.
export function makeIntegrityProbe(fetchFn, expectedBytesOf) {
  const range = makeRangeProbe(fetchFn, expectedBytesOf);
  return async (url) => (await range(url)).ok;
}

// Build an expectedBytesOf(url) from a lib-manifest ({libs:[{path,bytes}]}) for a given served base.
// Convenience for the UI so the integrity probe knows each vendored lib's true size.
export function expectedBytesFromManifest(manifest, base) {
  const map = new Map();
  for (const l of (manifest && manifest.libs) || []) map.set(base + l.path, l.bytes);
  return (url) => (map.has(url) ? map.get(url) : null);
}

// Build a WebLLM appConfig that loads the model WEIGHTS and the model_lib .wasm SD-first (offline),
// CDN-fallback — by overriding the prebuilt entry for `modelId`. Returns {appConfig, weightsSource, libSource}.
//   opts: { sdModelBase, hfModel, localModelLib, cdnModelLib }  (probe injected)
export async function webllmAppConfig(prebuilt, modelId, opts, probe) {
  const weights = await resolveLib(opts.sdModelBase + 'mlc-chat-config.json', null, probe);  // probe a known weight file
  const lib = await resolveLib(opts.localModelLib, opts.cdnModelLib, probe);
  const list = (prebuilt.model_list || []).map((m) => m.model_id === modelId
    ? { ...m, model: weights.source === 'sd' ? opts.sdModelBase : (opts.hfModel || m.model), model_lib: lib.url }
    : m);
  return { appConfig: { ...prebuilt, model_list: list }, weightsSource: weights.source === 'sd' ? 'sd' : 'cdn', libSource: lib.source };
}
