// i18n-hardcoded-check.mjs — flags user-visible text in a web app's index.html that is NOT routed
// through the i18n engine, so a hardcoded string can't silently ship untranslated.
//
// tools/i18n-check.mjs proves that every key a surface REFERENCES exists in all 5 languages. It says
// nothing about visible text that was never keyed in the first place. This gate closes that hole: it
// scans each apps/<id>/www/index.html (and the shell) for
//   • text nodes with real words that are NOT inside a [data-i18n]/[data-i18n-html] element, and
//   • user-facing attributes (title / placeholder / aria-label / alt) written as a literal instead of
//     via data-i18n-attr,
// and fails on anything not explicitly allow-listed.
//
// Scope + honesty:
//   • Only the STATIC index.html is scanned (what ships pre-rendered). Text a JS module builds at
//     runtime is the job of nucleo-webos tests/suite/app-i18n-keys.test.mjs (every t('key') must exist in 5 languages).
//   • Content inside <script>/<style>/<svg>/<template>/<code>/<pre>, HTML comments, and any element (or
//     ancestor) carrying data-i18n / data-i18n-html / data-i18n-ignore is skipped — those are handled
//     or opted out on purpose.
//   • An app that ships its own i18n fork (apps/<id>/www/i18n.js, e.g. settings) still renders real text
//     from that fork; its index.html literals are flagged like any other so the fork is visible, not a
//     blind spot. Mark deliberate exceptions in tools/i18n-hardcoded-allow.json.
//
// Zero dependencies, BOM-safe. Usage:
//   node tools/i18n-hardcoded-check.mjs            exit 1 on any non-allow-listed finding
//   node tools/i18n-hardcoded-check.mjs --report   list every finding, never fail (triage)
import { readdirSync, readFileSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const REPORT = process.argv.includes('--report');
const WRITE_BASELINE = process.argv.includes('--write-baseline');

// Tokens that are not translatable prose even when they look like words: brand/product nouns, units,
// and protocol/format names. A text node made ONLY of these (plus punctuation/digits) is not flagged.
const SAFE = new Set([
  'NucleoOS', 'ANIMA', 'Cardputer', 'M5Stack', 'ESP32', 'Wi-Fi', 'WiFi', 'BLE', 'USB', 'HID', 'IR',
  'DOS', 'QR', 'SD', 'PWA', 'OS', 'CPU', 'RAM', 'GPU', 'TLS', 'SSH', 'HTTP', 'HTTPS', 'URL', 'IP',
  'MAC', 'DHCP', 'DNS', 'ARP', 'PCAP', 'MITM', 'AP', 'SSID', 'WPA2', 'WPA3', 'PMKID', 'TV', 'AI',
  'LLM', 'TTS', 'MP3', 'WAV', 'PNG', 'JPG', 'SVG', 'JSON', 'CSV', 'PDF', 'OK', 'ID', 'v', 'px', 'ms',
  'KB', 'MB', 'GB', 'Hz', 'kHz', 'mAh', 'mA', 'mV', 'dB', 'GPIO', 'I2C', 'SPI', 'UART', 'ADC', 'DAC',
  // keyboard keys + chord parts: a shortcut hint is not translatable prose.
  'Ctrl', 'Alt', 'Shift', 'Fn', 'Cmd', 'Tab', 'Enter', 'Esc', 'Space', 'Del', 'Backspace', 'Caps',
  'PgUp', 'PgDn', 'Home', 'End', 'Ins', 'Return', 'Win', 'Opt', 'PiP',
]);

// Decode the handful of HTML entities that appear in visible text so the prose test sees real chars
// (">", "&", "×", "…", nbsp) instead of "&gt;"/"&times;" which would read as stray letters.
function decode(s) {
  return s.replace(/&(gt|lt|amp|times|hellip|nbsp|mdash|ndash|rarr|larr|uarr|darr|#\d+|#x[0-9a-f]+);/gi, (m, e) => {
    const map = { gt: '>', lt: '<', amp: '&', times: '×', hellip: '…', nbsp: ' ', mdash: '—', ndash: '–', rarr: '→', larr: '←', uarr: '↑', darr: '↓' };
    const k = e.toLowerCase();
    if (map[k]) return map[k];
    if (k[0] === '#') { try { return String.fromCodePoint(k[1] === 'x' ? parseInt(k.slice(2), 16) : parseInt(k.slice(1), 10)); } catch { return ' '; } }
    return ' ';
  });
}

// A chord like "Ctrl+O", "Ctrl+Shift+O", "Ctrl+←" is a shortcut hint, never prose: every '+'-separated
// part is a SAFE key, a single letter/digit, or an arrow glyph.
function isShortcut(txt) {
  if (!txt.includes('+')) return false;
  return txt.split('+').every((p) => { const x = p.trim(); return SAFE.has(x) || /^[\p{L}0-9]$/u.test(x) || /^[←→↑↓]$/.test(x); });
}

// A text node is "prose" worth translating if, after removing SAFE tokens, digits and punctuation, at
// least one run of 2+ letters survives. ("12:30", "—", "NucleoOS", "v0.7.4", "Ctrl+O" → not prose.)
function isProse(raw) {
  const txt = decode(raw).replace(/\s+/g, ' ').trim();
  if (!txt) return false;
  if (isShortcut(txt)) return false;
  let rest = txt;
  for (const s of SAFE) rest = rest.split(s).join(' ');
  rest = rest.replace(/[0-9]+/g, ' ').replace(/[^\p{L}]+/gu, ' ').trim();
  if (!rest) return false;
  // Need a run of 2+ letters that is itself not a lone SAFE token left over.
  return /\p{L}{2,}/u.test(rest);
}

function loadAllow() {
  const p = join(ROOT, 'tools', 'i18n-hardcoded-allow.json');
  if (!existsSync(p)) return { surfaces: {}, global: [] };
  let s = readFileSync(p, 'utf8'); if (s.charCodeAt(0) === 0xfeff) s = s.slice(1);
  const j = JSON.parse(s);
  return { surfaces: j.surfaces || {}, global: j.global || [] };
}

// Mask spans we must never scan: comments, script/style/svg/template/code/pre bodies, and the whole
// subtree of any element opening with data-i18n / data-i18n-html / data-i18n-ignore. Masking keeps byte
// offsets stable (so attribute scanning below still lines up) by overwriting with spaces.
function maskSkipped(html) {
  let h = html;
  const blank = (m) => ' '.repeat(m.length);
  h = h.replace(/<!--[\s\S]*?-->/g, blank);
  h = h.replace(/<(script|style|svg|template|code|pre|title)\b[\s\S]*?<\/\1>/gi, blank);
  // Element whose open tag carries a handled/opt-out binding: blank from the tag to its matching close.
  // Recognised bindings: the central engine's data-i18n / data-i18n-html, the explicit opt-out
  // data-i18n-ignore, AND a self-contained fork's data-t / data-t-html (e.g. the Settings app's i18n.js
  // table — its text IS translated in all 5 languages, just not on the central engine). Approximate (no
  // nesting of the SAME tag inside) — good enough for leaf UI nodes, where translated text lives; a
  // false "handled" only ever SUPPRESSES a finding, never invents one.
  h = h.replace(/<([a-zA-Z][\w-]*)\b[^>]*\b(?:data-i18n(?:-html|-ignore)?|data-t(?:-html)?)\b[^>]*>([\s\S]*?)<\/\1>/g,
    (m) => blank(m));
  // An element that binds its ATTRIBUTES at runtime (data-i18n-attr / data-t-attr) may still carry a
  // literal title/placeholder/aria-label/alt in the static markup as a design-time default — blank the
  // whole open tag so the attribute scan below doesn't flag that translated-at-runtime value.
  h = h.replace(/<[a-zA-Z][\w-]*\b[^>]*\bdata-(?:i18n|t)-attr\b[^>]*>/g, (m) => blank(m));
  return h;
}

function scanSurface(ns, file, allow) {
  const html = readFileSync(file, 'utf8');
  const masked = maskSkipped(html);
  const findings = [];
  const line = (idx) => html.slice(0, idx).split('\n').length;
  const allowed = new Set([...(allow.global || []), ...((allow.surfaces || {})[ns] || [])]);
  const ok = (t) => allowed.has(t.replace(/\s+/g, ' ').trim());

  // (a) text nodes: everything between '>' and the next '<' in the masked HTML.
  const reText = />([^<]+)</g;
  let m;
  while ((m = reText.exec(masked))) {
    const raw = m[1];
    if (!isProse(raw)) continue;
    const t = raw.replace(/\s+/g, ' ').trim();
    if (ok(t)) continue;
    findings.push({ kind: 'text', line: line(m.index + 1), text: t });
  }
  // (b) user-facing attributes written as literals (not via data-i18n-attr).
  const reAttr = /\b(title|placeholder|aria-label|alt)\s*=\s*"([^"]*)"/gi;
  while ((m = reAttr.exec(masked))) {
    const raw = m[2];
    if (!isProse(raw)) continue;
    const t = raw.replace(/\s+/g, ' ').trim();
    if (ok(t)) continue;
    findings.push({ kind: m[1].toLowerCase(), line: line(m.index), text: t });
  }
  return findings;
}

function surfaces() {
  const out = [];
  const shell = join(ROOT, 'web', 'shell', 'index.html');
  if (existsSync(shell)) out.push({ ns: 'shell', file: shell });
  const appsDir = join(ROOT, 'apps');
  if (existsSync(appsDir)) for (const id of readdirSync(appsDir)) {
    const idx = join(appsDir, id, 'www', 'index.html');
    if (existsSync(idx) && statSync(idx).isFile()) out.push({ ns: id, file: idx });
  }
  return out;
}

// --write-baseline: snapshot every CURRENT finding into the allow file so the gate goes green now and
// only NEW hardcoded text fails afterwards (a ratchet over the legacy debt). Run WITHOUT an existing
// allow so the snapshot is the raw set; re-running burns down entries as they are keyed/translated.
if (WRITE_BASELINE) {
  const empty = { surfaces: {}, global: [] };
  const bySurface = {};
  for (const { ns, file } of surfaces()) {
    const f = scanSurface(ns, file, empty);
    if (f.length) bySurface[ns] = [...new Set(f.map((x) => x.text))].sort();
  }
  const out = {
    _comment: 'Baseline of pre-existing hardcoded UI text (tools/i18n-hardcoded-check.mjs). Each entry is DEBT: route it through the i18n engine, then delete it here. NEW text not listed fails the gate. `global` applies to every surface (brand/symbol tokens).',
    global: (loadAllow().global || []),
    surfaces: bySurface,
  };
  writeFileSync(join(ROOT, 'tools', 'i18n-hardcoded-allow.json'), JSON.stringify(out, null, 2) + '\n');
  const n = Object.values(bySurface).reduce((a, b) => a + b.length, 0);
  console.log(`i18n-hardcoded: wrote baseline — ${n} entries across ${Object.keys(bySurface).length} surfaces`);
  process.exit(0);
}

const allow = loadAllow();
let total = 0, dirty = 0;
const perSurface = [];
for (const { ns, file } of surfaces()) {
  const f = scanSurface(ns, file, allow);
  if (f.length) { dirty++; perSurface.push({ ns, f }); total += f.length; }
}

if (REPORT) {
  for (const { ns, f } of perSurface) {
    console.log(`\n${ns}  (${f.length})`);
    for (const x of f) console.log(`  ${String(x.line).padStart(4)}  ${x.kind.padEnd(11)} ${x.text.slice(0, 80)}`);
  }
  console.log(`\ni18n-hardcoded: ${total} finding(s) in ${dirty} surface(s) [report mode — never fails]`);
  process.exit(0);
}

if (total) {
  console.error(`✗ i18n-hardcoded FAILED — ${total} un-translated literal(s) in ${dirty} surface(s):`);
  for (const { ns, f } of perSurface) {
    console.error(`  [${ns}]`);
    for (const x of f) console.error(`    ${ns}/www/index.html:${x.line}  ${x.kind}: ${x.text.slice(0, 80)}`);
  }
  console.error('\nRoute each through the i18n engine (data-i18n / data-i18n-attr / t()), or, if it is not');
  console.error('translatable prose, add it to tools/i18n-hardcoded-allow.json.');
  process.exit(1);
}
console.log(`✓ i18n-hardcoded OK — no un-translated literals across ${surfaces().length} surfaces`);
