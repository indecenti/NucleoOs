// game_text (the native games' 5-language text, firmware/components/nucleo_app/game_text.cpp): host-compiled
// with MinGW against packs built by build.mjs, a live language change, the English fallback, and damaged
// packs that must be refused. Also: build.mjs refuses non-ASCII, missing translations and collisions.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { pack } from './build.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const MINGW = 'C:/msys64/mingw64/bin';
const GPP = existsSync(`${MINGW}/g++.exe`) ? `${MINGW}/g++.exe` : 'g++';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };

const demo = { game: 'demo', strings: {
  'Play': { es: 'Jugar', fr: 'Jouer', de: 'Spielen' },
  'Level %d': { es: 'Nivel %d', fr: 'Niveau %d', de: 'Level %d' },
} };

test('build.mjs refuses what the device could not show', () => {
  assert.throws(() => pack({ game: 'x', strings: { 'Menu': { es: 'Menú', fr: 'Menu', de: 'Menue' } } }, 'es'), /non-ASCII/);
  assert.throws(() => pack({ game: 'x', strings: { 'Menu': { es: 'Menu', fr: '', de: 'Menue' } } }, 'fr'), /no fr translation/);
});

test('game_text.cpp: languages, live change, fallback, damaged packs', () => {
  const box = join(tmpdir(), `gtext-${process.pid}`);
  const dir = join(box, 'sd', 'system', 'i18n', 'games');
  rmSync(box, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  for (const l of ['es', 'fr', 'de']) writeFileSync(join(dir, `demo.${l}`), pack(demo, l));
  const good = pack(demo, 'es');
  writeFileSync(join(dir, 'truncated.es'), good.subarray(0, 9));
  const m = Buffer.from(good); m.write('XXXX', 0, 'latin1'); writeFileSync(join(dir, 'badmagic.es'), m);
  const o = Buffer.from(good); o.writeUInt16LE(60000, 6 + 4); writeFileSync(join(dir, 'offsetout.es'), o);
  const n = Buffer.from(good); n[n.length - 1] = 0x41; writeFileSync(join(dir, 'nonul.es'), n);
  writeFileSync(join(dir, 'huge.es'), Buffer.concat([good, Buffer.alloc(20000)]));
  const exe = join(box, 'gt.exe');
  execFileSync(GPP, ['-std=gnu++17', '-O1', '-Wall', '-fsanitize=bounds', '-fsanitize-undefined-trap-on-error',
    '-I', join(root, 'firmware', 'components', 'nucleo_app'), '-I', join(root, 'firmware', 'components', 'nucleo_storage', 'include'),
    '-I', join(root, 'tools', 'ui-host', 'shim'),
    join(here, 'game_text_test.cpp'), join(root, 'firmware', 'components', 'nucleo_app', 'game_text.cpp'), '-o', exe], { env, stdio: 'pipe' });
  const r = spawnSync(exe, [], { cwd: box, env, encoding: 'utf8' });
  rmSync(box, { recursive: true, force: true });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /game_text: all green/);
});
