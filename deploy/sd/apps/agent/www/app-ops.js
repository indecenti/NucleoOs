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
  if (!isValidId(id)) return fail('bad-id', 'Impossibile derivare un id valido da "' + (input.name || input.id || '') + '".');
  const reg = await io.sysReadJson('/system/registry/apps.json');
  const existing = reg && (reg.installed || []).find((a) => a && a.id === id);
  if (existing && !isAgentApp(existing)) return fail('system-id', 'id "' + id + '" è già di un\'app di sistema — scegline un altro.');
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
  if (bad.length) return fail('scaffold-partial', 'Scaffold parziale — errori: ' + bad.map((r) => r.error).join(', '));
  return { ok: true, id, files: files.map(([p]) => p),
    message: '✔ Scaffold "' + manifest.name + '" (' + (input.kind || 'blank') + ') creato in ' + id + '/ (manifest.json + www/index.html + icon.svg + i18n.en/it.json).\nOra MODIFICA ' + id + '/www/index.html per costruire l\'app, poi chiama publish_app({id:"' + id + '"}).' };
}

export async function orchestratePublish(io, { id: rawId } = {}) {
  const id = sanitizeId(rawId || '');
  if (!isValidId(id)) return fail('bad-id', 'id non valido: ' + rawId);

  const mf = await io.readWs(id + '/manifest.json', { maxBytes: 16384 });
  if (!mf.ok) return fail('no-manifest', 'Manca ' + id + '/manifest.json nello staging — usa prima scaffold_app.');
  let manifest; try { manifest = JSON.parse(mf.content); } catch (e) { return fail('bad-manifest-json', 'manifest.json non è JSON valido: ' + emsg(e)); }
  manifest.id = id; manifest.web_route = '/apps/' + id + '/'; manifest.runtime = manifest.runtime || 'web'; manifest.created_by = 'agent';
  const v = validateManifest(manifest);
  if (!v.ok) return fail('invalid-manifest', 'Manifest non valido:\n- ' + v.errors.join('\n- '));

  const idx = await io.readWs(id + '/www/index.html', { maxBytes: 256 * 1024 });
  if (!idx.ok || !String(idx.content || '').trim()) return fail('empty-index', 'Manca o è vuoto ' + id + '/www/index.html — costruisci l\'app prima di pubblicarla.');

  // Registry read FIRST (retry once for a blip), then the pure planner DECIDES — refusing a null/unreadable
  // registry (would wipe the other apps) or a system-owned id. Refusal here leaves the device untouched.
  // NOTE: read→plan→write is not atomic; two genuinely concurrent publishes (parallel workers sharing the
  // runtime) could lose-update one registry entry — low risk: tools run sequentially per worker and publish
  // is human-confirmed; worst case is a missing launcher entry, recoverable by re-publishing.
  let reg = await io.sysReadJson('/system/registry/apps.json');
  if (!reg && io.wait) { await io.wait(300); reg = await io.sysReadJson('/system/registry/apps.json'); }
  const plan = planRegistryUpdate(reg, manifest);
  if (!plan.ok) return fail(plan.reason === 'system-id' ? 'system-id' : 'registry-unreadable',
    plan.reason === 'system-id' ? 'Esiste già un\'app di SISTEMA con id "' + id + '": non la sovrascrivo. Scegli un altro id.'
      : 'Non riesco a leggere il registry di sistema (device occupato?). NON installo, per non rischiare di rimuovere le altre app. Riprova tra poco.');

  // Collect www/* with a size cap, read all, run the quality gate — ALL before any device write.
  const t = await io.treeWs(id + '/www', { depth: 4, maxEntries: 60 });
  const wwwFiles = (t.entries || []).filter((e) => e.type === 'file').slice(0, 40);
  if (!wwwFiles.length) return fail('no-www', 'Nessun file in ' + id + '/www.');
  const totalBytes = wwwFiles.reduce((s, e) => s + (e.size || 0), 0);
  if (totalBytes > MAX_APP_BYTES) return fail('too-large', 'App troppo grande (' + Math.round(totalBytes / 1024) + ' KB > limite ' + Math.round(MAX_APP_BYTES / 1024) + ' KB) per il device — riduci o dividi i file.');
  const staged = [];
  for (const e of wwwFiles) {
    const rel = e.path.slice((id + '/www/').length);
    const destAbs = safeAppFilePath(id, rel);   // defense-in-depth: no traversal (firmware also guards)
    if (!destAbs) return fail('unsafe-path', 'Percorso file non sicuro nello staging: ' + e.path);
    const r = await io.readWsPlain(e.path, { maxBytes: 256 * 1024 });
    if (r.ok) staged.push({ rel, destAbs, content: r.content });
  }
  const lint = lintApp(staged.map((s) => ({ path: s.rel, content: s.content })), io.checkSyntax);
  if (!lint.ok) return fail('lint', 'App NON installata — codice non valido (correggi e ripubblica):\n- ' + lint.errors.join('\n- '));

  // ADVISORY cross-provider review (best-effort): a DIFFERENT provider re-reads the app; notes only.
  // Runs BEFORE any write, so a user Stop here aborts the publish (rethrow 'stopped') without touching
  // the device; any other review failure is swallowed (advisory).
  let reviewNote = '';
  if (io.reviewApp) { try { reviewNote = (await io.reviewApp(manifest, idx.content)) || ''; } catch (e) { if (String(e && e.message) === 'stopped') throw e; reviewNote = ''; } }

  // Privileged writes: app tree → files → registry LAST (a half write leaves inert files, never a registry
  // pointing at a missing app).
  await io.sysMkdir('/apps/' + id); await io.sysMkdir('/apps/' + id + '/www');
  const mw = await io.sysWrite('/apps/' + id + '/manifest.json', JSON.stringify(manifest, null, 2));
  if (!mw.ok) return fail('sys-write-manifest', 'Scrittura manifest fallita: ' + mw.error + ' (device occupato? riprova)');
  const made = new Set(['/apps/' + id + '/www']);
  let copied = 0;
  for (const s of staged) {
    const parent = s.destAbs.slice(0, s.destAbs.lastIndexOf('/'));
    if (!made.has(parent)) { await io.sysMkdir(parent); made.add(parent); }
    const w = await io.sysWrite(s.destAbs, s.content);
    if (w.ok) copied++;
  }
  const rw = await io.sysWrite('/system/registry/apps.json', JSON.stringify(plan.doc, null, 2));
  if (!rw.ok) return fail('sys-write-registry', 'File copiati (' + copied + ') ma registry NON aggiornato: ' + rw.error + ' — l\'app potrebbe non comparire finché non riprovi.');

  if (io.notifyAppsChanged) io.notifyAppsChanged();
  return { ok: true, id, copied, updating: plan.updating,
    message: '✔ App "' + manifest.name + '" installata (id ' + id + ', ' + copied + ' file in /apps/' + id + '/www). È nel launcher ORA, senza riavvio. Aprila con open_in_os({app:"' + id + '"}).' + reviewNote };
}

export async function orchestrateManage(io, { id: rawId, action: rawAction } = {}) {
  const id = sanitizeId(rawId || '');
  const action = String(rawAction || '').toLowerCase();
  if (!isValidId(id)) return fail('bad-id', 'id non valido: ' + rawId);
  if (action !== 'enable' && action !== 'disable') return fail('bad-action', 'action deve essere "enable" o "disable".');
  let reg = await io.sysReadJson('/system/registry/apps.json');
  if (!reg && io.wait) { await io.wait(300); reg = await io.sysReadJson('/system/registry/apps.json'); }
  const plan = planRegistrySetEnabled(reg, id, action === 'enable');
  if (!plan.ok) return fail(plan.reason,
    plan.reason === 'system-id' ? 'Posso abilitare/disabilitare solo le app create dall\'agente, non quelle di sistema ("' + id + '").'
      : plan.reason === 'not-found' ? 'Nessuna app con id "' + id + '" nel registry.'
        : 'Non riesco a leggere il registry di sistema (device occupato?). Riprova tra poco.');
  if (plan.was === (action === 'enable')) return { ok: true, noop: true, message: 'App "' + id + '" è già ' + (action === 'enable' ? 'abilitata' : 'disabilitata') + '.' };
  const rw = await io.sysWrite('/system/registry/apps.json', JSON.stringify(plan.doc, null, 2));
  if (!rw.ok) return fail('sys-write-registry', 'Registry non aggiornato: ' + rw.error + ' — riprova.');
  if (io.notifyAppsChanged) io.notifyAppsChanged();
  return { ok: true, message: '✔ App "' + id + '" ' + (action === 'enable' ? 'abilitata (ripristinata nel launcher)' : 'disabilitata (nascosta dal launcher)') + '.' };
}
