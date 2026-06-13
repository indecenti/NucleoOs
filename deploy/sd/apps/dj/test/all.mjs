// Single entry point for the DJ gate: runs the planner + .npx decode tests, exits non-zero on the
// first failure. One `node` invocation so it spawns cleanly from the test-registry runner. The .npx
// test SKIPs (exit 0) if its fixture is absent — see _test_npx.mjs. `npm run dj:test` uses this.
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
for (const f of ['_test_dj.mjs', '_test_npx.mjs']) {
  const p = spawnSync(process.execPath, [join(here, f)], { stdio: 'inherit' });
  if (p.status !== 0) { console.error(`\n✗ dj:test failed at ${f}`); process.exit(p.status || 1); }
}
console.log('\n✓ dj:test — planner + npx decode passed');
