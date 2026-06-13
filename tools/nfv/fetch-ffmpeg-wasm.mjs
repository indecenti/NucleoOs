// Vendor ffmpeg.wasm (single-thread) into the Video Studio app so conversion can run IN THE
// BROWSER — no PC companion, no install. Offline-first: the files live on the SD under
// apps/video-studio/www/vendor/ffmpeg/ and are served same-origin by the device httpd.
//
//   node tools/nfv/fetch-ffmpeg-wasm.mjs
//
// We pin the single-thread @ffmpeg/core (0.12.6): it does NOT need SharedArrayBuffer, so it
// works without the cross-origin-isolation (COOP/COEP) headers the multi-thread core requires.
// Run once; then `push-ota --sync` mirrors the vendor/ tree onto the device.
import { mkdir, writeFile, stat } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const OUT = join(REPO, 'apps', 'video-studio', 'www', 'vendor', 'ffmpeg');

const FFMPEG = '0.12.10', UTIL = '0.12.1', CORE = '0.12.6';
const CDN = 'https://cdn.jsdelivr.net/npm';

const human = (n) => { for (const u of ['B', 'KB', 'MB']) { if (n < 1024 || u === 'MB') return `${n.toFixed(1)} ${u}`; n /= 1024; } };

// Tiny fetch-to-buffer with redirect support (Node 18+ has global fetch).
async function get(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`HTTP ${r.status} for ${url}`);
  return Buffer.from(await r.arrayBuffer());
}
// Ask jsdelivr for the package's flat file list so we don't hard-code the webpack worker
// chunk name (it changes between versions, e.g. 814.ffmpeg.js).
async function umdFiles(pkg, ver) {
  const j = await (await fetch(`https://data.jsdelivr.com/v1/packages/npm/${pkg}@${ver}?structure=flat`)).json();
  return (j.files || []).map((f) => f.name).filter((n) => n.startsWith('/dist/umd/'));
}

async function save(name, url) {
  const buf = await get(url);
  await writeFile(join(OUT, name), buf);
  console.log(`  ✓ ${name.padEnd(20)} ${human(buf.length).padStart(10)}   <- ${url.replace(CDN, '')}`);
  return buf.length;
}

async function main() {
  await mkdir(OUT, { recursive: true });
  console.log(`Vendoring ffmpeg.wasm (ST) into ${OUT}\n`);

  // Discover the @ffmpeg/ffmpeg umd files (main + worker chunk).
  const ff = await umdFiles('@ffmpeg/ffmpeg', FFMPEG);
  const main = ff.find((f) => /\/ffmpeg\.js$/.test(f));
  const worker = ff.find((f) => /\.ffmpeg\.js$/.test(f) && f !== main);   // the NNN.ffmpeg.js chunk
  if (!main || !worker) throw new Error(`could not locate ffmpeg umd files; saw: ${ff.join(', ')}`);

  // The worker MUST keep its original chunk basename (e.g. 814.ffmpeg.js): @ffmpeg/ffmpeg's
  // classic-worker path is `${publicPath}/814.ffmpeg.js`, baked into ffmpeg.js. Renaming it
  // would force passing classWorkerURL, which spawns a module worker that can't load the UMD core.
  const workerName = worker.split('/').pop();

  let total = 0;
  total += await save('ffmpeg.js', `${CDN}/@ffmpeg/ffmpeg@${FFMPEG}${main}`);
  total += await save(workerName, `${CDN}/@ffmpeg/ffmpeg@${FFMPEG}${worker}`);
  total += await save('ffmpeg-core.js', `${CDN}/@ffmpeg/core@${CORE}/dist/umd/ffmpeg-core.js`);
  total += await save('ffmpeg-core.wasm', `${CDN}/@ffmpeg/core@${CORE}/dist/umd/ffmpeg-core.wasm`);

  // A manifest recording versions + the worker chunk's real filename (informational).
  await writeFile(join(OUT, 'manifest.json'), JSON.stringify({
    ffmpeg: FFMPEG, core: CORE, threads: false,
    files: { main: 'ffmpeg.js', worker: workerName, core: 'ffmpeg-core.js', wasm: 'ffmpeg-core.wasm' },
  }, null, 2));
  console.log(`  ✓ manifest.json\n\nDone — ${human(total)} vendored. Next: push-ota --sync to put it on the SD.`);
}
main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
