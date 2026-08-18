// appbroker.js — the capability broker for SANDBOXED apps.
//
// WHY THIS EXISTS. Every app in NucleoOS runs in a same-origin iframe, which means it inherits the
// shell's origin: the pairing cookie rides along on every fetch it makes, it can read the shell's
// localStorage, and it can call ANY /api/* route. So an app that declared `storage.app` could just as
// well do GET /api/fs/read?path=/data/anima/teacher.json and walk off with the user's Claude, Groq and
// xAI keys. The Feature Policy added earlier covers exactly what the BROWSER can mediate — microphone,
// camera, geolocation — and nothing else; the /api/* half was still wide open, for every app.
//
// A service worker cannot fix this: on the device the OS is served over plain http, which is not a
// secure context, so there IS no service worker there (see the three notes in shell.js). Any gate put
// in sw.js is inert exactly where it matters.
//
// The only real fix is to take the origin away — sandbox="allow-scripts" WITHOUT allow-same-origin, so
// the frame gets an opaque origin with no cookie and no shared storage — and give it back a narrow,
// declared surface through postMessage. That is this file.
//
// SCOPE, deliberately. Only apps marked created_by:'agent' in the registry are sandboxed. They are the
// ones nobody vetted — written by a model, installed live — and, since none is installed yet, the
// contract can be their birth contract instead of a migration. The 46 curated apps are untouched.
//
// DEVICE DISCIPLINE. Every brokered filesystem call goes through ONE serial chain with a timeout. That
// is the point, not a side effect: N sandboxed apps sharing the Cardputer's 4-6 sockets cannot burst,
// because the broker is the only door and the door admits one at a time. This makes the device work
// LESS than the status quo, where each app fetched on its own schedule.

// ── path policy (pure) ────────────────────────────────────────────────────────────────────────
// An app's reach is decided by what its manifest DECLARED, not by what it asks for:
//   storage.app     → /data/apps/<id>/…      its own private corner
//   storage.shared  → /data/…                the shared user area (documents, pictures, recordings)
// Anything else — /system, /apps, /www, the ANIMA brain, another app's folder — is out of reach for
// both. Note /data/anima is excluded even under storage.shared: that is where the key vault lives.
const SHARED_ROOT = '/data';
const DENY_UNDER_SHARED = ['/data/anima', '/data/agent'];

// Normalise like the device does: collapse //, drop ./, resolve .. WITHOUT ever climbing past the root.
export function normPath(p) {
  const out = [];
  for (const seg of String(p == null ? '' : p).split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') { out.pop(); continue; }
    out.push(seg);
  }
  return '/' + out.join('/');
}

// Resolve an app-supplied path to the absolute SD path it is allowed to touch, or null if it is not.
// Returns null rather than throwing so a hostile app cannot tell "denied" from "malformed" by timing.
export function resolveAppPath(appId, permissions, path) {
  const id = String(appId || '').trim();
  if (!id || /[^a-z0-9._-]/i.test(id)) return null;
  const perms = Array.isArray(permissions) ? permissions : [];
  const abs = normPath(path);
  if (abs === '/') return null;

  const own = '/data/apps/' + id;
  if (perms.includes('storage.app') && (abs === own || abs.startsWith(own + '/'))) return abs;

  if (perms.includes('storage.shared')) {
    if (abs !== SHARED_ROOT && !abs.startsWith(SHARED_ROOT + '/')) return null;
    for (const d of DENY_UNDER_SHARED) if (abs === d || abs.startsWith(d + '/')) return null;
    return abs;
  }
  return null;
}

// The whole vocabulary. Anything not listed is refused — a deny-list would be a promise we cannot keep.
export const METHODS = ['fs.read', 'fs.write', 'fs.list', 'notify', 'sys.info', 'ai.ask', 'ai.complete'];
// ai.* bounds. Small on purpose: a sandboxed app asks questions, it does not stream conversations.
const AI_Q_MAX = 256;            // ai.ask query (chars)
const AI_PROMPT_MAX = 4096;      // ai.complete prompt (chars)
const AI_REPLY_MAX = 4000;       // either reply, truncated before it re-enters the sandbox
export function methodAllowed(method, permissions) {
  const perms = Array.isArray(permissions) ? permissions : [];
  if (!METHODS.includes(method)) return false;
  // sys.info needs no declaration: it returns only the OS display language. A sandboxed app cannot
  // import /nucleo-i18n.js (cross-origin module from an opaque origin), so without this it would be
  // stuck in one language — and shipping monolingual apps into a five-language OS is not a trade we
  // make for isolation. It discloses nothing the user has not already chosen and can see on screen.
  if (method === 'sys.info') return true;
  if (method === 'notify') return perms.includes('system.notify');
  // Intelligence as a declared capability, in two distinct trust tiers: ai.anima is the on-device
  // deterministic brain (cheap, offline, can't hallucinate); ai.cloud spends the USER'S API key.
  // Neither implies the other, and neither buys any storage reach.
  if (method === 'ai.ask') return perms.includes('ai.anima');
  if (method === 'ai.complete') return perms.includes('ai.cloud');
  return perms.includes('storage.app') || perms.includes('storage.shared');
}

// ── the broker (needs the shell's deps injected, so the policy above stays testable) ───────────
export function createBroker({ fetchFn, findApp, notify, onFsChange, getLang, aiComplete, aiCooldownMs = 2000 } = {}) {
  const doFetch = fetchFn || ((...a) => fetch(...a));
  // ONE call in flight, ever. See the device-discipline note at the top.
  let chain = Promise.resolve();
  const serial = (fn) => {
    const run = chain.then(fn, fn);
    chain = run.then(() => {}, () => {});
    return run;
  };
  // ai.complete is browser-direct (never touches the device), so it does not ride the device chain —
  // but it spends the user's key, so it is single-flight AND cooled down. Both refusals are immediate:
  // a queue here would just let an app buy delayed spending.
  let aiBusy = false, aiLastAt = 0;

  async function exec(app, method, args) {
    const perms = (app && app.permissions) || [];
    if (!methodAllowed(method, perms)) return { ok: false, error: 'denied: ' + method };

    if (method === 'sys.info') return { ok: true, lang: (getLang ? getLang() : 'en') || 'en' };

    if (method === 'notify') {
      if (notify) notify(String((args && args.text) || '').slice(0, 200), app.name || app.id);
      return { ok: true };
    }

    // ai.ask — the on-device deterministic brain. Rides the SAME serial chain as fs.*: it is a device
    // call, and the device discipline (one door, one at a time) is the whole point of this file.
    // mode=off pins the engine to its offline path: a sandboxed app must not make the device network.
    // The answer is STRIPPED to text: /api/anima replies can carry {action,tool,arg} OS effects, and
    // those belong to the copilot's dispatcher, never to untrusted machine-written code.
    if (method === 'ai.ask') {
      const q = String((args && args.q) || '').trim();
      if (!q) return { ok: false, error: 'empty query' };
      if (q.length > AI_Q_MAX) return { ok: false, error: 'query too long (max ' + AI_Q_MAX + ')' };
      const lang = (getLang ? getLang() : 'en') || 'en';
      return serial(async () => {
        const opts = { cache: 'no-store' };
        if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) opts.signal = AbortSignal.timeout(8000);
        try {
          const r = await doFetch('/api/anima?q=' + encodeURIComponent(q) + '&lang=' + encodeURIComponent(lang) + '&mode=off', opts);
          if (!r.ok) return { ok: false, error: 'http-' + r.status };
          const j = await r.json().catch(() => null);
          if (!j) return { ok: false, error: 'bad reply' };
          return { ok: true, reply: String(j.reply || '').slice(0, AI_REPLY_MAX), intent: typeof j.intent === 'string' ? j.intent : '' };
        } catch (e) {
          return { ok: false, error: String((e && e.message) || e) };
        }
      });
    }

    // ai.complete — the cloud, on the user's key, executed by the SHELL in its own origin. The key
    // never crosses into the sandbox; only the completion text comes back, truncated.
    if (method === 'ai.complete') {
      if (!aiComplete) return { ok: false, error: 'no-ai' };
      if (aiBusy) return { ok: false, error: 'busy' };
      const now = Date.now();
      if (now - aiLastAt < aiCooldownMs) return { ok: false, error: 'rate-limited' };
      const prompt = String((args && args.prompt) || '').trim();
      if (!prompt) return { ok: false, error: 'empty prompt' };
      if (prompt.length > AI_PROMPT_MAX) return { ok: false, error: 'prompt too long (max ' + AI_PROMPT_MAX + ')' };
      aiBusy = true; aiLastAt = now;
      try {
        const signal = (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) ? AbortSignal.timeout(20000) : undefined;
        const text = await aiComplete(prompt, { maxTokens: 256, signal });
        if (text == null || text === '') return { ok: false, error: 'no-ai' };
        return { ok: true, text: String(text).slice(0, AI_REPLY_MAX) };
      } catch (e) {
        return { ok: false, error: String((e && e.message) || e) };
      } finally {
        aiBusy = false;
      }
    }

    const abs = resolveAppPath(app.id, perms, args && args.path);
    if (!abs) return { ok: false, error: 'denied: path outside this app\'s reach' };

    return serial(async () => {
      const opts = { cache: 'no-store' };
      if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) opts.signal = AbortSignal.timeout(8000);
      try {
        if (method === 'fs.read') {
          const r = await doFetch('/api/fs/read?path=' + encodeURIComponent(abs), opts);
          if (!r.ok) return { ok: false, error: 'http-' + r.status };
          return { ok: true, content: await r.text() };
        }
        if (method === 'fs.list') {
          const r = await doFetch('/api/fs/list?path=' + encodeURIComponent(abs), opts);
          if (!r.ok) return { ok: false, error: 'http-' + r.status };
          const j = await r.json().catch(() => null);
          return { ok: true, entries: (j && j.entries) || [] };
        }
        // fs.write — bounded on purpose: an app that needs megabytes is not a small app, it is a bug,
        // and the device pays for the mistake.
        const body = String((args && args.content) != null ? args.content : '');
        if (body.length > 256 * 1024) return { ok: false, error: 'too large (max 256 KB)' };
        const r = await doFetch('/api/fs/write?path=' + encodeURIComponent(abs), { method: 'POST', body, ...opts });
        if (!r.ok) return { ok: false, error: 'http-' + r.status };
        if (onFsChange) onFsChange(abs);
        return { ok: true, bytes: body.length };
      } catch (e) {
        return { ok: false, error: String((e && e.message) || e) };
      }
    });
  }

  // The message handler the shell installs. IDENTITY IS BY SOURCE, never by origin: a sandboxed frame
  // has an opaque origin that arrives as the string "null", so e.origin proves nothing. findApp maps
  // e.source back to the window WE created — the only frames that exist.
  return async function onMessage(e) {
    const d = e && e.data;
    if (!d || d.type !== 'nucleo.broker' || typeof d.method !== 'string') return;
    const app = findApp ? findApp(e.source) : null;
    const reply = (payload) => { try { e.source.postMessage({ type: 'nucleo.broker.reply', id: d.id, ...payload }, '*'); } catch {} };
    if (!app) { reply({ ok: false, error: 'unknown caller' }); return; }
    reply(await exec(app, d.method, d.args || {}));
  };
}
