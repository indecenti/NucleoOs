// NucleoOS — over-the-air (network) update for the WEB layer.
//
// The device serves the shell + apps straight from the SD card and exposes an
// atomic file API (/api/fs/{read,write,mkdir}, write = temp + rename). So we can
// update the OS *wirelessly* — no SD removal, no firmware reflash — by mirroring
// the web assets in deploy/sd onto a running device over HTTP.
//
// --sync (the mode release.ps1 uses) is ONE dynamic, idempotent SD sync driven by the
// staging manifest (deploy/sd/.deploy-manifest.json) — NO hand-maintained subtree lists.
// Whatever deploy.ps1 staged gets reconciled onto the device. Policy is small + declarative:
//   - system/config/*  is create-only — user state (pins, wallpaper, settings) is NEVER clobbered.
//   - heavy media (data/ROMs|DOS|Music|Videos) is skipped unless --include-media (flaky over Wi-Fi).
//   - everything else is created if missing / overwritten if changed.
// Change detection is cheap: device sizes come from /api/fs/list (one listing per directory,
// cached) — a same-size small file (<256 KB) is content-verified, a same-size large file is
// trusted (a 12 MB ROM is never downloaded just to diff it). Writes retry with backoff and
// verify the landed size for big files, so a dropped Wi-Fi transfer is safe to re-run: the next
// pass only retouches what's still wrong (resumable). firmware is a separate OTA (ota.ps1).
//
// Legacy modes kept for manual use: default = web layer (www/apps/system/registry);
// --fill-missing = create absent files only (a subset of --sync's behaviour).
//
// Usage:
//   node tools/push-ota.mjs --host http://192.168.1.40 --pin 123456 --sync         (full dynamic sync)
//   node tools/push-ota.mjs --host <url> --pin <code> --sync --dry-run             (preview the plan)
//   node tools/push-ota.mjs --host <url> --pin <code> --sync --include-media       (also push ROMs/music/videos)
//   node tools/push-ota.mjs --host <url> --pin <code> --sync --only system         (restrict to a subtree)
//   node tools/push-ota.mjs --host <url> --dry-run                                 (legacy web-layer mode)
//
// Run it from a machine on the SAME network as the Cardputer (the device IP is
// shown on its screen and in the shell tray / Connection app).
//
// The device now requires PAIRING (see docs/security.md): /api/fs/* is gated by a
// session cookie. Pass --pin <code> with the 6-digit PIN shown on the Cardputer
// screen (Connection app → Pair); this tool pairs (POST /api/pair) and reuses the
// resulting nucleo_session cookie on every file request. Without --pin it probes
// /api/auth/status and, if pairing is required, tells you to read the PIN and retry.

import { readFile, readdir, stat } from 'node:fs/promises';
import { join, relative, posix, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { REGISTRY_REL, mergeRegistryText } from './lib/registry-merge.mjs';
import { isDeviceState } from './lib/sd-policy.mjs';

const REPO = join(fileURLToPath(import.meta.url), '..', '..');
const SD = join(REPO, 'deploy', 'sd');

// Web-layer subtrees that are safe to mirror over the air.
const DEFAULT_TREES = ['www', 'apps', 'system/registry'];

// Paths the sync may CREATE if missing but NEVER overwrites. None today: the old entries (system/config,
// data/anima/workspace.json) are device state in tools/lib/sd-policy.json, so they are never pushed at all.
const PROTECTED = [];
// Heavy media: synced only with --include-media (large WiFi uploads are flaky; copy via the SD).
const MEDIA = ['data/ROMs', 'data/DOS', 'data/Music', 'data/Videos'];

// Device / user STATE (API key, learned cards, caches, settings, keys, sessions, the user's documents...) is
// NEVER pushed nor overwritten: whatever is on the device always wins. Checked INDEPENDENTLY of the staging
// manifest, so a polluted staging tree can never clobber the device. One table for every SD tool:
// tools/lib/sd-policy.json (isDeviceState in tools/lib/sd-policy.mjs, held to tools/lib/sd-policy-vectors.json).
// Below this, a same-size file is content-verified (cheap read); above it we trust size (no download).
const SMALL = 256 * 1024;

function parseArgs(argv) {
  const a = { host: null, dryRun: false, only: null, timeout: 8000, pin: null, fillMissing: false, sync: false, includeMedia: false, exclude: [], timeoutExplicit: false };
  for (let i = 0; i < argv.length; i++) {
    const v = argv[i];
    if (v === '--host' || v === '-h') a.host = argv[++i];
    else if (v === '--dry-run' || v === '-n') a.dryRun = true;
    else if (v === '--only') a.only = argv[++i];
    else if (v === '--timeout') { a.timeout = parseInt(argv[++i]) || 8000; a.timeoutExplicit = true; }
    else if (v === '--pin' || v === '-p') a.pin = String(argv[++i] || '').trim();
    else if (v === '--fill-missing' || v === '--missing') a.fillMissing = true;
    else if (v === '--sync') a.sync = true;
    else if (v === '--include-media') a.includeMedia = true;
    else if (v === '--exclude') a.exclude = String(argv[++i] || '').split(',').map(s => s.trim().replace(/^\/+/, '')).filter(Boolean);
    else if (!a.host && /^https?:\/\//i.test(v)) a.host = v;
  }
  // Sync/fill can touch big files (a missing ROM/track) — give them room unless told otherwise.
  if ((a.fillMissing || a.sync) && !a.timeoutExplicit) a.timeout = 600000;
  return a;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const prefixed = (rel, list) => list.some((p) => rel === p || rel.startsWith(p + '/'));

// system/registry/apps.json is MERGED with the device's copy, never overwritten: the device also lists the
// user's own Agent-published apps (tools/lib/registry-merge.mjs). Reads the device copy with `fetchDev`
// (-> Response). Returns {buf} to write, {same:true} when the device already holds the merged text, or
// {unknown:true} when the device copy can't be read (then we don't write — never risk dropping user apps).
async function mergedRegistry(fetchDev, localBuf, present) {
  let devText = null;
  if (present) {
    try {
      const r = await fetchDev();
      if (r.status === 404) await r.arrayBuffer().catch(() => {});
      else if (r.ok) devText = Buffer.from(await r.arrayBuffer()).toString('utf8');
      else { await r.arrayBuffer().catch(() => {}); return { unknown: true }; }
    } catch { return { unknown: true }; }
  }
  const m = mergeRegistryText(localBuf.toString('utf8'), devText);
  if (m.kept.length) console.log(`  registry: keeping the user's apps ${m.kept.join(', ')}`);
  if (m.shadowed.length) console.warn(`  ⚠ registry: user apps shadowed by system apps with the same id: ${m.shadowed.join(', ')}`);
  if (devText !== null && devText === m.text) return { same: true };
  return { buf: Buffer.from(m.text, 'utf8') };
}

// Load deploy/sd/.deploy-manifest.json (the authoritative staged-file list deploy.ps1 writes).
// Falls back to walking the tree if it's absent. Returns [{rel, abs, size}] sorted.
async function loadStaged(only) {
  const root = only ? join(SD, only.replace(/^\/+/, '')) : SD;
  let entries = [];
  try {
    // The manifest is the file LIST; sizes come from the disk (the manifest's were measured on the staging
    // machine's checkout — CRLF vs LF — and it used to be written with a BOM that made this parse fail).
    // An entry whose file is gone is skipped and reported, never read.
    const man = JSON.parse((await readFile(join(SD, '.deploy-manifest.json'), 'utf8')).replace(/^﻿/, ''));
    let ghosts = 0;
    for (const rel of Object.keys(man)) {
      if (rel === '.deploy-manifest.json') continue;
      if (only && !prefixed(rel, [only.replace(/^\/+/, '')])) continue;
      const abs = join(SD, ...rel.split('/'));
      const st = await stat(abs).catch(() => null);
      if (!st || !st.isFile()) { ghosts++; continue; }
      entries.push({ rel, abs, size: st.size });
    }
    if (ghosts) console.warn(`  ⚠ staging manifest lists ${ghosts} file(s) not in deploy/sd — skipped (re-run deploy.ps1)`);
  } catch {
    for (const abs of await walk(root)) {
      const rel = relative(SD, abs).split(/[\\/]/).join('/');
      if (rel === '.deploy-manifest.json') continue;
      entries.push({ rel, abs, size: null });        // size filled from disk at push time
    }
  }
  entries.sort((x, y) => (x.rel < y.rel ? -1 : x.rel > y.rel ? 1 : 0));
  return entries;
}

// ONE dynamic, idempotent, bombproof SD sync driven by the staging manifest. Replaces the old
// hand-maintained "push web tree / push data/anima / fill missing" trio. Per file:
//   - missing on device           -> create
//   - under system/config/* (user state) and present -> skip (create-only, never clobber)
//   - present, size differs       -> overwrite
//   - present, same size, <256 KB -> content-verify (read+compare), overwrite only if different
//   - present, same size, large   -> skip (trust size; never download a 12 MB ROM to diff it)
// Writes retry with backoff and (for large files) verify the landed size, so a flaky Wi-Fi
// transfer is safe to re-run — the next pass only touches what's still wrong (resumable).
async function syncSd(host, args) {
  const staged = await loadStaged(args.only);
  if (!staged.length) { console.error(`✗ No staged files${args.only ? ' under ' + args.only : ''}. Run deploy.ps1 first.`); return 1; }
  const exclude = [...args.exclude, ...(args.includeMedia ? [] : MEDIA)];
  const stagedPaths = new Set(staged.map(f => '/' + f.rel));

  const listCache = new Map();                       // devDir -> {kind:'ok',files:Map(name->size)} | 'absent' | 'unknown'
  async function dirIndex(devDir, fresh = false) {
    if (!fresh && listCache.has(devDir)) return listCache.get(devDir);
    let res;
    try {
      const r = await fetchWithTimeout(host + '/api/fs/list?path=' + encodeURIComponent(devDir), { cache: 'no-store', headers: authHeaders() }, Math.min(args.timeout, 30000));
      if (r.status === 404) res = { kind: 'absent' };
      else if (r.ok) {
        // A listing cut mid-stream is unparseable: UNKNOWN, never "empty folder" (which would make
        // create-only user state look absent and get overwritten).
        const j = await r.json().catch(() => null);
        res = j && Array.isArray(j.entries)
          ? { kind: 'ok', files: new Map(j.entries.filter((e) => e.type !== 'dir').map((e) => [e.name, Number(e.size) || 0])) }
          : { kind: 'unknown' };
      }
      else res = { kind: 'unknown' };
    } catch { res = { kind: 'unknown' }; }
    listCache.set(devDir, res);
    return res;
  }

  const madeDirs = new Set();
  async function ensureDir(devDir) {
    let cur = '';
    for (const p of devDir.split('/').filter(Boolean)) {
      cur += '/' + p;
      if (madeDirs.has(cur)) continue;
      madeDirs.add(cur);
      if (args.dryRun) continue;
      try { await fetchWithTimeout(host + '/api/fs/mkdir?path=' + encodeURIComponent(cur), { method: 'POST', headers: authHeaders() }, Math.min(args.timeout, 30000)); } catch {}
    }
  }
  // One file's size when its folder can't be listed (firmware before the streaming /api/fs/list
  // answered 503 "oom" on 60+ entry folders): a 1-byte Range read carries the total in
  // Content-Range, so nothing is downloaded. {kind:'ok',size} | {kind:'absent'} | {kind:'unknown'}.
  async function probeFile(devPath) {
    try {
      const r = await fetchWithTimeout(host + '/api/fs/read?path=' + encodeURIComponent(devPath),
        { cache: 'no-store', headers: { ...authHeaders(), range: 'bytes=0-0' } }, Math.min(args.timeout, 30000));
      if (r.status === 404) { await r.arrayBuffer().catch(() => {}); return { kind: 'absent' }; }
      if (r.status === 206) {
        const m = /\/(\d+)\s*$/.exec(r.headers.get('content-range') || '');
        await r.arrayBuffer().catch(() => {});
        return m ? { kind: 'ok', size: Number(m[1]) } : { kind: 'unknown' };
      }
      if (r.ok) return { kind: 'ok', size: (await r.arrayBuffer()).byteLength };   // empty file (plain 200)
      await r.arrayBuffer().catch(() => {});
    } catch { /* fall through */ }
    return { kind: 'unknown' };
  }
  async function deviceEquals(devPath, buf) {
    try {
      const r = await fetchWithTimeout(host + '/api/fs/read?path=' + encodeURIComponent(devPath), { cache: 'no-store', headers: authHeaders() }, args.timeout);
      if (!r.ok) return false;
      return Buffer.from(await r.arrayBuffer()).equals(buf);
    } catch { return false; }
  }
  // Write with retry + backoff; large files get a landed-size verification (re-list the dir).
  async function pushFile(devPath, devDir, name, buf) {
    let last = '';
    for (let k = 1; k <= 3; k++) {
      try {
        await ensureDir(devDir);
        const r = await fetchWithTimeout(host + '/api/fs/write?path=' + encodeURIComponent(devPath), { method: 'POST', body: buf, headers: authHeaders() }, args.timeout);
        if (r.ok) {
          if (buf.length > SMALL) {                  // confirm the big transfer actually landed intact
            const idx = await dirIndex(devDir, true);
            const landed = idx.kind === 'ok' ? idx.files.get(name) : (await probeFile(devPath)).size;
            if (landed !== buf.length) throw new Error('landed size mismatch');
          }
          return true;
        }
        last = 'HTTP ' + r.status;
      } catch (e) { last = e.message; }
      if (k < 3) await sleep(1000 * Math.pow(3, k - 1));   // 1s, 3s
    }
    console.error(`  ✗ ${devPath} → ${last} (after 3 tries)`);
    return false;
  }

  let created = 0, updated = 0, skipped = 0, excluded = 0, unknown = 0, failed = 0, bytes = 0, stateKept = 0;
  for (const f of staged) {
    if (prefixed(f.rel, exclude)) { excluded++; continue; }
    if (isDeviceState(f.rel)) { stateKept++; continue; }   // device-owned state: never push / never overwrite (key, learned, settings, sessions)
    const devPath = '/' + f.rel, devDir = posix.dirname(devPath), name = posix.basename(devPath);
    const idx = await dirIndex(devDir);
    let present = idx.kind === 'ok' && idx.files.has(name);
    let devSize = present ? idx.files.get(name) : -1;

    if (idx.kind === 'unknown') {
      // Folder not listable: stat the file itself instead of skipping the whole folder. Create-only
      // user state stays skipped — a missing system/config/*.json reads back as "{}", so its absence
      // can't be told apart from a real file there.
      const probe = prefixed(f.rel, PROTECTED) ? { kind: 'unknown' } : await probeFile(devPath);
      if (probe.kind === 'unknown') { console.error(`  ? ${devPath} → dir not listable, skipped (won't risk a write)`); unknown++; continue; }
      present = probe.kind === 'ok';
      devSize = present ? probe.size : -1;
    }
    if (present && prefixed(f.rel, PROTECTED)) { skipped++; continue; }   // user state: create-only

    if (f.rel === REGISTRY_REL) {                                          // merge, never overwrite
      const mr = await mergedRegistry(() => fetchWithTimeout(host + '/api/fs/read?path=' + encodeURIComponent(devPath),
        { cache: 'no-store', headers: authHeaders() }, args.timeout), await readFile(f.abs), present);
      if (mr.unknown) { console.error(`  ? ${devPath} → device copy unreadable, registry left as is`); unknown++; continue; }
      if (mr.same) { skipped++; continue; }
      const why = present ? 'update' : 'create';
      if (args.dryRun) { console.log(`  would ${why} ${devPath} (${mr.buf.length} B, merged)`); (why === 'create' ? created++ : updated++); bytes += mr.buf.length; continue; }
      if (await pushFile(devPath, devDir, name, mr.buf)) {
        if (idx.kind === 'ok') idx.files.set(name, mr.buf.length);
        console.log(`  ${why === 'create' ? '+' : '↑'} ${devPath} (${mr.buf.length} B, merged)`);
        (why === 'create' ? created++ : updated++); bytes += mr.buf.length;
      } else failed++;
      continue;
    }

    // Delete stale gzipped orphans if they exist on the device but are not in our staged list.
    // This prevents the device from serving an outdated gzipped copy instead of the new raw file.
    let rewriteRaw = false;
    if (idx.kind === 'ok' && idx.files.has(name + '.gz')) {
      const gzDevPath = devPath + '.gz';
      if (!stagedPaths.has(gzDevPath)) {
        if (args.dryRun) {
          console.log(`  would delete stale gzipped orphan ${gzDevPath}`);
        } else {
          try {
            const r = await fetchWithTimeout(host + '/api/fs/delete?path=' + encodeURIComponent(gzDevPath), { method: 'POST', headers: authHeaders() }, args.timeout);
            if (r.ok) {
              idx.files.delete(name + '.gz');
              console.log(`  - ${gzDevPath} (deleted stale orphan)`);
            } else if (r.status === 403 && /^(www\/shell|apps\/[^/]+\/www)\//i.test(f.rel)) {
              // /www and /apps refuse deletes; the firmware instead drops a stale twin when the raw file is
              // written (nucleo_fsapi fstwin.c) — so rewrite the raw file even if it is current.
              rewriteRaw = true;
              console.log(`  ~ ${gzDevPath} (stale orphan: rewriting ${devPath} so the device drops it)`);
            } else {
              console.warn(`  ⚠ failed to delete stale orphan ${gzDevPath}: HTTP ${r.status}`);
            }
          } catch (e) {
            console.warn(`  ⚠ failed to delete stale orphan ${gzDevPath}: ${e.message}`);
          }
        }
      }
    }

    let need = !present || rewriteRaw, why = present ? 'update' : 'create';
    if (present && !rewriteRaw) {
      const size = f.size == null ? (await readFile(f.abs)).length : f.size;
      if (size !== devSize) { need = true; why = 'update'; }
      else if (size <= SMALL) { need = !(await deviceEquals(devPath, await readFile(f.abs))); why = 'update'; }
    }
    if (!need) { skipped++; continue; }

    const buf = await readFile(f.abs);
    if (args.dryRun) { console.log(`  would ${why} ${devPath} (${buf.length} B)`); (why === 'create' ? created++ : updated++); bytes += buf.length; continue; }
    if (await pushFile(devPath, devDir, name, buf)) {
      if (idx.kind === 'ok') idx.files.set(name, buf.length);
      console.log(`  ${why === 'create' ? '+' : '↑'} ${devPath} (${buf.length} B)`);
      (why === 'create' ? created++ : updated++); bytes += buf.length;
    } else failed++;
  }

  const kb = (bytes / 1024).toFixed(1);
  console.log(args.dryRun
    ? `\nSync dry run: ${created} create, ${updated} update (${kb} KB); ${skipped} current, ${excluded} excluded, ${stateKept} device-state preserved, ${unknown} unverifiable.`
    : `\nSync: ${created} created, ${updated} updated (${kb} KB), ${skipped} current, ${excluded} excluded, ${stateKept} device-state preserved, ${unknown} unverifiable, ${failed} failed.`);
  if (failed) console.error('Some files failed after retries — re-run the same command to resume (it only retouches what is still wrong).');
  return failed ? 1 : 0;
}

// SAFE provisioning: create only the staged files the device is MISSING, never overwrite.
// Existence is resolved per-directory via /api/fs/list (cached), so files aren't downloaded.
async function fillMissing(host, args) {
  const base = args.only ? join(SD, args.only.replace(/^\/+/, '')) : SD;
  let files = (await walk(base)).sort();
  if (!files.length) { console.error(`✗ No staged files under deploy/sd${args.only ? '/' + args.only : ''}. Run deploy.ps1 first.`); return 1; }

  const isExcluded = (rel) => rel === '.deploy-manifest.json' || args.exclude.some(p => rel === p || rel.startsWith(p + '/'));

  // Per-directory listing cache: 'ok' (set of names) | 'absent' (404) | 'unknown' (don't risk a write).
  const listCache = new Map();
  async function dirIndex(devDir) {
    if (listCache.has(devDir)) return listCache.get(devDir);
    let res;
    try {
      const r = await fetchWithTimeout(host + '/api/fs/list?path=' + encodeURIComponent(devDir), { cache: 'no-store', headers: authHeaders() }, args.timeout);
      if (r.status === 404) res = { kind: 'absent' };
      else if (r.ok) {
        const j = await r.json().catch(() => null);   // cut mid-stream -> unknown, never "empty"
        res = j && Array.isArray(j.entries) ? { kind: 'ok', names: new Set(j.entries.map(e => e.name)) } : { kind: 'unknown' };
      }
      else res = { kind: 'unknown' };
    } catch { res = { kind: 'unknown' }; }
    listCache.set(devDir, res);
    return res;
  }

  const madeDirs = new Set();
  async function ensureDir(devDir) {
    let cur = '';
    for (const p of devDir.split('/').filter(Boolean)) {
      cur += '/' + p;
      if (madeDirs.has(cur)) continue;
      madeDirs.add(cur);
      if (args.dryRun) continue;
      try { await fetchWithTimeout(host + '/api/fs/mkdir?path=' + encodeURIComponent(cur), { method: 'POST', headers: authHeaders() }, args.timeout); } catch {}
    }
  }

  let filled = 0, present = 0, unknown = 0, failed = 0, excluded = 0, bytes = 0;
  for (const abs of files) {
    const rel = relative(SD, abs).split(/[\\/]/).join('/');
    if (isExcluded(rel)) { excluded++; continue; }
    if (isDeviceState(rel)) { excluded++; continue; }   // device/user state: never created from staging either
    const devPath = '/' + rel, devDir = posix.dirname(devPath), name = posix.basename(devPath);
    const idx = await dirIndex(devDir);
    if (idx.kind === 'ok' && idx.names.has(name)) { present++; continue; }                 // already there → never overwrite
    if (idx.kind === 'unknown') { console.error('  ? ' + devPath + ' → cannot verify (listing failed), skipped'); unknown++; continue; }
    const buf = await readFile(abs);                                                        // dir absent, or present without this file → missing
    if (args.dryRun) { console.log('  would create ' + devPath + ` (${buf.length} B)`); filled++; bytes += buf.length; continue; }
    await ensureDir(devDir);
    try {
      const r = await fetchWithTimeout(host + '/api/fs/write?path=' + encodeURIComponent(devPath), { method: 'POST', body: buf, headers: authHeaders() }, args.timeout);
      if (!r.ok) { console.error('  ✗ ' + devPath + ' → HTTP ' + r.status); failed++; continue; }
      if (idx.kind === 'ok') idx.names.add(name);                                           // a sibling won't re-list
      console.log('  + ' + devPath + ` (${buf.length} B)`);
      filled++; bytes += buf.length;
    } catch (e) { console.error('  ✗ ' + devPath + ' → ' + e.message); failed++; }
  }

  const kb = (bytes / 1024).toFixed(1);
  console.log(args.dryRun
    ? `\nFill-missing dry run: ${filled} file(s) would be created (${kb} KB); ${present} present, ${excluded} excluded, ${unknown} unverifiable.`
    : `\nFill-missing: ${filled} created (${kb} KB), ${present} present, ${excluded} excluded, ${unknown} unverifiable, ${failed} failed.`);
  return failed ? 1 : 0;
}

function normHost(h) {
  if (!h) return null;
  if (!/^https?:\/\//i.test(h)) h = 'http://' + h;
  return h.replace(/\/+$/, '');
}

// Walk a directory, yielding absolute file paths.
async function walk(dir) {
  let out = [];
  let entries;
  try { entries = await readdir(dir, { withFileTypes: true }); } catch { return out; }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out = out.concat(await walk(p));
    else if (e.isFile()) out.push(p);
  }
  return out;
}

const fetchWithTimeout = (url, opts, ms) => {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), ms);
  return fetch(url, { ...opts, signal: ctrl.signal }).finally(() => clearTimeout(t));
};

// Session cookie obtained from /api/pair; attached to every protected (/api/fs) request.
let sessionCookie = null;
const authHeaders = () => (sessionCookie ? { cookie: sessionCookie } : {});

// Pull the nucleo_session=… pair out of a Set-Cookie header (drop the attributes).
function parseSessionCookie(setCookie) {
  if (!setCookie) return null;
  const m = /(?:^|,\s*)(nucleo_session=[^;]+)/.exec(setCookie);
  return m ? m[1] : null;
}

// Pair with the device using the screen PIN; stores the session cookie on success.
// Returns true if paired (or no auth needed), false otherwise.
async function ensurePaired(host, args) {
  // No PIN given: ask the device whether pairing is needed before bailing out.
  if (!args.pin) {
    let st;
    try {
      const r = await fetchWithTimeout(host + '/api/auth/status', { cache: 'no-store' }, args.timeout);
      st = await r.json();
    } catch {
      return true;   // endpoint absent (older firmware) → assume open, let later calls surface errors
    }
    if (st && st.required && !st.paired) {
      console.error('✗ This device requires pairing. Read the 6-digit PIN off the Cardputer');
      console.error('  screen (Connection app → Pair) and re-run with:  --pin <code>');
      return false;
    }
    return true;     // not required, or already paired
  }

  // PIN given: pair and capture the session cookie.
  let r;
  try {
    r = await fetchWithTimeout(host + '/api/pair',
      { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: args.pin }) },
      args.timeout);
  } catch (e) {
    console.error(`✗ Pairing request failed: ${e.message}`);
    return false;
  }
  if (!r.ok) {
    console.error(`✗ Pairing rejected (HTTP ${r.status}) — check the PIN on the Cardputer screen.`);
    return false;
  }
  sessionCookie = parseSessionCookie(r.headers.get('set-cookie'));
  if (!sessionCookie) {
    console.error('✗ Pairing succeeded but no session cookie was returned by the device.');
    return false;
  }
  console.log('✓ Paired with device (session cookie acquired).');
  return true;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const host = normHost(args.host);
  if (!host) {
    console.error('Usage: node tools/push-ota.mjs --host http://<device-ip> [--pin <code>] [--dry-run] [--only sub/tree]');
    console.error('  --sync           dynamic manifest-driven SD sync (create/update, never clobbers system/config; skips heavy media unless --include-media)');
    console.error('  --fill-missing   create only files the device lacks (never overwrite)');
    console.error('Find the device IP on its screen, or in the shell tray / Connection app.');
    return 2;
  }

  // Reachability probe.
  let status;
  try {
    const r = await fetchWithTimeout(host + '/api/status', {}, args.timeout);
    status = await r.json();
  } catch (e) {
    console.error(`✗ Cannot reach ${host} (/api/status): ${e.message}`);
    console.error('  Make sure you are on the same Wi-Fi/AP as the device and the IP is right.');
    return 1;
  }
  const net = status.network || {};
  console.log(`✓ Device reachable at ${host}  (v${status.version || '?'}, ${net.mode || '?'}${net.ssid ? ' ' + net.ssid : ''})`);
  if (status.storage && status.storage.mounted === false) {
    console.error('✗ SD card is NOT mounted on the device — cannot update web files. Insert the card and reboot.');
    return 1;
  }

  // Pair if the device requires it (gets the session cookie for /api/fs).
  if (!(await ensurePaired(host, args))) return 1;

  // Unified dynamic sync (the one release.ps1 uses): manifest-driven, idempotent, bombproof.
  if (args.sync) return await syncSd(host, args);
  // Provisioning mode: create only the files the device is missing (never overwrite).
  if (args.fillMissing) return await fillMissing(host, args);

  // Build the file list from the chosen subtrees.
  const trees = args.only ? [args.only.replace(/^\/+/, '')] : DEFAULT_TREES;
  let files = [];
  for (const t of trees) files = files.concat(await walk(join(SD, t)));
  files.sort();
  if (!files.length) { console.error(`✗ No files found under deploy/sd/{${trees.join(', ')}}. Run deploy.ps1 first.`); return 1; }

  const madeDirs = new Set();
  async function ensureDir(devDir) {
    // Create each ancestor in order (firmware mkdir is not recursive).
    const parts = devDir.split('/').filter(Boolean);
    let cur = '';
    for (const p of parts) {
      cur += '/' + p;
      if (madeDirs.has(cur)) continue;
      madeDirs.add(cur);
      if (args.dryRun) continue;
      try { await fetchWithTimeout(host + '/api/fs/mkdir?path=' + encodeURIComponent(cur), { method: 'POST', headers: authHeaders() }, args.timeout); }
      catch {}                                   // already exists → device returns non-2xx, harmless
    }
  }

  // Compare device copy with the local buffer; write only if different.
  async function deviceMatches(devPath, buf) {
    try {
      const r = await fetchWithTimeout(host + '/api/fs/read?path=' + encodeURIComponent(devPath), { cache: 'no-store', headers: authHeaders() }, args.timeout);
      if (!r.ok) return false;
      const cur = Buffer.from(await r.arrayBuffer());
      return cur.equals(buf);
    } catch { return false; }
  }

  let written = 0, skipped = 0, failed = 0, bytes = 0;
  for (const abs of files) {
    const rel = relative(SD, abs).split(/[\\/]/).join('/');     // POSIX for the device
    if (isDeviceState(rel)) { skipped++; continue; }           // device/user state: never pushed (sd-policy.json)
    const devPath = '/' + rel;
    let buf = await readFile(abs);
    if (rel === REGISTRY_REL) {                                            // merge, never overwrite
      const mr = await mergedRegistry(() => fetchWithTimeout(host + '/api/fs/read?path=' + encodeURIComponent(devPath),
        { cache: 'no-store', headers: authHeaders() }, args.timeout), buf, true);
      if (mr.unknown) { console.error('  ? ' + devPath + ' → device copy unreadable, registry left as is'); failed++; continue; }
      if (mr.same) { skipped++; continue; }
      buf = mr.buf;
    }
    if (await deviceMatches(devPath, buf)) { skipped++; continue; }
    if (args.dryRun) { console.log('  would update ' + devPath + ` (${buf.length} B)`); written++; bytes += buf.length; continue; }
    await ensureDir(posix.dirname(devPath));
    try {
      const r = await fetchWithTimeout(host + '/api/fs/write?path=' + encodeURIComponent(devPath),
        { method: 'POST', body: buf, headers: authHeaders() }, args.timeout);
      if (!r.ok) { console.error('  ✗ ' + devPath + ' → HTTP ' + r.status); failed++; continue; }
      console.log('  ↑ ' + devPath + ` (${buf.length} B)`);
      written++; bytes += buf.length;
    } catch (e) { console.error('  ✗ ' + devPath + ' → ' + e.message); failed++; }
  }

  const kb = (bytes / 1024).toFixed(1);
  console.log(args.dryRun
    ? `\nDry run: ${written} file(s) would change (${kb} KB), ${skipped} already current.`
    : `\nOTA web update: ${written} file(s) pushed (${kb} KB), ${skipped} unchanged, ${failed} failed.`);
  if (!args.dryRun && written && !failed) console.log('Done. Reload the shell in the browser (SW v18 will refresh cached assets).');
  return failed ? 1 : 0;
}

// Set exitCode and let the event loop drain (avoids a libuv teardown assertion on
// Windows when process.exit() races pending fetch/AbortController handles).
main().then((code) => { process.exitCode = code; }).catch((e) => { console.error(e); process.exitCode = 1; });
