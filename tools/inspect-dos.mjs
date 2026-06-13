// inspect-dos — diagnostic: open every .jsdos bundle in a folder, dump its dosbox.conf,
// list files, and flag likely-broken bundles (missing/mismatched run target, bad conf, etc).
// Usage: node tools/inspect-dos.mjs [dir]   (default deploy/sd/data/DOS)
import zlib from 'node:zlib';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const dir = process.argv[2] || 'deploy/sd/data/DOS';
const RUNNABLE = /\.(exe|com|bat)$/i;

function unzip(u8) {
  const dv = (o) => u8[o] | (u8[o + 1] << 8);
  const dw = (o) => (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0;
  let p = u8.length - 22;
  while (p >= 0 && dw(p) !== 0x06054b50) p--;
  if (p < 0) throw new Error('no EOCD (not a zip)');
  const count = dv(p + 10); let cd = dw(p + 16);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (dw(cd) !== 0x02014b50) break;
    const method = dv(cd + 10), csize = dw(cd + 20), nlen = dv(cd + 28), elen = dv(cd + 30), clen = dv(cd + 32), lho = dw(cd + 42);
    const name = Buffer.from(u8.subarray(cd + 46, cd + 46 + nlen)).toString('latin1');
    const lnlen = dv(lho + 26), lelen = dv(lho + 28);
    const start = lho + 30 + lnlen + lelen;
    const comp = Buffer.from(u8.subarray(start, start + csize));
    if (!name.endsWith('/')) {
      const data = method === 0 ? comp : method === 8 ? zlib.inflateRawSync(comp) : null;
      out.push({ name, method, csize, data });
    }
    cd += 46 + nlen + elen + clen;
  }
  return out;
}

const confOf = (entries) => { const c = entries.find((e) => e.name.toLowerCase() === '.jsdos/dosbox.conf'); return c && c.data ? c.data.toString('latin1') : null; };

// Extract the real command(s) the autoexec will run (excluding mount/cd/echo/drive-letter/exit)
function autoexecCmds(conf) {
  if (!conf) return [];
  const i = conf.toLowerCase().indexOf('[autoexec]');
  if (i < 0) return [];
  const tail = conf.slice(i + '[autoexec]'.length);
  return tail.split(/\r?\n/).map((s) => s.trim()).filter((l) => {
    if (!l || /^[#;\[]/.test(l)) return false;
    if (/^@?echo\b/i.test(l) || /^exit\b/i.test(l) || /^mount\b/i.test(l)) return false;
    if (/^cd\s/i.test(l) || /^cd$/i.test(l)) return false;        // a `cd` command (note the space) — NOT "CD-MAN.EXE"
    if (/^[a-z]:\\?$/i.test(l)) return false;                     // drive letter "c:"
    if (/^(type|pause|rem|cls|set|imgmount|boot)\b/i.test(l)) return false;  // cosmetic / non-launch
    return true;
  });
}

// Does the autoexec actually mount C: before running?
const hasMountC = (conf) => /\bmount\s+c\b/i.test(conf || '');

const files = readdirSync(dir).filter((n) => n.toLowerCase().endsWith('.jsdos')).sort();
let broken = 0;
for (const f of files) {
  const path = join(dir, f);
  let entries;
  try { entries = unzip(readFileSync(path)); }
  catch (e) { console.log(`\n### ${f}\n  ✗ UNREADABLE: ${e.message}`); broken++; continue; }

  const conf = confOf(entries);
  const names = entries.map((e) => e.name);
  const exes = names.filter((n) => RUNNABLE.test(n));
  const cmds = autoexecCmds(conf);
  const issues = [];

  if (!conf) issues.push('NO .jsdos/dosbox.conf');
  else {
    if (!hasMountC(conf)) issues.push('autoexec never mounts C:');
    if (!/\[autoexec\]/i.test(conf)) issues.push('no [autoexec] section');
    if (!cmds.length) issues.push('autoexec has no run command');
    // Verify each run command's target file actually exists in the bundle.
    for (const c of cmds) {
      const tok = c.replace(/^call\s+/i, '').split(/\s+/)[0].replace(/\\/g, '/');
      if (!tok || /^(echo|pause|rem|set|cls|ver|menu|choice|goto|if|for)\b/i.test(tok)) continue;
      const want = tok.toLowerCase();
      const wantExe = /\.[a-z0-9]{1,3}$/i.test(want) ? want : null;
      const hit = names.some((n) => {
        const ln = n.toLowerCase();
        if (wantExe) return ln === want || ln.endsWith('/' + want);
        // no extension given → DOS would try .com/.exe/.bat
        return ['com', 'exe', 'bat'].some((x) => ln === want + '.' + x || ln.endsWith('/' + want + '.' + x));
      });
      if (!hit) issues.push(`run target not in bundle: "${c}"`);
    }
  }
  if (!exes.length) issues.push('bundle contains no .exe/.com/.bat at all');

  const status = issues.length ? '✗' : '✓';
  if (issues.length) broken++;
  console.log(`\n### ${f}  ${status}`);
  console.log(`  files: ${entries.length}  exes: ${exes.length ? exes.slice(0, 6).join(', ') + (exes.length > 6 ? ' …' : '') : '(none)'}`);
  console.log(`  runs:  ${cmds.length ? cmds.join(' | ') : '(none detected)'}`);
  if (issues.length) console.log('  ISSUES: ' + issues.join('; '));
  if (conf && process.env.DUMP) console.log('  --- conf ---\n' + conf.split('\n').map((l) => '  | ' + l).join('\n'));
}
console.log(`\n${files.length} bundles, ${broken} with issues.`);
