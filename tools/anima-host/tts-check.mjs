// Gate voce offline: compila la C reale del firmware coi test host e la esegue. Due test:
//   1) planner (nucleo_tts_plan.c)        -> testo IT/EN -> token CLIP/PAUSE/UNKNOWN
//   2) indice  (nucleo_tts_index.c)       -> ricerca binaria su index.bin (retrieval del device)
// Nessun hardware. Uso: npm run anima:tts
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const build = join(here, 'build');
mkdirSync(build, { recursive: true });
const tts = join(repo, 'firmware', 'components', 'nucleo_tts');
const inc = join(tts, 'include');

function findGcc() {
  for (const c of ['C:\\msys64\\mingw64\\bin\\gcc.exe', 'C:\\msys64\\ucrt64\\bin\\gcc.exe',
                   'C:\\msys64\\clang64\\bin\\gcc.exe']) if (existsSync(c)) return { cmd: c, shell: false };
  const probe = spawnSync('gcc', ['--version'], { encoding: 'utf8', shell: true });
  if (!probe.error && probe.status === 0) return { cmd: 'gcc', shell: true };
  return null;
}
const gcc = findGcc();
if (!gcc) { console.error('gcc non trovato (MinGW-w64/MSYS2).'); process.exit(2); }
const env = (!gcc.shell && process.platform === 'win32')
  ? { ...process.env, PATH: dirname(gcc.cmd) + ';' + (process.env.PATH || '') } : process.env;

function compileRun(label, srcs, exeName) {
  const exe = join(build, exeName + (process.platform === 'win32' ? '.exe' : ''));
  const cc = spawnSync(gcc.cmd, ['-std=gnu11', '-O0', '-static', '-Wall', '-Wextra', '-I', inc, ...srcs, '-o', exe],
                       { encoding: 'utf8', shell: gcc.shell, env });
  if (cc.error || cc.status !== 0) {
    console.error(`[${label}] compilazione fallita:\n` + (cc.error ? cc.error.message + '\n' : '') + (cc.stderr || cc.stdout || ''));
    return 1;
  }
  const run = spawnSync(exe, [], { encoding: 'utf8', env, cwd: here });   // cwd=here -> "build/..." del test risolve
  process.stdout.write(run.stdout || '');
  if (run.stderr) process.stderr.write(run.stderr);
  return run.status ?? 1;
}

let rc = 0;
rc |= compileRun('planner', [join(tts, 'nucleo_tts_plan.c'), join(here, 'tts-plan-ctest.c')], 'ttsplan');
console.log('');
rc |= compileRun('index',   [join(tts, 'nucleo_tts_index.c'), join(here, 'ttsidx-ctest.c')], 'ttsidx');
process.exit(rc ? 1 : 0);
