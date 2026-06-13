// Verifica che l'ORARIO parlato (nucleo_tts_speak_time) sia pronunciabile per OGNI minuto del giorno:
// compila l'harness C reale del firmware e lo esegue contro gli indici clip generati
// (deploy/sd-safe/data/tts/<lang>/index.bin). SKIP pulito se gli indici non sono ancora stati generati.
// Uso: npm run anima:tts-time
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

const exe = join(build, 'ttstime' + (process.platform === 'win32' ? '.exe' : ''));
const cc = spawnSync(gcc, ['-std=gnu11', '-O0', '-static', '-I', inc,
  join(tts, 'nucleo_tts_plan.c'), join(tts, 'nucleo_tts_index.c'), join(here, 'tts-time-ctest.c'), '-o', exe],
  { encoding: 'utf8', shell, env });
if (cc.error || cc.status !== 0) { console.error('compile fail:\n' + (cc.stderr || cc.stdout || (cc.error && cc.error.message))); process.exit(1); }

const itIdx = join(repo, 'deploy', 'sd-safe', 'data', 'tts', 'it', 'index.bin');
const enIdx = join(repo, 'deploy', 'sd-safe', 'data', 'tts', 'en', 'index.bin');
if (!existsSync(itIdx) && !existsSync(enIdx)) { console.log(`SKIP (indici non generati sotto deploy/sd-safe/data/tts/)`); process.exit(0); }

const r = spawnSync(exe, [itIdx, enIdx], { encoding: 'utf8', env });
process.stdout.write(r.stdout || '');
if (r.stderr) process.stderr.write(r.stderr);
process.exit(r.status || 0);
