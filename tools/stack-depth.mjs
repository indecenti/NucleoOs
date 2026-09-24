// STACK-DEPTH — worst-case stack of a firmware entry point, measured on the DEVICE toolchain (xtensa, the
// exact IDF flags from firmware/build/compile_commands.json) instead of guessed. Compiles every .c of a
// component with -fstack-usage -fcallgraph-info=su into a temp dir (never touches firmware/build, so it can't
// race another build), parses the .ci call graphs and prints the deepest call chain from the entry, frame by
// frame. Callees outside the component (newlib, FreeRTOS, FATFS, mbedTLS) have no frame info: each counts
// a flat EXT_COST guess, so treat the total as a floor for paths that go deep into libraries.
// Used to size the ANIMA web workers (docs/memory-budget.md "ANIMA workers").
//
//   node tools/stack-depth.mjs <entry> [--component=nucleo_anima] [--build] [--exclude=regex] [--top=N]
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '..');
const args = process.argv.slice(2);
const component = (args.find((a) => a.startsWith('--component=')) || '--component=nucleo_anima').slice(12);
const OUT = path.join(tmpdir(), 'nucleo-stack-depth', component);
const entry = args.find((a) => !a.startsWith('--')) || 'nucleo_anima_query';
const exclude = (args.find((a) => a.startsWith('--exclude=')) || '').slice(10);
const exRe = exclude ? new RegExp(exclude) : null;
const top = Number((args.find((a) => a.startsWith('--top=')) || '--top=1').slice(6));
const rebuild = args.includes('--build') || !existsSync(OUT);

const db = JSON.parse(readFileSync(path.join(ROOT, 'firmware', 'build', 'compile_commands.json'), 'utf8'));
const srcDir = path.join(ROOT, 'firmware', 'components', component).replace(/\\/g, '/');
const tmpl = db.find((x) => x.file.replace(/\\/g, '/').includes('/components/' + component + '/'));
if (!tmpl) { console.error(`no ${component} source in compile_commands.json — build the firmware once`); process.exit(2); }
if (rebuild) {
  mkdirSync(OUT, { recursive: true });
  for (const f of readdirSync(srcDir).filter((n) => n.endsWith('.c'))) {
    const src = `${srcDir}/${f}`;
    const e = db.find((x) => x.file.replace(/\\/g, '/').endsWith(component + '/' + f)) || tmpl;
    let cmd = e.command.replace(/^ccache\s+/, '');
    const orig = e.file.replace(/\\/g, '/');
    cmd = cmd.split(e.file).join(src).split(orig).join(src);
    const obj = path.join(OUT, f + '.o').replace(/\\/g, '/');
    cmd = cmd.replace(/\s-o\s+\S+/, ` -o "${obj}"`).replace(/\s-MD\s/, ' ').replace(/\s-MT\s+\S+/, ' ').replace(/\s-MF\s+\S+/, ' ')
      + ' -fstack-usage -fcallgraph-info=su';
    const m = cmd.match(/^(\S+)\s+([\s\S]*)$/);
    const rsp = obj + '.rsp';
    writeFileSync(rsp, m[2].replace(/\\(?!")/g, '/'));
    const r = spawnSync(m[1], ['@' + rsp], { cwd: e.directory, encoding: 'utf8' });
    if (r.status !== 0) { console.error('FAIL ' + f + '\n' + (r.stderr || '').slice(0, 2000)); process.exit(1); }
  }
}

// ---- parse .ci (VCG): node { title: "fn" label: "fn\nfile:line:col\nN bytes (static)\n..." }, edge { sourcename targetname }
const self = new Map(), calls = new Map(), dyn = new Set();
for (const f of readdirSync(OUT).filter((n) => n.endsWith('.ci'))) {
  const t = readFileSync(path.join(OUT, f), 'utf8');
  for (const m of t.matchAll(/node:\s*\{\s*title:\s*"([^"]+)"\s*label:\s*"([^"]*)"/g)) {
    const name = m[1], label = m[2];
    const b = label.match(/(\d+) bytes \((static|dynamic[^)]*)\)/);
    if (b) { self.set(name, Math.max(self.get(name) || 0, Number(b[1]))); if (b[2] !== 'static') dyn.add(name); }
    else if (!self.has(name)) self.set(name, null);   // external / no info
  }
  for (const m of t.matchAll(/edge:\s*\{\s*sourcename:\s*"([^"]+)"\s*targetname:\s*"([^"]+)"/g)) {
    if (!calls.has(m[1])) calls.set(m[1], new Set());
    calls.get(m[1]).add(m[2]);
  }
}
// Static functions share names across TUs (title is "file:fn" for statics in some GCCs) — keep as is.
const EXT_COST = 512;   // unknown callee (newlib printf family, FreeRTOS, esp-idf): a conservative guess
const memo = new Map(), onStack = new Set(), recursive = new Set(), externals = new Map();
function worst(fn) {
  if (exRe && exRe.test(fn)) return { cost: 0, path: [] };
  if (memo.has(fn)) return memo.get(fn);
  if (onStack.has(fn)) { recursive.add(fn); return { cost: 0, path: [fn + ' (recursion)'] }; }
  onStack.add(fn);
  const s = self.get(fn);
  let own = s == null ? EXT_COST : s;
  if (s == null) externals.set(fn, (externals.get(fn) || 0) + 1);
  let best = { cost: 0, path: [] };
  for (const c of calls.get(fn) || []) {
    const w = worst(c);
    if (w.cost > best.cost) best = w;
  }
  onStack.delete(fn);
  const res = { cost: own + best.cost, path: [`${fn} ${s == null ? '(ext~' + EXT_COST + ')' : s}`, ...best.path] };
  memo.set(fn, res);
  return res;
}
const keyOf = (n) => [...self.keys()].find((k) => k === n || k.endsWith(':' + n)) || n;
const r = worst(keyOf(entry));
console.log(`entry ${entry}${exclude ? ' excluding /' + exclude + '/' : ''}: worst-case ${r.cost} B (xtensa frames; ext callees ~${EXT_COST} B each)`);
for (const p of r.path) console.log('  ' + p);
if (recursive.size) console.log('recursion at: ' + [...recursive].join(', '));
if (dyn.size) console.log('dynamic frames (alloca/VLA): ' + [...dyn].slice(0, 10).join(', '));
if (top > 1) {
  const big = [...self.entries()].filter(([, v]) => v != null).sort((a, b) => b[1] - a[1]).slice(0, top);
  console.log('\nlargest frames:'); for (const [k, v] of big) console.log(`  ${String(v).padStart(6)}  ${k}`);
}
