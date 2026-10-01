// seq-import.js — load an app's ES-module graph ONE FILE AT A TIME, then link it in the browser.
//
// The shell's service worker gates device requests (2 in flight, writes exclusive) — but a service worker
// only exists on a SECURE origin, and NucleoOS is opened at http://<cardputer-ip>: there it never runs. A
// dynamic import() then fetches the whole graph at once (the ANIMA agent runtime: ~12 modules in parallel);
// the firmware httpd has 4 sockets and resets the oldest on a 5th, one module is lost, the import fails —
// and the browser remembers that failure until the page is reloaded. On the ADV the agent stayed dead and
// ANIMA fell back to a tool-less chat that "applied" changes to no file.
//
// seqImport(url, { own }) fetches the modules whose URL starts with one of the `own` prefixes sequentially,
// rewrites their import specifiers to blob: URLs of each other (dependencies first), imports every OTHER
// dependency by its real absolute URL — one at a time, so shared modules (ai.js, fsclient.js…) keep a
// single instance with the rest of the page — and imports the linked root. Relative dynamic imports
// ('./vendor/x.mjs') become absolute. Any problem (a cycle, a fetch error) falls back to a plain import().
const STATIC_RE = /(^|[;\n}])(\s*(?:import|export)\s+(?:[^'"`;]*?\sfrom\s*)?)(['"])([^'"\n]+)\3/g;
const DYN_REL_RE = /\bimport\(\s*(['"])(\.{1,2}\/[^'"\n]+)\1\s*\)/g;

// The specifiers of one module's static imports / re-exports (side-effect imports included).
export function staticSpecifiers(src) {
  const out = [];
  for (const m of String(src).matchAll(STATIC_RE)) out.push(m[4]);
  return out;
}

// Rewrite one module's source: static specifiers through map(absUrl) → new specifier; relative dynamic
// imports made absolute against the module's own URL.
export function rewriteModule(src, selfUrl, map) {
  return String(src)
    .replace(STATIC_RE, (all, pre, kw, q, spec) => pre + kw + q + map(new URL(spec, selfUrl).href) + q)
    .replace(DYN_REL_RE, (all, q, spec) => 'import(' + q + new URL(spec, selfUrl).href + q + ')');
}

// Plan the graph: fetch (sequentially) every `own` module reachable from root. → { order (deps first), src,
// external (absolute URLs to pre-import) } or throws on a cycle / fetch error.
export async function planGraph(rootUrl, { own, fetchText }) {
  const isOwn = (u) => own.some((p) => u.startsWith(p));
  const src = new Map(), deps = new Map(), external = new Set();
  const queue = [rootUrl];
  while (queue.length) {
    const u = queue.shift();
    if (src.has(u)) continue;
    const text = await fetchText(u);
    src.set(u, text);
    const ds = staticSpecifiers(text).map((s) => new URL(s, u).href);
    deps.set(u, ds);
    for (const d of ds) (isOwn(d) ? queue.push(d) : external.add(d));
  }
  const order = [], state = new Map();          // 1 = visiting, 2 = done
  const visit = (u) => {
    if (state.get(u) === 2) return;
    if (state.get(u) === 1) throw new Error('module cycle at ' + u);
    state.set(u, 1);
    for (const d of deps.get(u) || []) if (src.has(d)) visit(d);
    state.set(u, 2); order.push(u);
  };
  visit(rootUrl);
  return { order, src, external: [...external] };
}

// blob: in a browser (no size limit, no re-encoding); data: elsewhere (Node imports data: but not blob:).
const toUrl = (code) => (typeof document !== 'undefined' && typeof URL !== 'undefined' && URL.createObjectURL && typeof Blob !== 'undefined')
  ? URL.createObjectURL(new Blob([code], { type: 'text/javascript' }))
  : 'data:text/javascript;charset=utf-8,' + encodeURIComponent(code).replace(/'/g, '%27');   // it sits inside '…'

const cache = new Map();
export function seqImport(path, { own, base = (typeof location !== 'undefined' ? location.href : undefined), fetchText, importer = (u) => import(u) } = {}) {
  const rootUrl = new URL(path, base).href;
  if (cache.has(rootUrl)) return cache.get(rootUrl);
  const ownAbs = (own || []).map((p) => new URL(p, base).href);
  const get = fetchText || (async (u) => { const r = await fetch(u); if (!r.ok) throw new Error('HTTP ' + r.status + ' ' + u); return r.text(); });
  const job = (async () => {
    let plan;
    try { plan = await planGraph(rootUrl, { own: ownAbs, fetchText: get }); }
    catch { return importer(rootUrl); }                                          // plain import as the fallback
    for (const u of plan.external) { try { await importer(u); } catch {} }        // shared deps: one at a time, real URL
    const linked = new Map();
    for (const u of plan.order) linked.set(u, toUrl(rewriteModule(plan.src.get(u), u, (d) => linked.get(d) || d)));
    return importer(linked.get(rootUrl));
  })();
  cache.set(rootUrl, job);
  job.catch(() => cache.delete(rootUrl));
  return job;
}
