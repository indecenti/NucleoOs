// readiness.js — "Air-Gap Readiness" auditor for ANIMA Forge engines. THE unifying lens over the
// offline story: given an engine, an injected presence/integrity PROBE over its SD runtime-lib closure,
// the WEIGHTS cache status (model-store), and the client CAPS, it classifies whether that engine can
// run with ZERO network — and if not, exactly what's missing and whether one click would fix it.
//
// Pure + host-testable: all I/O is injected (probe(url)->bool, weightsCached(modelId)->bool). No fetch,
// no heavy import. The UI wires the real HEAD/integrity probe (lib-resolver) + Cache API status.
//
// CLASSES (honest, never over-claiming offline):
//   air-gapped     runs with NO network now (all required libs whole on SD + weights ready, or a
//                  network-free engine: the device floor / demo)
//   network-once   usable, but needs the network ONCE to fetch a missing lib/weights → then air-gapped.
//                  ONLY when a source is actually reachable (caps.online) — never offline.
//   no-model       the RUNTIME is ready (libs whole on SD, caps OK) but the WEIGHTS are absent and we're
//                  offline → one download (when back online) away from air-gapped. Distinct from broken.
//   cloud          needs the network EVERY run by nature (the Grok teacher)
//   unavailable    cannot run here/now: a hard capability is missing (no WebGPU / no WASM / no key), OR
//                  a required lib is missing/corrupt on SD AND we're offline (no source to fetch it)
//
// missing[] entries: { role, kind:'lib'|'weights'|'capability', path?, modelId?, provisionable:bool, note? }
//
// NB: integrity here is SIZE-level (the injected probe confirms the SD copy is whole, not bit-identical).
// A same-size tampered/bit-rotted lib is not caught without an opt-in deep sha verify — so the UI must
// say "size-verified", not "checksum-verified", for the air-gapped class.

// The runtime-lib + weights CLOSURE per engine. `path` is relative to the served vendor/ dir.
// `required` libs gate running; `optional` libs only improve things (e.g. wllama multi-thread, which is
// used solely under cross-origin isolation). Paths match vendor/lib-manifest.json.
export const ENGINE_DEPS = {
  webgpu: {
    caps: 'webgpu',
    libs: [
      { role: 'esm', path: 'web-llm.js', required: true },
      { role: 'model_lib', path: 'Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm', required: true },
    ],
    weights: 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC',
  },
  wasm: {
    caps: 'wasm',
    libs: [
      { role: 'esm', path: 'wllama.mjs', required: true },
      // wllama loads exactly ONE thread variant at runtime: single-thread normally, multi-thread only
      // when the page is cross-origin isolated (COOP/COEP → SharedArrayBuffer). So the REQUIRED variant
      // depends on caps.crossOriginIsolated; the other is optional (its absence never lowers readiness).
      { role: 'wasm-st', path: 'wllama/single-thread/wllama.wasm', requiredWhenNotCOI: true },
      { role: 'wasm-mt', path: 'wllama/multi-thread/wllama.wasm', requiredWhenCOI: true },
    ],
    weights: 'Qwen2.5-Coder-0.5B-Instruct-GGUF',
  },
  cloud: { network: true },     // needs network every run
  device: { floor: true },      // served BY the device → offline floor, no libs, no weights
  demo: { none: true },         // scripted, no model
};

const VENDOR_BASE = '/apps/anima/forge/vendor/';

// Resolve a lib spec to a required/optional decision given caps (handles the COOP/COEP edge case).
// crossOriginIsolated defaults to false (fail-safe → require the single-thread binary) when undefined.
function isRequired(lib, caps) {
  if (lib.required) return true;
  const coi = !!(caps && caps.crossOriginIsolated);
  if (lib.requiredWhenCOI && coi) return true;
  if (lib.requiredWhenNotCOI && !coi) return true;
  return false;
}

// Normalize a probe result to the three-state shape. Accepts ctx.probeDetail (rich) or ctx.probe (bool).
async function probeOne(ctx, url) {
  try {
    if (ctx.probeDetail) { const d = await ctx.probeDetail(url); return d || { present: false, ok: false, reason: 'absent' }; }
    const present = ctx.probe ? !!(await ctx.probe(url)) : false;
    return present ? { present: true, ok: true, reason: null } : { present: false, ok: false, reason: 'absent' };
  } catch { return { present: false, ok: false, reason: 'absent' }; }
}

// engineReadiness(engineId, ctx) -> { id, klass, missing[], notes[] }
//   ctx: { probeDetail(url)->{present,ok,reason,size,want} | probe(url)->bool, weightsReady(modelId)->bool, caps, base? }
//   probeDetail  — three-state SD-lib probe (ranged-GET; lib-resolver.makeRangeProbe). ctx.probe (bool) accepted for back-compat.
//   weightsReady — can the weights load with NO network? (on SD OR in browser cache) — injected by the UI,
//                  which knows the truth (model-store.status==='cached', or SD shard presence).
//   caps         — { webgpu, wasm, online, hasKey, crossOriginIsolated, deviceBrain }
export async function engineReadiness(engineId, ctx = {}) {
  const caps = ctx.caps || {};
  const base = ctx.base != null ? ctx.base : VENDOR_BASE;
  const weightsReady = ctx.weightsReady || ctx.weightsCached || (async () => false);  // weightsCached alias
  const online = !!caps.online;
  const dep = ENGINE_DEPS[engineId];
  const out = { id: engineId, klass: 'unavailable', missing: [], notes: [] };
  if (!dep) { out.notes.push('unknown engine'); return out; }

  // Network-free engines.
  if (dep.floor) {
    if (caps.deviceBrain === false) { out.notes.push('device unreachable (page not served by the device)'); return out; }  // unavailable
    out.klass = 'air-gapped'; out.notes.push('served by the device · grounded · answer-only (not the agentic editor)'); return out;
  }
  if (dep.none) { out.klass = 'air-gapped'; out.notes.push('scripted · no model'); return out; }
  if (dep.network) {
    out.klass = (online && caps.hasKey) ? 'cloud' : 'unavailable';
    if (out.klass === 'unavailable') out.notes.push(online ? 'needs an API key' : 'needs network + API key');
    else out.notes.push('needs the network every run · not private');
    return out;
  }

  // Local model engines: a hard capability gate first (no point inspecting libs/weights).
  if (dep.caps && !capOk(dep.caps, caps)) {
    out.klass = 'unavailable';
    out.missing.push({ role: 'capability', kind: 'capability', provisionable: false, note: dep.caps === 'webgpu' ? 'this browser has no WebGPU' : 'this browser has no WebAssembly' });
    return out;
  }

  // Probe each lib in the closure. Distinguish absent (never synced) from CORRUPT (present-but-broken:
  // truncated/empty/oversize) from RETRY (device busy → can't confirm). Optional libs never lower class.
  let anyAbsentRequired = false, anyCorruptRequired = false, anyRetry = false;
  for (const lib of dep.libs) {
    const req = isRequired(lib, caps);
    const d = await probeOne(ctx, base + lib.path);
    if (d.present && d.ok) continue;                                  // whole → fine
    if (!req) {                                                       // an optional variant — NEVER lowers the class (note only),
      if (lib.requiredWhenCOI) out.notes.push('multi-thread lib not usable → single-thread only (slower)');  // incl. a transient 'retry' on it
      else out.notes.push('optional lib absent: ' + lib.path);
      continue;
    }
    if (d.reason === 'retry') { anyRetry = true; out.notes.push('checking ' + lib.path + ' (device busy)'); continue; }  // required + transient
    if (d.present && !d.ok) { anyCorruptRequired = true; out.missing.push({ role: lib.role, kind: 'lib', path: lib.path, reason: d.reason, provisionable: online, note: 're-sync the SD (' + d.reason + ')' + (online ? ' or fetch from CDN' : '') }); }
    else { anyAbsentRequired = true; out.missing.push({ role: lib.role, kind: 'lib', path: lib.path, reason: 'absent', provisionable: online, note: online ? 'fetch once from CDN, then air-gapped' : 'absent on SD and offline — re-sync the SD' }); }
  }
  if (caps.crossOriginIsolated === false && dep.libs.some((l) => l.requiredWhenCOI)) out.notes.push('not cross-origin isolated → single-thread CPU path');

  // Weights (load with no network = on SD or in cache; injected truth).
  const wReady = await safeBool(weightsReady, dep.weights);
  if (!wReady) out.missing.push({ role: 'weights', kind: 'weights', modelId: dep.weights, provisionable: online, note: online ? 'download from the Models panel (CDN→SD, SHA-verified)' : 'weights absent and offline' });

  // Classify — most-severe first: corrupt > absent-required > weights > retry-demote > air-gapped.
  // Honest about whether a source is reachable RIGHT NOW (network-once only when online).
  if (anyCorruptRequired) out.klass = 'corrupt';                      // present but broken — re-sync, never silently used
  else if (anyAbsentRequired) out.klass = online ? 'network-once' : 'unavailable';
  else if (!wReady) out.klass = online ? 'network-once' : 'no-model';
  // A required lib returned 503 (device busy): we couldn't confirm it. NEVER claim air-gapped (unverified)
  // and NEVER claim network-once while OFFLINE (no network can fix a busy device — re-check instead).
  else if (anyRetry) { out.klass = online ? 'network-once' : 'unavailable'; out.notes.push('device busy — re-check to confirm' + (online ? '' : ' (offline: cannot verify now)')); }
  else out.klass = 'air-gapped';
  return out;
}

export async function readinessMatrix(engineIds, ctx = {}) {
  const out = [];
  for (const id of engineIds) out.push(await engineReadiness(id, ctx));
  return out;
}

// Honest badge mapping for the UI. air-gapped is the ONLY "ok" tone (the only true offline-ready state).
export function classBadge(klass) {
  switch (klass) {
    case 'air-gapped': return { icon: '✈', text: 'Air-gapped', tone: 'ok', hint: 'runs with no network (size-verified)' };
    case 'network-once': return { icon: '↓', text: 'Network once', tone: 'warn', hint: 'one fetch, then offline' };
    case 'no-model': return { icon: '◌', text: 'No model', tone: 'warn', hint: 'runtime ready · needs weights' };
    case 'corrupt': return { icon: '!', text: 'SD corrupt', tone: 'bad', hint: 're-sync the SD card (incomplete file)' };
    case 'cloud': return { icon: '☁', text: 'Cloud', tone: 'net', hint: 'needs network every run' };
    case 'unavailable': default: return { icon: '⊘', text: 'Unavailable', tone: 'off', hint: 'cannot run here/now' };
  }
}

// Is this engine usable with zero network right now? (for an "offline ready" headline count)
// ONLY 'air-gapped' counts — never corrupt/network-once/no-model — so the headline can't over-claim.
export const OFFLINE_OK = new Set(['air-gapped']);
export function offlineCount(matrix) { return matrix.filter((m) => OFFLINE_OK.has(m.klass)).length; }

// One honest verdict over the whole matrix for the dial/headline.
//   { bestEngine, overall, line } — bestEngine = first air-gapped of [webgpu,wasm,device,demo] (the
//   "can I pull the cable?" answer); overall = best class present; line = one sentence.
// Severity-salience order for the headline: a corrupt SD is more notable than a fixable network-once/
// no-model (and must agree with `line`, which surfaces corrupt before network-once/no-model).
const CLASS_ORDER = ['air-gapped', 'corrupt', 'network-once', 'no-model', 'cloud', 'unavailable'];
export function readinessSummary(matrix) {
  const byId = (id) => matrix.find((m) => m.id === id);
  const bestEngine = ['webgpu', 'wasm', 'device', 'demo'].find((id) => { const m = byId(id); return m && m.klass === 'air-gapped'; }) || null;
  let overall = 'unavailable';
  for (const c of CLASS_ORDER) if (matrix.some((m) => m.klass === c)) { overall = c; break; }
  const anyCorrupt = matrix.find((m) => m.klass === 'corrupt');
  let line;
  if (bestEngine) line = 'Air-gap ready: ' + bestEngine + (anyCorrupt ? ' — but an SD file is incomplete, re-sync' : '');
  else if (anyCorrupt) { const f = (anyCorrupt.missing.find((x) => x.kind === 'lib') || {}).path || 'a lib'; line = 'SD copy of ' + f + ' is incomplete — re-sync the SD card'; }
  else if (overall === 'network-once' || overall === 'no-model') line = 'One download to go fully offline';
  else if (overall === 'cloud') line = 'Online only (cloud) — no offline engine ready';
  else line = 'No engine ready here';
  return { bestEngine, overall, line };
}

function capOk(name, caps) {
  if (name === 'webgpu') return !!caps.webgpu;
  if (name === 'wasm') return caps.wasm !== false;   // assume WASM unless explicitly false
  return true;
}
async function safeBool(fn, arg) { try { return !!(await fn(arg)); } catch { return false; } }
