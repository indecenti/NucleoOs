// Every relative link in the living docs resolves to a real file. docs/ is the project's source of truth;
// a link to a doc that was renamed or never written (anima-memory.md → anima-hdc.md, found 2026-10)
// sends the reader nowhere. docs/archive/ is history and is not checked. Runs in the portable CI gate.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..');
const docs = readdirSync(join(repo, 'docs')).filter((f) => f.endsWith('.md')).map((f) => join('docs', f));
const files = ['README.md', 'CONTRIBUTING.md', 'CHANGELOG.md', ...docs];

function brokenLinks(rel) {
  const src = readFileSync(join(repo, rel), 'utf8').replace(/```[\s\S]*?```/g, '');   // code blocks are examples
  const out = [];
  for (const m of src.matchAll(/\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g)) {
    const target = m[1].split('#')[0];
    if (!target || /^[a-z][a-z0-9+.-]*:/i.test(target)) continue;          // anchors, http:, mailto:
    const abs = join(repo, dirname(rel), decodeURIComponent(target));
    if (!existsSync(abs)) out.push(m[1]);
  }
  return out;
}

for (const rel of files) {
  test(`links in ${rel} resolve`, () => {
    assert.deepEqual(brokenLinks(rel), [], `${rel} links to missing files`);
  });
}
