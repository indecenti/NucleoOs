// registry-merge — THE rule for updating system/registry/apps.json on a card that is already in use.
//
// The release's registry is authoritative for every bundled (system) app. On top of it, the device may carry
// the user's own web apps, published by the Agent app (apps/agent/www/app-publish.js planRegistryUpdate) and
// stamped "created_by": "agent". Overwriting apps.json with the release copy — what every SD tool used to do —
// silently uninstalled them. Every tool that writes a card's apps.json goes through mergeRegistryText():
//   • start from the release document (its entries, fields and order, byte-exact when nothing is added);
//   • append each device entry with created_by === "agent" whose id the release does not ship, unchanged
//     (its enabled flag, permissions, version — the Agent owns them);
//   • an Agent app whose id the release now ships is shadowed by the system app (reported, not duplicated);
//   • a missing / unreadable / malformed device file carries nothing over (nothing can be preserved).
// Python twin: tools/nucleo-sd-deploy/sd_deploy.py merge_registry_text(). Both are held to the same cases by
// tools/lib/registry-merge-vectors.json (tools/registry-merge.test.mjs); the device-side installer will be too.
//
// CLI (for the PowerShell tools): node tools/lib/registry-merge.mjs <release.json> <device.json|-> <out.json>
//   prints one JSON line {changed, kept, shadowed, deviceReadable}; exit 1 only if the RELEASE is malformed.
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

export const REGISTRY_REL = 'system/registry/apps.json';

export function isAgentEntry(e) { return !!(e && typeof e === 'object' && e.created_by === 'agent' && typeof e.id === 'string'); }

function parse(text) {
  if (text == null) return null;
  try {
    const doc = JSON.parse(String(text).replace(/^﻿/, ''));
    return doc && Array.isArray(doc.installed) ? doc : null;
  } catch { return null; }
}

export function mergeRegistry(release, device) {
  if (!release || !Array.isArray(release.installed)) throw new Error('release registry is malformed');
  const sys = new Set(release.installed.filter((e) => e && typeof e.id === 'string').map((e) => e.id));
  const kept = [], shadowed = [], extra = [];
  const deviceReadable = !!(device && Array.isArray(device.installed));
  if (deviceReadable) {
    for (const e of device.installed) {
      if (!isAgentEntry(e)) continue;
      if (sys.has(e.id)) { if (!shadowed.includes(e.id)) shadowed.push(e.id); continue; }
      if (kept.includes(e.id)) continue;
      kept.push(e.id); extra.push(e);
    }
  }
  return { doc: extra.length ? { ...release, installed: [...release.installed, ...extra] } : release, kept, shadowed, deviceReadable };
}

// Text in, text out. When nothing is carried over the RELEASE TEXT is returned unchanged (byte-exact, so a
// card without Agent apps holds exactly the shipped file and its manifest hash); otherwise the merged doc is
// serialized like the release file (2-space JSON, the release's line ending, trailing newline if it had one).
export function mergeRegistryText(releaseText, deviceText) {
  const release = parse(releaseText);
  if (!release) throw new Error('release registry is malformed');
  const r = mergeRegistry(release, parse(deviceText));
  if (!r.kept.length) return { text: String(releaseText), ...r };
  const eol = /\r\n/.test(releaseText) ? '\r\n' : '\n';
  let text = JSON.stringify(r.doc, null, 2);
  if (eol !== '\n') text = text.replace(/\n/g, eol);
  if (/\r?\n$/.test(releaseText)) text += eol;
  return { text, ...r };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const [rel, dev, out] = process.argv.slice(2);
  if (!rel || !dev || !out) { console.error('usage: registry-merge.mjs <release.json> <device.json|-> <out.json>'); process.exit(2); }
  try {
    const releaseText = readFileSync(rel, 'utf8');
    const deviceText = dev !== '-' && existsSync(dev) ? readFileSync(dev, 'utf8') : null;
    const r = mergeRegistryText(releaseText, deviceText);
    const changed = deviceText == null || r.text !== deviceText;
    if (changed) writeFileSync(out, r.text);
    console.log(JSON.stringify({ changed, kept: r.kept, shadowed: r.shadowed, deviceReadable: r.deviceReadable }));
  } catch (e) { console.error(`registry-merge: ${e.message}`); process.exit(1); }
}
