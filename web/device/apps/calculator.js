// Calculator — device-simulator app. Mirrors the firmware app_calc.cpp: same input
// rules and same on-screen layout, so what you see here is what runs on the Cardputer.
// Drawing goes through the host-provided `g` helper bundle (see device.js), keeping
// this file free of canvas plumbing — each future native app is one small file here.
import { calcEval } from './calc-eval.js';
import { title } from './_list.js';

let expr = '';
let error = false;
let prev = '';            // the expression that produced the shown result (for the small line)

const ACCEPT = '0123456789+-*/().';

export const calculatorApp = {
  id: 'calculator',

  enter() { expr = ''; error = false; prev = ''; return { hint: '0-9 + - * / ( )   enter =   bksp del   esc back' }; },

  // key: the host-normalised key name ('enter'|'backspace'|'char'); ch: the character.
  key(key, ch) {
    if (key === 'enter' || ch === '=') { evaluate(); return; }
    if (key === 'backspace') { expr = expr.slice(0, -1); error = false; prev = ''; return; }
    if (ch === 'c' || ch === 'C') { expr = ''; error = false; prev = ''; return; }
    if (ch && ACCEPT.includes(ch)) {
      if (error) { expr = ''; error = false; }
      else if (prev) { if ('0123456789('.includes(ch)) expr = ''; prev = ''; }   // digit after '=' starts fresh; operator continues
      expr += ch;
    }
  },

  draw(g) {
    const top = g.contentTop, h = g.contentH;
    title(g, 'Calculator', '#7CFC9A');

    // small upper line: the source expression after '=', otherwise a gentle prompt
    const hint = error ? '' : (prev ? prev + ' =' : (expr ? '' : 'type a sum, Enter = result'));
    g.text(hint, g.W - 12, top + 38, g.COL.muted, 9, 'normal', 'right');

    // big lower line: the current input or the result, auto-fit, right-aligned
    const shown = error ? 'Error' : (expr || '0');
    const size = shown.length <= 9 ? 26 : shown.length <= 13 ? 20 : shown.length <= 20 ? 14 : 9;
    g.text(shown, g.W - 12, top + h - 18, error ? '#ff6b6b' : '#ffffff', size, 'bold', 'right');
    return { instruction: 'Calculator', hint: '0-9 + - * / ( )   enter =   bksp del   esc back' };
  },
};

function evaluate() {
  if (!expr) return;
  try { prev = expr; expr = String(calcEval(expr)); error = false; }
  catch { error = true; prev = ''; }
}

// Test/inspection hook (used by the preview harness; not part of the app contract).
export function _debugState() { return { expr, error, prev }; }
