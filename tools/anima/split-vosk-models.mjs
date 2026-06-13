// Split the Vosk model .tar.gz files into <8 MB parts so the device webfs can serve them.
//
// The Cardputer's HTTP server resets the connection a few MB into a large sustained read, so a
// single 34/41 MB model is unservable. voice.js `assembleParts()` instead fetches numbered parts
// (model.tar.gz.000, .001, ...) with per-part retries and reassembles them in the browser. This
// tool produces those parts and PROVES they reconcatenate to the exact original (SHA1) and that
// the join is a valid gzip whose tar holds the model files. Run after updating a model.
//
//   node tools/anima/split-vosk-models.mjs <srcModelsDir> <destDir1> [destDir2 ...]
//
import { readFileSync, writeFileSync, readdirSync, rmSync, mkdirSync, existsSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const PART = 7 * 1024 * 1024;                  // 7 MiB — safely under the device's ~8 MB write/read window
const MODELS = ['vosk-model-small-it-0.4.tar.gz', 'vosk-model-small-en-us-0.15.tar.gz'];

const [src, ...dests] = process.argv.slice(2);
if (!src || !dests.length) { console.error('usage: split-vosk-models.mjs <srcDir> <destDir...>'); process.exit(1); }

const sha1 = (buf) => createHash('sha1').update(buf).digest('hex');
// First file name inside an (uncompressed) tar — proves the archive is a real tar, not garbage.
const firstTarEntry = (tar) => tar.subarray(0, 100).toString('latin1').replace(/\0+$/, '');

for (const name of MODELS) {
  const full = readFileSync(join(src, name));
  const want = sha1(full);
  const nParts = Math.ceil(full.length / PART);
  console.log(`\n${name}  ${full.length} bytes  sha1=${want}  -> ${nParts} parts`);

  for (const dest of dests) {
    const dir = join(dest, 'vosk', 'models');
    mkdirSync(dir, { recursive: true });
    // Clear any stale parts from a previous run so a shrunk model can't leave an orphan tail part.
    for (const f of readdirSync(dir)) if (f.startsWith(name + '.')) rmSync(join(dir, f));

    for (let i = 0; i < nParts; i++) {
      const chunk = full.subarray(i * PART, Math.min((i + 1) * PART, full.length));
      writeFileSync(join(dir, `${name}.${String(i).padStart(3, '0')}`), chunk);
    }

    // Verify: read the parts back exactly as voice.js assembleParts() would (.000.. until missing).
    const parts = [];
    for (let i = 0; ; i++) {
      const p = join(dir, `${name}.${String(i).padStart(3, '0')}`);
      if (!existsSync(p)) break;
      parts.push(readFileSync(p));
    }
    const joined = Buffer.concat(parts);
    const got = sha1(joined);
    let tarOk = false, entry = '';
    try { const tar = gunzipSync(joined); entry = firstTarEntry(tar); tarOk = tar.length > 0; } catch (e) { entry = 'GUNZIP FAIL: ' + e.message; }
    const ok = got === want && tarOk;
    console.log(`  ${dest}: ${parts.length} parts, reassembled sha1=${got} match=${got === want} gunzip=${tarOk} firstEntry="${entry}"  => ${ok ? 'OK' : 'FAIL'}`);
    if (!ok) process.exit(2);
  }
}
console.log('\nAll models split and verified byte-exact + valid gzip/tar.');
