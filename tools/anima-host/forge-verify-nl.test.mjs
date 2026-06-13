// Gate: ANIMA Forge — NATURAL-LANGUAGE cross-substrate verification at scale. Replays 100+ Italian
// and 100+ English real NL claims (capitals, birth years, arithmetic, fictional entities) through the
// REAL device cascade exe and asserts each grounded verdict. Expectations are ground-truth, anchored
// to the corpus's actual coverage (fixtures built by forge-verify-gen.mjs). A verifier regression
// flips cases → fails. Run: node --test tools/anima-host/forge-verify-nl.test.mjs
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const fixtures = join(here, 'fixtures', 'forge-verify-cases.jsonl');

const cases = readFileSync(fixtures, 'utf8').split('\n').filter((l) => l.trim()).map((l) => JSON.parse(l));
const it = cases.filter((c) => c.lang === 'it');
const en = cases.filter((c) => c.lang === 'en');

before(() => {
  const b = spawnSync('node', [join(here, 'anima.mjs'), '--ensure'], { encoding: 'utf8' });
  assert.ok(b.status === 0 && existsSync(exe), 'anima.exe must build for the NL verify gate');
});

function run(c) {
  const r = spawnSync(exe, (c.lang === 'en' ? ['--en'] : []).concat(['--verify', `${c.kind}|${c.key}|${c.asserted}`]), { encoding: 'utf8' });
  return (/VERDICT=(\w+)/.exec(r.stdout || '') || [, 'error'])[1];
}

function replay(name, set) {
  test(`${name}: ${set.length} NL claims → correct grounded verdict`, () => {
    assert.ok(set.length >= 100, `need >= 100 ${name} cases, have ${set.length}`);
    const fails = [];
    for (const c of set) { const got = run(c); if (got !== c.expect) fails.push(`[${c.kind}] "${c.key}" = "${c.asserted}" expected ${c.expect} got ${got}`); }
    assert.equal(fails.length, 0, `${fails.length}/${set.length} mismatches:\n  ` + fails.slice(0, 25).join('\n  '));
    // not degenerate: all three verdict classes are actually exercised
    const kinds = new Set(set.map((c) => c.expect));
    for (const v of ['confirmed', 'contradicted', 'unknown']) assert.ok(kinds.has(v), `missing ${v} cases`);
  });
}

replay('Italian', it);
replay('English', en);
