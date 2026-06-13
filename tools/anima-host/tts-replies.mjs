// Verifica che le risposte REALI di ANIMA si "scatenino" sulla voce: compila l'harness C
// (tts-replies-check) e lo esegue contro l'indice generato (deploy/sd-safe/data/tts/<lang>/index.bin)
// con i casi test-replies.<lang>.txt. SKIP pulito se l'indice non e' ancora stato generato.
// Uso: npm run anima:tts-replies
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const tts = join(repo, 'firmware', 'components', 'nucleo_tts');
const inc = join(tts, 'include');
const build = join(here, 'build'); mkdirSync(build, { recursive: true });

let gcc = null, shell = false;
for (const c of ['C:\\msys64\\mingw64\\bin\\gcc.exe', 'C:\\msys64\\ucrt64\\bin\\gcc.exe']) if (existsSync(c)) { gcc = c; break; }
if (!gcc) { const p = spawnSync('gcc', ['--version'], { shell: true }); if (!p.error && p.status === 0) { gcc = 'gcc'; shell = true; } }
if (!gcc) { console.error('gcc non trovato'); process.exit(2); }
const env = (!shell && process.platform === 'win32') ? { ...process.env, PATH: dirname(gcc) + ';' + (process.env.PATH || '') } : process.env;

const exe = join(build, 'ttsreplies' + (process.platform === 'win32' ? '.exe' : ''));
const cc = spawnSync(gcc, ['-std=gnu11', '-O0', '-static', '-I', inc,
  join(tts, 'nucleo_tts_plan.c'), join(tts, 'nucleo_tts_index.c'), join(here, 'tts-replies-check.c'), '-o', exe],
  { encoding: 'utf8', shell, env });
if (cc.error || cc.status !== 0) { console.error('compile fail:\n' + (cc.stderr || cc.stdout || (cc.error && cc.error.message))); process.exit(1); }

let bad = 0, ran = 0;
for (const lang of ['it', 'en']) {
  const idx = join(repo, 'deploy', 'sd-safe', 'data', 'tts', lang, 'index.bin');
  const rep = join(here, `test-replies.${lang}.txt`);
  if (!existsSync(idx)) { console.log(`[${lang}] SKIP (indice non generato: ${idx})`); continue; }
  if (!existsSync(rep)) { console.log(`[${lang}] SKIP (niente ${rep})`); continue; }
  ran++;
  const r = spawnSync(exe, [idx, lang, rep], { encoding: 'utf8', env });
  process.stdout.write(`\n===== ${lang.toUpperCase()} =====\n` + (r.stdout || ''));
  const m = (r.stdout || '').match(/READ\(scoperte\) (\d+)/);
  if (m && Number(m[1]) > 0) bad++;
}
if (!ran) { console.log('\nNessun indice da verificare (genera prima le clip).'); process.exit(0); }
console.log(bad ? `\nFALLITO: ${bad} lingua/e con risposte non coperte` : '\nOK: ogni risposta reale si compone (0 scoperte)');
process.exit(bad ? 1 : 0);
