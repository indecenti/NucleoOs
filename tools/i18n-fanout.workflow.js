export const meta = {
  name: 'i18n-fanout',
  description: 'Translate NucleoOS apps: per-app it/en catalogs + safe engine integration',
  phases: [
    { title: 'Integrate', detail: 'one agent per app: catalogs + data-i18n + bootstrap' },
  ],
};

// The apps to integrate this pass (small/medium, not-yet-bilingual utilities). The giant or
// already-bilingual apps are handled separately. Override by passing an array as Workflow args.
const APPS = Array.isArray(args) && args.length ? args : [
  'calculator', 'clock', 'notepad', 'media-player', 'photo-viewer', 'recycle-bin',
  'log-viewer', 'updates', 'system-monitor', 'tasks', 'calendar', 'wifi-scanner',
  'video-studio', 'ssh', 'agent', 'dj', 'code-runner', 'archive-manager',
  'voice-manager', 'dos-importer', 'dosbox', 'groq-chat', 'terminal', 'radio',
  'dictation', 'help',
];

const SCHEMA = {
  type: 'object',
  additionalProperties: false,
  properties: {
    app: { type: 'string' },
    integrated: { type: 'boolean', description: 'true if bootstrap + annotations were added' },
    keysAdded: { type: 'number' },
    filesChanged: { type: 'array', items: { type: 'string' } },
    dynamicConverted: { type: 'boolean', description: 'true if JS-built strings were also annotated/translated' },
    risky: { type: 'array', items: { type: 'string' }, description: 'spots left untranslated or that need human review' },
    note: { type: 'string', description: 'one-line summary' },
  },
  required: ['app', 'integrated', 'keysAdded', 'filesChanged', 'note'],
};

const CORE_KEYS = 'ok cancel close save saveAs open openWith delete remove rename edit new add copy cut paste duplicate share refresh reload search filter sort yes no confirm apply reset back next previous done finish skip continue retry undo redo settings help about more less all none select selectAll download upload import export print start stop pause resume play send on off auto enabled disabled online offline loading saving saved ready busy error warning success connected connecting disconnected empty unknown notAvailable file files folder folders app apps name fileName size date type path today yesterday language theme device network storage';

function promptFor(id) {
  return `You are integrating the NucleoOS web app "${id}" with the OS's new CENTRALIZED i18n engine.
NucleoOS is bilingual: **Italian (primary) + English**. Your job: make this app's UI fully translatable
and live-switchable, with ZERO change to its behavior.

## The engine (already built, do NOT modify it)
Module served at the OS root: \`/nucleo-i18n.js\` (import it with that ABSOLUTE path — apps are iframes at
/apps/${id}/, and the webfs maps /apps/<id>/x → apps/<id>/www/x, so NEVER use /www/ in an import).
API:
  import I18N from '/nucleo-i18n.js';
  const t = await I18N.init('${id}');   // loads core + this app's catalog for the active language, paints the DOM,
                                        // installs a MutationObserver that auto-translates [data-i18n] added LATER,
                                        // and live-switches when the OS language changes (no reload). Returns t().
  t('key')                  // → translated string (this app's catalog, then 'core', then the key)
  t('greeting',{name:'Ada'})// → interpolates {name}
  I18N.onChange(cb)         // optional: re-run cb after a live language change (for purely-imperative redraws)

## DOM annotations (the safe, preferred path — the observer translates them automatically)
  <h1 data-i18n="title">…</h1>                         textContent = t('title')
  <input data-i18n-attr="placeholder:search_ph;title:hint">   set those attributes to t(key)
  <p data-i18n-html="rich">…</p>                       innerHTML = t('rich')   (catalog strings only, trusted)
  <span data-i18n="count" data-i18n-args='{"count":3}'>…</span>   interpolation vars
If an element has an icon (SVG) PLUS a text label, wrap ONLY the text in a <span data-i18n="…"> so the icon survives.

## Catalogs (you create these)
Write apps/${id}/www/i18n.it.json and apps/${id}/www/i18n.en.json.
- Flat JSON: { "key": "string" }. Plurals: { "key": { "one":"…{count}…", "other":"…{count}…" } }.
- The two files MUST have the SAME set of keys (the gate fails otherwise). IT = real Italian, EN = real English.
- DO NOT redefine generic words — the engine merges a shared 'core' catalog under every app, so you may
  reference these core keys directly in data-i18n WITHOUT adding them to your files:
  ${CORE_KEYS}
  Only add keys for strings specific to this app. Keep keys short, lowercase, semantic (e.g. "empty_list", "btn_record").
- You may add a "_lang":"it"/"en" metadata key (ignored by the gate).

## Bootstrap (add once)
If the app's main script is a <script type="module">, add at its top:
    import I18N from '/nucleo-i18n.js';
    const t = await I18N.init('${id}');
and use t() for dynamic strings. If the app's script is NOT a module (or inline non-module), instead add a
SEPARATE small module near the end of <body> (independent, no coordination needed):
    <script type="module">import I18N from '/nucleo-i18n.js'; await I18N.init('${id}');</script>
The observer + data-i18n annotations then handle everything, including DOM the app builds later (annotate the
strings inside the app's innerHTML template literals with data-i18n so they translate on insertion).

## Rules
- Surgical edits ONLY. Preserve ALL logic, ids, classes, event wiring. If unsure about a string, leave it and
  list it in "risky". Better correct-and-partial than broken.
- Cover: visible text, button labels, placeholders, title/aria-label tooltips, headings, empty states,
  toast/alert/confirm messages, status text.
- Do NOT translate: code, file paths, URLs, app ids, units/symbols, proper nouns (ANIMA, NucleoOS, Cardputer, Wi-Fi).
- Edit ONLY files under apps/${id}/www/. Do NOT touch other apps, the shell, the engine, or the gate.
- Do NOT run gzip, deploy, sd-sync, or flash. Do NOT create .gz files.
- Verify your two catalogs have identical key sets before finishing.

## First, READ the app
Read apps/${id}/www/index.html and any JS it loads (look for <script src> and inline scripts). THEN make the edits.

Return the structured result.`;
}

phase('Integrate');
const results = await parallel(APPS.map((id) => () =>
  agent(promptFor(id), { label: `i18n:${id}`, phase: 'Integrate', schema: SCHEMA })
));

const ok = results.filter(Boolean);
const integrated = ok.filter((r) => r.integrated);
const totalKeys = ok.reduce((a, r) => a + (r.keysAdded || 0), 0);
const risky = ok.filter((r) => r.risky && r.risky.length).map((r) => ({ app: r.app, risky: r.risky }));
log(`integrated ${integrated.length}/${APPS.length} apps, ${totalKeys} keys added`);

return {
  requested: APPS.length,
  integrated: integrated.map((r) => r.app),
  notIntegrated: ok.filter((r) => !r.integrated).map((r) => ({ app: r.app, note: r.note })),
  totalKeys,
  risky,
  perApp: ok,
};
