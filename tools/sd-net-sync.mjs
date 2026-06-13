#!/usr/bin/env node
// sd-net-sync.mjs — push the NucleoOS SD payload to a LIVE device over HTTP (no card removal).
//
// Mirrors tools/sd-sync.ps1 (robocopy to a mounted SD) but targets the device's /api/fs over the
// network: pairs with the PIN, diffs deploy/sd against the live SD, and by default uploads
// ONLY what's missing. Never deletes. Protects device-state files the same way sd-sync.ps1 does.
//
// Usage:
//   node tools/sd-net-sync.mjs                         # host .166, only-missing, real upload
//   node tools/sd-net-sync.mjs --dry                   # preview: list what WOULD be sent
//   node tools/sd-net-sync.mjs --force                 # re-upload everything (overwrite)
//   node tools/sd-net-sync.mjs --size                  # also re-upload when device size differs
//   node tools/sd-net-sync.mjs --host 192.168.0.166 --pin 689614
//
// Notes: /api/fs/write does NOT create parent dirs and /api/fs/mkdir is single-level, so we mkdir
// every ancestor first. Uploads are strictly sequential (device heap is tiny) with light retries.

import { readFileSync, statSync, readdirSync } from 'node:fs';
import { join, posix, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const argv = process.argv.slice(2);
const opt = (name, def) => { const i = argv.indexOf('--' + name); return i >= 0 ? (argv[i + 1] ?? true) : def; };
const flag = (name) => argv.includes('--' + name);

const HOST    = String(opt('host', '192.168.0.166'));
const PIN     = String(opt('pin', '689614'));
const FORCE   = flag('force');
const BYSIZE  = flag('size');
const DRY     = flag('dry');
const VERBOSE = flag('verbose');
const BASE    = `http://${HOST}`;

const here = fileURLToPath(new URL('.', import.meta.url));
const SRC  = join(here, '..', 'deploy', 'sd');

// Device-state that must never be clobbered (same set as sd-sync.ps1). The payload shouldn't
// contain these, but enforce defensively so a stray file can't overwrite a key or learned card.
const PROT_FILES = new Set(['teacher.json', 'telemetry.ndjson', 'session.txt', '.httptrace',
  'auth.json', 'volume.json', 'settings.json', 'workspace.json']);
const PROT_EXT = ['.vec'];
const PROT_DIRS = ['data/anima/learned', 'system/config', 'config', 'backups', 'journal'];

const isProtected = (rel) => {
  const base = rel.split('/').pop();
  if (PROT_FILES.has(base)) return true;
  if (PROT_EXT.some((e) => base.endsWith(e))) return true;
  if (PROT_DIRS.some((d) => rel === d || rel.startsWith(d + '/'))) return true;
  return false;
};

// ---- HTTP helpers -----------------------------------------------------------
let COOKIE = '';
const enc = (p) => encodeURIComponent(p);   // %2F decodes back to '/' device-side; rejects '..'

async function http(method, path, { body, headers } = {}) {
  let lastErr;
  for (let attempt = 1; attempt <= 3; attempt++) {
    try {
      const res = await fetch(BASE + path, {
        method,
        headers: { ...(COOKIE ? { Cookie: COOKIE } : {}), ...(headers || {}) },
        body,
      });
      return res;
    } catch (e) {
      lastErr = e;
      await new Promise((r) => setTimeout(r, 200 * attempt));
    }
  }
  throw lastErr;
}

async function pair() {
  const res = await http('POST', '/api/pair', {
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ pin: PIN }),
  });
  if (!res.ok) throw new Error(`pair failed: HTTP ${res.status}`);
  const sc = res.headers.get('set-cookie') || '';
  const m = sc.match(/nucleo_session=[0-9a-f]+/i);
  if (!m) throw new Error('pair: no session cookie returned');
  COOKIE = m[0];
}

// Cached directory listing: rel dir -> Map(name -> {type,size}) | null (does not exist).
const dirCache = new Map();
async function listDir(relDir) {
  if (dirCache.has(relDir)) return dirCache.get(relDir);
  const res = await http('GET', `/api/fs/list?path=${enc('/' + relDir)}`);
  if (res.status === 404) { dirCache.set(relDir, null); return null; }
  if (!res.ok) throw new Error(`list /${relDir}: HTTP ${res.status}`);
  const j = await res.json();
  const map = new Map();
  for (const e of j.entries || []) map.set(e.name, { type: e.type, size: e.size });
  dirCache.set(relDir, map);
  return map;
}

const knownDirs = new Set();   // rel dirs confirmed/created to exist on device
async function ensureDir(relDir) {
  if (!relDir || relDir === '.' || knownDirs.has(relDir)) return;
  const existing = await listDir(relDir);
  if (existing) { knownDirs.add(relDir); return; }
  const parent = relDir.includes('/') ? relDir.slice(0, relDir.lastIndexOf('/')) : '';
  await ensureDir(parent);
  if (!DRY) {
    const res = await http('POST', `/api/fs/mkdir?path=${enc('/' + relDir)}`);
    // mkdir returns 500 when the dir already exists (FATFS EEXIST) — treat as success.
    if (!res.ok && res.status !== 500) throw new Error(`mkdir /${relDir}: HTTP ${res.status}`);
  }
  dirCache.set(relDir, dirCache.get(relDir) || new Map());
  knownDirs.add(relDir);
  if (DRY) log('  mkdir', '/' + relDir);
}

async function uploadFile(relPath, absLocal, size) {
  const parent = relPath.includes('/') ? relPath.slice(0, relPath.lastIndexOf('/')) : '';
  await ensureDir(parent);
  if (DRY) { log('  PUT  ', '/' + relPath, `(${size} B)`); return; }
  const buf = readFileSync(absLocal);
  const res = await http('POST', `/api/fs/write?path=${enc('/' + relPath)}`, {
    headers: { 'Content-Type': 'application/octet-stream' },
    body: buf,
  });
  if (!res.ok) throw new Error(`write /${relPath}: HTTP ${res.status} ${await res.text().catch(() => '')}`);
  await new Promise((r) => setTimeout(r, 15));   // breathe — device heap is small
}

// ---- local walk -------------------------------------------------------------
function walk(dir, out = []) {
  for (const name of readdirSync(dir)) {
    const abs = join(dir, name);
    const st = statSync(abs);
    if (st.isDirectory()) walk(abs, out);
    else out.push({ rel: relative(SRC, abs).split(sep).join('/'), abs, size: st.size });
  }
  return out;
}

const log = (...a) => console.log(...a);

// ---- main -------------------------------------------------------------------
(async () => {
  log(`NucleoOS network SD sync`);
  log(`  source : ${SRC}`);
  log(`  device : ${BASE}   mode: ${DRY ? 'DRY (preview)' : FORCE ? 'FORCE overwrite' : BYSIZE ? 'missing + size-diff' : 'only missing'}`);

  const st = await http('GET', '/api/status');
  if (!st.ok) throw new Error(`device unreachable: HTTP ${st.status}`);
  const status = await st.json();
  if (!status.storage?.mounted) throw new Error('device SD not mounted');
  log(`  SD     : ${status.storage.fs}, free ${(status.storage.free_bytes / 1e9).toFixed(1)} GB\n`);

  await pair();
  log('paired ✓\n');

  const files = walk(SRC).sort((a, b) => a.rel.localeCompare(b.rel));
  let uploaded = 0, skippedExist = 0, skippedProt = 0, dirsMade = knownDirs.size, errors = 0;
  const before = new Set(); // track dirs created during run for the summary
  const dirsAtStart = knownDirs.size;

  for (const f of files) {
    if (isProtected(f.rel)) { skippedProt++; if (VERBOSE) log('  prot ', '/' + f.rel); continue; }

    let needs = FORCE;
    if (!needs) {
      const parent = f.rel.includes('/') ? f.rel.slice(0, f.rel.lastIndexOf('/')) : '';
      const listing = await listDir(parent);
      const base = f.rel.split('/').pop();
      const entry = listing?.get(base);
      if (!entry) needs = true;                                   // missing
      else if (BYSIZE && entry.type === 'file' && entry.size !== f.size) needs = true; // changed
    }

    if (!needs) { skippedExist++; if (VERBOSE) log('  have ', '/' + f.rel); continue; }

    try {
      await uploadFile(f.rel, f.abs, f.size);
      uploaded++;
      if (!DRY) log('  sent ', '/' + f.rel, `(${f.size} B)`);
    } catch (e) {
      errors++;
      log('  ERR  ', '/' + f.rel, '->', e.message);
    }
  }

  dirsMade = knownDirs.size - dirsAtStart;
  log(`\nDone. ${DRY ? 'would upload' : 'uploaded'} ${uploaded} file(s), dirs ensured ${knownDirs.size}` +
      ` (new ${dirsMade}), already present ${skippedExist}, protected-skipped ${skippedProt}, errors ${errors}.`);
  process.exit(errors ? 1 : 0);
})().catch((e) => { console.error('FATAL:', e.message); process.exit(2); });
