#!/usr/bin/env node
// native-ui-lint — enforces docs/native-ui-kit.md across the on-device apps.
//
// Two rules, both about the app CHROME looking like one product:
//   1. no-raw-color  — a bare RGB565 literal (0xABCD) passed straight into a draw call
//                       (d.fill*/d.draw*/setTextColor/…). Chrome must use the THEME_* roles or a
//                       NAMED semantic constant, never an inline magic number that ignores the theme.
//   2. hint-not-tr   — nucleo_app_set_hint("literal") with a bare string instead of TR(it,en).
//                       Every hint must route through the i18n switch (English-first repo).
//
// Declarations (`static const uint16_t SUN = 0xFE60;`), #defines and the app-registration struct are
// NOT flagged — naming a color is exactly what we want. Genuine per-pixel graphics apps (games, viz,
// media frame/QR/pattern renderers) are exempt from rule 1 via GRAPHICS_EXEMPT; rule 2 applies to all.
//
// Usage: node tools/native-ui-lint.mjs            (report + exit 1 on any violation)
//        node tools/native-ui-lint.mjs --list     (also print the per-file compliant/exempt summary)
import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const APP_DIR = join(ROOT, 'firmware', 'components', 'nucleo_app');

// Files whose whole job is drawing arbitrary pixels — game boards, sprites, visualizers, video/photo
// frames, QR modules, LCD test patterns. Their content colors are legitimately literal, so rule 1 is
// off for them. (Rule 2 — hints via TR() — still applies.) Chrome-bearing apps are deliberately absent.
const GRAPHICS_EXEMPT = new Set([
  'app_snake.cpp', 'app_vs.cpp', 'app_cardler.cpp', 'app_tanks.cpp', 'app_tankduel.cpp',
  'app_pinball.cpp', 'app_poker.cpp', 'app_slots.cpp', 'app_yahtzee.cpp', 'app_pong.cpp',
  'app_dice.cpp', 'app_reactor.cpp', 'app_sandgarden.cpp', 'app_constellations.cpp',
  'app_screensaver.cpp', 'app_micspec.cpp', 'app_pixelfix.cpp', 'app_video.cpp', 'app_photos.cpp',
  'app_qr.cpp', 'app_torch.cpp', 'gamefront.cpp',
  'brawler_scene.cpp', 'brawler_combat.cpp', 'brawler_chars.cpp', 'brawler_enemies.cpp',
  'brawler_fx.cpp', 'brawler_menu.cpp', 'brawler_levels.cpp',
]);

// A draw/color call sink: if one of these appears on the (comment-stripped) line AND the line carries a
// bare 0xXXXX, the literal is being fed to the renderer — a raw-color violation.
const DRAW_SINK = /\b(?:d\.[A-Za-z]\w*|setTextColor|fillRect|fillRoundRect|drawRoundRect|drawRect|fillCircle|drawCircle|fillTriangle|drawFastHLine|drawFastVLine|drawLine|drawPixel|fillArc|drawArc|fillScreen|setColor|drawString|drawCircleHelper|fillSmoothRoundRect)\s*\(/;
const HEX = /\b0x[0-9A-Fa-f]{4}\b/;
const HINT_BARE = /nucleo_app_set_hint\s*\(\s*"/;

// Strip // and /* */ comments so a hex in a comment never trips the lint. Also drops string literals so
// a color-like token inside "text 0x1234" (rare) isn't flagged — colors are numeric args, not strings.
function sanitize(line) {
  let s = line.replace(/\/\/.*$/, '').replace(/\/\*.*?\*\//g, '');
  s = s.replace(/"(?:[^"\\]|\\.)*"/g, '""');
  return s;
}
// Declaration of a named color? (const/#define/enum initializer) — allowed, that's the fix we want.
function isDeclaration(line) {
  return /^\s*#\s*define\b/.test(line)
      || /\b(?:const|constexpr)\b[^;]*=\s*0x[0-9A-Fa-f]{4}/.test(line)
      || /=\s*0x[0-9A-Fa-f]{4}\s*[,;]/.test(line) && !DRAW_SINK.test(line);
}

const files = readdirSync(APP_DIR).filter(f => /^(app_|gamefront|brawler_).*\.cpp$/.test(f)).sort();
const violations = [];
const summary = [];

for (const f of files) {
  const text = readFileSync(join(APP_DIR, f), 'utf8');
  const lines = text.split(/\r?\n/);
  const exemptColor = GRAPHICS_EXEMPT.has(f);
  let nColor = 0, nHint = 0;

  lines.forEach((raw, i) => {
    const line = sanitize(raw);
    if (!exemptColor && HEX.test(line) && DRAW_SINK.test(line) && !isDeclaration(raw)) {
      violations.push({ file: f, line: i + 1, rule: 'no-raw-color', text: raw.trim() });
      nColor++;
    }
    if (HINT_BARE.test(line)) {
      violations.push({ file: f, line: i + 1, rule: 'hint-not-tr', text: raw.trim() });
      nHint++;
    }
  });
  summary.push({ f, exemptColor, nColor, nHint });
}

if (process.argv.includes('--list')) {
  console.log('file                         color  hint  status');
  for (const s of summary) {
    const bad = s.nColor + s.nHint;
    const status = s.exemptColor ? '(gfx-exempt)' : bad ? 'FAIL' : 'ok';
    console.log(`${s.f.padEnd(28)} ${String(s.nColor).padStart(5)} ${String(s.nHint).padStart(5)}  ${status}`);
  }
  console.log('');
}

if (violations.length === 0) {
  console.log(`native-ui-lint: clean — ${files.length} app files, ${GRAPHICS_EXEMPT.size} gfx-exempt.`);
  process.exit(0);
}

const byFile = new Map();
for (const v of violations) (byFile.get(v.file) ?? byFile.set(v.file, []).get(v.file)).push(v);
console.error(`native-ui-lint: ${violations.length} violation(s) in ${byFile.size} file(s)\n`);
for (const [file, vs] of byFile) {
  console.error(`  ${file}`);
  for (const v of vs) console.error(`    ${String(v.line).padStart(4)}  ${v.rule.padEnd(12)}  ${v.text.slice(0, 88)}`);
  console.error('');
}
console.error('See docs/native-ui-kit.md. Chrome colors -> THEME_* or a named constant; hints -> TR(it,en).');
process.exit(1);
