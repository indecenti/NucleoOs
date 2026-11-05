// agent-bench.mjs: Nucleo Code's agent on hard tasks against the real Anthropic API (it costs: a few cents a
// task). Each task gets a fresh workspace with its fixture files; the agent never sees the checker, which is
// copied in only after the agent ended and run in the device's engine (build/code-probe/probe.exe).
//   node tools/code/agent-bench.mjs [--model claude-sonnet-5-5] [--effort medium] [--only calc,sudoku]
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync, writeFileSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const model = arg('--model', 'claude-sonnet-5-5'), effort = arg('--effort', 'medium');
const only = (arg('--only', '') || '').split(',').filter(Boolean);
const host = join(root, 'build', 'code-agent', 'agent-host.exe'), probe = join(root, 'build', 'code-probe', 'probe.exe');
const benchDir = join(root, 'tools', 'code', 'bench');
const outDir = join(root, 'build', 'code-agent', 'bench', `${model}-${effort}`);

// what each task's result must show (run in the device engine after the agent ended)
const TASKS = {
  calc:   { run: 'check.js', expect: ['CHECK OK 19'] },
  bugfix: { run: 'inventory.js', expect: ['TOTAL 307.35', 'display 127.75\ncase 57.60\ntastiera 44.00\ncavo USB 42.00\nbatteria 36.00', 'LOW batteria,tastiera'] },
  csv:    { run: 'report.js', expect: ['REGION Centro 231.20\nREGION Isole 191.20\nREGION Nord 192.70\nREGION Sud 185.40', 'GRAND 800.50', 'TOP Olio 312.00'], files: ['report.md'] },
  sudoku: { run: 'sudoku.js', expect: ['534678912', '345286179', 'VALID'] },
  ledger: { run: 'ledger.js', expect: ['BALANCE ANNA 1294.57\nBALANCE BRUNO 120.92\nBALANCE CARLA 495.56\nBALANCE DARIO 784.55\nREJECTED 5'] },
  story:  { run: 'check.js', expect: ['CHECK OK'] },
  lisp:   { run: 'check.js', expect: ['CHECK OK 15'] },
  snake:  { run: 'snake.js', screen: ['240x135', '320x240'], expect: ['DREW 240x135', 'DREW 320x240'] },
  port:   { run: 'stats.js', expect: [readFileSync(join(dirname(fileURLToPath(import.meta.url)), 'bench', 'port', 'expected.txt'), 'utf8').replace(/\r\n/g, '\n').trim()] },
};

// build the two host tools from the sources the module uses (MinGW gcc, cJSON from ESP-IDF)
{
  const MINGW = 'C:/msys64/mingw64/bin', GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
  const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
  const C = join(root, 'firmware', 'components', 'nucleo_code'), V = join(C, 'vendor', 'mquickjs');
  const J = [process.env.IDF_PATH && join(process.env.IDF_PATH, 'components/json/cJSON'), 'C:/esp/esp-idf/components/json/cJSON', 'D:/esp/esp-idf-v5.5.2/components/json/cJSON'].filter(Boolean).find((d) => existsSync(join(d, 'cJSON.c')));
  spawnSync(process.execPath, [join(root, 'tools', 'code', 'gen-tables.mjs')], { encoding: 'utf8' });
  mkdirSync(dirname(host), { recursive: true }); mkdirSync(dirname(probe), { recursive: true });
  const common = ['-std=gnu11', '-O2', '-D_GNU_SOURCE', '-w', '-I', join(root, 'build', 'code-host'), '-I', join(C, 'include'), '-I', V,
    '-I', join(root, 'firmware', 'components', 'nucleo_anima'), '-I', J];
  const eng = [join(C, 'code_rt.c'), join(C, 'code_gfx.c'), join(V, 'mquickjs.c'), join(V, 'dtoa.c'), join(V, 'libm.c'), join(V, 'cutils.c')];
  for (const [exe, srcs] of [[host, [join(root, 'tools', 'code', 'agent-host.c'), join(C, 'code_agent.c'), join(J, 'cJSON.c')]], [probe, [join(root, 'tools', 'code', 'probe.c')]]]) {
    const r = spawnSync(GCC, [...common, ...srcs, ...eng, '-lm', '-o', exe], { encoding: 'utf8', env });
    if (r.status !== 0) { console.error('[agent-bench] build failed: ' + (r.stderr || r.stdout)); process.exit(1); }
  }
}

const results = [];
for (const [name, t] of Object.entries(TASKS)) {
  if (only.length && !only.includes(name)) continue;
  const fx = join(benchDir, name), ws = join(outDir, name);
  rmSync(ws, { recursive: true, force: true }); mkdirSync(ws, { recursive: true });
  for (const f of readdirSync(fx)) if (!['prompt.txt', 'check.js', 'expected.json', 'expected.txt'].includes(f)) cpSync(join(fx, f), join(ws, f));
  process.stdout.write(`\n=== ${name} (${model}, ${effort})\n`);
  const r = spawnSync(host, [ws, model, effort, join(fx, 'prompt.txt'), '64'], { encoding: 'utf8', timeout: 900000, maxBuffer: 64 << 20 });
  const log = (r.stdout || '') + (r.stderr || '');
  writeFileSync(join(outDir, `${name}.log`), log);
  const m = log.match(/STATS (\{.*\})/);
  const st = m ? JSON.parse(m[1]) : { ended: false, error: 'no stats' };
  if (existsSync(join(fx, 'check.js'))) cpSync(join(fx, 'check.js'), join(ws, 'check.js'));
  let out = '';
  for (const scr of t.screen || [null]) {             // an app: once per screen size, its last frame kept next to the log
    const v = spawnSync(probe, [t.run], { encoding: 'utf8', cwd: ws, timeout: 60000, env: { ...process.env, ...(scr ? { PROBE_SCREEN: scr } : {}) } });
    out += (v.stdout || '').replace(/^\d+\s*\| /, '').replace(/\r\n/g, '\n');
    if (scr && existsSync(join(ws, 'probe-shot.png'))) cpSync(join(ws, 'probe-shot.png'), join(outDir, `${name}-${scr}.png`));
  }
  const missing = t.expect.filter((e) => !out.includes(e)).concat((t.files || []).filter((f) => !existsSync(join(ws, f))).map((f) => `file ${f}`));
  const pass = missing.length === 0;
  results.push({ task: name, pass, ...st, missing, output: out.slice(0, 600) });
  console.log(`${pass ? 'PASS' : 'FAIL'} ${name}: ${st.calls} calls, ${st.tools} tools (${st.tool_errors} errors), $${(st.usd || 0).toFixed(3)}, ${(st.ms / 1000).toFixed(0)} s${pass ? '' : '  missing: ' + missing.join(' | ').slice(0, 300)}`);
}
writeFileSync(join(outDir, 'results.json'), JSON.stringify(results, null, 2));
const pass = results.filter((r) => r.pass).length, usd = results.reduce((s, r) => s + (r.usd || 0), 0);
console.log(`\n${pass}/${results.length} passed, $${usd.toFixed(3)} total -> ${join(outDir, 'results.json')}`);
