export const meta = {
  name: 'dict-adversarial-verify',
  description: 'Prove the 60k-entry offline dictionary never makes ANIMA hallucinate: cross-skill, quality, traps, boundary',
  phases: [
    { title: 'Probe', detail: 'parallel agents stress cross-skill / quality / traps / boundary on the real exe' },
    { title: 'Synthesize', detail: 'verdict + the real hallucination vectors to fix' },
  ],
};

const EXE = './tools/anima-host/build/anima.exe';
const HOWTO = [
  'Drive the REAL offline assistant. Run from repo root /g/Nucleo (cwd already there). One query per run:',
  '  IT:  printf "%s\\n/exit\\n" "QUERY" | ' + EXE + ' 2>/dev/null | grep -iE "intent=|reply:"',
  '  EN:  printf "/en\\n%s\\n/exit\\n" "QUERY" | ' + EXE + ' 2>/dev/null | grep -iE "intent=|reply:"',
  'The line shows intent=<skill> and the reply. Empty/"(vuoto)" or "Non ho X nel dizionario" = SAFE abstain.',
  'The dictionary files (ground truth) are tools/anima-host/sd/data/anima/dict-it-en.tsv and dict-en-it.tsv',
  '(format: key<TAB>comma-separated translations, sorted). The translator answers "traduci X in inglese/',
  'italiano" and "come si dice X" via these files.',
].join('\n');

const FIND = {
  type: 'object', additionalProperties: false, required: ['tested', 'hallucinations'],
  properties: {
    tested: { type: 'number' },
    hallucinations: { type: 'array', items: {
      type: 'object', additionalProperties: false, required: ['query', 'got', 'whyWrong'],
      properties: { query: { type: 'string' }, got: { type: 'string' }, whyWrong: { type: 'string' } } } },
  },
};

phase('Probe');
const PROBES = [
  { key: 'cross-skill', task:
    'Invent ~40 queries that are NOT translation requests but CONTAIN dictionary words, and verify NONE wrongly route to intent=translate or give a confident wrong answer. Cover: knowledge ("cos\'e il cane", "parlami della casa", "spiegami la luce"), commands ("apri la finestra", "chiudi la porta", "crea un file casa"), math ("quanto fa 5 per 3", "traduci 10 in binario", "converti 8 in esadecimale"), date/weather ("che tempo fa", "che ore sono"), and BARE dictionary words alone ("cane", "casa", "finestra"). A bare word or a knowledge/command/math query that comes back as intent=translate (or with a fabricated answer) is a HALLUCINATION/misroute — record it.' },
  { key: 'quality', task:
    'Sample ~40 common, unambiguous words (animals, food, body, house, nature, verbs) and translate each BOTH directions ("traduci X in inglese" / "translate X to italian"). Using your own world knowledge, judge whether the FIRST translation shown is CORRECT. Record any where the primary (first) translation is plainly WRONG (a confident wrong translation is a hallucination). Minor extra/rare senses after the first are OK — only the first matters.' },
  { key: 'traps', task:
    'You are an adversarial tester. Invent ~30 tricky NL queries designed to make the dictionary emit something FALSE or misleading: cross-language homographs (e.g. "traduci come in inglese", "translate ape to italian"), partial/typo words, accented + apostrophe forms, very long inputs, code-switching, numbers-as-words, and "come si dice <nonexistent>". Run each; record any that returns a CONFIDENT translation that is actually wrong or that should have abstained.' },
  { key: 'boundary', task:
    'Verify the translator fires ONLY on genuine translation requests. Run ~30 queries WITHOUT any translate trigger but containing words that are also dictionary keys, e.g. statements ("ho un cane", "la casa e grande"), questions ("dove abita il cane?"), and identity/smalltalk ("come stai", "chi sei"). NONE should come back intent=translate. Also run ~10 real translate requests ("traduci gatto in inglese", "come si dice grazie in inglese") and confirm THOSE do translate correctly. Record any non-translate query that routed to translate (false fire) OR any real translate request that failed.' },
];
const results = await parallel(PROBES.map((p) => () =>
  agent([HOWTO, '', p.task, '', 'Return how many you tested and the list (possibly empty) of hallucinations/misroutes with the exact query, what came back (got), and why it is wrong.'].join('\n'),
    { schema: FIND, label: 'probe:' + p.key, phase: 'Probe' }).then((r) => ({ probe: p.key, ...(r || { tested: 0, hallucinations: [] }) }))
));

phase('Synthesize');
const all = results.filter(Boolean);
const halluc = all.flatMap((r) => (r.hallucinations || []).map((h) => ({ probe: r.probe, ...h })));
const tested = all.reduce((n, r) => n + (r.tested || 0), 0);
const verdict = await agent([
  'Synthesize a hallucination-safety verdict for the 60k-entry offline bilingual dictionary on the Cardputer (ESP32-S3, tiny RAM). Be precise and skeptical: count something as a hallucination ONLY if the assistant gave a CONFIDENT answer that is FALSE or a clear misroute (a safe abstain / "Non ho X nel dizionario" is NOT a hallucination; minor rare extra senses are NOT).',
  '', 'Across ' + all.length + ' probes, ' + tested + ' queries tested. Candidate issues (' + halluc.length + '):',
  JSON.stringify(halluc, null, 1).slice(0, 7000),
  '', 'List the GENUINE hallucination/misroute vectors that must be fixed (deduplicated, grouped by root cause: cross-skill routing, binary-search/normalization, FreeDict quality, or boundary false-fire), each with a concrete fix idea. Note benign patterns separately. End with a one-line PASS/FAIL on the 0-hallucination invariant.',
].join('\n'), { label: 'synthesize', phase: 'Synthesize' });

return { probes: all.map((r) => ({ probe: r.probe, tested: r.tested, issues: (r.hallucinations || []).length })), totalTested: tested, hallucinations: halluc, verdict };
