// One OS version everywhere. firmware/version/VERSION is the source of truth (the release tag is cut from
// it); package.json and CITATION.cff drifted two releases behind it once, and the CHANGELOG had no
// section for the shipped version. Runs in the portable CI gate (node --test tools/*.test.mjs).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => readFileSync(join(repo, p), 'utf8');
const VERSION = read('firmware/version/VERSION').trim();

test('firmware VERSION is plain semver', () => {
  assert.match(VERSION, /^\d+\.\d+\.\d+$/);
});

test('package.json carries the firmware version', () => {
  assert.equal(JSON.parse(read('package.json')).version, VERSION);
});

test('CITATION.cff carries the firmware version', () => {
  assert.equal((/^version:\s*"?([^"\s]+)"?/m.exec(read('CITATION.cff')) || [])[1], VERSION);
});

test('CHANGELOG has a section for the firmware version, right under [Unreleased]', () => {
  const heads = [...read('CHANGELOG.md').matchAll(/^## \[([^\]]+)\]/gm)].map((m) => m[1]);
  assert.equal(heads[0], 'Unreleased');
  assert.equal(heads[1], VERSION, `newest released section is [${heads[1]}], firmware is ${VERSION}`);
});
