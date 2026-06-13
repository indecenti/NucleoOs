// One-off repair: shell.js got its template-literal backticks escaped (\` and \${), which is a JS
// SyntaxError ("Invalid or unexpected token") that breaks the whole web OS. Un-escape them.
import { readFileSync, writeFileSync } from 'node:fs';
for (const f of process.argv.slice(2)) {
  let s = readFileSync(f, 'utf8');
  const n = (s.match(/\\`/g) || []).length + (s.match(/\\\$\{/g) || []).length;
  s = s.replaceAll('\\`', '`').replaceAll('\\${', '${');
  writeFileSync(f, s);
  console.log(`${f}: fixed ${n} escaped token(s)`);
}
