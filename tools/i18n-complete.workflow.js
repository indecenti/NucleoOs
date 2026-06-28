export const meta = {
  name: 'i18n-complete',
  description: 'Finish NucleoOS translation: integrate the giant single-language apps + live-sync the already-bilingual ones',
  phases: [
    { title: 'Giants', detail: 'full engine integration of the large single-language apps' },
    { title: 'Bilingual', detail: 'audit leftovers + live-sync the already-bilingual apps' },
  ],
};

// Large apps that are currently single-language → full integration (catalogs + data-i18n + bootstrap).
const GIANTS = ['browser', 'file-commander', 'video-player', 'spreadsheet'];
// Apps that already render IT/EN from their own logic → audit any non-bilingual leftovers and make
// them react LIVE to an OS language change. Do NOT rewrite their translation system or add catalogs.
const BILINGUAL = ['games', 'miei-fatti', 'recorder', 'paint'];

const SCHEMA = {
  type: 'object', additionalProperties: false,
  properties: {
    app: { type: 'string' },
    mode: { type: 'string' },
    changed: { type: 'boolean' },
    keysAdded: { type: 'number' },
    filesChanged: { type: 'array', items: { type: 'string' } },
    liveSync: { type: 'boolean', description: 'true if the app now reacts to OS language changes without reload' },
    leftoversFixed: { type: 'number', description: 'count of previously-untranslated user-facing strings made bilingual' },
    risky: { type: 'array', items: { type: 'string' } },
    note: { type: 'string' },
  },
  required: ['app', 'mode', 'changed', 'note'],
};

const CORE_KEYS = 'ok cancel close save saveAs open openWith delete remove rename edit new add copy cut paste duplicate share refresh reload search scan scanning filter sort yes no confirm apply reset back next previous done finish skip continue retry undo redo settings help about more less all none select selectAll download upload import export print start stop pause resume play send on off auto enabled disabled online offline loading saving saved ready busy error warning success connected connecting disconnected empty unknown notAvailable file files folder folders app apps name fileName size date type path today yesterday language theme device network storage';

function giantPrompt(id) {
  return `You are integrating the NucleoOS web app "${id}" with the OS's CENTRALIZED i18n engine.
NucleoOS is bilingual: **Italian (primary) + English**. Make this app's UI fully translatable and
live-switchable, with ZERO behavior change. This is a LARGE app — be thorough but surgical.

## Engine (already built — do NOT modify it)
Served at OS root: import with ABSOLUTE path \`/nucleo-i18n.js\` (apps are iframes at /apps/${id}/; the
webfs maps /apps/<id>/x → apps/<id>/www/x, so NEVER write /www/ in an import).
  import I18N from '/nucleo-i18n.js';
  const t = await I18N.init('${id}');   // loads core + this app's catalog, paints [data-i18n], installs a
                                        // MutationObserver that auto-translates nodes added later, live-switches.
  t('key') / t('k',{name})              // returns translated string (app ns → core → key)
  I18N.onChange(cb)                     // optional: re-run cb after a live language change

## DOM annotations (preferred; the observer also handles nodes added later)
  <h1 data-i18n="title">…</h1> · <input data-i18n-attr="placeholder:ph;title:hint"> · <p data-i18n-html="rich">…</p>
  <span data-i18n="left" data-i18n-args='{"count":3}'>…</span>
Icon+label element → wrap ONLY the text in <span data-i18n>. For strings built in JS innerHTML
templates, put data-i18n inside the template so the observer translates them on insertion.

## Catalogs (create both, identical key sets — the gate fails otherwise)
apps/${id}/www/i18n.it.json and i18n.en.json. Flat JSON; plurals = { "one":"…{count}…","other":"…" }.
Do NOT redefine generic words — reference these shared 'core' keys directly in data-i18n without adding
them: ${CORE_KEYS}. Add keys only for app-specific text. Keys: short, lowercase, semantic.

## Bootstrap (once)
If the main script is a module: add \`import I18N from '/nucleo-i18n.js'; const t = await I18N.init('${id}');\`
at its top and use t() for dynamic strings. If it is NOT a module, add a separate module near </body>:
\`<script type="module">import I18N from '/nucleo-i18n.js'; await I18N.init('${id}');</script>\` and rely on
data-i18n annotations (including inside JS templates).

## Rules
- Surgical edits ONLY; preserve ALL logic, ids, classes, wiring. If unsure about a string, leave it and list in "risky".
- Cover: visible text, buttons, placeholders, title/aria-label, headings, empty states, toasts/alerts/confirms, status.
- Do NOT translate: code, paths, URLs, app ids, units/symbols, proper nouns (ANIMA, NucleoOS, Cardputer, Wi-Fi), live data (file names, cell contents).
- Edit ONLY files under apps/${id}/www/. Do not touch other apps, the shell, the engine, or the gate.
- Do NOT gzip/deploy/sd-sync/flash; do NOT create .gz files.
- Verify both catalogs have identical key sets before finishing.

First READ apps/${id}/www/index.html and the JS it loads. Then edit. Return the structured result with mode:"giant".`;
}

function bilingualPrompt(id) {
  return `The NucleoOS web app "${id}" is ALREADY bilingual (Italian + English): it reads
localStorage['anima.lang'] and renders text itself (its own STR table / ternaries). Your job is NOT to
rewrite that — it is to (A) AUDIT for completeness and (B) wire LIVE OS-wide language sync. ZERO behavior change.

## A. Audit leftovers
Read apps/${id}/www/index.html and the JS it loads. Find any USER-FACING string that is NOT bilingual —
i.e. hardcoded in one language (a label/button/placeholder/title/toast/empty-state shown to the user that
stays Italian-only or English-only regardless of anima.lang). Make each one bilingual USING THE APP'S
EXISTING PATTERN (add a key to its STR table, or an \`en ? '…' : '…'\` matching nearby code). Do NOT introduce
the engine's catalog/data-i18n system here — stay consistent with how THIS app already does it.
Do NOT touch: code, paths, URLs, units, proper nouns, live data (file names, ASR text, model output).

## B. Live sync (the integration improvement)
Today this app likely only reads the language at load, so it does NOT update when the language is changed
elsewhere (e.g. Settings) while it is open. Fix that with a minimal hook:
  import I18N from '/nucleo-i18n.js';        // ABSOLUTE path, never /www/
  I18N.onChange(() => { /* call the app's EXISTING re-render / apply-language function */ });
Find the function the app's own language toggle calls (search for anima.lang writes / a lang button) and
reuse it. If there is no clean re-render entry point, add the smallest possible one (e.g. re-run the render
that paints labels). If the app's main script is not a module, add a tiny \`<script type="module">\` that
imports the engine and registers the onChange (it can call a function the app exposes on window, or
re-trigger the app's render). Keep it minimal and safe.

## Rules
- Edit ONLY files under apps/${id}/www/. Do NOT create i18n.json catalogs for this app. Do NOT touch the
  shell/engine/gate/other apps. Do NOT gzip/deploy/flash.
- Preserve all logic. If the live-sync hook can't be added safely, skip it and explain in "risky".

Return the structured result with mode:"bilingual", leftoversFixed=<count>, liveSync=<true/false>.`;
}

phase('Giants');
const giants = await parallel(GIANTS.map((id) => () =>
  agent(giantPrompt(id), { label: `giant:${id}`, phase: 'Giants', schema: SCHEMA })));

phase('Bilingual');
const bilingual = await parallel(BILINGUAL.map((id) => () =>
  agent(bilingualPrompt(id), { label: `biling:${id}`, phase: 'Bilingual', schema: SCHEMA })));

const all = [...giants, ...bilingual].filter(Boolean);
const changed = all.filter((r) => r.changed).map((r) => r.app);
const liveSynced = all.filter((r) => r.liveSync).map((r) => r.app);
const totalKeys = all.reduce((a, r) => a + (r.keysAdded || 0), 0);
const totalLeftovers = all.reduce((a, r) => a + (r.leftoversFixed || 0), 0);
const risky = all.filter((r) => r.risky && r.risky.length).map((r) => ({ app: r.app, risky: r.risky }));
log(`changed ${changed.length} apps, ${totalKeys} catalog keys, ${totalLeftovers} leftovers fixed, ${liveSynced.length} live-synced`);

return { changed, liveSynced, totalKeys, totalLeftovers, risky, perApp: all };
