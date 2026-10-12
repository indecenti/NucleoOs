// The card's content record (system/content.json) after a dev sync.
//
// system/content.json is the SD-content engine's own record of what the card holds:
// {"tag":"x.y.z","complete":bool,"declined":bool} (firmware sdc_engine.c: sdc_content_write / _read /
// sdc_content_status). The engine rewrites it — and system/content/manifest.txt — on every install, so on a
// card it filled they always match. A dev sync (push-ota --sync) used to rewrite the files and leave the old
// record behind: a card installed by the 0.5.0 engine and dev-synced to 0.7.2 since still read "0.5.0,
// complete", so the device called it OUTDATED and nagged for a download it does not need.
//
// After a FULL, clean sync the tool now writes the record for the content it just verified onto the card.
// It names the CONTENT's release (firmware/version/VERSION of the tree that staged deploy/sd), not the
// firmware's: on matching firmware the card reads COMPLETE; on any other it reads OUTDATED, and the device's
// update then fetches only the files that differ (the engine hashes the card, it never trusts a record).
// The firmware parses the record with plain substring matches — keep the exact `"key":value` spelling and
// the whole record well under its 192-byte read.

export const CONTENT_JSON = 'system/content.json';

// The record a dev sync writes. `by` is informational (the firmware ignores unknown keys).
export function syncRecord(version) {
  const v = String(version || '').trim().replace(/^v/i, '');
  if (!/^\d+\.\d+\.\d+$/.test(v)) throw new Error(`sd-provenance: not a release version: "${version}"`);
  return `{"tag":"${v}","complete":true,"declined":false,"by":"dev-sync"}\n`;
}

// Read a record the way the firmware does (sdc_content_read: the "tag" string, `"complete":true`,
// `"declined":true` substrings). null for no record.
export function readRecord(text) {
  if (text == null) return null;
  const s = String(text).slice(0, 191);
  const m = /"tag"\s*:\s*"([^"]*)"/.exec(s);
  return { tag: m ? m[1] : '', complete: s.includes('"complete":true'), declined: s.includes('"declined":true') };
}

// Does the card already say "complete, for this release"? (Then the sync leaves the record alone — an
// engine-written record stays the engine's.)
export function recordCurrent(text, version) {
  const r = readRecord(text);
  const v = String(version || '').trim().replace(/^v/i, '');
  return !!r && r.tag.replace(/^v/i, '') === v && r.complete && !r.declined;
}

// May this sync vouch for the whole card? Only a full run (no --only / --exclude) with nothing failed or
// left unverified. Media skipped by default is fine: the release's core content carries no user media.
export function mayRecord({ dryRun = false, only = null, exclude = [], failed = 0, unknown = 0 } = {}) {
  return !dryRun && !only && !(exclude && exclude.length) && failed === 0 && unknown === 0;
}
