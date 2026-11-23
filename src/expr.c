/*
 * expr.c - SET /A arithmetic and a small regex engine for FINDSTR
 */
#include "cmd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ---------------------------------------------------------------- */
/* SET /A                                                            */
static const char *ep;
static int eerr;

#define E_MISSING 1
#define E_DIV0    2
#define E_SYNTAX  3

static void skip(void) { while (*ep == ' ' || *ep == '\t') ep++; }

static bool is_idc(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '$' || c == '#' || c == '@' || c == '.';
}

static long var_val(const char *name)
{
    const char *v = env_get(name);
    return v ? strtol(v, NULL, 0) : 0;
}

static void var_store(const char *name, long v)
{
    char b[16];
    sprintf(b, "%ld", v);
    env_set(name, b);
}

static long e_assign(void);

static long e_primary(void)
{
    long v = 0;
    skip();
    if (eerr) return 0;
    if (*ep == '(') {
        ep++;
        v = e_assign();
        while (!eerr && (skip(), *ep == ',')) { ep++; v = e_assign(); }
        skip();
        if (*ep != ')') { eerr = E_SYNTAX; return 0; }
        ep++;
        return v;
    }
    if (*ep == '!') { ep++; return !e_primary(); }
    if (*ep == '~') { ep++; return ~e_primary(); }
    if (*ep == '-') { ep++; return -e_primary(); }
    if (*ep == '+') { ep++; return e_primary(); }
    if (isdigit((unsigned char)*ep)) {
        char *end;
        v = strtol(ep, &end, 0);
        if (is_idc(*end) && *end != '.') { eerr = E_SYNTAX; return 0; }
        ep = end;
        return v;
    }
    if (is_idc(*ep)) {
        char name[40];
        int n = 0;
        while (is_idc(*ep) && n < 39) name[n++] = *ep++;
        name[n] = 0;
        return var_val(name);
    }
    eerr = E_MISSING;
    return 0;
}

static long e_mul(void)
{
    long v = e_primary();
    for (;;) {
        char op;
        long r;
        skip();
        op = *ep;
        if ((op != '*' && op != '/' && op != '%') || ep[1] == '=') return v;
        ep++;
        r = e_primary();
        if (eerr) return 0;
        if (op == '*') v *= r;
        else if (!r) { eerr = E_DIV0; return 0; }
        else if (op == '/') v /= r;
        else v %= r;
    }
}

static long e_add(void)
{
    long v = e_mul();
    for (;;) {
        char op;
        skip();
        op = *ep;
        if ((op != '+' && op != '-') || ep[1] == '=') return v;
        ep++;
        if (op == '+') v += e_mul(); else v -= e_mul();
    }
}

static long e_shift(void)
{
    long v = e_add();
    for (;;) {
        skip();
        if (ep[0] == '<' && ep[1] == '<' && ep[2] != '=') { ep += 2; v <<= e_add(); }
        else if (ep[0] == '>' && ep[1] == '>' && ep[2] != '=') { ep += 2; v >>= e_add(); }
        else return v;
    }
}

static long e_and(void)
{
    long v = e_shift();
    while (skip(), *ep == '&' && ep[1] != '=') { ep++; v &= e_shift(); }
    return v;
}

static long e_xor(void)
{
    long v = e_and();
    while (skip(), *ep == '^' && ep[1] != '=') { ep++; v ^= e_and(); }
    return v;
}

static long e_or(void)
{
    long v = e_xor();
    while (skip(), *ep == '|' && ep[1] != '=') { ep++; v |= e_xor(); }
    return v;
}

static long e_assign(void)
{
    const char *save;
    char name[40];
    int n = 0;
    skip();
    save = ep;
    if (is_idc(*ep) && !isdigit((unsigned char)*ep)) {
        while (is_idc(*ep) && n < 39) name[n++] = *ep++;
        name[n] = 0;
        skip();
        {
            char op = 0;
            if (ep[0] == '=' && ep[1] != '=') { op = '='; ep++; }
            else if (strchr("+-*/%&|^", ep[0]) && ep[0] && ep[1] == '=') { op = ep[0]; ep += 2; }
            else if ((ep[0] == '<' || ep[0] == '>') && ep[1] == ep[0] && ep[2] == '=') { op = ep[0]; ep += 3; }
            if (op) {
                long r = e_assign(), v = var_val(name);
                if (eerr) return 0;
                switch (op) {
                case '=': v = r; break;
                case '+': v += r; break;
                case '-': v -= r; break;
                case '*': v *= r; break;
                case '/': if (!r) { eerr = E_DIV0; return 0; } v /= r; break;
                case '%': if (!r) { eerr = E_DIV0; return 0; } v %= r; break;
                case '&': v &= r; break;
                case '|': v |= r; break;
                case '^': v ^= r; break;
                case '<': v <<= r; break;
                case '>': v >>= r; break;
                }
                var_store(name, v);
                return v;
            }
        }
    }
    ep = save;
    return e_or();
}

int eval_set_a(const char *expr, long *result)
{
    long v = 0;
    ep = expr;
    eerr = 0;
    skip();
    if (!*ep) return E_MISSING;
    for (;;) {
        v = e_assign();
        if (eerr) return eerr;
        skip();
        if (*ep == ',') { ep++; continue; }
        break;
    }
    if (*ep) return E_SYNTAX;
    *result = v;
    return 0;
}

/* ---------------------------------------------------------------- */
/* regex: . * + ? ^ $ [set] [^set] [a-z] \escape                      */
static bool re_ic;

static bool ceq(char a, char b)
{
    if (re_ic) return tolower((unsigned char)a) == tolower((unsigned char)b);
    return a == b;
}

static int elem_len(const char *re)
{
    if (re[0] == '\\' && re[1]) return 2;
    if (re[0] == '[') {
        const char *p = re + 1;
        if (*p == '^') p++;
        if (*p == ']') p++;
        while (*p && *p != ']') p++;
        return *p ? (int)(p - re + 1) : (int)(p - re);
    }
    return 1;
}

static bool elem_match(const char *re, int len, char c)
{
    if (!c) return false;
    if (re[0] == '.') return true;
    if (re[0] == '\\') return ceq(re[1], c);
    if (re[0] == '[') {
        bool neg = re[1] == '^', m = false;
        const char *p = re + 1 + neg, *end = re + len - 1;
        while (p < end) {
            if (p + 2 < end && p[1] == '-') {
                char lo = p[0], hi = p[2], cc = c;
                if (cc >= lo && cc <= hi) m = true;
                if (re_ic) {
                    cc = (char)tolower((unsigned char)c);
                    if (cc >= tolower((unsigned char)lo) && cc <= tolower((unsigned char)hi)) m = true;
                }
                p += 3;
            } else {
                if (ceq(*p, c)) m = true;
                p++;
            }
        }
        return m != neg;
    }
    return ceq(re[0], c);
}

static bool match_here(const char *re, const char *s)
{
    int l;
    char q;
    if (!*re) return true;
    if (re[0] == '$' && !re[1]) return !*s;
    l = elem_len(re);
    q = re[l];
    if (q == '*' || q == '+' || q == '?') {
        const char *rest = re + l + 1;
        int n = 0, min = q == '+';
        if (q == '?') {
            if (elem_match(re, l, *s) && match_here(rest, s + 1)) return true;
            return match_here(rest, s);
        }
        while (elem_match(re, l, s[n])) n++;
        for (int k = n; k >= min; k--)
            if (match_here(rest, s + k)) return true;
        return false;
    }
    if (elem_match(re, l, *s)) return match_here(re + l, s + 1);
    return false;
}

bool re_search(const char *re, const char *text, bool icase)
{
    re_ic = icase;
    if (*re == '^') return match_here(re + 1, text);
    do {
        if (match_here(re, text)) return true;
    } while (*text++);
    return false;
}
