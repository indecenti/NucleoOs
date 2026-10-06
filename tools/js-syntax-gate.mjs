#!/usr/bin/env node
// js-syntax-gate.mjs — parse EVERY shipped web source (apps/*/www and web/shell): each .js/.mjs file, each
// inline <script> of each .html page, and each .json. A syntax error in a page's module script blanks the
// whole app with no other symptom, and nothing else in the gate matrix parses the shipped sources (the .gz
// twins are byte-checked, never parsed). Reuses the agent's pre-publish linter
// (apps/agent/www/app-publish.js lintApp) with acorn loaded, so built-in and agent-made apps meet one bar.
//   node tools/js-syntax-gate.mjs
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { lintApp } from '../apps/agent/www/app-publish.js';
import { loadParser, checkSyntax } from '../apps/code-runner/www/nucleo-run.js';

const repo = join(fileURLToPath(import.meta.url), '..', '..');
const CHECKED = /\.(m?js|cjs|html?|json)$/i;

function walk(dir, out) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (CHECKED.test(name)) out.push(p);
  }
  return out;
}

if (!(await loadParser())) { console.error('js:gate — acorn did not load (apps/code-runner/www/vendor/acorn.mjs)'); process.exit(1); }

const roots = [join(repo, 'web', 'shell')];
for (const id of readdirSync(join(repo, 'apps'))) {
  const www = join(repo, 'apps', id, 'www');
  try { if (statSync(www).isDirectory()) roots.push(www); } catch { /* app without a web side */ }
}

const files = roots.flatMap((r) => walk(r, []));
const rel = (p) => relative(repo, p).split(sep).join('/');
const res = lintApp(files.map((p) => ({ path: rel(p), content: readFileSync(p, 'utf8') })), checkSyntax);

if (!res.ok) {
  console.error(`js:gate — ${res.errors.length} broken file(s):`);
  for (const e of res.errors) console.error('  ' + e);
  process.exit(1);
}
console.log(`js:gate OK — ${files.length} files parsed (${roots.length} roots)`);
