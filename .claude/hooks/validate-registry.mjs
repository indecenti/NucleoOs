// PostToolUse (Edit|Write): after editing the registry or an app manifest, run `npm run validate`
// and block (feed the problem back to the model) if it broke registry/manifest integrity. Pre-existing
// gz-staleness is intentionally ignored — that's gz:check's job, not the registry guard's, and it
// would otherwise fire on every edit.
import { execSync } from 'node:child_process';
let s = '';
process.stdin.on('data', (d) => (s += d));
process.stdin.on('end', () => {
  let f = '';
  try {
    const j = JSON.parse(s);
    f = String((j.tool_input && j.tool_input.file_path) || (j.tool_response && j.tool_response.filePath) || '');
  } catch { /* ignore */ }
  if (!/[\\/]registry[\\/]|manifest\.json$/i.test(f)) return;   // only registry / manifests
  let out = '';
  try {
    execSync('npm run validate', { cwd: 'G:/Nucleo', encoding: 'utf8', stdio: 'pipe' });
    return;   // exit 0 → all good, stay silent
  } catch (e) {
    out = String(e.stdout || '') + String(e.stderr || '');
  }
  const probs = out.split(/\r?\n/).filter((l) => /^\s*-\s/.test(l) && !/gz:/i.test(l));
  if (probs.length) {
    process.stdout.write(JSON.stringify({
      decision: 'block',
      reason: 'Registry/manifest validation problems after editing ' + f + ':\n' + probs.join('\n')
    }));
  }
});
