// Claude client gate: compiles firmware/components/nucleo_anima/anima_claude.h (the code the device runs)
// with tools/anima-host/claude-stream-test.c and runs it. No network, no device. Usage: npm run claude:test
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const build = join(here, 'build');
mkdirSync(build, { recursive: true });
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const cjsonDirs = [process.env.IDF_PATH && join(process.env.IDF_PATH, 'components/json/cJSON'),
  'C:/esp/esp-idf/components/json/cJSON', 'D:/esp/esp-idf/components/json/cJSON'].filter(Boolean);
const CJSON = cjsonDirs.find((d) => existsSync(join(d, 'cJSON.c')));
if (!CJSON) { console.error(`[claude-stream] cJSON (ESP-IDF) not found in ${cjsonDirs.join(', ')}; set IDF_PATH`); process.exit(1); }
const exe = join(build, 'claude-stream' + (process.platform === 'win32' ? '.exe' : ''));
const cc = spawnSync(GCC, ['-std=gnu11', '-O0', '-Wall', '-Wno-unused-function',
  '-I', join(repo, 'firmware/components/nucleo_anima'), '-I', CJSON,
  join(here, 'claude-stream-test.c'), join(CJSON, 'cJSON.c'), '-o', exe], { encoding: 'utf8', env });
if (cc.error || cc.status !== 0) { console.error('[claude-stream] compile failed:\n' + (cc.stderr || cc.stdout || cc.error?.message)); process.exit(1); }
const run = spawnSync(exe, [], { encoding: 'utf8', env });
const out = run.stdout || '';
console.log(`[claude-stream] ${run.status === 0 ? 'OK' : 'FAIL'}: ${out.trim().split('\n').filter((l) => l.startsWith('FAIL') || l === 'ALL OK').join(' | ')}`);
process.exit(run.status === 0 ? 0 : 1);
