var src = fs.read('lisp.js'), bad = [], ok = 0;
if (!src) print('CHECK FAIL: no lisp.js'); else {
  (0, eval)(src);
  var cases = [
    ['(+ 1 2 3)', 6], ['(* 2 (- 10 4))', 12], ['(/ 9 2)', 4.5],
    ['(define (fact n) (if (< n 2) 1 (* n (fact (- n 1))))) (fact 10)', 3628800],
    ['(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) (fib 15)', 610],
    ['(define (adder x) (lambda (y) (+ x y))) ((adder 3) 4)', 7],
    ['(length (list 1 2 3 4))', 4], ['(car (cdr (list 7 8 9)))', 8], ['(if (null? (quote ())) 1 2)', 1],
    ["(car '(5 6))", 5], ['(begin (define a 2) (define b 3) (* a b)) ; comment', 6], ['(if (not (= 1 2)) 10 20)', 10]];
  for (var i = 0; i < cases.length; i++) { var v; try { v = lisp(cases[i][0]); } catch (e1) { v = 'throw ' + e1.message; }
    if (v === cases[i][1]) ok++; else bad.push(cases[i][0].slice(0, 30) + ' => ' + v); }
  var errs = ['(undefined-thing 1)', '(+ 1 2', '((lambda (x) x))'];
  for (var j = 0; j < errs.length; j++) { var threw = false; try { lisp(errs[j]); } catch (e2) { threw = true; } if (threw) ok++; else bad.push('no error: ' + errs[j]); }
  print(bad.length ? 'CHECK FAIL: ' + bad.join(' | ') : 'CHECK OK ' + ok);
}
