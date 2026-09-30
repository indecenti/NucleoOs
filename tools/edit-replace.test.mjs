// apps/anima/www/edit-replace.js — the agent's tolerant edit_file (ported from OpenCode, MIT).
// Local models send an "old" text that differs from the file in indentation / spacing / escaping; an
// exact-only match made those edits fail and the agent loop. The chain must land them — once, never a guess.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tolerantReplace } from '../apps/anima/www/edit-replace.js';

const FILE = [
  'function add(a, b) {',
  '    return a + b;',
  '}',
  '',
  'function main() {',
  '    const x = add(1, 2);',
  '    console.log("result:", x);',
  '}',
].join('\n');

test('exact match still works, and is the first strategy', () => {
  const r = tolerantReplace(FILE, 'return a + b;', 'return a + b + 0;');
  assert.equal(r.ok, true); assert.equal(r.strategy, 'simple');
  assert.match(r.text, /return a \+ b \+ 0;/);
});

test('wrong indentation (a 9B model re-indents what it copies) still lands on the right lines', () => {
  const r = tolerantReplace(FILE, 'function main() {\n  const x = add(1, 2);\n  console.log("result:", x);\n}', 'function main() {\n    console.log(add(1, 2));\n}');
  assert.equal(r.ok, true, r.message);
  assert.equal(r.text.split('\n').length, 7);
  assert.match(r.text, /console\.log\(add\(1, 2\)\)/);
  assert.match(r.text, /^function add/, 'the rest of the file untouched');
});

test('trailing spaces and collapsed whitespace', () => {
  assert.equal(tolerantReplace(FILE, 'const x = add(1, 2);   ', 'const x = 3;').ok, true);
  const r = tolerantReplace(FILE, 'console.log("result:",   x);', 'console.log(x);');
  assert.equal(r.ok, true, r.message); assert.match(r.text, /console\.log\(x\);/);
});

test('escaped text from a JSON tool call (\\" and \\n) is unescaped', () => {
  const r = tolerantReplace(FILE, 'console.log(\\"result:\\", x);', 'console.log(x);');
  assert.equal(r.ok, true, r.message);
});

test('a slightly different middle line still matches between exact anchors (block anchor)', () => {
  const r = tolerantReplace(FILE, 'function main() {\n    const x = add(1,2);\n    console.log("result:", x);\n}', 'function main() {}');
  assert.equal(r.ok, true, r.message);
  assert.match(r.text, /function main\(\) \{\}$/);
});

test('never guesses between two places', () => {
  const two = 'x = 1;\ny = 2;\nx = 1;\n';
  const r = tolerantReplace(two, 'x = 1;', 'x = 9;');
  assert.equal(r.ok, false); assert.equal(r.error, 'not-unique');
  const all = tolerantReplace(two, 'x = 1;', 'x = 9;', { all: true });
  assert.equal(all.ok, true); assert.equal(all.count, 2); assert.equal(all.text, 'x = 9;\ny = 2;\nx = 9;\n');
});

test('honest failures the model can act on', () => {
  assert.equal(tolerantReplace(FILE, 'nothing like this', 'y').error, 'not-found');
  assert.match(tolerantReplace(FILE, 'nothing like this', 'y').message, /Re-read it/);
  assert.equal(tolerantReplace(FILE, 'same', 'same').error, 'identical');
  assert.equal(tolerantReplace(FILE, '', 'y').error, 'empty-old');
});

test('CRLF files keep their line endings', () => {
  const crlf = 'a\r\nb\r\nc\r\n';
  const r = tolerantReplace(crlf, 'b\n', 'B\n');
  assert.equal(r.ok, true); assert.equal(r.text, 'a\r\nB\r\nc\r\n');
});
