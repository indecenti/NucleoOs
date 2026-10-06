// Every bare function call in a web/shell module must resolve to something: a binding of that module (a
// function / class / const / let / var / parameter / catch binding / import, in any scope), or a global the
// browser (or a service worker) provides. v149 removed micgate.js's activeMicLabel() as a "dead export"
// while four call sites still used it — nothing parses the shell for that, the syntax gate is happy, and
// every microphone busy/error path threw a ReferenceError at run time. This catches the whole class.
//
// Deliberately simple and low-false-positive: scopes are not modelled (a name declared ANYWHERE in the file
// counts), only `foo(...)` / `new Foo(...)` with a plain identifier callee is checked.
//
//   node --test tools/shell-undeclared-calls.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse } from '../apps/code-runner/www/vendor/acorn.mjs';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const SHELL = join(REPO, 'web', 'shell');

// Globals a page or a worker provides that Node's globalThis does not (Node's own are added below).
const BROWSER_GLOBALS = [
  'alert', 'confirm', 'prompt', 'open', 'close', 'print', 'focus', 'blur', 'stop', 'scroll', 'scrollTo', 'scrollBy',
  'matchMedia', 'getComputedStyle', 'getSelection', 'requestAnimationFrame', 'cancelAnimationFrame',
  'requestIdleCallback', 'cancelIdleCallback', 'importScripts', 'postMessage', 'addEventListener',
  'removeEventListener', 'dispatchEvent', 'createImageBitmap', 'reportError',
  'Image', 'Audio', 'Option', 'FileReader', 'XMLHttpRequest', 'WebSocket', 'Worker', 'SharedWorker',
  'AudioContext', 'OfflineAudioContext', 'AudioWorkletNode', 'MediaRecorder', 'MediaStream', 'IntersectionObserver',
  'ResizeObserver', 'MutationObserver', 'PerformanceObserver', 'Notification', 'DOMParser', 'XMLSerializer',
  'KeyboardEvent', 'MouseEvent', 'PointerEvent', 'DragEvent', 'CustomEvent', 'ErrorEvent', 'FocusEvent', 'InputEvent',
  'ClipboardItem', 'DataTransfer', 'OffscreenCanvas', 'ImageData', 'Path2D', 'DOMMatrix', 'DOMRect', 'FontFace',
  'SpeechSynthesisUtterance', 'SpeechRecognition', 'webkitSpeechRecognition', 'VideoDecoder', 'AudioDecoder',
  'EncodedVideoChunk', 'VideoFrame', 'Range', 'TreeWalker', 'NodeFilter', 'Selection', 'StaticRange',
  'Response', 'Request', 'Headers', 'FormData', 'Blob', 'File', 'URL', 'URLSearchParams', 'TextEncoder', 'TextDecoder',
  'ReadableStream', 'WritableStream', 'TransformStream', 'CompressionStream', 'DecompressionStream',
  'AbortController', 'AbortSignal', 'BroadcastChannel', 'MessageChannel', 'EventSource', 'Event', 'EventTarget',
];
const GLOBALS = new Set([...Object.getOwnPropertyNames(globalThis), ...BROWSER_GLOBALS]);

// Parse a module (or, failing that, a classic script: sw.js, the i18n loader) with acorn.
function parseAny(src) {
  const o = { ecmaVersion: 'latest', allowHashBang: true, allowAwaitOutsideFunction: true };
  try { return parse(src, { ...o, sourceType: 'module' }); } catch { return parse(src, { ...o, sourceType: 'script' }); }
}

// Generic AST walk (acorn nodes are plain objects with a string `type`).
function walk(node, visit) {
  if (!node || typeof node.type !== 'string') return;
  visit(node);
  for (const k of Object.keys(node)) {
    if (k === 'type' || k === 'start' || k === 'end' || k === 'loc') continue;
    const v = node[k];
    if (Array.isArray(v)) { for (const c of v) if (c && typeof c.type === 'string') walk(c, visit); }
    else if (v && typeof v.type === 'string') walk(v, visit);
  }
}

// The names a binding pattern introduces: a, {a, b: c, ...d}, [e, f = 1], ...g
function patternNames(p, out) {
  if (!p) return out;
  switch (p.type) {
    case 'Identifier': out.push(p.name); break;
    case 'ObjectPattern': for (const pr of p.properties) patternNames(pr.type === 'RestElement' ? pr.argument : pr.value, out); break;
    case 'ArrayPattern': for (const el of p.elements) patternNames(el, out); break;
    case 'RestElement': patternNames(p.argument, out); break;
    case 'AssignmentPattern': patternNames(p.left, out); break;
    default: break;
  }
  return out;
}

// → [{ name, line }] for every call whose callee identifier resolves to nothing.
export function undeclaredCalls(src) {
  const ast = parseAny(src);
  const declared = new Set();
  const calls = [];
  walk(ast, (n) => {
    switch (n.type) {
      case 'FunctionDeclaration': case 'FunctionExpression': case 'ArrowFunctionExpression':
        if (n.id) declared.add(n.id.name);
        for (const p of n.params) patternNames(p, []).forEach((x) => declared.add(x));
        break;
      case 'ClassDeclaration': case 'ClassExpression': if (n.id) declared.add(n.id.name); break;
      case 'VariableDeclarator': patternNames(n.id, []).forEach((x) => declared.add(x)); break;
      case 'CatchClause': patternNames(n.param, []).forEach((x) => declared.add(x)); break;
      case 'ImportSpecifier': case 'ImportDefaultSpecifier': case 'ImportNamespaceSpecifier': declared.add(n.local.name); break;
      case 'CallExpression': case 'NewExpression':
        if (n.callee.type === 'Identifier') calls.push({ name: n.callee.name, at: n.start });
        break;
      default: break;
    }
  });
  const lineOf = (pos) => src.slice(0, pos).split('\n').length;
  return calls.filter((c) => !declared.has(c.name) && !GLOBALS.has(c.name)).map((c) => ({ name: c.name, line: lineOf(c.at) }));
}

const shellModules = readdirSync(SHELL).filter((f) => /\.m?js$/.test(f)).sort();

test('the checker parses every web/shell module', () => {
  assert.ok(shellModules.length >= 20, 'found only ' + shellModules.length + ' shell modules');
  for (const f of shellModules) assert.doesNotThrow(() => undeclaredCalls(readFileSync(join(SHELL, f), 'utf8')), f);
});

for (const f of shellModules) {
  test(`web/shell/${f}: every bare call resolves to a declaration, an import or a global`, () => {
    const bad = undeclaredCalls(readFileSync(join(SHELL, f), 'utf8'));
    assert.deepEqual(bad, [], `${f} calls undeclared function(s): ` + bad.map((b) => `${b.name}() at line ${b.line}`).join(', '));
  });
}

// The checker must have caught the v149 regression: micgate.js with activeMicLabel's definition removed.
test('the checker flags a removed helper that is still called (micgate.js activeMicLabel, v149)', () => {
  const src = readFileSync(join(SHELL, 'micgate.js'), 'utf8');
  const broken = src.replace(/^.*function activeMicLabel\(\)[^\n]*\n/m, '');
  assert.notEqual(broken, src, 'fixture: activeMicLabel() is no longer defined the way this test expects');
  const names = [...new Set(undeclaredCalls(broken).map((b) => b.name))];
  assert.deepEqual(names, ['activeMicLabel']);
  // ...and is quiet about globals, imports, parameters, destructured bindings and nested declarations.
  assert.deepEqual(undeclaredCalls(`import { a } from './x.js'; import * as N from './y.js';
    const { b, c: [d] } = N; function f(g, { h } = {}, ...i) { function j() {} try { j(); } catch (k) { k(); } g(); h(); i(); }
    a(); b(); d(); f(); setTimeout(() => {}); new URL('x'); fetch('/'); new Map(); requestAnimationFrame(f); new AbortController();`), []);
});
