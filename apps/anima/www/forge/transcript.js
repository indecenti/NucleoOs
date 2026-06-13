// transcript.js — the pure VIEW-MODEL for the ANIMA chat: it turns a canonical Envelope v1 into the
// exact, deterministic shapes the renderer paints (substrate chip, verdict chip, reasoning trace,
// grounding list, reply, awaiting) and computes the mode-dial segments. No DOM, no styling, no
// colour-only signalling — every chip carries an icon AND text so provenance is honest and
// accessible. Keeping the UX behaviour HERE (pure) makes it CERTAIN under host tests even though the
// DOM glue lands later. Pure & DOM-free → host-testable.

import { provenance } from './envelope.js';
import { verdictChip as verdictChipFor } from './verify.js';

// Bilingual label table — icon+text, never colour-only. Substrate chips mirror provenance().kind.
const L = {
  it: {
    off: 'Offline', on: 'Ibrida', only: 'Cloud', local: 'GPU locale', auto: 'Auto',
    sub: { 'on-device': 'su dispositivo', hybrid: 'ancorata', cloud: 'cloud', 'local-gpu': 'GPU locale', unknown: 'ignota' },
    noGpu: 'WebGPU non disponibile in questo browser',
  },
  en: {
    off: 'Offline', on: 'Hybrid', only: 'Cloud', local: 'Local GPU', auto: 'Auto',
    sub: { 'on-device': 'on-device', hybrid: 'grounded', cloud: 'cloud', 'local-gpu': 'local GPU', unknown: 'unknown' },
    noGpu: 'WebGPU unavailable in this browser',
  },
};
const lang = (l) => (l === 'en' ? L.en : L.it);

// Icon for each substrate kind (paired with text — accessibility, never colour-only).
const SUB_ICON = { 'on-device': '▣', hybrid: '◈', cloud: '☁', 'local-gpu': '⊞', unknown: '·' };

// Split the device's single trace string into ordered reasoning steps. The firmware joins steps with
// ' > ' (cascade dispatch) or ' · ' (sub-steps); we accept both and drop empties.
function splitTrace(trace) {
  return String(trace || '')
    .split(/\s*(?:>|·)\s*/)
    .map((s) => s.trim())
    .filter(Boolean);
}

// turnModel(env) → everything the renderer needs for one assistant turn. `lang` defaults to the
// envelope's own language so the chips localise to the answer.
export function turnModel(env, opts = {}) {
  env = env || {};
  const l = lang(opts.lang || env.lang || 'it');
  const prov = provenance(env);
  return {
    substrateChip: {
      kind: prov.kind,
      icon: SUB_ICON[prov.kind] || SUB_ICON.unknown,
      label: l.sub[prov.kind] || l.sub.unknown,
      grounded: prov.grounded === true,
    },
    verdictChip: verdictChipFor(env.verdict),   // {icon,label,tone} — handles null → 'unchecked'
    traceSteps: splitTrace(env.trace),
    grounding: Array.isArray(env.grounding) ? env.grounding : [],
    reply: typeof env.reply === 'string' ? env.reply : '',
    awaiting: env.awaiting === true,
  };
}

// Ordered dial segments for the mode picker. 'local' (M4 browser GPU) is only AVAILABLE when
// caps.webgpu — otherwise it is shown disabled with an honest reason (never silently hidden).
// AUTO is always available (it degrades to the device brain when no GPU/online). Icon+text only.
const DIAL_ICON = { off: '⛶', on: '◈', only: '☁', local: '⊞', auto: '✶' };

export function dialModel(currentMode, caps = {}) {
  const l = lang(caps.lang || 'it');
  const seg = (id, available, reason) => ({
    id,
    label: l[id],
    icon: DIAL_ICON[id],
    available: available !== false,
    current: id === currentMode,
    ...(reason ? { reason } : {}),
  });
  return [
    seg('off', true),
    seg('on', true),
    seg('only', true),
    caps.webgpu ? seg('local', true) : seg('local', false, l.noGpu),
    seg('auto', true),
  ];
}
