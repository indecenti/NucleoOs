// End-to-end check of EVERY tool that writes a NucleoOS SD card: none of them may delete or overwrite what
// the user (or another firmware) keeps on the card, all of them must keep the user's Agent-published apps
// in system/registry/apps.json, and a second run must change nothing.
//
// Each tool runs FOR REAL against its own fresh fake card (a temp folder): the release payload
// (sd_deploy.py release) + sentinels in every mixed place — the ANIMA API key and learned caches, device
// config/state under /system, an Agent app + its registry entry, downloaded models, app data, other
// firmwares' files on a shared M5Launcher card, empty user folders. Network tools talk to the device
// simulator (tools/serve-shell.mjs) pointed at the card; card tools write the folder directly
// (deploy.ps1 -To via its -TestTarget seam, which only accepts a folder under %TEMP%).
//
//   node tools/sd-tools-e2e.mjs            (npm run sdtools:e2e; Windows runs the PowerShell tools too)
//
// Local gate — it copies a few hundred MB per tool, so it is not part of the fast CI gate.
import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync, mkdirSync, writeFileSync, readFileSync, existsSync, cpSync, statSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { gzipSync, gunzipSync } from 'node:zlib';
import { join, dirname } from 'node:path';

const ROOT = process.cwd();
const PY = ['python', 'python3'].find((b) => spawnSync(b, ['--version']).status === 0);
const WIN = process.platform === 'win32';

// path -> content. Every one of these must be byte-identical after every tool run.
const SENTINELS = {
  'data/anima/teacher.json': '{"provider":"groq","key":"USER-SECRET-KEY"}',
  'data/anima/learned/it.jsonl': '{"q":"user learned"}\n',
  'data/anima/learned/mind.it.jsonl': '{"t":"user mind"}\n',
  'data/anima/session.txt': 'user session\n',
  'data/anima/workspace.json': '{"user":"workspace"}',
  'data/anima/profile.tsv': 'user\tprofile\n',
  'system/config/settings.json': '{"user":"settings","lang":"it"}',
  'system/config/calendar.json': '{"events":["user event"]}',
  'system/keys/device.key': 'USER-KEY',
  'system/sessions/s1': 'session',
  'system/volume.json': '{"id":"user-volume"}',
  'system/time.json': '{"t":1}',
  'system/notify.jsonl': '{"n":1}',
  'system/mail/sent.log': 'user mail log',
  'system/voice/owner.tpl': 'voice template',
  'system/bg.jpg': 'user wallpaper',
  'system/.trash/deleted.txt': 'trash',
  'apps/theme.cfg': 'user-theme',
  'apps/terminal/data/history.txt': 'user history\n',
  'apps/paint/www/models/user-model.bin': 'USER-DOWNLOADED-MODEL',
  'apps/anima/www/forge/models/user-llm.bin': 'USER-LLM',
  'apps/anima/www/vosk/models/user-vosk.bin': 'USER-VOSK',
  'apps/myapp/manifest.json': '{"id":"myapp","created_by":"agent"}',
  'apps/myapp/www/index.html': '<h1>my agent app</h1>',
  'data/Documents/note.txt': 'user note',
  'data/ROMs/gb/game.gb': 'USER-ROM',
  'data/ir/userdata.json': '{"user":"ir"}',
  'data/tts/speak.cfg': '1',
  'journal/events.ndjson': '{"e":1}\n',
  'backups/b.zip': 'backup',
  'config/x.conf': 'other-firmware-config',
  'downloads/launcher-file.bin': 'M5LAUNCHER-FILE',
  'BruceRF/remote.sub': 'OTHER-FIRMWARE',
  'launcher/config.conf': 'OTHER-FIRMWARE-2',
};
const EMPTY_DIRS = ['data/Music', 'data/Pictures', 'BruceRF/empty'];
// A stale .gz twin of a file the payload ships WITHOUT a twin: the device would serve it instead of the file
// (webfs serves "<file>.gz" first). Every tool must leave that file served correctly: twin gone or byte-exact.
const STALE_TWIN_OF = 'apps/calculator/www/i18n.en.json';
// System files an old list used to block by NAME (sd-sync's basename /XF, sd-net-sync's whole learned/ dir):
// removed from the card first, every tool must deliver them.
const MUST_DELIVER = ['system/registry/settings.json', 'data/anima/learned/facets.it.jsonl'];
const AGENT = { id: 'myapp', version: '0.1.0', path: '/apps/myapp', enabled: true, created_by: 'agent', permissions: ['storage.app'] };

const sha = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');
const work = mkdtempSync(join(tmpdir(), 'sdtools-e2e-'));
let failures = 0;

function makeCard(dir, payload) {
  cpSync(payload, dir, { recursive: true });
  for (const [rel, body] of Object.entries(SENTINELS)) { mkdirSync(dirname(join(dir, rel)), { recursive: true }); writeFileSync(join(dir, rel), body); }
  for (const d of EMPTY_DIRS) mkdirSync(join(dir, d), { recursive: true });
  if (!existsSync(join(payload, STALE_TWIN_OF)) || existsSync(join(payload, STALE_TWIN_OF + '.gz')))
    throw new Error(`fixture: ${STALE_TWIN_OF} must ship without a twin`);
  writeFileSync(join(dir, STALE_TWIN_OF + '.gz'), gzipSync('{"stale":"old code"}'));
  for (const rel of MUST_DELIVER) rmSync(join(dir, rel), { force: true });
  const reg = join(dir, 'system/registry/apps.json');
  const doc = JSON.parse(readFileSync(reg, 'utf8'));
  doc.installed.push(AGENT);
  writeFileSync(reg, JSON.stringify(doc, null, 2) + '\n');
}

function check(label, dir) {
  const bad = [];
  for (const [rel, body] of Object.entries(SENTINELS)) {
    const p = join(dir, rel);
    if (!existsSync(p)) bad.push(`deleted ${rel}`);
    else if (readFileSync(p, 'utf8') !== body) bad.push(`changed ${rel}`);
  }
  for (const d of EMPTY_DIRS) if (!existsSync(join(dir, d))) bad.push(`deleted empty dir ${d}`);
  let reg = null;
  try { reg = JSON.parse(readFileSync(join(dir, 'system/registry/apps.json'), 'utf8').replace(/^﻿/, '')); } catch (e) { bad.push(`registry unreadable: ${e.message}`); }
  const mine = reg && reg.installed.find((a) => a.id === 'myapp');
  if (reg && !mine) bad.push('Agent app uninstalled from apps.json');
  else if (mine && JSON.stringify(mine) !== JSON.stringify(AGENT)) bad.push('Agent app entry altered');
  if (reg && reg.installed.length < 40) bad.push(`registry lost the bundled apps (${reg.installed.length})`);
  if (!existsSync(join(dir, 'www/shell/index.html'))) bad.push('payload missing: www/shell/index.html');
  for (const rel of MUST_DELIVER) if (!existsSync(join(dir, rel))) bad.push(`not delivered: ${rel}`);
  const tw = join(dir, STALE_TWIN_OF + '.gz');
  if (existsSync(tw) && !gunzipSync(readFileSync(tw)).equals(readFileSync(join(dir, STALE_TWIN_OF)))) bad.push(`stale twin still shadows ${STALE_TWIN_OF}`);
  console.log(`  ${bad.length ? 'FAIL' : 'ok  '} ${label}${bad.length ? '\n        ' + bad.join('\n        ') : ''}`);
  failures += bad.length ? 1 : 0;
}

const tree = (dir) => {
  const out = [];
  const walk = (d) => { for (const n of readdirSync(d)) { const p = join(d, n); statSync(p).isDirectory() ? walk(p) : out.push(`${p.slice(dir.length)} ${sha(p)}`); } };
  walk(dir); return out.sort().join('\n');
};

async function withSimulator(card, fn, extraEnv = {}) {
  const sim = spawn(process.execPath, ['tools/serve-shell.mjs'], {
    cwd: ROOT, env: { ...process.env, NUCLEO_SD_ROOT: card, PORT: '0', SIM_REGISTRY_FROM_SD: '1', ...extraEnv }, stdio: ['ignore', 'pipe', 'pipe'] });
  try {
    const port = await new Promise((res, rej) => {
      const t = setTimeout(() => rej(new Error('simulator did not start')), 30000);
      sim.stdout.on('data', (b) => { const m = /localhost:(\d+)/.exec(String(b)); if (m) { clearTimeout(t); res(m[1]); } });
      sim.on('exit', (c) => rej(new Error(`simulator exited ${c}`)));
    });
    const pin = (await (await fetch(`http://localhost:${port}/api/_dev/pin`)).json()).pin;
    return await fn(port, pin);
  } finally { sim.kill(); }
}

function run(label, cmd, args) {
  const r = spawnSync(cmd, args, { cwd: ROOT, encoding: 'utf8', maxBuffer: 256 << 20 });
  if (r.status !== 0 && !(r.status === 1 && /sd-net-sync/.test(args.join(' ')))) {
    console.log(`  FAIL ${label}: exit ${r.status}\n${(r.stdout || '').slice(-1500)}${(r.stderr || '').slice(-1500)}`); failures++; return false;
  }
  return r;
}

const TOOLS = [
  { name: 'sd_deploy.py update', go: async (card, payload) => {
      writeFileSync(join(payload, '.deploy-manifest.json'), '{}');   // provision() only requires the master's manifest to exist
      const code = `import sys; sys.path.insert(0, 'tools/nucleo-sd-deploy'); import sd_deploy as S\nS.provision(sys.argv[1], 'update', False, lambda m: None, master=sys.argv[2])`;
      return run('sd_deploy.py update', PY, ['-c', code, card, payload]); } },
  { name: 'push-ota --sync', go: (card) => withSimulator(card, async (port, pin) =>
      run('push-ota --sync', process.execPath, ['tools/push-ota.mjs', '--host', `http://localhost:${port}`, '--pin', pin, '--sync'])) },
  { name: 'sd-net-sync --force', go: (card) => withSimulator(card, async (port, pin) =>
      run('sd-net-sync --force', process.execPath, ['tools/sd-net-sync.mjs', '--host', `localhost:${port}`, '--pin', pin, '--force'])) },
  ...(WIN ? [
    { name: 'sd-sync.ps1', go: (card) => run('sd-sync.ps1', 'powershell', ['-ExecutionPolicy', 'Bypass', '-File', 'tools/sd-sync.ps1', '-Target', card]) },
    { name: 'deploy.ps1 -To', go: (card) => run('deploy.ps1 -To', 'powershell', ['-ExecutionPolicy', 'Bypass', '-File', 'tools/deploy.ps1', '-To', card, '-TestTarget']) },
    // the one-command release (SD path) against the simulator: staging + push-ota --sync + /api/reboot
    { name: 'release.ps1 -SdOnly', go: (card) => withSimulator(card, async (port, pin) =>
        run('release.ps1 -SdOnly', 'powershell', ['-ExecutionPolicy', 'Bypass', '-File', 'tools/release.ps1',
          '-DeviceHost', `localhost:${port}`, '-Pin', pin, '-SkipGate', '-SkipBuild', '-SdOnly'])) },
    // a device installed by M5Launcher: the full release must NOT send firmware (it would be refused) and still
    // sync the SD and reboot
    { name: 'release.ps1 (M5Launcher guest)', go: (card) => withSimulator(card, async (port, pin) => {
        const r = run('release.ps1 guest', 'powershell', ['-ExecutionPolicy', 'Bypass', '-File', 'tools/release.ps1',
          '-DeviceHost', `localhost:${port}`, '-Pin', pin, '-SkipGate', '-SkipBuild']);
        if (r && !/firmware NOT sent/.test(r.stdout + r.stderr)) { console.log('  FAIL release.ps1 guest: no "firmware NOT sent" notice'); failures++; }
        if (r && /OTA firmware \(device reboots/.test(r.stdout)) { console.log('  FAIL release.ps1 guest: tried the firmware OTA'); failures++; }
        return r; }, { SIM_GUEST: '1' }) },
  ] : []),
];

try {
  if (!PY) throw new Error('Python 3 is required');
  const payload = join(work, 'payload');
  const b = spawnSync(PY, ['tools/nucleo-sd-deploy/sd_deploy.py', 'release', payload, '--manifest', join(work, 'm.txt')], { cwd: ROOT, encoding: 'utf8' });
  if (b.status !== 0) throw new Error(`payload build failed:\n${b.stdout}\n${b.stderr}`);
  console.log(`sd-tools e2e: ${TOOLS.length} tools, ${Object.keys(SENTINELS).length} sentinels + ${EMPTY_DIRS.length} empty dirs + an Agent app per card`);
  const only = process.env.SDTOOLS_ONLY;   // e.g. SDTOOLS_ONLY=deploy to run one tool
  for (const t of TOOLS) {
    if (only && !t.name.includes(only)) continue;
    const card = join(work, t.name.replace(/[^a-z0-9]+/gi, '_'));
    makeCard(card, payload);
    if (!(await t.go(card, payload))) continue;
    check(`${t.name}: first run`, card);
    const before = tree(card);
    if (!(await t.go(card, payload))) continue;
    check(`${t.name}: second run`, card);
    const after = tree(card);
    // the only files a converged second run may rewrite are the tools' own bookkeeping
    const diff = after.split('\n').filter((l) => !before.includes(l) && !/deploy-manifest\.json/.test(l));
    if (diff.length) { console.log(`  FAIL ${t.name}: second run rewrote ${diff.length} file(s), e.g. ${diff[0].split(' ')[0]}`); failures++; }
    else console.log(`  ok   ${t.name}: second run changed nothing`);
  }
  if (WIN && (!process.env.SDTOOLS_ONLY || 'deploy.ps1'.includes(process.env.SDTOOLS_ONLY))) {   // -DryRun must not write a single byte on the card
    const card = join(work, 'deploy_dryrun');
    makeCard(card, payload);
    const before = tree(card);
    if (run('deploy.ps1 -To -DryRun', 'powershell', ['-ExecutionPolicy', 'Bypass', '-File', 'tools/deploy.ps1', '-To', card, '-TestTarget', '-DryRun'])) {
      if (tree(card) !== before) { console.log('  FAIL deploy.ps1 -DryRun wrote to the card'); failures++; }
      else console.log('  ok   deploy.ps1 -To -DryRun: card byte-identical');
    }
  } else if (!WIN) console.log('  (PowerShell tools skipped: not Windows)');
} catch (e) { console.log(`sd-tools e2e: ERROR ${e.message}`); failures++; }
finally {
  rmSync(work, { recursive: true, force: true });
  // deploy.ps1 re-stages deploy/sd as a side effect; the staging tree is the tool's own and is left as it wrote it
}
console.log(`sd-tools e2e: ${failures ? `${failures} FAILURE(S)` : 'all tools preserve the card'}`);
process.exit(failures ? 1 : 0);
