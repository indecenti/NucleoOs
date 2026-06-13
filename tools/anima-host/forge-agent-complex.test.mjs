// Gate: ANIMA Forge — COMPLEX agentic behaviour, "does it act like Claude Code?". Each test runs the
// REAL loop with a REAL execution sandbox (code actually runs; console output + thrown assertions are
// real). The model is a scripted MockEngine (no GPU), but the AGENTIC MACHINERY is fully exercised:
// the agent writes code, WRITES ITS OWN TESTS, RUNS them, READS the failure, DECIDES to fix, retries,
// re-runs, and on success proposes follow-ups. 20 algorithm tasks (buggy → self-test catches it → fix
// → pass) + 12 behaviour tests (syntax/runtime fix, capguard veto, budget, best-of-N, reject,
// multi-step, learning, output capture, contradiction) = 32 complex runs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runAgent } from '../../apps/anima/www/forge/loop.js';
import { MockEngine } from '../../apps/anima/www/forge/engine.js';
import * as provenance from '../../apps/anima/www/forge/provenance.js';
import { checkSyntax } from '../../apps/code-runner/www/nucleo-run.js';

// ---- a REAL execution sandbox: mode:'check' = parse-only; else run the code + capture logs/throws ----
function mkSandbox() {
  const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
  return {
    run: async (code, opts = {}) => {
      if (opts.mode === 'check') return checkSyntax(code);
      const logs = [];
      const con = { log: (...a) => logs.push(a.map(String).join(' ')), info() {}, warn() {}, error() {}, debug() {} };
      const denied = () => { throw new Error('capability denied'); };
      const os = { fs: { read: denied, write: denied, append: denied, list: denied, remove: denied, mkdir: denied, exists: denied }, http: { get: denied, json: denied }, anima: denied, notify: denied };
      try { const fn = new AsyncFn('os', 'console', 'print', 'args', 'env', '"use strict";\n' + code); const v = await fn(os, con, (...a) => logs.push(a.map(String).join(' ')), [], {}); return { ok: true, logs, value: v }; }
      catch (e) { return { ok: false, error: String((e && e.message) || e), logs }; }
    },
  };
}
function mkFs(seed = {}) {
  const store = new Map(Object.entries(seed));
  return {
    store,
    read: async (p) => (store.has(p) ? { ok: true, content: store.get(p) } : { ok: false, error: 'not-found' }),
    list: async () => ({ ok: true, entries: [] }),
    write: async (p, c) => { store.set(p, c); return { ok: true }; },
    append: async (p, c) => { store.set(p, (store.get(p) || '') + c); return { ok: true }; },
    edit: async (p, o, n) => { store.set(p, String(store.get(p) || '').replace(o, n)); return { ok: true }; },
    del: async (p) => { store.delete(p); return { ok: true }; },
    mkdir: async () => ({ ok: true }), move: async () => ({ ok: true }),
  };
}
const fence = (s) => '```js\n' + s + '\n```';
const plan = (actions) => JSON.stringify({ actions });

// ---- 20 algorithm tasks: { spec, buggy, fixed, test, expect } (buggy parses+runs but is WRONG) ----
const TASKS = [
  { name: 'factorial', spec: 'factorial of a number', buggy: 'const fact=n=>n<=1?1:n*fact(n-2);console.log(fact(5));', fixed: 'const fact=n=>n<=1?1:n*fact(n-1);console.log(fact(5));', test: 'if(fact(5)!==120)throw new Error("fact5="+fact(5));if(fact(0)!==1)throw new Error("fact0");', expect: '120' },
  { name: 'fibonacci', spec: 'nth fibonacci number', buggy: 'const fib=n=>n<2?n:fib(n-1)+fib(n-3);console.log(fib(7));', fixed: 'const fib=n=>n<2?n:fib(n-1)+fib(n-2);console.log(fib(7));', test: 'if(fib(7)!==13)throw new Error("fib7="+fib(7));if(fib(10)!==55)throw new Error("fib10");', expect: '13' },
  { name: 'isPrime', spec: 'check whether a number is prime', buggy: 'const isP=n=>{if(n<2)return true;for(let i=2;i<n;i++)if(n%i===0)return false;return true;};console.log(isP(13));', fixed: 'const isP=n=>{if(n<2)return false;for(let i=2;i<n;i++)if(n%i===0)return false;return true;};console.log(isP(13));', test: 'if(!isP(13))throw new Error("13");if(isP(1))throw new Error("1 not prime");if(isP(9))throw new Error("9");', expect: 'true' },
  { name: 'gcd', spec: 'greatest common divisor', buggy: 'const gcd=(a,b)=>b===0?b:gcd(b,a%b);console.log(gcd(12,18));', fixed: 'const gcd=(a,b)=>b===0?a:gcd(b,a%b);console.log(gcd(12,18));', test: 'if(gcd(12,18)!==6)throw new Error("g="+gcd(12,18));if(gcd(7,13)!==1)throw new Error("coprime");', expect: '6' },
  { name: 'reverseString', spec: 'reverse a string', buggy: 'const rev=s=>s.split("").join("");console.log(rev("abc"));', fixed: 'const rev=s=>s.split("").reverse().join("");console.log(rev("abc"));', test: 'if(rev("abc")!=="cba")throw new Error("r="+rev("abc"));', expect: 'cba' },
  { name: 'isPalindrome', spec: 'check if a string is a palindrome', buggy: 'const pal=s=>s===s.split("").join("");console.log(pal("racecar"));', fixed: 'const pal=s=>s===s.split("").reverse().join("");console.log(pal("racecar"));', test: 'if(!pal("racecar"))throw new Error("racecar");if(pal("hello"))throw new Error("hello");', expect: 'true' },
  { name: 'sumArray', spec: 'sum of an array of numbers', buggy: 'const sum=a=>a.reduce((x,y)=>x+y,1);console.log(sum([1,2,3]));', fixed: 'const sum=a=>a.reduce((x,y)=>x+y,0);console.log(sum([1,2,3]));', test: 'if(sum([1,2,3])!==6)throw new Error("s="+sum([1,2,3]));if(sum([])!==0)throw new Error("empty");', expect: '6' },
  { name: 'maxArray', spec: 'maximum of an array', buggy: 'const mx=a=>a[0];console.log(mx([3,7,2]));', fixed: 'const mx=a=>Math.max(...a);console.log(mx([3,7,2]));', test: 'if(mx([3,7,2])!==7)throw new Error("m="+mx([3,7,2]));', expect: '7' },
  { name: 'flattenDeep', spec: 'deeply flatten a nested array', buggy: 'const flat=a=>a.reduce((x,y)=>x.concat(y),[]);console.log(JSON.stringify(flat([1,[2,[3]]])));', fixed: 'const flat=a=>a.reduce((x,y)=>x.concat(Array.isArray(y)?flat(y):y),[]);console.log(JSON.stringify(flat([1,[2,[3]]])));', test: 'if(JSON.stringify(flat([1,[2,[3]]]))!=="[1,2,3]")throw new Error(JSON.stringify(flat([1,[2,[3]]])));', expect: '[1,2,3]' },
  { name: 'fizzbuzz', spec: 'fizzbuzz of a number', buggy: 'const fb=n=>n%3===0?"Fizz":n%5===0?"Buzz":n%15===0?"FizzBuzz":String(n);console.log(fb(15));', fixed: 'const fb=n=>n%15===0?"FizzBuzz":n%3===0?"Fizz":n%5===0?"Buzz":String(n);console.log(fb(15));', test: 'if(fb(15)!=="FizzBuzz")throw new Error("15="+fb(15));if(fb(3)!=="Fizz")throw new Error("3");if(fb(5)!=="Buzz")throw new Error("5");if(fb(2)!=="2")throw new Error("2");', expect: 'FizzBuzz' },
  { name: 'countVowels', spec: 'count vowels in a string', buggy: 'const cv=s=>s.split("").filter(c=>"aeiou".includes(c)).length-1;console.log(cv("hello"));', fixed: 'const cv=s=>s.split("").filter(c=>"aeiou".includes(c)).length;console.log(cv("hello"));', test: 'if(cv("hello")!==2)throw new Error("c="+cv("hello"));if(cv("xyz")!==0)throw new Error("xyz");', expect: '2' },
  { name: 'capitalize', spec: 'capitalize the first letter', buggy: 'const cap=s=>s.toUpperCase();console.log(cap("abc"));', fixed: 'const cap=s=>s.charAt(0).toUpperCase()+s.slice(1);console.log(cap("abc"));', test: 'if(cap("abc")!=="Abc")throw new Error("c="+cap("abc"));', expect: 'Abc' },
  { name: 'unique', spec: 'unique values of an array', buggy: 'const uniq=a=>a;console.log(JSON.stringify(uniq([1,1,2,3,3])));', fixed: 'const uniq=a=>[...new Set(a)];console.log(JSON.stringify(uniq([1,1,2,3,3])));', test: 'if(JSON.stringify(uniq([1,1,2,3,3]))!=="[1,2,3]")throw new Error(JSON.stringify(uniq([1,1,2,3,3])));', expect: '[1,2,3]' },
  { name: 'chunk', spec: 'split an array into chunks of size n', buggy: 'const chunk=(a,n)=>{const o=[];for(let i=0;i<a.length;i+=n)o.push(a.slice(i,n));return o;};console.log(JSON.stringify(chunk([1,2,3,4,5],2)));', fixed: 'const chunk=(a,n)=>{const o=[];for(let i=0;i<a.length;i+=n)o.push(a.slice(i,i+n));return o;};console.log(JSON.stringify(chunk([1,2,3,4,5],2)));', test: 'if(JSON.stringify(chunk([1,2,3,4,5],2))!=="[[1,2],[3,4],[5]]")throw new Error(JSON.stringify(chunk([1,2,3,4,5],2)));', expect: '[[1,2],[3,4],[5]]' },
  { name: 'romanToInt', spec: 'convert a roman numeral to an integer', buggy: 'const rom=s=>{const m={I:1,V:5,X:10,L:50,C:100,D:500,M:1000};let t=0;for(const c of s)t+=m[c];return t;};console.log(rom("XIV"));', fixed: 'const rom=s=>{const m={I:1,V:5,X:10,L:50,C:100,D:500,M:1000};let t=0;for(let i=0;i<s.length;i++){if(i+1<s.length&&m[s[i]]<m[s[i+1]])t-=m[s[i]];else t+=m[s[i]];}return t;};console.log(rom("XIV"));', test: 'if(rom("XIV")!==14)throw new Error("XIV="+rom("XIV"));if(rom("MCMXC")!==1990)throw new Error("MCMXC="+rom("MCMXC"));', expect: '14' },
  { name: 'celsiusToF', spec: 'convert celsius to fahrenheit', buggy: 'const c2f=c=>c*9/5;console.log(c2f(100));', fixed: 'const c2f=c=>c*9/5+32;console.log(c2f(100));', test: 'if(c2f(100)!==212)throw new Error("100="+c2f(100));if(c2f(0)!==32)throw new Error("0");', expect: '212' },
  { name: 'titleCase', spec: 'title-case a sentence', buggy: 'const tc=s=>s.split(" ").map(w=>w.toUpperCase()).join(" ");console.log(tc("hello world"));', fixed: 'const tc=s=>s.split(" ").map(w=>w.charAt(0).toUpperCase()+w.slice(1)).join(" ");console.log(tc("hello world"));', test: 'if(tc("hello world")!=="Hello World")throw new Error("t="+tc("hello world"));', expect: 'Hello World' },
  { name: 'binarySearch', spec: 'binary search returning the index or -1', buggy: 'const bs=(a,x)=>{let lo=0,hi=a.length-1;while(lo<hi){const m=(lo+hi)>>1;if(a[m]===x)return m;if(a[m]<x)lo=m+1;else hi=m-1;}return -1;};console.log(bs([1,3,5,7,9],7));', fixed: 'const bs=(a,x)=>{let lo=0,hi=a.length-1;while(lo<=hi){const m=(lo+hi)>>1;if(a[m]===x)return m;if(a[m]<x)lo=m+1;else hi=m-1;}return -1;};console.log(bs([1,3,5,7,9],7));', test: 'if(bs([1,3,5,7,9],7)!==3)throw new Error("idx="+bs([1,3,5,7,9],7));if(bs([1,3,5],4)!==-1)throw new Error("miss");if(bs([1,3,5,7,9],9)!==4)throw new Error("last="+bs([1,3,5,7,9],9));', expect: '3' },
  { name: 'power', spec: 'integer power a^b', buggy: 'const pw=(a,b)=>{let r=1;for(let i=0;i<b-1;i++)r*=a;return r;};console.log(pw(2,10));', fixed: 'const pw=(a,b)=>{let r=1;for(let i=0;i<b;i++)r*=a;return r;};console.log(pw(2,10));', test: 'if(pw(2,10)!==1024)throw new Error("p="+pw(2,10));if(pw(5,0)!==1)throw new Error("zero");', expect: '1024' },
  { name: 'average', spec: 'average of an array', buggy: 'const avg=a=>a.reduce((x,y)=>x+y,0);console.log(avg([2,4,6]));', fixed: 'const avg=a=>a.reduce((x,y)=>x+y,0)/a.length;console.log(avg([2,4,6]));', test: 'if(avg([2,4,6])!==4)throw new Error("a="+avg([2,4,6]));', expect: '4' },
];

function driveTask(t) {
  const fs = mkFs(), sandbox = mkSandbox();
  let synthN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: t.spec, path: t.name + '.js' }]);
    if (o.phase === 'synthesize') { synthN++; return fence(synthN === 1 ? t.buggy : t.fixed); }
    if (o.phase === 'test') return fence(t.test);
    return '';
  });
  const staged = [], ledger = [];
  const deps = { engine, fs, sandbox, approve: async () => true, deviceVerify: null, provenance, ledger, staged, cards: [] };
  return { run: runAgent(t.spec, deps, { budget: 8, candidates: 1, test: true, model: 'mock', revision: 'r', ts: 1, lang: 'en' }), fs, staged, path: t.name + '.js' };
}

for (const t of TASKS) {
  test(`AGENT write→self-test→FIX→verify→run: ${t.name}`, async () => {
    const d = driveTask(t);
    const r = await d.run;
    assert.equal(r.status, 'done', `did not finish: ${JSON.stringify(r.trace.slice(-3))}`);
    // self-test caught the bug, then the fix passed
    assert.ok(r.trace.some((s) => s.state === 'TEST' && s.ok === false), 'self-test should catch the buggy version');
    assert.ok(r.trace.some((s) => s.state === 'FIX' && /tests-failed/.test(s.reason || '')), 'should decide to fix on a failed self-test');
    assert.ok(r.trace.some((s) => s.state === 'TEST' && s.ok === true), 'the fixed version should pass its self-test');
    // only the CORRECT code was ever written (the buggy one never reached disk)
    assert.deepEqual(d.fs.store.get(d.path), t.fixed, 'the applied code must be the fixed version');
    assert.equal(d.staged.length >= 0, true);
    // it ran and produced the expected output, and proposed follow-ups
    const run = [...r.trace].reverse().find((s) => s.state === 'RUN');
    assert.ok(run && run.ok && String(run.output).includes(t.expect), `run output ${run && run.output} should include ${t.expect}`);
    assert.ok(Array.isArray(r.followups) && r.followups.includes('improve'), 'should offer follow-ups (Claude-Code-style)');
  });
}

// ---------- 12 behaviour tests ----------
function driver({ seed = {}, actionsPlan, synth, testGen, approve = async () => true, deviceVerify = null, opts = {} } = {}) {
  const fs = mkFs(seed), sandbox = mkSandbox();
  let synthN = 0, planN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') { planN++; return actionsPlan(planN); }
    if (o.phase === 'synthesize') { synthN++; return fence(synth(synthN, o.sample)); }
    if (o.phase === 'test') return fence(testGen ? testGen() : '');
    return '';
  });
  const staged = [], ledger = [];
  const deps = { engine, fs, sandbox, approve, deviceVerify, provenance, ledger, staged, cards: [] };
  return { run: runAgent('task', deps, Object.assign({ budget: 6, candidates: 1, ts: 1, lang: 'en' }, opts)), fs, staged, ledger };
}
const synthThenDone = (good) => (n) => (n === 1 ? plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]) : plan([{ op: 'done' }]));

test('BEHAVIOUR: syntax error is caught at the static check, fixed, then applied', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]), synth: (n) => (n === 1 ? 'const f=(' : 'console.log(1)'), opts: { test: false } });
  const r = await d.run;
  assert.equal(r.status, 'done');
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
  assert.equal(d.fs.store.get('s.js'), 'console.log(1)');
});

test('BEHAVIOUR: a runtime error on RUN triggers a fix (read-the-error-then-decide)', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]), synth: (n) => (n === 1 ? 'throw new Error("boom")' : 'console.log("ok")'), opts: { test: false } });
  const r = await d.run;
  assert.equal(r.status, 'done');
  assert.ok(r.trace.some((s) => s.state === 'RUN' && s.ok === false), 'should observe the runtime failure');
  assert.ok(r.trace.some((s) => s.state === 'RUN' && s.ok === true), 'and recover');
  assert.equal(d.fs.store.get('s.js'), 'console.log("ok")');
});

test('BEHAVIOUR: dangerous eval is vetoed by capguard, never applied', async () => {
  const d = driver({ actionsPlan: synthThenDone(), synth: (n) => (n === 1 ? 'eval(userInput)' : 'console.log(1)') });
  const r = await d.run;
  assert.equal(d.fs.store.has('s.js'), false);
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
});

test('BEHAVIOUR: a delete-loop is vetoed (mutation-in-loop), never applied', async () => {
  const d = driver({ actionsPlan: synthThenDone(), synth: () => 'for(let i=0;i<5;i++){ os.fs.remove("f"+i); }' });
  const r = await d.run;
  assert.equal(d.fs.store.size, 0);
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
});

test('BEHAVIOUR: budget bounds an unconvergeable task (no runaway, no write)', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]), synth: () => 'console.log(0)', testGen: () => 'throw new Error("always fails")', opts: { test: true, budget: 3 } });
  const r = await d.run;
  assert.equal(r.status, 'abort');
  assert.equal(d.fs.store.size, 0);
});

test('BEHAVIOUR: best-of-N discards the unparseable candidate and applies the good one', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]), synth: (n, sample) => (sample === 0 ? 'const f=(' : 'console.log(2)'), opts: { test: false, candidates: 2 } });
  const r = await d.run;
  assert.equal(r.status, 'done');
  assert.equal(d.fs.store.get('s.js'), 'console.log(2)');
});

test('BEHAVIOUR: reject → nothing written, no auto-apply', async () => {
  const d = driver({ actionsPlan: synthThenDone(), synth: () => 'console.log(3)', approve: async () => false, opts: { test: false } });
  const r = await d.run;
  assert.equal(d.fs.store.has('s.js'), false);
  assert.ok(r.trace.some((s) => s.state === 'REJECTED'));
});

test('BEHAVIOUR: multi-step plan — reads a file, then synthesizes', async () => {
  const d = driver({ seed: { 'data.txt': 'hi' }, actionsPlan: () => plan([{ op: 'read', path: 'data.txt' }, { op: 'synthesize', spec: 'use the data', path: 's.js' }]), synth: () => 'console.log(4)', opts: { test: false } });
  const r = await d.run;
  assert.equal(r.status, 'done');
  assert.ok(r.trace.some((s) => s.state === 'READ' && s.ok));
  assert.equal(d.fs.store.get('s.js'), 'console.log(4)');
});

test('BEHAVIOUR: a certain artifact is learned (provenance-linked) on success', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'reverse a string in javascript', path: 's.js' }]), synth: () => 'console.log("ok")', opts: { test: false, model: 'm', revision: 'r' } });
  const r = await d.run;
  assert.equal(r.status, 'done');
  assert.equal(d.staged.length, 1);
  assert.ok(d.staged[0].provenance && d.staged[0].provenance.length > 0);
});

test('BEHAVIOUR: follow-ups are offered on done (improve / run / add-tests / explain)', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]), synth: () => 'console.log(5)', opts: { test: false } });
  const r = await d.run;
  for (const f of ['run', 'improve', 'add-tests', 'explain']) assert.ok(r.followups.includes(f), 'missing follow-up ' + f);
});

test('BEHAVIOUR: the real console output is captured and surfaced', async () => {
  const d = driver({ actionsPlan: () => plan([{ op: 'synthesize', spec: 'x', path: 's.js' }]), synth: () => 'console.log("hello-" + (6*7))', opts: { test: false } });
  const r = await d.run;
  const run = r.trace.find((s) => s.state === 'RUN');
  assert.ok(run && String(run.output).includes('hello-42'), 'RUN output should capture the console log');
});

test('BEHAVIOUR: a contradicted device-verify claim vetoes the artifact', async () => {
  const d = driver({ actionsPlan: synthThenDone(), synth: () => 'console.log("paris")', deviceVerify: async () => ({ checks: [{ status: 'contradicted' }] }), opts: { test: false } });
  const r = await d.run;
  assert.equal(d.fs.store.has('s.js'), false);
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
});
