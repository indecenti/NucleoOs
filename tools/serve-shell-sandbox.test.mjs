// The device simulator (tools/serve-shell.mjs) keeps the file API inside its SD root and does not put the
// pairing PIN on the LAN. A sibling folder whose name merely STARTS with the SD root (<sd>-x) used to pass
// the containment check, and the server listened on every interface while /api/_dev/pin answers anyone.
//
//   node --test tools/serve-shell-sandbox.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { networkInterfaces, tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');

async function startSim(env = {}) {
  const base = await mkdtemp(join(tmpdir(), 'nucleo-simbox-'));
  const sd = join(base, 'sd');
  await mkdir(join(sd, 'data'), { recursive: true });
  await writeFile(join(sd, 'data', 'inside.txt'), 'inside');
  await mkdir(join(base, 'sd-x'), { recursive: true });
  await writeFile(join(base, 'sd-x', 'secret.txt'), 'SECRET-OUTSIDE-THE-SD');
  const proc = spawn(process.execPath, [join(REPO, 'tools', 'serve-shell.mjs')], {
    env: { ...process.env, PORT: '0', NUCLEO_SD_ROOT: sd, SIM_DEVICE_SOCKETS: '0', ...env }, stdio: ['ignore', 'pipe', 'pipe'],
  });
  let out = '';
  const port = await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('simulator did not start:\n' + out)), 30000);
    const on = (d) => { out += d; const m = out.match(/device simulator on http:\/\/localhost:(\d+)/); if (m) { clearTimeout(t); resolve(Number(m[1])); } };
    proc.stdout.on('data', on); proc.stderr.on('data', on);
    proc.on('exit', (c) => { clearTimeout(t); reject(new Error('simulator exited ' + c + '\n' + out)); });
  });
  const api = `http://127.0.0.1:${port}`;
  const { pin } = await (await fetch(api + '/api/_dev/pin')).json();
  const pr = await fetch(api + '/api/pair', { method: 'POST', body: JSON.stringify({ pin }) });
  const cookie = (pr.headers.get('set-cookie') || '').split(';')[0];
  return {
    port, api, cookie,
    read: (p) => fetch(api + '/api/fs/read?path=' + encodeURIComponent(p), { headers: { cookie } }),
    async stop() { try { proc.kill(); } catch {} await new Promise((r) => setTimeout(r, 200)); await rm(base, { recursive: true, force: true }).catch(() => {}); },
  };
}

const lanIPv4 = () => Object.values(networkInterfaces()).flat().find((i) => i && i.family === 'IPv4' && !i.internal)?.address || null;

test('the file API cannot reach a sibling folder that shares the SD root as a name prefix', async (t) => {
  const sim = await startSim();
  t.after(() => sim.stop());
  const ok = await sim.read('/data/inside.txt');
  assert.equal(ok.status, 200);
  assert.equal(await ok.text(), 'inside');
  for (const p of ['/../sd-x/secret.txt', '../sd-x/secret.txt', '/data/../../sd-x/secret.txt']) {
    const r = await sim.read(p);
    const body = await r.text();
    assert.ok(!body.includes('SECRET-OUTSIDE-THE-SD'), `${p} escaped the SD root (HTTP ${r.status})`);
  }
});

test('the simulator listens on loopback only unless SIM_HOST opts in', async (t) => {
  const ip = lanIPv4();
  const sim = await startSim();
  t.after(() => sim.stop());
  const local = await fetch(`http://127.0.0.1:${sim.port}/api/auth/status`);
  assert.equal(local.status, 200, 'loopback must keep working (the E2E suite maps nucleo.test to 127.0.0.1)');
  if (!ip) { t.diagnostic('no non-loopback IPv4 interface on this machine: LAN reachability not checked'); return; }
  let reached = false;
  try { const r = await fetch(`http://${ip}:${sim.port}/api/_dev/pin`, { signal: AbortSignal.timeout(3000) }); reached = r.ok; } catch {}
  assert.equal(reached, false, `the pairing PIN is served to the LAN on ${ip}`);
});
