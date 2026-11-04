#!/usr/bin/env node
// release-announce.mjs — the short release post for the community Discord's #announcements.
//
// release.yml runs it after `verify`, so a post only goes out once the release is really installable. The
// text comes from CHANGELOG.md: one line per entry, the entry's bold lead ("**Choose your time zone.**"),
// grouped by Added / Changed / Fixed, then the download links. Discord caps a message at 2000 characters:
// entries past MAX_CHARS are counted in "…and N more".
//
//   node tools/release-announce.mjs v0.8.5           print the post (dry run)
//   node tools/release-announce.mjs v0.8.5 --post    send it to $DISCORD_RELEASE_WEBHOOK
// The webhook URL is a repository secret (DISCORD_RELEASE_WEBHOOK), never a file in the repo. Without it
// --post prints a notice and exits 0: forks and local runs stay quiet.
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const REPO = 'indecenti/NucleoOs';
const MAX_CHARS = 1900;            // under Discord's 2000, room for the "…and N more" line
const MAX_LINE = 140;

// The CHANGELOG section of one version: { Added: [...], Changed: [...], Fixed: [...] } of entry texts.
export function section(changelog, version) {
  const v = version.replace(/^v/, '');
  const lines = changelog.split(/\r?\n/);
  const start = lines.findIndex((l) => l.startsWith(`## [${v}]`));
  if (start < 0) return null;
  const out = {};
  let group = null, entry = null;
  for (const l of lines.slice(start + 1)) {
    if (l.startsWith('## ')) break;
    const h = /^### (.+)/.exec(l);
    if (h) { group = h[1].trim(); out[group] = []; entry = null; continue; }
    if (!group) continue;
    if (/^- /.test(l)) { entry = l.slice(2).trim(); out[group].push(entry); continue; }
    if (entry !== null && /^\s+\S/.test(l)) out[group][out[group].length - 1] += ' ' + l.trim();
    else if (!l.trim()) entry = null;
  }
  return out;
}

// One line for an entry: its bold lead, or its first sentence; links reduced to their text.
export function headline(entry) {
  const bold = /^\*\*(.+?)\*\*/.exec(entry);
  let t = bold ? bold[1] : entry.split(/(?<=\.)\s/)[0];
  t = t.replace(/\[([^\]]+)\]\([^)]+\)/g, '$1').replace(/\*\*/g, '').trim();
  return t.length > MAX_LINE ? t.slice(0, MAX_LINE - 1).trimEnd() + '…' : t;
}

export function post(changelog, tag) {
  const s = section(changelog, tag);
  if (!s) throw new Error(`CHANGELOG.md has no section for ${tag}`);
  const foot = `\nDownload and full notes: <https://github.com/${REPO}/releases/tag/${tag}>\n`
    + `Web installer: <https://indecenti.github.io/NucleoOs>`;
  let text = `# NucleoOS ${tag} 🚀\n`;
  let dropped = 0;
  for (const [group, entries] of Object.entries(s)) {
    if (!entries.length) continue;
    let block = `\n**${group}**\n`;
    for (const e of entries) {
      const line = `• ${headline(e)}\n`;
      if (dropped || (text + block + line + foot).length > MAX_CHARS) { dropped++; continue; }
      block += line;
    }
    if (block.includes('•')) text += block;
  }
  if (dropped) text += `…and ${dropped} more in the release notes.\n`;
  return text + foot;
}

async function main() {
  const [tag, flag] = process.argv.slice(2);
  if (!/^v\d+\.\d+\.\d+$/.test(tag || '')) { console.error('usage: release-announce.mjs vX.Y.Z [--post]'); process.exit(2); }
  const content = post(readFileSync(join(ROOT, 'CHANGELOG.md'), 'utf8'), tag);
  if (flag !== '--post') { console.log(content); return; }
  const hook = process.env.DISCORD_RELEASE_WEBHOOK;
  if (!hook) { console.log('release-announce: DISCORD_RELEASE_WEBHOOK not set, nothing posted'); return; }
  if (!/^https:\/\/discord\.com\/api\/webhooks\/\d+\/[\w-]+$/.test(hook)) {
    console.error('release-announce: DISCORD_RELEASE_WEBHOOK is not a Discord webhook URL'); process.exit(1);
  }
  const r = await fetch(hook, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    // no pings, ever: the post names no one and @everyone stays off
    body: JSON.stringify({ content, username: 'NucleoOS', allowed_mentions: { parse: [] } }),
  });
  if (!r.ok) { console.error(`release-announce: Discord answered ${r.status}`); process.exit(1); }
  console.log(`release-announce: ${tag} posted (${content.length} chars)`);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) await main();
