// app-publish-check.mjs — DETERMINISTIC (no API, no device) verification of the "create a NucleoOS app"
// pure core in apps/agent/www/app-publish.js: id sanitising, manifest build+validate, registry upsert,
// scaffold output. The privileged I/O (writing /apps + /system/registry) lives in runtime.js and is
// exercised live on the device; this locks the contract every install relies on.
//   node tools/anima-host/app-publish-check.mjs
import { sanitizeId, isValidId, buildManifest, validateManifest, starterHtml, starterIcon, starterI18n, upsertRegistry, registryEntry, isAgentApp, CATEGORIES, planRegistryUpdate, planRegistrySetEnabled, safeAppFilePath, MAX_APP_BYTES, lintApp, inlineScripts, APP_KINDS, normKind } from '../../apps/agent/www/app-publish.js';

let pass = 0, fail = 0; const fails = [];
const ok = (name, cond, detail) => { if (cond) { pass++; } else { fail++; fails.push(name + (detail ? ' — ' + detail : '')); } };

/* ---- id sanitising ---- */
ok('sanitizeId kebab', sanitizeId('My Notes!') === 'my-notes');
ok('sanitizeId collapses', sanitizeId('  unit__converter  ') === 'unit-converter');
ok('sanitizeId leading digit gets prefix', /^app-/.test(sanitizeId('3d viewer')));
ok('sanitizeId trims trailing dash', !sanitizeId('foo---').endsWith('-'));
ok('isValidId accepts good', isValidId('todo') && isValidId('unit-converter'));
ok('isValidId rejects bad', !isValidId('A') && !isValidId('1bad') && !isValidId('has space') && !isValidId('UPPER'));

/* ---- manifest build + validate ---- */
const m = buildManifest({ name: 'Todo List', description: 'simple todos', category: 'productivity' });
ok('build id from name', m.id === 'todo-list');
ok('build web_route', m.web_route === '/apps/todo-list/');
ok('build category kept', m.category === 'productivity');
ok('build category default', buildManifest({ name: 'X', category: 'nope' }).category === 'tools');
ok('build runtime web', m.runtime === 'web');
ok('build marks agent author', m.created_by === 'agent');
ok('build has all required', ['id','name','version','version_code','min_os','platforms','description','category','runtime','entry_service','web_route','permissions','mounts','handles','subscribes','publishes','power','mesh'].every((k) => m[k] != null));
ok('validate ok', validateManifest(m).ok);
ok('validate catches bad id', !validateManifest({ ...m, id: 'Bad Id', web_route: '/apps/Bad Id/' }).ok);
ok('validate catches web_route mismatch', !validateManifest({ ...m, web_route: '/apps/other/' }).ok);
ok('validate catches missing name', !validateManifest({ ...m, name: '' }).ok);
ok('validate rejects non-web runtime', !validateManifest({ ...m, runtime: 'elf' }).ok);
ok('CATEGORIES exported', Array.isArray(CATEGORIES) && CATEGORIES.includes('games'));

/* ---- scaffold output ---- */
const html = starterHtml({ id: 'todo-list', name: 'Todo List', description: 'd' });
ok('starter html doctype', /^<!doctype html>/i.test(html));
ok('starter html i18n wired', html.includes("I18N.init('todo-list')") && html.includes('data-i18n="title"'));
ok('starter icon svg', /^<svg/.test(starterIcon({ name: 'Todo' })) && starterIcon({ name: 'Todo' }).includes('>T<'));
const en = JSON.parse(starterI18n({ name: 'Todo' }, true)); const it = JSON.parse(starterI18n({ name: 'Todo' }, false));
ok('i18n en lang', en._lang === 'en' && en.title === 'Todo');
ok('i18n it lang', it._lang === 'it');

/* ---- registry upsert (immutable, by id) ---- */
const base = { schema: 1, installed: [{ id: 'calculator', enabled: true }] };
const e = registryEntry(m);
const r1 = upsertRegistry(base, e);
ok('upsert appends new', r1.installed.length === 2 && r1.installed.some((a) => a.id === 'todo-list'));
ok('upsert does not mutate input', base.installed.length === 1);
ok('entry marks agent app', isAgentApp(e) && e.path === '/apps/todo-list' && e.enabled === true);
const r2 = upsertRegistry(r1, { ...e, version: '0.2.0' });
ok('upsert updates existing by id', r2.installed.length === 2 && r2.installed.find((a) => a.id === 'todo-list').version === '0.2.0');
ok('upsert keeps schema', r2.schema === 1);
ok('isAgentApp false for bundled', !isAgentApp({ id: 'calculator', enabled: true }));

/* ---- ANTI-DESTRUCTIVE: planRegistryUpdate (the one decision that guards the system registry) ---- */
const populated = { schema: 1, installed: [{ id: 'calculator', enabled: true }, { id: 'notepad', enabled: true }] };
const okPlan = planRegistryUpdate(populated, m);
ok('plan ok on readable registry', okPlan.ok && !okPlan.updating);
ok('plan PRESERVES other apps (no wipe)', okPlan.doc.installed.length === 3 && okPlan.doc.installed.some((a) => a.id === 'calculator') && okPlan.doc.installed.some((a) => a.id === 'notepad'));
ok('plan REFUSES null registry (transient read fail → no wipe)', planRegistryUpdate(null, m).ok === false && planRegistryUpdate(null, m).reason === 'registry-unreadable');
ok('plan REFUSES malformed registry', planRegistryUpdate({ schema: 1 }, m).reason === 'registry-unreadable');
ok('plan REFUSES a system (non-agent) id', planRegistryUpdate({ schema: 1, installed: [{ id: 'todo-list', enabled: true }] }, m).reason === 'system-id');
const upd = planRegistryUpdate({ schema: 1, installed: [{ id: 'todo-list', created_by: 'agent', version: '0.1.0' }] }, m);
ok('plan ALLOWS updating an agent-owned id', upd.ok && upd.updating && upd.doc.installed.length === 1);

/* ---- defense-in-depth path guard ---- */
ok('safe path accepts a plain file', safeAppFilePath('todo-list', 'index.html') === '/apps/todo-list/www/index.html');
ok('safe path accepts a nested file', safeAppFilePath('todo-list', 'js/app.js') === '/apps/todo-list/www/js/app.js');
ok('safe path rejects traversal', safeAppFilePath('todo-list', '../../system/x') === null && safeAppFilePath('todo-list', 'a/../../b') === null);
ok('safe path rejects absolute/backslash/empty', safeAppFilePath('todo-list', '/etc') === null && safeAppFilePath('todo-list', 'a\\b') === null && safeAppFilePath('todo-list', '') === null);
ok('safe path rejects bad id', safeAppFilePath('Bad Id', 'index.html') === null);
ok('MAX_APP_BYTES is a sane cap', typeof MAX_APP_BYTES === 'number' && MAX_APP_BYTES >= 64 * 1024);

/* ---- app lifecycle: planRegistrySetEnabled (disable/enable agent apps, never system apps) ---- */
const lifeReg = { schema: 1, installed: [{ id: 'calculator', enabled: true }, { id: 'todo-list', created_by: 'agent', enabled: true }] };
const dis = planRegistrySetEnabled(lifeReg, 'todo-list', false);
ok('disable an agent app', dis.ok && dis.was === true && dis.doc.installed.find((a) => a.id === 'todo-list').enabled === false);
ok('disable preserves other apps', dis.doc.installed.find((a) => a.id === 'calculator').enabled === true && dis.doc.installed.length === 2);
ok('does not mutate input registry', lifeReg.installed.find((a) => a.id === 'todo-list').enabled === true);
const ena = planRegistrySetEnabled({ schema: 1, installed: [{ id: 'todo-list', created_by: 'agent', enabled: false }] }, 'todo-list', true);
ok('enable an agent app', ena.ok && ena.was === false && ena.doc.installed[0].enabled === true);
ok('refuse toggling a SYSTEM app', planRegistrySetEnabled(lifeReg, 'calculator', false).reason === 'system-id');
ok('refuse a missing id', planRegistrySetEnabled(lifeReg, 'nope', false).reason === 'not-found');
ok('refuse an unreadable registry', planRegistrySetEnabled(null, 'todo-list', false).reason === 'registry-unreadable');

/* ---- pre-publish QUALITY GATE: lintApp / inlineScripts (injected fake checkSyntax) ---- */
const chk = (code) => (/BROKEN/.test(code) ? { ok: false, line: 2, error: 'unexpected token' } : { ok: true });
ok('lint passes a clean app', lintApp([{ path: 'i18n.en.json', content: '{"a":1}' }, { path: 'app.js', content: 'const x=1;' }], chk).ok);
ok('lint REFUSES broken JSON (e.g. i18n)', !lintApp([{ path: 'i18n.it.json', content: '{bad' }], chk).ok);
ok('lint REFUSES broken .js', !lintApp([{ path: 'app.js', content: 'BROKEN code' }], chk).ok);
ok('lint skips ES-module .js (no false alarm)', lintApp([{ path: 'm.js', content: 'import x from "y";\nBROKEN' }], chk).ok);
ok('lint REFUSES broken inline <script>', !lintApp([{ path: 'index.html', content: '<h1>x</h1><script>BROKEN</script>' }], chk).ok);
ok('lint skips inline MODULE script', lintApp([{ path: 'index.html', content: '<script type="module">import a from "/x";\nBROKEN</script>' }], chk).ok);
ok('lint passes html with no scripts', lintApp([{ path: 'index.html', content: '<h1>hi</h1>' }], chk).ok);
ok('inlineScripts skips external + json, keeps plain', inlineScripts('<script src="a.js"></script><script type="application/json">{}</script><script>var y=1;</script>').length === 1);
ok('starter scaffold passes its own lint', lintApp([{ path: 'manifest.json', content: JSON.stringify(buildManifest({ name: 'X' })) }, { path: 'www/i18n.en.json', content: starterI18n({ name: 'X' }, true) }, { path: 'www/index.html', content: starterHtml({ name: 'X' }) }], chk).ok);

/* ---- scaffold KINDS: each template is valid HTML, its module script PARSES, and i18n is complete ---- */
const moduleScript = (html) => { const mm = /<script type="module">([\s\S]*?)<\/script>/.exec(html); return mm ? mm[1] : ''; };
const jsParses = (code) => { try { new Function('return(async()=>{' + code.replace(/^\s*import .*$/gm, '') + '})'); return true; } catch { return false; } };
ok('normKind defaults to blank, keeps known', normKind('nonsense') === 'blank' && normKind('timer') === 'timer');
for (const kind of APP_KINDS) {
  const spec = { name: 'K ' + kind, kind };
  const html = starterHtml(spec);
  ok('kind ' + kind + ': valid doctype + i18n wiring', /^<!doctype html>/i.test(html) && html.includes('I18N.init(') && html.includes('data-i18n="title"'));
  ok('kind ' + kind + ': module script PARSES (no syntax break)', jsParses(moduleScript(html)));
  const enj = JSON.parse(starterI18n(spec, true)), itj = JSON.parse(starterI18n(spec, false));
  ok('kind ' + kind + ': i18n EN/IT valid', enj._lang === 'en' && itj._lang === 'it' && !!enj.title);
}
ok('list kind wires list+add', starterHtml({ name: 'L', kind: 'list' }).includes('id="list"') && starterHtml({ name: 'L', kind: 'list' }).includes('id="add"'));
ok('timer kind wires disp+start', starterHtml({ name: 'T', kind: 'timer' }).includes('id="disp"') && starterHtml({ name: 'T', kind: 'timer' }).includes('id="start"'));
ok('converter kind wires out+from', starterHtml({ name: 'C', kind: 'converter' }).includes('id="out"') && starterHtml({ name: 'C', kind: 'converter' }).includes('id="from"'));
ok('list i18n keys present', ['add', 'empty', 'placeholder'].every((k) => k in JSON.parse(starterI18n({ name: 'L', kind: 'list' }, false))));
ok('timer i18n keys present', ['start', 'reset', 'minutes'].every((k) => k in JSON.parse(starterI18n({ name: 'T', kind: 'timer' }, true))));

console.log(`\napp-publish-check: ${pass} passed, ${fail} failed`);
if (fail) { console.log('FAILED:\n - ' + fails.join('\n - ')); process.exit(1); }
console.log('all green ✓');
