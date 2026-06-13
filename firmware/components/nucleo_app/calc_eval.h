// Pure arithmetic evaluator: + - * / and parentheses, with operator precedence
// and unary minus. No eval(), no heap — fixed stacks sized for the Cardputer.
//
// This is the C twin of web/device/apps/calc-eval.js; keep the two algorithms in
// sync (the JS one is unit-tested in tools/calc-eval.test.mjs).
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Evaluate `expr`. Returns the result and sets *ok = true on success; on malformed
// input, overflow of the internal stacks, or division by zero, sets *ok = false
// and returns 0. `ok` may be NULL.
double calc_eval(const char *expr, bool *ok);

#ifdef __cplusplus
}
#endif
