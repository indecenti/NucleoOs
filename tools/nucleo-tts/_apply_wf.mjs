// Applica il risultato del workflow tts-coverage: scrive lexicon.wf.<lang>.txt (voci autorate+verificate)
// e accoda i casi-test reali a tools/anima-host/test-replies.<lang>.txt (deduplicati). Uso:
//   node _apply_wf.mjs <workflow-output.json>
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const src = process.argv[2];
if (!src || !existsSync(src)) { console.error('passa il file risultato del workflow'); process.exit(2); }

const raw = JSON.parse(readFileSync(src, 'utf8'));
const R = raw.result || raw.return || raw.output || raw;   // il return dello script di workflow
const get = (k) => Array.isArray(R[k]) ? R[k] : (R.result && Array.isArray(R.result[k]) ? R.result[k] : []);
const lexIt = get('lexicon_it'), lexEn = get('lexicon_en'), testIt = get('test_it'), testEn = get('test_en');
if (!lexIt.length && !lexEn.length) { console.error('niente lexicon nel risultato'); process.exit(1); }

const clean = (arr) => [...new Set(arr.map(s => String(s).replace(/\s+/g, ' ').trim()).filter(Boolean))];

const head = '# Lessico autorato dal workflow tts-coverage (grounded sul parlato reale di ANIMA, verificato).\n';
writeFileSync(join(here, 'lexicon.wf.it.txt'), head + clean(lexIt).join('\n') + '\n', 'utf8');
writeFileSync(join(here, 'lexicon.wf.en.txt'), head + clean(lexEn).join('\n') + '\n', 'utf8');

// accoda i casi-test (dedup contro gli esistenti)
for (const [lang, tests] of [['it', testIt], ['en', testEn]]) {
  const p = join(repo, 'tools', 'anima-host', `test-replies.${lang}.txt`);
  const existing = existsSync(p) ? readFileSync(p, 'utf8') : '';
  const have = new Set(existing.split('\n').map(s => s.trim()));
  const add = clean(tests).filter(t => !have.has(t));
  if (add.length) writeFileSync(p, existing.replace(/\s*$/, '\n') + '# --- casi-test dal workflow ---\n' + add.join('\n') + '\n', 'utf8');
  console.log(`[${lang}] test-replies: +${add.length} nuovi casi`);
}
console.log(`lexicon.wf: IT ${clean(lexIt).length} voci, EN ${clean(lexEn).length} voci`);
