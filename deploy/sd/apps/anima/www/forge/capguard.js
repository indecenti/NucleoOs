// capguard.js — least-privilege capability inference + static danger scan for MODEL-GENERATED code,
// run BEFORE the sandbox even spins up. Generated code that runs in the user's browser is the
// highest-severity surface: this infers which `os.*` capabilities the code actually exercises (so
// the runner can grant the MINIMUM), and flags patterns that should block or warn — dynamic eval,
// file-mutation inside a loop, network exfiltration when the task is offline, dynamic import,
// obfuscation. The verdict combiner folds `severity` in (block → veto, warn → warn).
// Pure & DOM-free → host-testable.

const CAP_PATTERNS = [
  ['fs.read',   /\bos\s*\.\s*fs\s*\.\s*(?:read|list|exists)\s*\(/],
  ['fs.write',  /\bos\s*\.\s*fs\s*\.\s*(?:write|append|mkdir)\s*\(/],
  ['fs.remove', /\bos\s*\.\s*fs\s*\.\s*remove\s*\(/],
  ['http',      /\bos\s*\.\s*http\s*\./],
  ['anima',     /\bos\s*\.\s*anima\s*\(/],
  ['notify',    /\bos\s*\.\s*notify\s*\(/],
  // Hardware actuation (IR blaster, GPIO, Wi-Fi radio) — the ONE capability that acts on the world
  // outside the browser. It was missing here, so generated code touching os.hw.* was neither
  // inferred nor reported as over-privilege; the runner's hw default is ON, so nothing caught it.
  ['hw',        /\bos\s*\.\s*hw\s*(?:\.|\[)/],
];

export function inferCapabilities(code) {
  const s = String(code || '');
  const caps = [];
  for (const [name, rx] of CAP_PATTERNS) if (rx.test(s)) caps.push(name);
  return caps;
}

// Find the body {…} that follows a loop header and return its source (brace-matched), bounded.
function loopBodies(s) {
  const bodies = [];
  const rx = /\b(?:for|while)\s*\([^)]*\)\s*\{/g;
  let m;
  while ((m = rx.exec(s))) {
    let depth = 1, i = rx.lastIndex;
    for (; i < s.length && depth; i++) { if (s[i] === '{') depth++; else if (s[i] === '}') depth--; }
    bodies.push(s.slice(rx.lastIndex, i - 1));
    rx.lastIndex = i;
  }
  return bodies;
}

export function scanDangers(code, { allowNetwork = false } = {}) {
  const s = String(code || '');
  const dangers = [];
  if (/\beval\s*\(|\bnew\s+Function\s*\(|(?:^|[^.\w])Function\s*\(/.test(s)) dangers.push({ kind: 'dynamic-eval', level: 'block' });
  // Both ways to pull in foreign code: importScripts() (classic worker) AND dynamic import().
  // import() is the one that still works — importScripts is already stubbed out in the worker, so
  // scanning only for it left the live escape hatch unguarded.
  if (/\bimportScripts\s*\(/.test(s) || /(?:^|[^.\w$])import\s*\(/.test(s)) dangers.push({ kind: 'dynamic-import', level: 'block' });
  // Hardware actuation from generated code is never incidental — surface it for the human gate.
  if (/\bos\s*\.\s*hw\s*(?:\.|\[)/.test(s)) dangers.push({ kind: 'hardware-actuation', level: 'warn' });
  const mut = /\bos\s*\.\s*fs\s*\.\s*(?:write|append|remove|mkdir)\s*\(/;
  if (loopBodies(s).some((b) => mut.test(b))) dangers.push({ kind: 'mutation-in-loop', level: 'block' });
  if (/\bos\s*\.\s*http\s*\./.test(s) && !allowNetwork) dangers.push({ kind: 'network-exfil', level: 'block' });
  if (/\batob\s*\(/.test(s) || /(?:\\x[0-9a-f]{2}){8,}/i.test(s) || /[A-Za-z0-9+/]{160,}={0,2}/.test(s)) dangers.push({ kind: 'obfuscation', level: 'warn' });
  return dangers;
}

// assess(code, { granted, allowNetwork }) → { capabilities, over, dangers, severity }
//   over: capabilities used beyond the granted set (least-privilege violation)
//   severity: 'block' (any block-level danger) | 'warn' (over-privilege or warn-level danger) | 'ok'
export function assess(code, { granted = null, allowNetwork = false } = {}) {
  const capabilities = inferCapabilities(code);
  const dangers = scanDangers(code, { allowNetwork });
  const over = Array.isArray(granted) ? capabilities.filter((c) => !granted.includes(c)) : [];
  // F2: hardware ACTUATION the app was not granted is a BLOCK, not a warning. Over-reaching the
  // filesystem is contained by the sandbox; firing the IR blaster or driving a GPIO pin is not — it
  // acts on the room. So `hw` in `over` (granted was provided AND does not include it) vetoes.
  const hwOverreach = Array.isArray(granted) && over.includes('hw');
  let severity = 'ok';
  if (dangers.some((d) => d.level === 'block') || hwOverreach) severity = 'block';
  else if (over.length || dangers.length) severity = 'warn';
  return { capabilities, over, dangers, severity, hwOverreach };
}
