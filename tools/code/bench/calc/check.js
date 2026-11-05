var src = fs.read('calc.js');
if (!src) print('CHECK FAIL: no calc.js'); else {
  (0, eval)(src);
  var ok = 0, bad = [];
  var cases = [['1+2*3', 7], ['(1+2)*3', 9], ['2^3^2', 512], ['2^10', 1024], ['-(2+3)*2', -10], ['2*-3', -6], ['10/4', 2.5],
    ['3.5+1.25', 4.75], ['((7))', 7], [' 1 + 2 ', 3], ['8/2/2', 2], ['1-2-3', -4], ['2*(3+4)^2', 98]];
  for (var i = 0; i < cases.length; i++) { var v; try { v = evaluate(cases[i][0]); } catch (e1) { v = 'throw ' + e1.message; }
    if (Math.abs(v - cases[i][1]) < 1e-9) ok++; else bad.push(cases[i][0] + '=' + v); }
  var errs = ['2+', '(1+2', '1+a', '', '2 3', ')('];
  for (var j = 0; j < errs.length; j++) { var threw = false; try { evaluate(errs[j]); } catch (e2) { threw = true; } if (threw) ok++; else bad.push('no error for [' + errs[j] + ']'); }
  print(bad.length ? 'CHECK FAIL: ' + bad.join(' | ') : 'CHECK OK ' + ok);
}
