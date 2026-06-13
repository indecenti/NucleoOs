// engine-policy.js — the INTELLIGENT FALLBACK for picking how to answer a code request, degrading
// gracefully by client capability so the user is NEVER left with nothing. Offline-first and
// privacy-first: prefer local generation; bottom out at the device's own grounded recipes (always
// reachable — the web app is served BY the device, same-origin, no GPU needed). The fallback floor
// gets better over time as the silent learning flywheel (learn.js) adds verified recipes.
//
// Tier order (best → floor):
//   local-webgpu  WebLLM/Qwen2.5-Coder on WebGPU            — best, offline, generative
//   local-wasm    wllama on WebAssembly/CPU                 — no-GPU fallback, slower, smaller model
//   online-grok   the cloud teacher (network + key)         — generative, not private/offline
//   device-recipe ANIMA on the device (/api/anima)          — grounded curated/learned JS recipes,
//                                                             offline-safe, grows as it learns
// Pure & DOM-free → host-testable. Mirrors the router's capability-aware degradation; the actual
// engines (webllm-engine.js / wasm-engine.js) are wired in by the UI behind a capability probe.

import { modelPlan } from './router.js';

const MIN_VRAM_MB = 900;   // below this even the 0.5B coder won't fit → WebGPU not usable for codegen

// caps: { webgpu, vramMB, wasm, wasmCached, coderCached, online, hasKey, deviceBrain }
// deviceBrain defaults TRUE (the page is served by the device); set false only for a detached page.
export function readyTiers(caps = {}) {
  const t = [];
  const webgpuReady = !!(caps.webgpu && (caps.vramMB || 0) >= MIN_VRAM_MB && caps.coderCached);
  if (webgpuReady) {
    const plan = modelPlan(caps);
    t.push({ tier: 'local-webgpu', engine: 'webllm', quality: 'high', plan, note: 'local on your GPU, offline & private' });
  }
  // WASM is the no-GPU local fallback — offered whenever a real WebGPU run isn't ready (no GPU, or
  // disqualified by VRAM, or model not cached) and a WASM model IS cached.
  if (!webgpuReady && caps.wasm && caps.wasmCached) {
    t.push({ tier: 'local-wasm', engine: 'wllama', quality: 'medium', note: 'local on CPU (WASM) — slower, smaller model, but offline & private' });
  }
  if (caps.online && caps.hasKey) {
    t.push({ tier: 'online-grok', engine: 'grok', quality: 'high', note: 'cloud teacher (needs network + key; not private)' });
  }
  if (caps.deviceBrain !== false) {
    t.push({ tier: 'device-recipe', engine: 'anima', quality: 'grounded', note: 'curated/learned recipes from the device — always available offline, grows as it learns' });
  }
  return t;
}

// The IDEAL tier this hardware could reach (maybe after a one-time model download).
export function bestPossible(caps = {}) {
  if (caps.webgpu && (caps.vramMB || 0) >= MIN_VRAM_MB) return { tier: 'local-webgpu', engine: 'webllm', needsDownload: !caps.coderCached };
  if (caps.wasm) return { tier: 'local-wasm', engine: 'wllama', needsDownload: !caps.wasmCached };
  if (caps.online && caps.hasKey) return { tier: 'online-grok', engine: 'grok', needsDownload: false };
  if (caps.deviceBrain !== false) return { tier: 'device-recipe', engine: 'anima', needsDownload: false };
  return { tier: 'decline', engine: null, needsDownload: false };
}

const LOCAL_TIERS = new Set(['local-webgpu', 'local-wasm']);
const PRIVATE = new Set(['local-webgpu', 'local-wasm', 'device-recipe']);   // never leaves the device/GPU

// Honest, tier-accurate reason — surfaces the privacy/speed truth and reads the REAL network state
// (offline is !online, not webgpu===false).
function reasonFor(tier, caps) {
  switch (tier) {
    case 'local-webgpu': return 'WebGPU available — generating locally on your GPU (offline & private)';
    case 'local-wasm': return 'no WebGPU — generating locally on CPU (WASM): slower and a smaller model, but offline & private';
    case 'online-grok': return 'no usable local model — answering via the cloud teacher (sent to the network, not private)';
    case 'device-recipe':
      if (caps.online === false) return 'offline — using the device’s grounded recipes (on-device, private)';
      if (caps.online && !caps.hasKey) return 'no local model and no cloud key — using the device’s grounded recipes (on-device, private)';
      return 'no local model — using the device’s grounded recipes (on-device, private)';
    default: return 'no engine available — open the page from the device, or enable a model';
  }
}

// planEngine(caps) → what to use NOW + what to converge to + the fallback chain.
//   selected   : the tier answering THIS turn (privacy-coherent: a local/private download is bridged
//                through the private device floor, NEVER silently through the cloud)
//   target     : the best tier this hardware can reach (may need a one-time download)
//   needsDownload: true only when the target's weights are actually fetchable (CDN or device SD)
//   bridge     : the (private) tier used while `target` downloads; null when none
//   private    : true when the SELECTED tier keeps the prompt on the device/GPU
//   degraded   : true when selected isn't the top local-webgpu tier (false on decline)
export function planEngine(caps = {}) {
  const chain = readyTiers(caps);
  const target = bestPossible(caps);
  const declined = chain.length === 0;

  // A download is only real if there's a SOURCE: the CDN (online) or the device SD (deviceBrain).
  const downloadReachable = caps.online === true || caps.deviceBrain !== false;
  const needsDownload = !declined && target.needsDownload && downloadReachable && target.tier !== (chain[0] && chain[0].tier);
  const localTargetDownloading = needsDownload && LOCAL_TIERS.has(target.tier);

  // Selection: normally the best ready tier; but if a LOCAL/private target is downloading, answer NOW
  // through a PRIVATE ready tier (a ready local, else the device floor) rather than leaking to cloud.
  let selectedObj;
  if (declined) selectedObj = { tier: 'decline', engine: null };
  else if (localTargetDownloading) selectedObj = chain.find((c) => PRIVATE.has(c.tier)) || chain[0];
  else selectedObj = chain[0];

  const selected = selectedObj.tier;
  const bridge = (localTargetDownloading && selected !== target.tier) ? selected : null;

  return {
    selected, engine: selectedObj.engine || null, reason: reasonFor(selected, caps),
    target: declined ? 'decline' : target.tier, needsDownload, bridge,
    private: PRIVATE.has(selected),
    degraded: !declined && selected !== 'local-webgpu',
    chain: chain.map((c) => c.tier), detail: chain,
  };
}

// A short, honest one-liner for the dial / status. No download CTA on a hard decline; the download is
// phrased by reachability ("from the device" offline-with-SD, "when back online" otherwise); bridging
// is framed as temporary + converging. The reason already carries the privacy/speed truth.
export function capabilityNote(caps = {}) {
  const p = planEngine(caps);
  if (p.selected === 'decline') return p.reason;
  let s = p.reason;
  if (p.bridge && p.bridge !== p.target) s += `; temporary until your local model finishes downloading, then it stays on-device`;
  if (p.needsDownload) {
    const size = p.target === 'local-webgpu' ? '~1 GB' : '~0.5 GB';
    const when = caps.online === false ? (caps.deviceBrain !== false ? ' from the device' : ' when back online') : '';
    s += ` (one-time ${size} download${when} to enable ${p.target})`;
  }
  return s;
}
