// Single entry point for the Game Center gate: runs every games-host test in sequence and exits
// non-zero on the first failure. One `node` invocation (not a 6-way `&&` chain) so it spawns cleanly
// from the test-registry CLI runner / test-lab GUI on every platform. `npm run games:gate` uses it too.
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const files = ['test-foundation.mjs', 'test-multiplayer.mjs', 'test-forza4.mjs', 'test-tris.mjs', 'test-pong.mjs', 'test-brain.mjs'];

for (const f of files) {
  const p = spawnSync(process.execPath, [join(here, f)], { stdio: 'inherit' });
  if (p.status !== 0) { console.error(`\n✗ games:gate failed at ${f}`); process.exit(p.status || 1); }
}
console.log('\n✓ games:gate — all suites passed');
