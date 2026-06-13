// Shunting-yard evaluator (see calc_eval.h). Bounded, allocation-free.
#include "calc_eval.h"
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#define CE_MAX 64   // max numbers / operators on the stacks; plenty for a 64-char expr

static int ce_prec(char op) { return (op == '+' || op == '-') ? 1 : (op == '*' || op == '/') ? 2 : 0; }

double calc_eval(const char *s, bool *ok)
{
    double num[CE_MAX]; int ns = 0;     // operand stack
    char   op[CE_MAX];  int os = 0;     // operator stack
    bool expect_operand = true;         // true at start, after '(' or after an operator
    bool fail = false;

    // Pop one operator and apply it to the top two operands.
    #define APPLY() do { \
        if (ns < 2 || os < 1) { fail = true; break; } \
        double b = num[--ns], a = num[--ns]; char o = op[--os]; double r; \
        if (o == '+') r = a + b; else if (o == '-') r = a - b; else if (o == '*') r = a * b; \
        else { if (b == 0) { fail = true; break; } r = a / b; } \
        num[ns++] = r; \
    } while (0)

    for (const char *p = s; *p && !fail; ) {
        char c = *p;
        if (c == ' ' || c == '\t') { p++; continue; }

        if (isdigit((unsigned char)c) || c == '.') {
            char *end; double v = strtod(p, &end);
            if (end == p || ns >= CE_MAX) { fail = true; break; }
            num[ns++] = v; p = end; expect_operand = false; continue;
        }
        if (c == '(') {
            if (os >= CE_MAX) { fail = true; break; }
            op[os++] = '('; expect_operand = true; p++; continue;
        }
        if (c == ')') {
            while (os && op[os - 1] != '(' && !fail) APPLY();
            if (fail) break;
            if (os == 0) { fail = true; break; }   // unmatched ')'
            os--;                                  // discard '('
            expect_operand = false; p++; continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/') {
            if (expect_operand) {
                // Unary context: "-x" => (0 - x); unary '+' is a no-op; '*' '/' are invalid here.
                if (c == '-') {
                    if (ns >= CE_MAX || os >= CE_MAX) { fail = true; break; }
                    num[ns++] = 0; op[os++] = '-';
                } else if (c == '*' || c == '/') { fail = true; break; }
                p++; continue;   // still expecting an operand
            }
            while (os && op[os - 1] != '(' && ce_prec(op[os - 1]) >= ce_prec(c) && !fail) APPLY();
            if (fail || os >= CE_MAX) { fail = true; break; }
            op[os++] = c; expect_operand = true; p++; continue;
        }
        fail = true; break;   // unknown character
    }

    while (!fail && os) { if (op[os - 1] == '(') { fail = true; break; } APPLY(); }

    if (fail || ns != 1) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return round(num[0] * 1e9) / 1e9;   // tame floating-point noise, like the web app
    #undef APPLY
}
