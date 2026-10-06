// Browser E2E — DOS Importer against the simulator. The bundle the app writes to /data/DOS is parsed HERE,
// in Node, with an independent ZIP reader: every file byte-identical with a valid CRC, a .jsdos/dosbox.conf
// that runs the chosen program with the chosen profile. Also: a .zip (deflated, nested folders) is unpacked,
// a ready .jsdos passes through untouched, and every refusal (not a ZIP, nothing runnable, SD write failed)
// is SAID on screen — through the file picker as well as drag & drop — and never writes a file.
import test from 'node:test';
import assert from 'node:assert/strict';
import zlib from 'node:zlib';
import { readFile, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const EN = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/dos-importer/www/i18n.en.json', import.meta.url), 'utf8'));

// ---- ZIP helpers (test side, independent of the app) ----
function makeZip(entries, { deflate = true } = {}) {            // entries: [[name, Buffer]]
  const locals = [], central = []; let off = 0;
  for (const [name, data] of entries) {
    const nb = Buffer.from(name, 'utf8'), comp = deflate ? zlib.deflateRawSync(data) : data, crc = zlib.crc32(data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(deflate ? 8 : 0, 8);
    lh.writeUInt32LE(crc, 14); lh.writeUInt32LE(comp.length, 18); lh.writeUInt32LE(data.length, 22); lh.writeUInt16LE(nb.length, 26);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(deflate ? 8 : 0, 10);
    ch.writeUInt32LE(crc, 16); ch.writeUInt32LE(comp.length, 20); ch.writeUInt32LE(data.length, 24); ch.writeUInt16LE(nb.length, 28); ch.writeUInt32LE(off, 42);
    locals.push(lh, nb, comp); central.push(ch, nb); off += 30 + nb.length + comp.length;
  }
  const cd = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(off, 16);
  return Buffer.concat([...locals, cd, end]);
}
function readZip(buf) {                                          // → Map(name → Buffer), asserting structural validity
  const eocd = buf.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  assert.ok(eocd >= 0, 'no end-of-central-directory record');
  const count = buf.readUInt16LE(eocd + 10), cdSize = buf.readUInt32LE(eocd + 12);
  let cd = buf.readUInt32LE(eocd + 16);
  assert.equal(cd + cdSize, eocd, 'central directory size/offset are inconsistent');
  const out = new Map();
  for (let i = 0; i < count; i++) {
    assert.equal(buf.readUInt32LE(cd), 0x02014b50, 'bad central header');
    const method = buf.readUInt16LE(cd + 10), crc = buf.readUInt32LE(cd + 16), csize = buf.readUInt32LE(cd + 20), usize = buf.readUInt32LE(cd + 24);
    const nlen = buf.readUInt16LE(cd + 28), elen = buf.readUInt16LE(cd + 30), clen = buf.readUInt16LE(cd + 32), lho = buf.readUInt32LE(cd + 42);
    const name = buf.subarray(cd + 46, cd + 46 + nlen).toString('utf8');
    assert.equal(buf.readUInt32LE(lho), 0x04034b50, `bad local header for ${name}`);
    assert.equal(buf.subarray(lho + 30, lho + 30 + buf.readUInt16LE(lho + 26)).toString('utf8'), name, 'local/central names differ');
    assert.equal(buf.readUInt32LE(lho + 14), crc, `local CRC of ${name} differs from the central one`);
    const start = lho + 30 + buf.readUInt16LE(lho + 26) + buf.readUInt16LE(lho + 28);
    const comp = buf.subarray(start, start + csize);
    const data = method === 0 ? Buffer.from(comp) : method === 8 ? zlib.inflateRawSync(comp) : assert.fail('method ' + method);
    assert.equal(data.length, usize, `size of ${name}`);
    assert.equal(zlib.crc32(data), crc, `CRC of ${name}`);
    out.set(name, data);
    cd += 46 + nlen + elen + clen;
  }
  return out;
}
const conf = (zip) => zip.get('.jsdos/dosbox.conf').toString('utf8');
const autoexec = (c) => c.split('[autoexec]\n')[1].trim().split('\n');

// ---- page helpers ----
async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/dos-importer/`);
  assert.ok(await page.waitFor(`document.querySelector('[data-i18n="btn_install"]').textContent === ${JSON.stringify(EN.btn_install)}`, { timeout: 45000 }), 'app started');
  await page.eval(`window.__msgs = []; addEventListener('message', (e) => window.__msgs.push(e.data)); true`);
  return page;
}
const fileList = (files) => JSON.stringify(files.map(([n, b]) => [n, Buffer.from(b).toString('base64')]));
const MAKE_DT = (files) => `const dt = new DataTransfer(); for (const [n, b] of ${fileList(files)}) dt.items.add(new File([Uint8Array.from(atob(b), (c) => c.charCodeAt(0))], n));`;
const pick = (files) => `(() => { ${MAKE_DT(files)} const i = document.getElementById('picker'); i.files = dt.files; i.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`;
const drop = (files) => `(() => { ${MAKE_DT(files)} document.getElementById('drop').dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); return true; })()`;
const STATUS = `(() => { const s = document.getElementById('status'); return { cls: s.className.replace('status', '').trim(), text: s.textContent }; })()`;
// settled = not "Reading…/Unzipping…" any more
const settled = (page) => page.waitFor(`(() => { const s = ${STATUS}; return !s.cls.includes('busy') && s; })()`, { timeout: 15000 });
const writes = (sim) => sim.control('/api/_sim/stats').then((s) => s.byPath['/api/fs/write'] || 0);
const sdFile = (sim, p) => readFile(join(sim.sd, p));
const sdHas = (sim, p) => stat(join(sim.sd, p)).then(() => true, () => false);
async function install(page) {
  await page.eval(`document.getElementById('install').click(), true`);
  return page.waitFor(`(() => { const s = ${STATUS}; return (s.cls === 'ok' || s.cls === 'err') && !document.getElementById('install').disabled && s; })()`, { timeout: 20000 });
}

test('dos-importer: picked files become a valid js-dos bundle on the SD, with the chosen profile', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const EXE = Buffer.from([0x4d, 0x5a, 0x90, 0x00, 0x03, 0xff, 0x00, 0x10]), DAT = Buffer.from(Array.from({ length: 3000 }, (_, i) => (i * 7) & 0xff));
  const ODD = '<img src=x onerror="window.__xss=1">.txt';
  await page.eval(pick([['PRINCE.EXE', EXE], ['LEVELS.DAT', DAT], [ODD, Buffer.from('hi')]]));
  await settled(page);
  assert.equal(await page.eval(`document.getElementById('config').classList.contains('hidden')`), false, 'the options card opened');
  assert.equal(await page.eval(`document.getElementById('name').value`), 'PRINCE', 'install name from the program');
  assert.equal(await page.eval(`document.getElementById('run').value`), 'PRINCE.EXE', 'the only executable is pre-selected');
  assert.deepEqual(await page.eval(`[...document.querySelectorAll('#filelist .nm')].map((e) => e.textContent)`), ['PRINCE.EXE', 'LEVELS.DAT', ODD], 'file names listed as text');
  assert.equal(await page.eval(`document.querySelectorAll('#filelist img').length`), 0, 'a file name never becomes markup');
  assert.equal(await page.eval(`document.querySelector('#filelist .row.exe .nm').textContent`), 'PRINCE.EXE', 'the runnable file is marked');

  // EGA profile, no sound, a friendlier install name.
  await page.eval(`(() => { const p = document.getElementById('profile'); p.value = 'ega'; p.dispatchEvent(new Event('change')); document.getElementById('sound').checked = false;
    const n = document.getElementById('name'); n.value = 'Prince of Persia!'; n.dispatchEvent(new Event('input')); return true; })()`);
  assert.equal(await page.eval(`document.getElementById('dest').textContent`), EN.dest_write_to.replace('{path}', '/data/DOS/Prince_of_Persia_.jsdos'), 'the destination shows the sanitised name');
  const st = await install(page);
  assert.equal(st.cls, 'ok', 'installed: ' + st.text);
  assert.ok(st.text.startsWith(EN.st_installed.split('{path}')[0] + '/data/DOS/Prince_of_Persia_.jsdos'), st.text);
  const zip = readZip(await sdFile(sim, '/data/DOS/Prince_of_Persia_.jsdos'));
  assert.deepEqual([...zip.keys()].sort(), ['.jsdos/dosbox.conf', 'LEVELS.DAT', 'PRINCE.EXE', ODD].sort(), 'every picked file + the generated config');
  assert.deepEqual(zip.get('PRINCE.EXE'), EXE, 'the program is byte-identical');
  assert.deepEqual(zip.get('LEVELS.DAT'), DAT, 'the data file is byte-identical');
  const c = conf(zip);
  assert.match(c, /^machine=ega$/m); assert.match(c, /^cycles=fixed 3000$/m); assert.match(c, /^memsize=16$/m);
  assert.doesNotMatch(c, /\[sblaster\]/, 'sound off = no Sound Blaster section');
  assert.match(c, /^xms=true$/m);
  assert.deepEqual(autoexec(c), ['@echo off', 'mount c .', 'c:', 'PRINCE.EXE'], 'the bundle starts the chosen program');
  assert.deepEqual(await page.eval(`window.__msgs.filter((m) => m && m.type === 'open-app')`), [{ type: 'open-app', id: 'dosbox', reload: true }], 'DOS Box is surfaced once');
  assert.equal(await page.eval(`window.__xss`), undefined);
});

test('dos-importer: a deflated .zip with folders is unpacked; the run target and paths are kept', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const files = [['GAME/BIN/GAME.EXE', Buffer.from('MZ' + 'x'.repeat(5000))], ['GAME/SETUP.BAT', Buffer.from('@echo setup\r\n')], ['GAME/DATA/L1.DAT', Buffer.alloc(2048, 0x41)]];
  await page.eval(pick([['Game Pack.zip', makeZip(files)]]));
  await settled(page);
  assert.equal(await page.eval(`document.getElementById('name').value`), 'Game_Pack', 'name from the archive');
  assert.deepEqual(await page.eval(`[...document.getElementById('run').options].map((o) => o.value)`), ['GAME/SETUP.BAT', 'GAME/BIN/GAME.EXE'], 'runnable files, shallowest first');
  assert.equal(await page.eval(`document.getElementById('run').value`), 'GAME/SETUP.BAT', 'the shallowest one is the default');
  await page.eval(`(() => { const r = document.getElementById('run'); r.value = 'GAME/BIN/GAME.EXE'; const p = document.getElementById('profile'); p.value = 'dos4gw'; p.dispatchEvent(new Event('change')); return true; })()`);
  assert.equal((await install(page)).cls, 'ok');
  const zip = readZip(await sdFile(sim, '/data/DOS/Game_Pack.jsdos'));
  for (const [n, b] of files) assert.deepEqual(zip.get(n), b, `${n} survived unzip + re-bundle byte-identical`);
  const c = conf(zip);
  assert.deepEqual(autoexec(c), ['@echo off', 'mount c .', 'c:', 'cd GAME\\BIN', 'GAME.EXE'], 'DOS path separators + the chosen program');
  assert.match(c, /^memsize=64$/m); assert.match(c, /^cycles=max$/m); assert.match(c, /^\[sblaster\]$/m);

  // A .BAT target is CALLed (so control returns to the autoexec).
  await page.eval(pick([['setup.zip', makeZip([['INSTALL.BAT', Buffer.from('@echo hi')], ['X.COM', Buffer.from([0xc3])]], { deflate: false })]]));
  await settled(page);
  assert.equal(await page.eval(`document.getElementById('run').value`), 'INSTALL.BAT');
  assert.equal((await install(page)).cls, 'ok');
  assert.deepEqual(autoexec(conf(readZip(await sdFile(sim, '/data/DOS/setup.jsdos')))), ['@echo off', 'mount c .', 'c:', 'call INSTALL.BAT']);
});

test('dos-importer: a ready .jsdos is installed byte-for-byte, options locked', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const bundle = makeZip([['.jsdos/dosbox.conf', Buffer.from('[autoexec]\nKEEN.EXE\n')], ['KEEN.EXE', Buffer.from('MZkeen')]]);
  await page.eval(pick([['keen4.jsdos', bundle]]));
  await settled(page);
  assert.equal(await page.eval(`document.getElementById('title').textContent`), EN.files_ready_bundle);
  assert.deepEqual(await page.eval(`['profile','cycles','mem','machine','sound'].map((id) => document.getElementById(id).disabled)`), [true, true, true, true, true], 'nothing to configure');
  assert.equal(await page.eval(`document.getElementById('run').style.display`), 'none');
  assert.equal((await install(page)).cls, 'ok');
  assert.deepEqual(await sdFile(sim, '/data/DOS/keen4.jsdos'), bundle, 'the bundle is written unchanged');
});

test('dos-importer: refusals are shown on screen and never write a file — picker AND drag & drop', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const NOT_ZIP = [['broken.zip', Buffer.from('this is not a zip archive at all, just text')]];
  const NO_RUN = [['README.TXT', Buffer.from('read me')], ['DATA.DAT', Buffer.from([1, 2, 3])]];

  for (const [how, put] of [['picker', pick], ['drop', drop]]) {
    await t.test(`${how}: a file named .zip that is not a ZIP`, async () => {
      await page.eval(`document.getElementById('reset').click(), true`);
      await sim.control('/api/_sim/stats', { reset: true });
      await page.eval(put(NOT_ZIP));
      const st = await settled(page);
      assert.deepEqual(st, { cls: 'err', text: EN.err_not_zip }, 'the user is told the archive is not valid');
      assert.equal(await page.eval(`document.getElementById('config').classList.contains('hidden')`), true, 'no options for a broken archive');
      assert.equal(await writes(sim), 0);
    });
    await t.test(`${how}: files with nothing runnable`, async () => {
      await page.eval(`document.getElementById('reset').click(), true`);
      await sim.control('/api/_sim/stats', { reset: true });
      await page.eval(put(NO_RUN));
      const st = await settled(page);
      assert.deepEqual(st, { cls: 'err', text: EN.err_no_runnable }, 'the user is told WHY Install is disabled');
      assert.equal(await page.eval(`document.getElementById('install').disabled`), true, 'nothing to run → nothing to install');
      assert.equal(await writes(sim), 0);
    });
  }

  await t.test('the SD write fails: the error is shown, nothing claims success, Install can be retried', async () => {
    await page.eval(`document.getElementById('reset').click(), true`);
    await page.eval(pick([['GO.COM', Buffer.from([0xcd, 0x20])]]));
    await settled(page);
    await page.eval(`window.__msgs = []; true`);
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500, times: 1 });
    const st = await install(page);
    assert.equal(st.cls, 'err');
    assert.ok(st.text.startsWith(EN.err_install_failed.split('{msg}')[0]), st.text);
    assert.equal(await page.eval(`window.__msgs.filter((m) => m && m.type === 'open-app').length`), 0, 'DOS Box is not opened for a failed install');
    assert.equal(await sdHas(sim, '/data/DOS/GO.jsdos'), false);
    assert.equal((await install(page)).cls, 'ok', 'the retry (device healthy again) installs');
    assert.ok(readZip(await sdFile(sim, '/data/DOS/GO.jsdos')).has('GO.COM'));
  });
});
