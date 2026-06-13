// Pure arithmetic evaluator: + - * / and parentheses, precedence, unary minus.
// No eval(). This is the JS twin of firmware/components/nucleo_app/calc_eval.c —
// keep the two algorithms in sync. Unit-tested in tools/calc-eval.test.mjs.
//
// Throws Error('calc') on malformed input or division by zero.
export function calcEval(s) {
  const num = [], op = [];
  let expect = true;          // true at start, after '(' or after an operator
  const prec = o => (o === '+' || o === '-') ? 1 : (o === '*' || o === '/') ? 2 : 0;
  const apply = () => {
    if (num.length < 2 || op.length < 1) throw new Error('calc');
    const b = num.pop(), a = num.pop(), o = op.pop();
    let r;
    if (o === '+') r = a + b; else if (o === '-') r = a - b; else if (o === '*') r = a * b;
    else { if (b === 0) throw new Error('calc'); r = a / b; }
    num.push(r);
  };

  for (let i = 0; i < s.length; ) {
    const c = s[i];
    if (c === ' ' || c === '\t') { i++; continue; }

    const m = /^(\d+\.?\d*|\.\d+)/.exec(s.slice(i));
    if (m) { num.push(parseFloat(m[0])); i += m[0].length; expect = false; continue; }

    if (c === '(') { op.push('('); expect = true; i++; continue; }
    if (c === ')') {
      while (op.length && op[op.length - 1] !== '(') apply();
      if (!op.length) throw new Error('calc');   // unmatched ')'
      op.pop(); expect = false; i++; continue;
    }
    if (c === '+' || c === '-' || c === '*' || c === '/') {
      if (expect) {
        if (c === '-') { num.push(0); op.push('-'); }       // unary minus => (0 - x)
        else if (c === '*' || c === '/') throw new Error('calc');
        i++; continue;                                       // unary '+' is a no-op
      }
      while (op.length && op[op.length - 1] !== '(' && prec(op[op.length - 1]) >= prec(c)) apply();
      op.push(c); expect = true; i++; continue;
    }
    throw new Error('calc');   // unknown character
  }

  while (op.length) { if (op[op.length - 1] === '(') throw new Error('calc'); apply(); }
  if (num.length !== 1) throw new Error('calc');
  return Math.round(num[0] * 1e9) / 1e9;
}
