// tool-guard.js — the checks every tool call of the agent goes through before it runs.
// Two failure modes of small local models, handled the way OpenCode does (session/llm.ts tool-call repair +
// tool/invalid.ts, session/processor.ts DOOM_LOOP_THRESHOLD; github.com/anomalyco/opencode, MIT):
//   • a misspelled / mis-cased tool name ("Read_File", "readfile", "write_fle") — repaired when the intended
//     tool is unambiguous, otherwise answered with a readable error listing the tools, so the loop goes on
//     instead of dying on "unknown tool";
//   • the doom loop — the same tool with the same arguments over and over (a 9B model re-reading one file
//     forever): the 3rd identical call in a row is NOT run; the model is told to change approach.
// Pure, no I/O. tools/agent-tool-guard.test.mjs.

export const DOOM_LOOP_THRESHOLD = 3;

function lev(a, b) {
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j);
  for (let i = 1; i <= a.length; i++) {
    const cur = [i];
    for (let j = 1; j <= b.length; j++) cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    prev = cur;
  }
  return prev[b.length];
}
const canon = (s) => String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '');
// Arguments compared by value, key order ignored ({a,b} and {b,a} are the same call).
function stable(v) {
  if (Array.isArray(v)) return '[' + v.map(stable).join(',') + ']';
  if (v && typeof v === 'object') return '{' + Object.keys(v).sort().map((k) => JSON.stringify(k) + ':' + stable(v[k])).join(',') + '}';
  return JSON.stringify(v === undefined ? null : v);
}

// known: iterable of tool names. → { check(name, input) → { run:true, name } | { run:false, content } , reset() }
export function createToolGuard(known, { threshold = DOOM_LOOP_THRESHOLD } = {}) {
  const names = [...new Set(known || [])];
  const byCanon = new Map(names.map((n) => [canon(n), n]));
  let lastKey = '', repeats = 0;

  function resolveName(name) {
    if (names.includes(name)) return name;
    const c = canon(name);
    if (byCanon.has(c)) return byCanon.get(c);                       // case / separators: Read_File, read-file, readfile
    let best = null, bestD = Infinity, tie = false;
    for (const n of names) {
      const d = lev(c, canon(n));
      if (d < bestD) { best = n; bestD = d; tie = false; } else if (d === bestD) tie = true;
    }
    return bestD <= 2 && !tie && c.length >= 4 ? best : null;           // a small, unambiguous typo only
  }

  return {
    check(name, input) {
      const real = resolveName(name);
      if (!real) {
        lastKey = ''; repeats = 0;
        return { run: false, content: 'Unknown tool "' + String(name) + '". Use one of: ' + names.join(', ') + '.' };
      }
      const key = real + ' ' + stable(input || {});
      repeats = key === lastKey ? repeats + 1 : 1;
      lastKey = key;
      if (repeats >= threshold) {
        return { run: false, content: 'You have called ' + real + ' with exactly the same arguments ' + repeats +
          ' times in a row and got the same result. Do not repeat it: use what it already returned, try a different tool or different arguments, or give your answer now.' };
      }
      return { run: true, name: real };
    },
    reset() { lastKey = ''; repeats = 0; },
  };
}
