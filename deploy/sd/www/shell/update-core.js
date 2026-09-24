// update-core.js — pure decision logic for the OS update check. No DOM, no timers, no direct
// network: everything effectful is injected, so tools/update-core.test.mjs can drive every branch.
// Shared by the shell's passive notifier (update-check.js) and the Settings "Updates" tab.
//
// Architecture rule this module encodes: the BROWSER talks to GitHub, the device never does
// (no TLS heap, no battery, no phone-home — the same browser-direct rule the LLM keys follow).

export const UPDATE_REPO = 'indecenti/NucleoOs';
export const UPDATE_API_LATEST = `https://api.github.com/repos/${UPDATE_REPO}/releases/latest`;
// GitHub Release assets send NO CORS headers (a browser fetch of one is blocked — see the note in
// .github/workflows/pages.yml), so the updater downloads from the Pages site, which republishes the
// OTA image + checksums + a version marker on every release and serves them with CORS enabled.
export const UPDATE_PAGES_BASE = 'https://indecenti.github.io/NucleoOs/';
export const UPDATE_OTA_BIN = 'nucleoos-latest-ota.bin';   // app-only image for POST /api/ota — NEVER the merged 0x0 image
export const UPDATE_SUMS = 'SHA256SUMS';
export const UPDATE_VERSION_JSON = 'version.json';
export const UPDATE_RELEASES_URL = `https://github.com/${UPDATE_REPO}/releases/latest`;
export const UPDATE_SD_ZIP_URL = `https://github.com/${UPDATE_REPO}/releases/latest/download/nucleoos-latest-sd.zip`;

// Shared localStorage keys (same origin for the shell and every app iframe → one coherent state).
export const LS_CACHE = 'nucleo.update.cache';       // { etag, tag, name, url, notes, checkedAt }
export const LS_NOTIFIED = 'nucleo.update.notified'; // [tags already announced in this browser]
export const LS_ENABLED = 'nucleo.update.enabled';   // '0' = auto-check off (per browser — the check runs here)

export const UPDATE_TTL_MS = 24 * 3600 * 1000;       // one conditional request per browser per day

// --- version parsing -------------------------------------------------------------------------
// The running firmware reports PROJECT_VER: "<maj>.<min>.<pat>+<build>.g<hash>[*]" (see
// firmware/version/version.cmake). Release tags are "v<maj>.<min>.<pat>[.<build>]". Only the
// semver triplet is compared: a rebuild of the same release must never nag anyone.
export function parseSemver(s) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)/.exec(String(s || '').trim());
  if (!m) return null;
  return { maj: +m[1], min: +m[2], pat: +m[3] };
}

export function cmpSemver(a, b) {
  const pa = parseSemver(a), pb = parseSemver(b);
  if (!pa || !pb) return 0;                       // unparsable → "equal": never act on garbage
  for (const k of ['maj', 'min', 'pat']) { if (pa[k] !== pb[k]) return pa[k] < pb[k] ? -1 : 1; }
  return 0;
}

// --- check cadence ---------------------------------------------------------------------------
export function checkDue({ cache, nowMs, ttlMs = UPDATE_TTL_MS }) {
  if (!cache || !cache.checkedAt) return true;
  return (nowMs - cache.checkedAt) >= ttlMs;
}

// --- notification decision -------------------------------------------------------------------
// Notify once per tag per browser; a dev build newer than the release must stay quiet.
export function decideNotify({ currentVer, latestTag, notifiedTags }) {
  if (!parseSemver(currentVer) || !parseSemver(latestTag)) return { notify: false, newer: false };
  const newer = cmpSemver(currentVer, latestTag) < 0;
  const seen = Array.isArray(notifiedTags) && notifiedTags.includes(latestTag);
  return { notify: newer && !seen, newer };
}

// --- GitHub "latest release" fetch (conditional, rate-limit friendly) ------------------------
// Injected fetch → unit-testable. A 304 keeps the old cache (and is free against the API rate
// limit); any error returns the cache unchanged — the caller stays silent on failure by design.
export async function fetchLatestRelease(fetchImpl, cache, nowMs) {
  const headers = { Accept: 'application/vnd.github+json' };
  if (cache && cache.etag) headers['If-None-Match'] = cache.etag;
  let r;
  try { r = await fetchImpl(UPDATE_API_LATEST, { headers, cache: 'no-store' }); }
  catch { return { cache, ok: false }; }
  if (r.status === 304 && cache) return { cache: { ...cache, checkedAt: nowMs }, ok: true };
  if (!r.ok) return { cache, ok: false };
  let d;
  try { d = await r.json(); } catch { return { cache, ok: false }; }
  if (!d || !d.tag_name) return { cache, ok: false };
  return {
    ok: true,
    cache: {
      etag: (r.headers && r.headers.get && r.headers.get('etag')) || '',
      tag: d.tag_name,
      name: d.name || d.tag_name,
      url: d.html_url || UPDATE_RELEASES_URL,
      notes: String(d.body || '').slice(0, 4000),   // shown as plain text, capped
      checkedAt: nowMs,
    },
  };
}

// --- SHA256SUMS parsing ----------------------------------------------------------------------
// `sha256sum` output: "<64 hex>  <name>" per line (a leading '*' on the name = binary mode).
export function parseSha256Sums(text) {
  const map = new Map();
  for (const line of String(text || '').split('\n')) {
    const m = /^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$/.exec(line.trim());
    if (m) map.set(m[2], m[1].toLowerCase());
  }
  return map;
}
