// app-recipe.js — F5: live install-and-smoke on the paired device, and NucleoOS app-recipe learning.
//
// Two halves of the roadmap's final phase (docs/anima-code.md §8, §10 F5), both PURE and injected so
// the whole thing is host-tested with no device:
//
//   SMOKE  After publish_app installs an app LIVE, prove it actually works on the device before
//          declaring success — fetch /apps/<id>/, check the manifest is the one we wrote and the
//          index loads non-empty. A publish that the launcher accepted but that 404s is a silent
//          failure the agent should catch, not celebrate. Opt-in: the caller runs it only when asked.
//
//   LEARN  A verified-AND-smoke-passed app becomes a learned RECIPE (via forge/learn.js distill), so
//          the offline `device-recipe` floor gets steadily better at THIS OS's apps — a keyless,
//          GPU-less user gets better scaffolds over time. Reversible: every card links its provenance.
//          This does NOT auto-promote into the shipped corpus; promote-learned.mjs remains the
//          conservative build-time gate. It only STAGES, like the WebGPU forge loop already does.

// NO static imports from the served /apps/anima/forge/* path (node cannot resolve it; and a relative
// path that works in node breaks in the browser — www/ is invisible in URLs). The forge helpers
// (distill, canonical, sha256hex) are INJECTED, so this whole module is pure and host-tested; agent.js
// supplies the real ones via dynamic import at the call site.

// ── SMOKE (pure over an injected fetch) ────────────────────────────────────────────────────────

// smokeApp(id, deps) → { ok, checks:[{name,ok,detail}] }. deps.fetchFn(url)->{ok,status,text()}.
// Every check is observational (GET only); nothing mutates. A failed check never throws — it is
// reported, because "the app is broken" is the answer, not an exception.
export async function smokeApp(id, { fetchFn, fsFetch = null, expectManifest = null, canonical = null, sha256hex = null } = {}) {
  const checks = [];
  const add = (name, ok, detail) => { checks.push({ name, ok: !!ok, detail: detail || '' }); return ok; };
  const clean = String(id || '').trim();
  if (!clean || /[^a-z0-9._-]/i.test(clean)) { add('id', false, 'bad app id'); return { ok: false, checks }; }

  // 1. the app route serves (www/ is invisible in URLs → /apps/<id>/ maps to www/index.html)
  let indexOk = false, indexText = '';
  try {
    const r = await fetchFn('/apps/' + clean + '/');
    indexOk = add('route', r && r.ok, r ? ('HTTP ' + r.status) : 'no response');
    if (indexOk) { indexText = await r.text(); }
  } catch (e) { add('route', false, String(e && e.message || e)); }

  // 2. the index is real HTML, not an error page or an empty file
  add('index', indexOk && /<\s*(html|body|main|div|script)/i.test(indexText) && indexText.length > 40,
    indexOk ? (indexText.length + ' bytes') : 'not fetched');

  // 3. the installed manifest is the one we published (id matches; and, if given, deep-equals ours).
  // A manifest lives OUTSIDE www/, so it is not on the public /apps/<id>/ route (that 404s by design)
  // — it is read through the device fs API. fsFetch is that reader (pairing cookie rides it); without
  // one, the manifest check is skipped rather than asserted false, so a caller with no fs reader still
  // gets the route+index smoke.
  // 3a. the device's own registry lists the app, enabled. This is the authoritative "the launcher
  // accepted it" signal and is available everywhere (/api/apps); the manifest deep-compare below is an
  // extra, best-effort check where the fs path is readable.
  if (fsFetch) {
    try {
      const ra = await fsFetch('/api/apps');
      if (ra && ra.ok) { const j = JSON.parse(await ra.text()); const app = (j.apps || j || []).find((a) => a && a.id === clean);
        add('registered', !!(app && app.enabled !== false), app ? (app.enabled === false ? 'disabled' : 'enabled') : 'not in registry'); }
      else add('registered', false, ra ? ('HTTP ' + ra.status) : 'no response');
    } catch (e) { add('registered', false, String(e && e.message || e)); }
  }

  const mfetch = fsFetch || null;
  try {
    const r = mfetch ? await mfetch('/api/fs/read?path=' + encodeURIComponent('/apps/' + clean + '/manifest.json')) : null;
    if (r && r.ok) {
      const m = JSON.parse(await r.text());
      const idOk = m && m.id === clean;
      const sameAsPublished = !expectManifest || !(canonical && sha256hex) || (await sha256hex(canonical(m))) === (await sha256hex(canonical(expectManifest)));
      add('manifest', idOk && sameAsPublished, idOk ? (sameAsPublished ? 'matches' : 'differs from published') : 'id mismatch');
    }   // manifest not readable here (device fs vs sim): 'registered' is the authoritative signal, so we do not fail on its absence
  } catch (e) { add('manifest', false, String(e && e.message || e)); }

  return { ok: checks.every((c) => c.ok), checks };
}

// A human-readable one-liner for the transcript (i18n-key friendly: caller supplies t()).
export function smokeSummary(result, t = (k, v) => (v ? k + ' ' + JSON.stringify(v) : k)) {
  if (!result) return t('smoke_none');
  const failed = result.checks.filter((c) => !c.ok);
  return result.ok
    ? t('smoke_ok', { n: result.checks.length })
    : t('smoke_fail', { names: failed.map((c) => c.name).join(', ') });
}

// ── LEARN (an app pattern → a recipe, via the proven distill gate) ──────────────────────────────

// The spec text a learned APP recipe is keyed on. An app is described by what it DOES, not its code,
// so the recall phrasings ("crea un timer", "make a device status app") match how a user would ask —
// distill/derivePhrasings turns this into the ask set.
export function appSpecText(manifest = {}, kind = '') {
  const name = String(manifest.name || manifest.id || 'app').trim();
  const desc = String(manifest.description || '').trim();
  const base = kind ? (name + ' (' + kind + ')') : name;
  return desc && desc.length > 4 ? base + ' — ' + desc : base;
}

// stageAppRecipe(turn, ctx) → { staged, reason }. turn: { manifest, kind, html, smoke, approved,
// provenanceHash, lang }. It maps the app publish onto distill's turn shape: the app is "certain"
// when it was approved (publish is human-gated), it smoke-PASSED (ranOk), and carries provenance.
// The verdict is 'pass' by construction here — the smoke result IS the run gate, so a failed smoke
// yields ranOk:false and distill refuses it. No new gate logic; the existing one, pointed at apps.
export async function stageAppRecipe(turn = {}, deps = {}, ctx = {}) {
  const { distill, canonical, sha256hex } = deps;
  if (typeof distill !== 'function') throw new Error('stageAppRecipe: distill must be injected');
  const { manifest, kind, html, smoke, approved, provenanceHash, lang } = turn;
  const spec = appSpecText(manifest || {}, kind || '');
  // Provenance over the actual artifact (manifest + html), so the card is auditable and reversible.
  const prov = provenanceHash || (html && canonical && sha256hex ? await sha256hex(canonical({ id: manifest && manifest.id, html })) : (provenanceHash || 'app:' + (manifest && manifest.id || 'x')));
  return distill({
    spec,
    code: String(html || '').slice(0, 4000),   // the recipe body is the app's own starter HTML
    verdict: { verdict: 'pass' },
    approved: approved === true,
    ranOk: !!(smoke && smoke.ok),
    substrate: 'app',
    provenanceHash: prov,
    lang,
  }, ctx);
}
