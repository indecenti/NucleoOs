// app-ops.js — PURE orchestration of the agent's app lifecycle: scaffold → publish → manage.
//
// No DOM, no fetch: ALL I/O arrives via the injected `io` object, so the full publish flow (with its
// anti-destructive ordering: read registry → plan → lint BEFORE any write → registry written LAST) is
// host-testable end-to-end with an in-memory device (tools/anima-host/app-ops-check.mjs). runtime.js
// builds `io` from the device queue + fsclient + the privileged /api/fs helpers and calls these.
//
// Imports only from ./app-publish.js (also pure) — the schema/id/registry/lint building blocks.
import {
  buildManifest, validateManifest, starterHtml, starterIcon, starterI18n,
  sanitizeId, isValidId, isAgentApp, planRegistryUpdate, planRegistrySetEnabled,
  safeAppFilePath, MAX_APP_BYTES, lintApp,
} from './app-publish.js';

const fail = (error, message) => ({ ok: false, error, message });
const emsg = (e) => String((e && e.message) || e);
// Bilingual message via the injected translator (io.t). Absent on the Node host gates → returns the
// key string (the gates assert on error/reason codes, not localized message text), so this is safe.
const tr = (io, k, vars) => (io && io.t ? io.t(k, vars) : k);

// io contract (all async unless noted):
//   readWs(rel,{maxBytes})  -> {ok,content?}            workspace read WITH transient-retry (manifest/index)
//   readWsPlain(rel,{maxBytes}) -> {ok,content?}        workspace read without retry (the www/* copy loop)
//   treeWs(rel,{depth,maxEntries}) -> {ok,entries:[{path,type,size}]}
//   writeWs(rel,content,{overwrite,mkdir}) -> {ok,error?}   workspace write (scaffold staging)
//   sysReadJson(abs) -> object|null                     PRIVILEGED read outside the workspace (registry)
//   sysMkdir(abs) -> void                               PRIVILEGED mkdir (best-effort)
//   sysWrite(abs,content) -> {ok,error?}                PRIVILEGED write outside the workspace
//   checkSyntax(code) -> {ok,line?,error?}              injected JS parser for the lint gate
//   wait(ms) -> Promise   (optional)                    registry-read retry backoff
//   reviewApp(manifest, html) -> string  (optional)     ADVISORY cross-provider review → note ('' = none)
//   notifyAppsChanged() -> void  (optional)             nudge the launcher after a registry change

export async function orchestrateScaffold(io, { input = {} } = {}) {
  const manifest = buildManifest(input);
  const id = manifest.id;
  if (!isValidId(id)) return fail('bad-id', tr(io, 'ao_bad_id_derive', { name: (input.name || input.id || '') }));
  const reg = await io.sysReadJson('/system/registry/apps.json');
  const existing = reg && (reg.installed || []).find((a) => a && a.id === id);
  if (existing && !isAgentApp(existing)) return fail('system-id', tr(io, 'ao_system_id', { id }));
  const files = [
    [id + '/manifest.json', JSON.stringify(manifest, null, 2)],
    [id + '/www/index.html', starterHtml(input)],
    [id + '/www/icon.svg', starterIcon(input)],
    [id + '/www/i18n.en.json', starterI18n(input, true)],
    [id + '/www/i18n.it.json', starterI18n(input, false)],
  ];
  const res = [];
  for (const [p, c] of files) res.push(await io.writeWs(p, c, { overwrite: true, mkdir: true }));
  const bad = res.filter((r) => !r.ok);
  if (bad.length) return fail('scaffold-partial', tr(io, 'ao_scaffold_partial', { errors: bad.map((r) => r.error).join(', ') }));
  return { ok: true, id, files: files.map(([p]) => p),
    message: tr(io, 'ao_scaffold_ok', { name: manifest.name, kind: (input.kind || 'blank'), id }) };
}

export async function orchestratePublish(io, { id: rawId } = {}) {
  const id = sanitizeId(rawId || '');
  if (!isValidId(id)) return fail('bad-id', tr(io, 'ao_bad_id', { id: rawId }));

  const mf = await io.readWs(id + '/manifest.json', { maxBytes: 16384 });
  if (!mf.ok) return fail('no-manifest', tr(io, 'ao_no_manifest', { id }));
  let manifest; try { manifest = JSON.parse(mf.content); } catch (e) { return fail('bad-manifest-json', tr(io, 'ao_bad_manifest_json', { error: emsg(e) })); }
  manifest.id = id; manifest.web_route = '/apps/' + id + '/'; manifest.runtime = manifest.runtime || 'web'; manifest.created_by = 'agent';
  const v = validateManifest(manifest);
  if (!v.ok) return fail('invalid-manifest', tr(io, 'ao_invalid_manifest', { errors: v.errors.join('\n- ') }));

  const idx = await io.readWs(id + '/www/index.html', { maxBytes: 256 * 1024 });
  if (!idx.ok || !String(idx.content || '').trim()) return fail('empty-index', tr(io, 'ao_empty_index', { id }));

  // Registry read FIRST (retry once for a blip), then the pure planner DECIDES — refusing a null/unreadable
  // registry (would wipe the other apps) or a system-owned id. Refusal here leaves the device untouched.
  // NOTE: read→plan→write is not atomic; two genuinely concurrent publishes (parallel workers sharing the
  // runtime) could lose-update one registry entry — low risk: tools run sequentially per worker and publish
  // is human-confirmed; worst case is a missing launcher entry, recoverable by re-publishing.
  let reg = await io.sysReadJson('/system/registry/apps.json');
  if (!reg && io.wait) { await io.wait(300); reg = await io.sysReadJson('/system/registry/apps.json'); }
  const plan = planRegistryUpdate(reg, manifest);
  if (!plan.ok) return fail(plan.reason === 'system-id' ? 'system-id' : 'registry-unreadable',
    plan.reason === 'system-id' ? tr(io, 'ao_system_exists', { id })
      : tr(io, 'ao_registry_unreadable'));

  // Collect www/* with a size cap, read all, run the quality gate — ALL before any device write.
  const t = await io.treeWs(id + '/www', { depth: 4, maxEntries: 60 });
  const wwwFiles = (t.entries || []).filter((e) => e.type === 'file').slice(0, 40);
  if (!wwwFiles.length) return fail('no-www', tr(io, 'ao_no_www', { id }));
  const totalBytes = wwwFiles.reduce((s, e) => s + (e.size || 0), 0);
  if (totalBytes > MAX_APP_BYTES) return fail('too-large', tr(io, 'ao_too_large', { kb: Math.round(totalBytes / 1024), limit: Math.round(MAX_APP_BYTES / 1024) }));
  const staged = [];
  for (const e of wwwFiles) {
    const rel = e.path.slice((id + '/www/').length);
    const destAbs = safeAppFilePath(id, rel);   // defense-in-depth: no traversal (firmware also guards)
    if (!destAbs) return fail('unsafe-path', tr(io, 'ao_unsafe_path', { path: e.path }));
    const r = await io.readWsPlain(e.path, { maxBytes: 256 * 1024 });
    if (r.ok) staged.push({ rel, destAbs, content: r.content });
  }
  const lint = lintApp(staged.map((s) => ({ path: s.rel, content: s.content })), io.checkSyntax);
  if (!lint.ok) return fail('lint', tr(io, 'ao_lint', { errors: lint.errors.join('\n- ') }));

  // ADVISORY cross-provider review (best-effort): a DIFFERENT provider re-reads the app; notes only.
  // Runs BEFORE any write, so a user Stop here aborts the publish (rethrow 'stopped') without touching
  // the device; any other review failure is swallowed (advisory).
  let reviewNote = '';
  if (io.reviewApp) { try { reviewNote = (await io.reviewApp(manifest, idx.content)) || ''; } catch (e) { if (String(e && e.message) === 'stopped') throw e; reviewNote = ''; } }

  // Privileged writes: app tree → files → registry LAST (a half write leaves inert files, never a registry
  // pointing at a missing app).
  await io.sysMkdir('/apps/' + id); await io.sysMkdir('/apps/' + id + '/www');
  const mw = await io.sysWrite('/apps/' + id + '/manifest.json', JSON.stringify(manifest, null, 2));
  if (!mw.ok) return fail('sys-write-manifest', tr(io, 'ao_write_manifest_fail', { error: mw.error }));
  const made = new Set(['/apps/' + id + '/www']);
  let copied = 0;
  for (const s of staged) {
    const parent = s.destAbs.slice(0, s.destAbs.lastIndexOf('/'));
    if (!made.has(parent)) { await io.sysMkdir(parent); made.add(parent); }
    const w = await io.sysWrite(s.destAbs, s.content);
    if (w.ok) copied++;
  }
  const rw = await io.sysWrite('/system/registry/apps.json', JSON.stringify(plan.doc, null, 2));
  if (!rw.ok) return fail('sys-write-registry', tr(io, 'ao_write_registry_fail', { copied, error: rw.error }));

  if (io.notifyAppsChanged) io.notifyAppsChanged();
  return { ok: true, id, copied, updating: plan.updating,
    message: tr(io, 'ao_publish_ok', { name: manifest.name, id, copied }) + reviewNote };
}

export async function orchestrateManage(io, { id: rawId, action: rawAction } = {}) {
  const id = sanitizeId(rawId || '');
  const action = String(rawAction || '').toLowerCase();
  if (!isValidId(id)) return fail('bad-id', tr(io, 'ao_bad_id', { id: rawId }));
  if (action !== 'enable' && action !== 'disable') return fail('bad-action', tr(io, 'ao_bad_action'));
  let reg = await io.sysReadJson('/system/registry/apps.json');
  if (!reg && io.wait) { await io.wait(300); reg = await io.sysReadJson('/system/registry/apps.json'); }
  const plan = planRegistrySetEnabled(reg, id, action === 'enable');
  if (!plan.ok) return fail(plan.reason,
    plan.reason === 'system-id' ? tr(io, 'ao_manage_system', { id })
      : plan.reason === 'not-found' ? tr(io, 'ao_not_found', { id })
        : tr(io, 'ao_registry_unreadable2'));
  if (plan.was === (action === 'enable')) return { ok: true, noop: true, message: tr(io, 'ao_already', { id, state: (action === 'enable' ? tr(io, 'ao_enabled') : tr(io, 'ao_disabled')) }) };
  const rw = await io.sysWrite('/system/registry/apps.json', JSON.stringify(plan.doc, null, 2));
  if (!rw.ok) return fail('sys-write-registry', tr(io, 'ao_registry_fail', { error: rw.error }));
  if (io.notifyAppsChanged) io.notifyAppsChanged();
  return { ok: true, message: tr(io, 'ao_manage_ok', { id, state: (action === 'enable' ? tr(io, 'ao_enabled_full') : tr(io, 'ao_disabled_full')) }) };
}
