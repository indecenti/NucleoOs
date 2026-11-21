#!/usr/bin/env node
// XiaoZhi client host test: builds xz_core.c + libopus (fixed point, pseudostack: the module's own flags) with the
// host shims, starts the fake service, runs a whole session, then checks what the service saw.
//   node tools/xiaozhi-host/run.mjs
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const here = join(root, 'tools', 'xiaozhi-host');
const FW = join(root, 'firmware', 'components');
const OPUS = join(FW, 'opus', 'opus');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };
const IDF = process.env.IDF_PATH || 'C:/esp/esp-idf';
const CJSON = join(IDF, 'components', 'json', 'cJSON');
const out = join(root, 'build', 'xiaozhi-host');
mkdirSync(out, { recursive: true });

const opusSrc = readFileSync(join(FW, 'opus', 'sources.cmake'), 'utf8').match(/"opus\/[^"]+"/g).map((s) => join(FW, 'opus', s.slice(1, -1)));
const exe = join(out, 'xz_host.exe');
const args = ['-O1', '-w', '-DOPUS_BUILD', '-DFIXED_POINT', '-DDISABLE_FLOAT_API', '-DNONTHREADSAFE_PSEUDOSTACK',
  '-DGLOBAL_STACK_SIZE=16384', '-DHAVE_LRINT', '-DHAVE_LRINTF',
  '-include', join(here, 'shim', 'host_compat.h'), `-I${join(here, 'shim')}`, `-I${join(FW, 'nucleo_app')}`, `-I${join(FW, 'nucleo_app', 'include')}`,
  `-I${join(FW, 'nucleo_audio', 'include')}`, `-I${join(OPUS, 'include')}`, `-I${join(OPUS, 'celt')}`, `-I${join(OPUS, 'silk')}`,
  `-I${join(OPUS, 'silk', 'fixed')}`, `-I${join(OPUS, 'src')}`, `-I${OPUS}`, `-I${CJSON}`,
  join(here, 'host.c'), join(FW, 'nucleo_app', 'xz_core.c'), join(CJSON, 'cJSON.c'), ...opusSrc,
  '-o', exe, '-lws2_32', '-lm'];
const b = spawnSync(GCC, args, { env, encoding: 'utf8' });
if (b.status !== 0) { console.error(b.stderr || b.stdout); process.exit(1); }

const port = 18790 + Math.floor(Math.random() * 1000);
const report = join(out, 'server-report.json');
const py = process.platform === 'win32' ? 'python' : 'python3';
const srv = spawn(py, ['-I', join(here, 'fake_server.py'), String(port), report], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((res) => srv.stdout.once('data', res));
const r = spawnSync(exe, [String(port)], { cwd: root, env, encoding: 'utf8', timeout: 90000 });
srv.kill();
process.stdout.write(r.stdout || '');
if (r.stderr) process.stderr.write(r.stderr);
let fails = r.status === 0 ? 0 : 1;
const rep = JSON.parse(readFileSync(report, 'utf8'));
const check = (c, what) => { console.log(`${c ? 'ok  ' : 'FAIL'} server: ${what}`); if (!c) fails++; };
check(rep.ota >= 3, `OTA asked ${rep.ota} times (code, after binding, after restart)`);
check(rep.activate === 2, `activate polled ${rep.activate} times (202, 200)`);
check(rep.frames_up >= 45, `${rep.frames_up} Opus frames received (3 s of 60 ms)`);
check(rep.pong === true, 'ping answered');
check(/^Bearer /.test(rep.headers.authorization || ''), 'Authorization: Bearer token');
const mac0 = parseInt((rep.headers['device-id'] || 'zz').slice(0, 2), 16);
check((mac0 & 3) === 2 && /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/.test(rep.headers['device-id']), `Device-Id is a locally administered unicast MAC (${rep.headers['device-id']})`);
check(/^[0-9a-f]{8}-[0-9a-f]{4}-4/.test(rep.headers['client-id'] || ''), 'Client-Id is a UUID v4');
check(rep.errors.length === 0, `no protocol errors${rep.errors.length ? ': ' + rep.errors.join('; ') : ''}`);
console.log(fails ? `xiaozhi-host: ${fails} FAILED` : 'xiaozhi-host: everything passed');
process.exit(fails ? 1 : 0);
