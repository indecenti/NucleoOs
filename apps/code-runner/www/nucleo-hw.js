// nucleo-hw.js — NucleoOS hardware capability MANIFEST (single source of truth).
//
// One declaration per hardware capability. From this ONE list three consumers are derived, so a
// capability is never triplicated:
//   1. the sandbox binding  os.hw.<ns>.<action>()  for JS automations (nucleo-run.js)
//   2. the LLM agent tools   toAgentTools()         for the multi-agent runtime (apps/agent)
//   3. the offline brain     ANIMA intents (seam)   wired in firmware via the same ids
//
// Why this shape (and NOT an on-device JS interpreter): the Cardputer has ~18 KB usable heap and no
// PSRAM — every embeddable JS/Lua/Python VM needs far more (Elk 12-15 KB, mJS 20-25 KB, QuickJS
// 30-50 KB; Lua was already removed for size). So the firmware exposes thin HTTP primitives and the
// SCRIPT LOGIC runs where there's RAM: the browser Web Worker (nucleo-run.js) or the agent. The MCU
// only does the timing-critical I/O. Each capability maps to one /api/* endpoint that already
// respects the auth guard + heavy-work arbiter + heap rules.
//
// Pure module: no DOM, no absolute /apps imports, fetch is injected — so it runs in the browser AND
// under Node (host-testable, mirror of agent-tools.js).

// Safe GPIO pins, mirrored from the firmware allowlist (nucleo_gpio.c). Client-side validation is a
// fast, clear-error first line — the firmware still enforces it, so the client is never trusted.
export const GPIO_WRITE_PINS = [1, 2];        // Grove G2/G1
export const GPIO_READ_PINS = [0, 1, 2];      // + BtnA/G0

// kind: 'read' = side-effect-free (safe to auto-run); 'act' = changes hardware state (the agent
// gates these behind human approval; ANIMA requires high confidence).
// guard(args) -> error string | null : capability-specific safety beyond the JSON schema.
export const HW_CAPABILITIES = [
  {
    id: 'ir.send', ns: 'ir', action: 'send', kind: 'act', permission: 'device.ir',
    summary: 'Send an infrared remote code through the Cardputer IR LED. Give protocol+address+command (e.g. protocol "nec"/"samsung"/"sony12"/"rc5"/"jvc") OR a raw[] microsecond list with a carrier.',
    endpoint: { method: 'POST', path: '/api/ir/send' },
    args: { type: 'object', properties: {
      protocol: { type: 'string', description: 'nec|necext|samsung|sony12|sony15|sony20|rc5|jvc' },
      address: { type: 'number' }, command: { type: 'number' },
      raw: { type: 'array', items: { type: 'number' }, description: 'alternating mark/space µs (use instead of protocol)' },
      carrier: { type: 'number', description: 'carrier Hz for raw (default 38000)' },
      repeats: { type: 'number' },
    }, required: [] },
  },
  {
    id: 'ir.tvbgone', ns: 'ir', action: 'tvbgone', kind: 'act', permission: 'device.ir',
    summary: 'Run (or stop) the TV-B-Gone power-off sweep over a region. action "start"|"stop", region "all"|"us"|"eu"|"asia".',
    endpoint: { method: 'POST', path: '/api/ir/tvbgone' },
    args: { type: 'object', properties: { action: { type: 'string', enum: ['start', 'stop'] }, region: { type: 'string' } }, required: [] },
  },
  {
    id: 'ir.jammer', ns: 'ir', action: 'jammer', kind: 'act', permission: 'device.ir',
    summary: 'Start/stop the IR jammer (authorised testing). action "start"|"stop", mode "sweep"|"random"|"constant".',
    endpoint: { method: 'POST', path: '/api/ir/jammer' },
    args: { type: 'object', properties: { action: { type: 'string', enum: ['start', 'stop'] }, mode: { type: 'string' } }, required: [] },
  },
  {
    id: 'wifi.scan', ns: 'wifi', action: 'scan', kind: 'read', permission: 'net.wifi',
    summary: 'Scan for nearby Wi-Fi access points. Returns a list of {ssid, rssi, channel, auth}.',
    endpoint: { method: 'GET', path: '/api/wifi/scan' },
    args: { type: 'object', properties: {}, required: [] },
    pick: (j) => (Array.isArray(j) ? j : (j && (j.networks || j.aps || j.results)) || []),
  },
  {
    id: 'gpio.write', ns: 'gpio', action: 'write', kind: 'act', permission: 'hardware.gpio',
    summary: 'Drive a GPIO pin. pin = a safe header/Grove pin (1 or 2 on the Cardputer), value 0|1, mode "out" (default). Use for relays, LEDs, triggers on the Grove port.',
    endpoint: { method: 'POST', path: '/api/gpio' },
    args: { type: 'object', properties: { pin: { type: 'number' }, value: { type: 'number', enum: [0, 1] }, mode: { type: 'string', enum: ['out'] } }, required: ['pin', 'value'] },
    guard: (a) => GPIO_WRITE_PINS.includes(a.pin) ? null : ('pin ' + a.pin + ' not writable — safe pins are ' + GPIO_WRITE_PINS.join(',')),
  },
  {
    id: 'gpio.read', ns: 'gpio', action: 'read', kind: 'read', permission: 'hardware.gpio',
    summary: 'Read a GPIO pin level (0|1). pin = a safe header pin (0, 1 or 2 on the Cardputer).',
    endpoint: { method: 'GET', path: '/api/gpio', query: ['pin'] },
    args: { type: 'object', properties: { pin: { type: 'number' } }, required: ['pin'] },
    guard: (a) => GPIO_READ_PINS.includes(a.pin) ? null : ('pin ' + a.pin + ' not readable — safe pins are ' + GPIO_READ_PINS.join(',')),
  },
  {
    id: 'sys.status', ns: 'sys', action: 'status', kind: 'read', permission: 'system.events',
    summary: 'Read the device live state: time, free/total SD space, Wi-Fi (mode/ssid/ip), uptime, free RAM.',
    endpoint: { method: 'GET', path: '/api/status' },
    args: { type: 'object', properties: {}, required: [] },
  },
];

const BY_ID = Object.fromEntries(HW_CAPABILITIES.map((c) => [c.id, c]));

// A valid capability id?  (used by the sandbox host to validate os.hw.<ns>.<action> ids)
export function isCapability(id) { return Object.prototype.hasOwnProperty.call(BY_ID, id); }
export function capabilityIds() { return HW_CAPABILITIES.map((c) => c.id); }

// GROUNDING: validate args against the capability's JSON schema (required/type/enum) + its safety
// guard, BEFORE any request leaves the browser. So a hallucinating LLM, a typo in a script, or a
// bad NL parse fails fast with a clear message instead of poking the hardware. Returns
// {ok:true, value} | {ok:false, error}. The firmware re-checks everything — this is never trusted.
export function validateArgs(id, args) {
  const cap = BY_ID[id];
  if (!cap) return { ok: false, error: 'unknown hardware capability: ' + id };
  args = args || {};
  const props = (cap.args && cap.args.properties) || {};
  const req = (cap.args && cap.args.required) || [];
  for (const k of req) if (args[k] === undefined || args[k] === null) return { ok: false, error: 'missing required "' + k + '"' };
  for (const k of Object.keys(args)) {
    const p = props[k]; if (!p) continue;            // unknown extras are ignored (firmware drops them)
    const val = args[k];
    if (p.type === 'number' && typeof val !== 'number') return { ok: false, error: '"' + k + '" must be a number' };
    if (p.type === 'string' && typeof val !== 'string') return { ok: false, error: '"' + k + '" must be a string' };
    if (p.type === 'array' && !Array.isArray(val)) return { ok: false, error: '"' + k + '" must be an array' };
    if (p.enum && !p.enum.includes(val)) return { ok: false, error: '"' + k + '" must be one of ' + p.enum.join('|') };
  }
  if (cap.guard) { const g = cap.guard(args); if (g) return { ok: false, error: g }; }
  return { ok: true, value: args };
}

// Invoke a capability by id with a plain args object. fetchFn is injected (browser fetch / Node /
// a mock in tests). Returns the parsed device response (capability.pick may reshape it). Throws on
// a non-OK HTTP status so callers (sandbox, agent) surface the failure.
export async function callCapability(id, args, fetchFn) {
  const cap = BY_ID[id];
  if (!cap) throw new Error('unknown hardware capability: ' + id);
  const v = validateArgs(id, args);
  if (!v.ok) throw new Error(cap.id + ': ' + v.error);
  args = v.value;
  const ep = cap.endpoint;
  let resp;
  if (ep.method === 'GET') {
    let url = ep.path;
    const qs = [];
    for (const k of (ep.query || [])) if (args[k] != null) qs.push(encodeURIComponent(k) + '=' + encodeURIComponent(args[k]));
    if (qs.length) url += '?' + qs.join('&');
    resp = await fetchFn(url, { cache: 'no-store' });
  } else {
    resp = await fetchFn(ep.path, { method: ep.method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(args), cache: 'no-store' });
  }
  if (!resp.ok) throw new Error(cap.id + ' failed (HTTP ' + resp.status + ')');
  let data;
  const txt = await resp.text();
  try { data = JSON.parse(txt); } catch { data = txt; }
  return cap.pick ? cap.pick(data) : data;
}

// ---- consumer 2: LLM agent tools (Anthropic-shaped, mirrors agent-tools.js CLIENT_TOOLS) ----
// Tool names use ns_action (dots aren't allowed in tool names). The reverse map lets the agent's
// execTool route a tool name back to a capability id.
export function toolName(id) { return id.replace(/\./g, '_'); }
export function toAgentTools() {
  return HW_CAPABILITIES.map((c) => ({
    name: toolName(c.id),
    description: c.summary + (c.kind === 'act' ? ' [hardware action — changes device state]' : ' [hardware read]'),
    input_schema: c.args,
  }));
}
export function capabilityForTool(name) {
  const found = HW_CAPABILITIES.find((c) => toolName(c.id) === name);
  return found ? found.id : null;
}
export const HW_MUTATING = new Set(HW_CAPABILITIES.filter((c) => c.kind === 'act').map((c) => toolName(c.id)));

// ---- consumer 3: OFFLINE natural-language resolver (deterministic, no LLM) ----
// Maps a few high-value phrases to a capability. CRITICAL CONTRACT: this must NEVER step on the
// real LLMs. When `opts.online` is set it returns null unconditionally, so an online session is
// ALWAYS handled by the integrated LLM (agent tool-use) — the offline resolver only acts where it
// is genuinely useful and necessary: offline, deterministic, no network. The returned {id,args}
// still passes through validateArgs/callCapability, so a bad pin or unknown verb fails safely.
export function resolveOffline(text, opts = {}) {
  if (opts.online) return null;                       // online -> the LLM owns it. Never shadow it.
  const q = String(text || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').trim();
  if (!q) return null;
  const pinOf = () => { const m = q.match(/(?:gpio|pin)\s*(\d{1,2})/); return m ? parseInt(m[1], 10) : null; };

  if (/tv-?b-?gone|spegni tutte le tv|(spegni|turn off).*(tutt|all).*(tv|televis)/.test(q)) return { id: 'ir.tvbgone', args: { action: 'start', region: 'all' } };
  if (/(ferma|stop).*(sweep|tv-?b-?gone)/.test(q)) return { id: 'ir.tvbgone', args: { action: 'stop' } };
  if (/(ferma|stop).*(jammer|disturb)/.test(q)) return { id: 'ir.jammer', args: { action: 'stop' } };
  if (/(jammer|disturba).*(ir|infraross)/.test(q)) return { id: 'ir.jammer', args: { action: 'start', mode: 'sweep' } };
  if (/(spegni|accendi|turn off|turn on).*(tv|televis)/.test(q)) return { id: 'ir.tvbgone', args: { action: 'start', region: 'all' } };   // offline can't know the brand -> sweep
  if (/(scansiona|scansione|scan|cerca|trova).*(rete|reti|wi-?fi|network)/.test(q)) return { id: 'wifi.scan', args: {} };

  const p = pinOf();
  if (p != null) {
    if (/(alt|acces|accend|high|attiv|\bon\b)/.test(q)) return { id: 'gpio.write', args: { pin: p, value: 1 } };
    if (/(bass|spent|spegn|low|disattiv|\boff\b)/.test(q)) return { id: 'gpio.write', args: { pin: p, value: 0 } };
    if (/(leggi|read|stato|state|valore|value|livello)/.test(q)) return { id: 'gpio.read', args: { pin: p } };
  }
  if (/(stato|status|salute|health).*(dispositiv|device|sistema|system)/.test(q)) return { id: 'sys.status', args: {} };
  return null;
}

export default HW_CAPABILITIES;
