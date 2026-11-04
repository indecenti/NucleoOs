// The Discord release post (tools/release-announce.mjs): built from CHANGELOG.md, under Discord's 2000
// characters, one line per entry, and it fails loudly when the tag has no CHANGELOG section.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { section, headline, post } from './release-announce.mjs';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const CHANGELOG = readFileSync(join(REPO, 'CHANGELOG.md'), 'utf8');

const SAMPLE = `# Changelog

## [1.2.0] — 2026-12-01

### Added
- **A new thing.** It does
  a lot of things.
- Plain entry without bold. Second sentence.

### Fixed
- **A [linked](docs/x.md) fix.** Details.

## [1.1.0] — 2026-11-01

### Added
- **Older.** Not in 1.2.0.
`;

test('section groups entries and joins wrapped lines', () => {
  const s = section(SAMPLE, 'v1.2.0');
  assert.deepEqual(Object.keys(s), ['Added', 'Fixed']);
  assert.equal(s.Added.length, 2);
  assert.match(s.Added[0], /does a lot of things/);
  assert.equal(section(SAMPLE, 'v9.9.9'), null);
});

test('headline takes the bold lead, else the first sentence, without links', () => {
  assert.equal(headline('**A new thing.** It does a lot.'), 'A new thing.');
  assert.equal(headline('Plain entry without bold. Second sentence.'), 'Plain entry without bold.');
  assert.equal(headline('**A [linked](docs/x.md) fix.** Details.'), 'A linked fix.');
});

test('post has the title, the groups and the links, and nothing from other versions', () => {
  const p = post(SAMPLE, 'v1.2.0');
  assert.match(p, /^# NucleoOS v1\.2\.0/);
  assert.match(p, /\*\*Added\*\*\n• A new thing\.\n• Plain entry without bold\.\n/);
  assert.match(p, /releases\/tag\/v1\.2\.0>/);
  assert.doesNotMatch(p, /Older/);
  assert.throws(() => post(SAMPLE, 'v9.9.9'), /no section/);
});

test('a long release stays under Discord\'s limit', () => {
  const many = Array.from({ length: 80 }, (_, i) => `- **Entry number ${i} with a fairly long headline here.** x`).join('\n');
  const p = post(`## [2.0.0] — x\n\n### Added\n${many}\n`, 'v2.0.0');
  assert.ok(p.length <= 2000, `${p.length} chars`);
  assert.match(p, /…and \d+ more in the release notes\./);
});

test('every released version in CHANGELOG.md makes a valid post', () => {
  const tags = [...CHANGELOG.matchAll(/^## \[(\d+\.\d+\.\d+)\]/gm)].map((m) => 'v' + m[1]);
  assert.ok(tags.length > 5);
  for (const t of tags) assert.ok(post(CHANGELOG, t).length <= 2000, t);
});
