// Unit tests for the shared calculator evaluator (web/device/apps/calc-eval.js),
// which is the JS twin of firmware/components/nucleo_app/calc_eval.c.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { calcEval } from '../web/device/apps/calc-eval.js';

test('addition', () => assert.equal(calcEval('7+8'), 15));
test('subtraction', () => assert.equal(calcEval('20-5-3'), 12));
test('precedence: * before +', () => assert.equal(calcEval('2+3*4'), 14));
test('precedence: / before -', () => assert.equal(calcEval('20-8/2'), 16));
test('parentheses override precedence', () => assert.equal(calcEval('(2+3)*4'), 20));
test('nested parentheses', () => assert.equal(calcEval('((1+2)*(3+4))'), 21));
test('decimals', () => assert.equal(calcEval('0.1+0.2'), 0.3));   // rounded to 1e-9
test('division result', () => assert.equal(calcEval('22/7'), 3.142857143));
test('unary minus at start', () => assert.equal(calcEval('-5+2'), -3));
test('unary minus after paren', () => assert.equal(calcEval('3*(-2)'), -6));
test('whitespace tolerated', () => assert.equal(calcEval(' 2  +  2 '), 4));

test('division by zero throws', () => assert.throws(() => calcEval('1/0')));
test('unmatched closing paren throws', () => assert.throws(() => calcEval('1+2)')));
test('unmatched opening paren throws', () => assert.throws(() => calcEval('(1+2')));
test('dangling operator throws', () => assert.throws(() => calcEval('5*')));
test('empty expression throws', () => assert.throws(() => calcEval('')));
test('unknown character throws', () => assert.throws(() => calcEval('2&3')));
