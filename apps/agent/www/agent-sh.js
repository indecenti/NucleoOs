// agent-sh.js — a small POSIX-like shell for the agent, over the Cardputer's files.
//
// Models already know `ls`, `cat`, `grep -rn`, `find -name`, `head`, `sed -n '10,40p'`, `wc -l`… from their
// training: one `sh` tool speaks that language, so the prompt need not teach a dozen bespoke tools and a small
// local model makes fewer mistakes. The grammar is the NucleoOS Terminal's (apps/terminal/www/index.html:
// quoting, $VAR, | > >> <, ; && ||) so the user and the agent type the same commands.
//
// Everything is confined to the agent's workspace through the injected fsclient (makeFS): no path can
// escape it. Any command line that CHANGES files (mkdir touch cp mv rm, > >>) is shown to the human ONCE,
// as typed, before anything runs; `rm` always asks. Output is capped so one command cannot flood a small
// model's context — the note says how to narrow it. DOM-free, I/O only through the injected adapters.
// tools/agent-sh.test.mjs.

const OPS = ['&&', '||', '>>', '|', '>', '<', ';'];
const MUTATING_CMDS = new Set(['mkdir', 'touch', 'cp', 'mv', 'rm']);
export const SH_LIMITS = { maxOut: 12 * 1024, maxRows: 200, maxMatches: 200, maxListCalls: 60, maxDepth: 6, maxStages: 8, readBytes: 256 * 1024 };

// ---- grammar (after the Terminal's tokenize / expandWord / parse) ------------------------------------
function tokenize(line) {
  const toks = []; let parts = null, quoted = false;
  const put = (text, expand) => { if (!parts) parts = []; const l = parts[parts.length - 1]; if (l && l.expand === expand) l.text += text; else parts.push({ text, expand }); };
  const flush = () => { if (parts) toks.push({ w: parts, quoted }); parts = null; quoted = false; };
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === '\\') { if (i + 1 >= line.length) return { error: 'syntax error: unexpected end of input' }; put(line[++i], false); }
    else if (c === "'") { const e = line.indexOf("'", i + 1); if (e < 0) return { error: 'syntax error: unterminated quote' }; quoted = true; put(line.slice(i + 1, e), false); i = e; }
    else if (c === '"') {
      quoted = true; put('', true); let j = i + 1, closed = false;
      for (; j < line.length; j++) { const d = line[j]; if (d === '\\' && j + 1 < line.length && '"\\$'.includes(line[j + 1])) { put(line[++j], false); continue; } if (d === '"') { closed = true; break; } put(d, true); }
      if (!closed) return { error: 'syntax error: unterminated quote' }; i = j;
    } else if (/\s/.test(c)) flush();
    else { const op = OPS.find((o) => line.startsWith(o, i)); if (op) { flush(); toks.push({ op }); i += op.length - 1; } else put(c, true); }
  }
  flush();
  return { tokens: toks };
}
function expandWord(tok, env) {
  let text = '', sub = false;
  for (const p of tok.w) {
    if (!p.expand) { text += p.text; continue; }
    text += p.text.replace(/\$(?:\{(\w+)\}|(\w+))/g, (_, a, b) => { sub = true; const v = env[a || b]; return v == null ? '' : String(v); });
  }
  return { text, drop: text === '' && !tok.quoted && sub };
}
export function parse(line, env = {}) {
  const lex = tokenize(String(line || ''));
  if (lex.error) return lex;
  const list = []; let stages = [], stage = null, redir = null, join = ';';
  const close = () => { if (!stage) return null; if (!stage.argv.length) return 'syntax error: redirection without a command'; stages.push(stage); stage = null; return null; };
  for (const tk of lex.tokens) {
    if (!tk.op) {
      const w = expandWord(tk, env);
      if (!stage) stage = { argv: [], redirs: [] };
      if (redir) { if (w.drop || !w.text) return { error: 'syntax error: missing redirection target' }; stage.redirs.push({ op: redir, path: w.text }); redir = null; }
      else if (!w.drop) stage.argv.push(w.text);
      continue;
    }
    if (redir) return { error: `syntax error near '${tk.op}'` };
    if (tk.op === '>' || tk.op === '>>' || tk.op === '<') { redir = tk.op; continue; }
    if (tk.op !== ';' && !stage) return { error: `syntax error near '${tk.op}'` };
    const bad = close(); if (bad) return { error: bad };
    if (tk.op === '|') continue;
    if (!stages.length) return { error: `syntax error near '${tk.op}'` };
    list.push({ stages, join }); stages = []; join = tk.op;
  }
  if (redir) return { error: 'syntax error: missing redirection target' };
  const bad = close(); if (bad) return { error: bad };
  if (stages.length) list.push({ stages, join });
  else if (join !== ';' && list.length) return { error: `syntax error: '${join}' with no command` };
  for (const seg of list) if (seg.stages.length > SH_LIMITS.maxStages) return { error: `too many stages in one pipeline (max ${SH_LIMITS.maxStages})` };
  return { list };
}

// What a command line would change — shown to the human before it runs. → [{ op, path }] (empty = read-only)
export function plannedWrites(parsed) {
  const out = [];
  for (const seg of (parsed && parsed.list) || []) for (const st of seg.stages) {
    const name = (st.argv[0] || '').toLowerCase();
    const args = st.argv.slice(1).filter((a) => !a.startsWith('-'));
    if (MUTATING_CMDS.has(name)) out.push({ op: name, path: args.join(' ') });
    for (const r of st.redirs) if (r.op === '>' || r.op === '>>') out.push({ op: r.op === '>' ? 'write' : 'append', path: r.path });
  }
  return out;
}

// ---- helpers -------------------------------------------------------------------------------------------
const globRe = (g, ci) => new RegExp('^' + String(g).replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$', ci ? 'i' : '');
function flags(args, spec) {          // spec: { short: 'ilnrv', withValue: { n: 'n' } } → { f:{}, rest:[] }
  const f = {}, rest = [];
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '--') { rest.push(...args.slice(i + 1)); break; }
    if (/^-\d+$/.test(a) && spec.numeric) { f[spec.numeric] = +a.slice(1); continue; }
    if (/^-[A-Za-z]+$/.test(a)) {
      const letters = a.slice(1);
      if (spec.withValue && spec.withValue[letters]) { f[spec.withValue[letters]] = args[++i]; continue; }
      if ([...letters].every((ch) => spec.short.includes(ch))) { for (const ch of letters) f[ch] = true; continue; }
    }
    if (a.startsWith('--') && spec.long && spec.long[a]) { f[spec.long[a]] = true; continue; }
    rest.push(a);
  }
  return { f, rest };
}
const lines = (s) => { const t = String(s ?? ''); const ls = t.split('\n'); if (ls.length && ls[ls.length - 1] === '') ls.pop(); return ls; };
const unescapeC = (s) => String(s).replace(/\\([nt\\"'r0])/g, (m, c) => ({ n: '\n', t: '\t', '\\': '\\', '"': '"', "'": "'", r: '\r', 0: '' }[c]));

export function createAgentShell({ fs, device = {}, confirm = async () => true, limits = {} } = {}) {
  const L = { ...SH_LIMITS, ...limits };
  let cwd = '';                                          // relative to the workspace root ('' = root)
  const env = { HOME: '/', SHELL: '/bin/sh', USER: 'agent', OS: 'NucleoOS' };
  const path = (p) => { const s = String(p ?? '').trim(); if (!s || s === '.') return cwd || '.'; if (s.startsWith('/')) return s; return cwd ? cwd + '/' + s : s; };
  const show = (p) => { try { return fs.rel(fs.resolve(path(p))); } catch { return String(p); } };

  async function readText(p) {
    const r = await fs.read(path(p), { maxBytes: L.readBytes });
    if (!r.ok) throw new Error(`${p}: ${r.error === 'not-found' ? 'No such file' : r.error}`);
    return r.content + (r.truncated ? `\n… (file larger than ${Math.round(L.readBytes / 1024)} KB: read it in parts with sed -n 'A,Bp')` : '');
  }
  async function isDir(p) { const r = await fs.list(path(p)); return r.ok; }
  // Bounded walk: depth + number of list calls, like the Terminal's find (the device lists slowly).
  async function walk(start, { maxDepth = L.maxDepth } = {}, visit) {
    let calls = 0, capped = false;
    const go = async (dir, depth) => {
      if (calls >= L.maxListCalls) { capped = true; return; }
      calls++;
      const r = await fs.list(dir);
      if (!r.ok) return;
      for (const e of r.entries) {
        const child = (dir === '.' ? '' : dir.replace(/\/$/, '') + '/') + e.name;
        if (await visit(child, e, depth) === false) return;
        if (e.type === 'dir' && depth < maxDepth) await go(child, depth + 1);
      }
    };
    await go(path(start), 1);
    return { capped };
  }

  const C = {
    help: async () => 'Commands: ' + Object.keys(C).sort().join(' ') + '\nPipes | redirection > >> < and ; && || work like a POSIX shell. Paths are inside the workspace.',
    pwd: async () => '/' + (cwd || ''),
    cd: async (a) => { const d = a[0] || ''; if (!d || d === '/' || d === '~') { cwd = ''; return ''; } if (!(await isDir(d))) throw new Error(`cd: ${d}: No such directory`); cwd = show(d) === '.' ? '' : show(d); return ''; },
    // echo -e / -n / -ne and printf: what models reach for to write a multi-line file (`echo -e "a\nb" > f`).
    // A model wrote "-e …\n…" literally into a README before these were understood.
    echo: async (a) => {
      let i = 0, esc = false, nl = true;
      for (; i < a.length && /^-[neE]+$/.test(a[i]); i++) { if (a[i].includes('e')) esc = true; if (a[i].includes('E')) esc = false; if (a[i].includes('n')) nl = false; }
      const s = a.slice(i).join(' ');
      return { out: (esc ? unescapeC(s) : s), raw: !nl };
    },
    printf: async (a) => {
      if (!a.length) throw new Error('printf: usage: printf FORMAT [ARG...]');
      let k = 1;
      const out = unescapeC(a[0]).replace(/%([sd%])/g, (m, c) => (c === '%' ? '%' : c === 'd' ? String(parseInt(a[k++] ?? '0', 10) || 0) : String(a[k++] ?? '')));
      return { out, raw: true };
    },
    ls: async (a) => {
      const { f, rest } = flags(a, { short: 'laR1h' });
      const targets = rest.length ? rest : ['.'];
      const out = [];
      for (const tgt of targets) {
        const r = await fs.list(path(tgt));
        if (!r.ok) {                                        // a file: ls prints its name
          const parent = path(tgt).includes('/') ? path(tgt).replace(/\/[^/]*$/, '') : '.';
          const pr = await fs.list(parent); const base = path(tgt).split('/').pop();
          const e = pr.ok && pr.entries.find((x) => x.name === base);
          if (!e) throw new Error(`ls: ${tgt}: No such file or directory`);
          out.push(f.l ? `-  ${String(e.size || 0).padStart(8)}  ${tgt}` : tgt); continue;
        }
        if (targets.length > 1) out.push(tgt + ':');
        const ents = r.entries.filter((e) => f.a || !e.name.startsWith('.'));
        for (const e of ents.slice(0, L.maxRows)) out.push(f.l ? `${e.type === 'dir' ? 'd' : '-'}  ${String(e.type === 'dir' ? '' : (e.size || 0)).padStart(8)}  ${e.name}${e.type === 'dir' ? '/' : ''}` : e.name + (e.type === 'dir' ? '/' : ''));
        if (ents.length > L.maxRows) out.push(`… ${ents.length - L.maxRows} more`);
      }
      return out.join('\n');
    },
    cat: async (a, io) => {
      // cat -n numbers the lines (qwen3.5:9b ran it and got "No such file: -n")
      const { f, rest } = flags(a, { short: 'n' });
      let src;
      if (!rest.length) src = io.stdin ?? ''; else { const parts = []; for (const p of rest) parts.push(await readText(p)); src = parts.join('\n'); }
      if (!f.n) return src;
      return lines(src).map((l, i) => String(i + 1).padStart(6) + '\t' + l).join('\n');
    },
    head: async (a, io) => { const { f, rest } = flags(a, { short: '', numeric: 'n', withValue: { n: 'n' } }); const n = Math.max(0, +(f.n ?? 10) || 0); const src = rest.length ? await readText(rest[0]) : (io.stdin ?? ''); return lines(src).slice(0, n).join('\n'); },
    tail: async (a, io) => { const { f, rest } = flags(a, { short: '', numeric: 'n', withValue: { n: 'n' } }); const n = Math.max(0, +(f.n ?? 10) || 0); const src = rest.length ? await readText(rest[0]) : (io.stdin ?? ''); const ls = lines(src); return ls.slice(Math.max(0, ls.length - n)).join('\n'); },
    sed: async (a, io) => {
      // Only the read form models use to page through a file: sed -n 'A,Bp' / 'Ap' / 'A,$p'. Changes go through edit_file.
      const { f, rest } = flags(a, { short: 'n' });
      const m = f.n && rest[0] && /^(\d+)(?:,(\d+|\$))?p$/.exec(rest[0]);
      if (!m) throw new Error("sed: only sed -n 'A,Bp' (print lines A..B) is supported — to change a file use edit_file");
      const src = rest[1] ? await readText(rest[1]) : (io.stdin ?? ''); const ls = lines(src);
      const from = +m[1], to = m[2] === '$' ? ls.length : m[2] ? +m[2] : from;
      return ls.slice(Math.max(0, from - 1), Math.max(0, to)).join('\n');
    },
    wc: async (a, io) => {
      const { f, rest } = flags(a, { short: 'lwc' }); const all = !f.l && !f.w && !f.c;
      const one = (s, name) => { const parts = []; if (all || f.l) parts.push(lines(s).length); if (all || f.w) parts.push((s.match(/\S+/g) || []).length); if (all || f.c) parts.push(new TextEncoder().encode(s).length); return parts.join(' ') + (name ? ' ' + name : ''); };
      if (!rest.length) return one(io.stdin ?? '');
      const out = []; for (const p of rest) out.push(one(await readText(p), p)); return out.join('\n');
    },
    grep: async (a, io) => {
      const { f, rest } = flags(a, { short: 'inrvlcEFwH', long: { '--recursive': 'r', '--ignore-case': 'i' } });
      if (!rest.length) throw new Error('grep: missing pattern');
      const pat = rest[0], targets = rest.slice(1);
      let re; try { const src = f.F ? pat.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') : pat; re = new RegExp(f.w ? '\\b(?:' + src + ')\\b' : src, f.i ? 'i' : ''); }
      catch { throw new Error(`grep: invalid pattern: ${pat}`); }
      const out = []; let total = 0;
      const scan = (text, name) => {
        let n = 0; const ls = lines(text);
        const tag = name && (targets.length > 1 || f.r || f.H) ? name + ':' : '';   // like grep: the file name only when there are several
        for (let i = 0; i < ls.length && total < L.maxMatches; i++) {
          if (re.test(ls[i]) === !f.v) { n++; total++; if (!f.l && !f.c) out.push(tag + (f.n ? (i + 1) + ':' : '') + ls[i]); }
        }
        if (f.l && n) out.push(name); if (f.c) out.push(tag + n);
      };
      if (!targets.length) { scan(io.stdin ?? '', ''); }
      for (const t of targets) {
        if (await isDir(t)) {
          if (!f.r) { out.push(`grep: ${t}: Is a directory (use -r)`); continue; }
          const w = await walk(t, {}, async (p, e) => { if (e.type !== 'dir' && total < L.maxMatches && isTextName(e.name) && (e.size || 0) < L.readBytes) { try { scan(await readText(p), show(p)); } catch {} } });
          if (w.capped) out.push('grep: (directory scan stopped early: narrow the path)');
        } else scan(await readText(t), t);
      }
      if (total >= L.maxMatches) out.push(`… stopped at ${L.maxMatches} matches: narrow the pattern or the path`);
      return { out: out.join('\n'), code: total || f.c ? 0 : 1 };
    },
    find: async (a) => {
      let start = '.', i = 0; const crit = { depth: L.maxDepth };
      if (a[0] && !a[0].startsWith('-')) { start = a[0]; i = 1; }
      for (; i < a.length; i++) {
        if (a[i] === '-name' || a[i] === '-iname') { crit.name = globRe(a[++i], a[i - 1] === '-iname'); }
        else if (a[i] === '-type') crit.type = a[++i];
        else if (a[i] === '-maxdepth') crit.depth = Math.min(L.maxDepth, +a[++i] || 1);
        else throw new Error(`find: unsupported option ${a[i]} (use -name -iname -type f|d -maxdepth N)`);
      }
      const out = [];
      const w = await walk(start, { maxDepth: crit.depth }, async (p, e) => {
        if (crit.type === 'f' && e.type === 'dir') return; if (crit.type === 'd' && e.type !== 'dir') return;
        if (crit.name && !crit.name.test(e.name)) return;
        if (out.length < L.maxRows) out.push(show(p)); else return false;
      });
      if (out.length >= L.maxRows) out.push(`… stopped at ${L.maxRows} results: narrow it with -name or a path`);
      if (w.capped) out.push('find: (scan stopped early: narrow the path)');
      return out.join('\n');
    },
    tree: async (a) => {
      const { f, rest } = flags(a, { short: '', withValue: { L: 'L' } }); const depth = Math.min(L.maxDepth, +(f.L || 3));
      const start = rest[0] || '.'; const out = [show(start)];
      await walk(start, { maxDepth: depth }, async (p, e, d) => { if (out.length > L.maxRows) return false; out.push('  '.repeat(d - 1) + '├─ ' + e.name + (e.type === 'dir' ? '/' : '')); });
      return out.join('\n');
    },
    sort: async (a, io) => { const { f, rest } = flags(a, { short: 'rnu' }); let ls = lines(rest.length ? await readText(rest[0]) : (io.stdin ?? '')); ls.sort(f.n ? (x, y) => parseFloat(x) - parseFloat(y) : (x, y) => x.localeCompare(y)); if (f.r) ls.reverse(); if (f.u) ls = [...new Set(ls)]; return ls.join('\n'); },
    uniq: async (a, io) => { const { f, rest } = flags(a, { short: 'c' }); const ls = lines(rest.length ? await readText(rest[0]) : (io.stdin ?? '')); const out = []; for (const l of ls) { const last = out[out.length - 1]; if (last && last.t === l) last.n++; else out.push({ t: l, n: 1 }); } return out.map((x) => (f.c ? String(x.n).padStart(4) + ' ' : '') + x.t).join('\n'); },
    mkdir: async (a) => { const { rest } = flags(a, { short: 'p' }); for (const d of rest) { const r = await fs.mkdir(path(d)); if (!r.ok) throw new Error(`mkdir: ${d}: ${r.error}`); } return ''; },
    touch: async (a) => { for (const p of a) { const r = await fs.read(path(p), { maxBytes: 1 }); if (!r.ok) { const w = await fs.write(path(p), '', { overwrite: false, mkdir: true }); if (!w.ok) throw new Error(`touch: ${p}: ${w.error}`); } } return ''; },
    cp: async (a) => { const { rest } = flags(a, { short: 'r' }); if (rest.length !== 2) throw new Error('cp: usage: cp SOURCE DEST'); let dst = rest[1]; if (await isDir(dst)) dst = dst.replace(/\/$/, '') + '/' + rest[0].split('/').pop(); const src = await fs.read(path(rest[0]), { maxBytes: L.readBytes }); if (!src.ok) throw new Error(`cp: ${rest[0]}: ${src.error}`); const w = await fs.write(path(dst), src.content, { overwrite: true, mkdir: true }); if (!w.ok) throw new Error(`cp: ${dst}: ${w.error}`); return ''; },
    mv: async (a) => { if (a.length !== 2) throw new Error('mv: usage: mv SOURCE DEST'); let dst = a[1]; if (await isDir(dst)) dst = dst.replace(/\/$/, '') + '/' + a[0].split('/').pop(); const r = await fs.move(path(a[0]), path(dst), { overwrite: false }); if (!r.ok) throw new Error(`mv: ${r.error}`); return ''; },
    rm: async (a) => {
      const { f, rest } = flags(a, { short: 'rf' });
      for (const p of rest) {
        if (await isDir(p)) {
          if (!f.r) throw new Error(`rm: ${p}: Is a directory (use -r)`);
          const files = [], dirs = [];
          await walk(p, {}, async (c, e) => { (e.type === 'dir' ? dirs : files).push(c); });
          for (const x of files) await fs.del(path(x));
          for (const x of dirs.reverse()) await fs.del(path(x));
          const r = await fs.del(path(p)); if (!r.ok && !f.f) throw new Error(`rm: ${p}: ${r.error}`);
        } else { const r = await fs.del(path(p)); if (!r.ok && !f.f) throw new Error(`rm: ${p}: ${r.error === 'not-found' ? 'No such file' : r.error}`); }
      }
      return '';
    },
    // The Cardputer itself (read-only, from /api/status and the app registry)
    df: async () => { const s = device.status && await device.status(); const st = s && s.storage; if (!st || !st.mounted) return 'SD card not mounted'; const g = (b) => (b / 1e9).toFixed(1) + 'G'; return `Filesystem  Size   Used   Avail  Use%\nsd          ${g(st.total_bytes)}  ${g(st.total_bytes - st.free_bytes)}  ${g(st.free_bytes)}  ${Math.round(100 * (1 - st.free_bytes / st.total_bytes))}%`; },
    free: async () => { const s = device.status && await device.status(); if (!s) throw new Error('free: device status unavailable'); return `heap free: ${Math.round(s.free_heap / 1024)} KB  min: ${Math.round((s.min_free_heap || 0) / 1024)} KB  largest block: ${Math.round((s.largest_free_block || 0) / 1024)} KB`; },
    uptime: async () => { const s = device.status && await device.status(); if (!s) throw new Error('uptime: device status unavailable'); const u = s.uptime_s | 0; return `up ${Math.floor(u / 3600)}h ${Math.floor((u % 3600) / 60)}m` + (s.battery && typeof s.battery.pct === 'number' ? `, battery ${Math.round(s.battery.pct)}%` : ''); },
    date: async () => { const s = device.status && await device.status(); const t = s && s.network && s.network.time; return (t > 1672531200 ? new Date(t * 1000) : new Date()).toString(); },
    uname: async () => { const s = device.status && await device.status(); return 'NucleoOS ' + ((s && s.version) || '') + ' esp32s3' + (s && s.profile ? ' (' + s.profile + ' profile)' : ''); },
    // id, name, and — when the catalog has it — category and what the app does, so `apps | grep -i audio` works
    apps: async () => { const l = device.apps ? await device.apps() : []; return l.map((x) => x.id + '\t' + x.name + (x.category ? '\t[' + x.category + ']' : '') + (x.description ? '\t' + x.description : '')).join('\n'); },
    open: async (a) => { if (!device.open) throw new Error('open: not available here'); const x = a[0]; if (!x) throw new Error('open: usage: open APP-ID | FILE'); return await device.open(/[./]/.test(x) ? { path: path(x) } : { app: x }); },
  };

  async function runStage(st, stdin, last) {
    const name = st.argv[0].toLowerCase();
    const inFrom = st.redirs.find((r) => r.op === '<'), outTo = st.redirs.find((r) => r.op === '>' || r.op === '>>');
    const feed = inFrom ? await readText(inFrom.path) : stdin;
    let res = await C[name](st.argv.slice(1), { stdin: feed, last });
    let code = 0, raw = false;                              // raw: echo -n / printf — no newline added, as in a real shell
    if (res && typeof res === 'object') { code = res.code | 0; raw = !!res.raw; res = res.out; }
    let out = res == null ? '' : String(res);
    if (outTo) {
      let body = raw ? out : (out ? out.replace(/\n?$/, '\n') : '');
      if (outTo.op === '>>') { const r = await fs.append(path(outTo.path), body); if (!r.ok) throw new Error(`${name}: cannot write ${outTo.path}: ${r.error}`); }
      else { const r = await fs.write(path(outTo.path), body, { overwrite: true, mkdir: true }); if (!r.ok) throw new Error(`${name}: cannot write ${outTo.path}: ${r.error}`); }
      out = '';
    }
    return { out, code };
  }

  // → { out, code, writes }. One human confirmation per line that changes files, before anything runs.
  async function run(line) {
    const p = parse(line, { ...env, PWD: '/' + cwd });
    if (p.error) return { out: p.error, code: 2, writes: [] };
    for (const seg of p.list) for (const st of seg.stages) {
      const nm = st.argv[0].toLowerCase();
      if (!C[nm]) return { out: `${nm}: command not found. Available: ${Object.keys(C).sort().join(' ')}`, code: 127, writes: [] };
    }
    const writes = plannedWrites(p);
    if (writes.length) {
      const ok = await confirm({ cmd: String(line), writes, destructive: writes.some((w) => w.op === 'rm') });
      if (!ok) return { out: 'not run: the user declined', code: 1, writes };
    }
    const outs = []; let code = 0;
    for (const seg of p.list) {
      if (seg.join === '&&' && code !== 0) continue;
      if (seg.join === '||' && code === 0) continue;
      let data = null;
      for (let i = 0; i < seg.stages.length; i++) {
        try { const r = await runStage(seg.stages[i], data, i === seg.stages.length - 1); data = r.out; code = r.code; }
        catch (e) { outs.push(String(e && e.message || e)); code = 1; data = null; break; }
        if (code !== 0 && i < seg.stages.length - 1) break;          // pipefail
      }
      if (data) outs.push(data);
    }
    let out = outs.join('\n');
    if (out.length > L.maxOut) {
      const ls = out.slice(L.maxOut).split('\n').length;
      out = out.slice(0, L.maxOut) + `\n… output truncated (~${ls} more lines): narrow it with grep, head, or sed -n 'A,Bp'`;
    }
    return { out, code, writes };
  }

  return { run, get cwd() { return '/' + cwd; }, reset() { cwd = ''; }, commands: Object.keys(C) };
}

const TEXT_EXT = /\.(txt|md|markdown|json|jsonl|csv|tsv|log|c|h|cpp|hpp|cc|js|mjs|cjs|ts|tsx|jsx|py|html|htm|css|svg|xml|ya?ml|toml|ini|cfg|conf|sh|bat|ps1|lua|rs|go|java|kt|swift|rb|php|sql|todo|env)$/i;
const isTextName = (n) => TEXT_EXT.test(n) || !/\./.test(n);
