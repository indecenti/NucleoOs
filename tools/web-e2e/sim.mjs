// Start the device simulator (tools/serve-shell.mjs) for the browser E2E suite: a free port and a
// throwaway COPY of the simulated SD, so a test run never dirties the tracked tools/sd-sim and several
// instances (one per language) can run side by side.
import { spawn } from 'node:child_process';
import { cp, mkdtemp, rm, readFile, writeFile, mkdir, link, copyFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

export const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

// Copy the simulated SD. The ~90 MB of ANIMA knowledge packs (*.bin) are read-only for the simulator,
// so they are hard-linked instead of copied (fast, and immune to the transient Windows copyfile
// failures a 90 MB copy hits under antivirus scanning); anything else is a real copy the test may mutate.
async function copySd(src, dst) {
  const big = [];
  await cp(src, dst, { recursive: true, filter: (s) => { if (s.endsWith('.bin')) { big.push(s); return false; } return true; } });
  for (const s of big) {
    const d = join(dst, relative(src, s));
    await mkdir(dirname(d), { recursive: true });
    try { await link(s, d); continue; } catch {}
    for (let i = 0; ; i++) {
      try { await copyFile(s, d); break; } catch (e) { if (i >= 4) throw e; await new Promise((r) => setTimeout(r, 250 * (i + 1))); }
    }
  }
}

export async function startSim({ seed, deviceSockets = 0 } = {}) {
  const sd = await mkdtemp(join(tmpdir(), 'nucleo-e2e-sd-'));
  await copySd(join(REPO, 'tools', 'sd-sim'), sd);
  if (seed) for (const [p, body] of Object.entries(seed)) {        // pre-seed SD files (e.g. a session.json)
    const abs = join(sd, p.replace(/^\//, ''));
    await mkdir(dirname(abs), { recursive: true });
    await writeFile(abs, typeof body === 'string' ? body : JSON.stringify(body));
  }
  const proc = spawn(process.execPath, [join(REPO, 'tools', 'serve-shell.mjs')], {
    env: { ...process.env, PORT: '0', NUCLEO_SD_ROOT: sd, SIM_DEVICE_SOCKETS: String(deviceSockets) }, stdio: ['ignore', 'pipe', 'pipe'],
  });
  let out = '';
  const port = await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('simulator did not start:\n' + out)), 30000);
    const onData = (d) => {
      out += d;
      const m = out.match(/device simulator on http:\/\/localhost:(\d+)/);
      if (m) { clearTimeout(t); resolve(Number(m[1])); }
    };
    proc.stdout.on('data', onData); proc.stderr.on('data', onData);
    if (process.env.SIM_ECHO) { proc.stdout.pipe(process.stdout); }
    proc.on('exit', (c) => { clearTimeout(t); reject(new Error('simulator exited ' + c + '\n' + out)); });
  });
  const api = `http://127.0.0.1:${port}`;
  const control = async (path, body) => {
    const r = await fetch(api + path, { method: body === undefined ? 'GET' : 'POST', body: body === undefined ? undefined : JSON.stringify(body) });
    return r.json();
  };
  return {
    port, sd, api,
    // The shell is loaded from a NON-loopback hostname (mapped to 127.0.0.1 inside Chrome), so the page
    // is NOT a secure context — exactly like the real device on http://192.168.x.x. On localhost the
    // service worker and navigator.locks would run and hide the failure modes the device actually has.
    origin: `http://nucleo.test:${port}`,
    local: `http://localhost:${port}`,          // a SECURE context (loopback): WebGPU, Cache API, SW available
    control,
    readSd: async (p) => readFile(join(sd, p.replace(/^\//, '')), 'utf8'),
    async stop() {
      try { proc.kill(); } catch {}
      await new Promise((r) => setTimeout(r, 200));
      await rm(sd, { recursive: true, force: true }).catch(() => {});
    },
  };
}
