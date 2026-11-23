/*
 * shell.c - command line parsing, variables, redirection, pipes,
 *           batch files and flow control (IF / FOR / GOTO / CALL ...)
 *
 * NOTE: the eZ80 C stack is only ~4KB, so anything large is malloc'd.
 */
#include "cmd.h"
#include <fileioc.h>
#include <ti/vars.h>
#include <sys/rtc.h>
#include <sys/util.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define BUFSZ (LINE_MAX * 2)

int  g_errorlevel;
bool g_echo = true;
bool g_quit;
bool g_in_batch;
bool g_delayed;
sbuf_t *g_stdin;
size_t  g_stdin_pos;
static bool g_goto;        /* a GOTO happened: abandon the rest of the line */
static bool g_err_nul;     /* 2>nul */
static bool g_err_to_out;  /* 2>&1 */
static bool g_batch_abort; /* stop all running batch files */

/* ---------------------------------------------------------------- */
/* small string helpers                                              */
int stricmp_(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = toupper((unsigned char)*a) - toupper((unsigned char)*b);
        if (d || !*a) return d;
    }
}

int strnicmp_(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = toupper((unsigned char)*a) - toupper((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}

const char *stristr_(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strnicmp_(h, n, l)) return h;
    return l ? NULL : h;
}

void str_upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

bool is_switch(const char *a, const char *sw)
{
    return !stricmp_(a, sw);
}

static char *xstrdup(const char *s)
{
    char *d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

void err_out(const char *s)
{
    if (g_err_nul) return;
    if (g_err_to_out) { out_puts(s); return; }
    while (*s) con_putc(*s++);
}

bool ask_yn(const char *q)
{
    char b[8];
    for (;;) {
        out_puts(q);
        out_puts(" (Y/N)? ");
        b[0] = 0;
        if (read_line(b, sizeof b, false)) return false;
        if (toupper((unsigned char)b[0]) == 'Y') return true;
        if (toupper((unsigned char)b[0]) == 'N') return false;
    }
}

/* split into argv, honoring quotes (quotes are removed) */
int split_args(char *s, char **argv, int max)
{
    int argc = 0;
    while (*s && argc < max) {
        char *d;
        bool q = false;
        while (*s == ' ' || *s == '\t' || *s == ',' || *s == ';' || *s == '=') s++;
        if (!*s) break;
        argv[argc++] = d = s;
        while (*s && (q || (*s != ' ' && *s != '\t'))) {
            if (*s == '"') { q = !q; s++; continue; }
            *d++ = *s++;
        }
        if (*s) s++;
        *d = 0;
    }
    return argc;
}

bool read_stdin_line(char *buf, int max)
{
    int n = 0;
    if (!g_stdin || !g_stdin->buf || g_stdin_pos >= g_stdin->len) return false;
    while (g_stdin_pos < g_stdin->len) {
        char c = g_stdin->buf[g_stdin_pos++];
        if (c == '\n') break;
        if (c != '\r' && n < max - 1) buf[n++] = c;
    }
    buf[n] = 0;
    return true;
}

/* ---------------------------------------------------------------- */
/* environment variables                                             */
#define ENV_MAX 48
typedef struct { char *name, *val; } env_t;
static env_t env[ENV_MAX];
static int env_n;

#define LOCAL_MAX 4
static struct { env_t *vars; int n; bool delayed; } local_stack[LOCAL_MAX];
static int local_depth;

static int env_find(const char *name)
{
    for (int i = 0; i < env_n; i++) if (!stricmp_(env[i].name, name)) return i;
    return -1;
}

bool env_set(const char *name, const char *val)
{
    int i = env_find(name);
    if (!val || !*val) {
        if (i >= 0) {
            free(env[i].name);
            free(env[i].val);
            memmove(&env[i], &env[i + 1], (env_n - i - 1) * sizeof(env_t));
            env_n--;
        }
        return true;
    }
    if (i >= 0) {
        char *v = xstrdup(val);
        if (!v) return false;
        free(env[i].val);
        env[i].val = v;
        return true;
    }
    if (env_n >= ENV_MAX) return false;
    {
        char *n = xstrdup(name), *v = xstrdup(val);
        int j;
        if (!n || !v) { free(n); free(v); return false; }
        for (j = env_n; j > 0 && stricmp_(env[j - 1].name, name) > 0; j--) env[j] = env[j - 1];
        env[j].name = n;
        env[j].val = v;
        env_n++;
    }
    return true;
}

const char *env_get(const char *name)
{
    static char dyn[48];
    int i = env_find(name);
    if (i >= 0) return env[i].val;
    if (!stricmp_(name, "CD")) {
        static char cd[LINE_MAX];
        fs_path(fs_cwd, cd);
        return cd;
    }
    if (!stricmp_(name, "DATE")) {
        uint8_t d, m; uint16_t y;
        static const char *dn[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int a, yy, mm, w;
        boot_GetDate(&d, &m, &y);
        a = (14 - m) / 12; yy = y - a; mm = m + 12 * a - 2;
        w = (d + yy + yy / 4 - yy / 100 + yy / 400 + (31 * mm) / 12) % 7;
        sprintf(dyn, "%s %02u/%02u/%04u", dn[w], m, d, y);
        return dyn;
    }
    if (!stricmp_(name, "TIME")) {
        uint8_t s, m, h;
        boot_GetTime(&s, &m, &h);
        sprintf(dyn, "%2u:%02u:%02u.00", h, m, s);
        return dyn;
    }
    if (!stricmp_(name, "RANDOM")) {
        sprintf(dyn, "%u", (unsigned)(random() & 0x7FFF));
        return dyn;
    }
    if (!stricmp_(name, "ERRORLEVEL")) {
        sprintf(dyn, "%d", g_errorlevel);
        return dyn;
    }
    if (!stricmp_(name, "CMDEXTVERSION")) return "2";
    return NULL;
}

void env_list(const char *prefix)
{
    size_t l = prefix ? strlen(prefix) : 0;
    pager_reset(true);
    for (int i = 0; i < env_n; i++) {
        if (l && strnicmp_(env[i].name, prefix, l)) continue;
        out_printf("%s=%s\n", env[i].name, env[i].val);
        if (!pager_line()) break;
    }
}

void setlocal_push(bool delayed)
{
    env_t *copy;
    if (local_depth >= LOCAL_MAX) { err_out("Maximum setlocal recursion level reached.\n"); return; }
    copy = malloc(sizeof(env_t) * (env_n ? env_n : 1));
    if (!copy) return;
    for (int i = 0; i < env_n; i++) {
        copy[i].name = xstrdup(env[i].name);
        copy[i].val = xstrdup(env[i].val);
    }
    local_stack[local_depth].vars = copy;
    local_stack[local_depth].n = env_n;
    local_stack[local_depth].delayed = g_delayed;
    local_depth++;
    if (delayed) g_delayed = true;
}

void setlocal_pop(void)
{
    if (!local_depth) return;
    local_depth--;
    for (int i = 0; i < env_n; i++) { free(env[i].name); free(env[i].val); }
    env_n = local_stack[local_depth].n;
    memcpy(env, local_stack[local_depth].vars, sizeof(env_t) * env_n);
    free(local_stack[local_depth].vars);
    g_delayed = local_stack[local_depth].delayed;
}

/* ---------------------------------------------------------------- */
/* DOSKEY macros                                                     */
#define MAC_MAX 12
static env_t macros[MAC_MAX];
static int mac_n;

const char *macro_get(const char *name)
{
    for (int i = 0; i < mac_n; i++) if (!stricmp_(macros[i].name, name)) return macros[i].val;
    return NULL;
}

bool macro_set(const char *name, const char *val)
{
    int i;
    for (i = 0; i < mac_n; i++) if (!stricmp_(macros[i].name, name)) break;
    if (i < mac_n) {
        free(macros[i].val);
        if (!val || !*val) {
            free(macros[i].name);
            macros[i] = macros[--mac_n];
            return true;
        }
        macros[i].val = xstrdup(val);
        return true;
    }
    if (!val || !*val) return true;
    if (mac_n >= MAC_MAX) return false;
    macros[mac_n].name = xstrdup(name);
    macros[mac_n].val = xstrdup(val);
    mac_n++;
    return true;
}

void macro_list(void)
{
    for (int i = 0; i < mac_n; i++) out_printf("%s=%s\n", macros[i].name, macros[i].val);
}

/* ---------------------------------------------------------------- */
/* batch file context                                                */
typedef struct bctx {
    char *text;
    char **lines;
    int nlines, pc;
    char *args[10];
    int nargs;
    int local_base;
    bool owner, done;
    int code;
    struct bctx *parent;
} bctx_t;

static bctx_t *g_bc;
static int batch_depth;

/* append helper with bounds */
typedef struct { char *p; int n, max; } ob_t;
static void ob_c(ob_t *o, char c) { if (o->n < o->max - 1) o->p[o->n++] = c; }
static void ob_s(ob_t *o, const char *s) { while (*s) ob_c(o, *s++); }

static void path_parts(const char *arg, const char *mods, ob_t *o)
{
    char *full, *name, *dot;
    int n;
    full = malloc(LINE_MAX);
    if (!full) return;
    n = fs_resolve(arg);
    if (n >= 0) fs_path((uint8_t)n, full);
    else {
        fs_path(fs_cwd, full);
        if (full[strlen(full) - 1] != '\\') strcat(full, "\\");
        strncat(full, arg, LINE_MAX - strlen(full) - 1);
    }
    name = strrchr(full, '\\');
    name = name ? name + 1 : full;
    dot = strrchr(name, '.');
    if (strchr(mods, 'f')) ob_s(o, full);
    else {
        if (strchr(mods, 'd')) ob_s(o, "C:");
        if (strchr(mods, 'p'))
            for (char *c = full + 2; c < name; c++) ob_c(o, *c);
        if (strchr(mods, 'n'))
            for (char *c = name; *c && c != dot; c++) ob_c(o, *c);
        if (strchr(mods, 'x') && dot) ob_s(o, dot);
    }
    free(full);
}

static void strip_quotes_to(const char *s, ob_t *o)
{
    for (; *s; s++) if (*s != '"') ob_c(o, *s);
}

/* look up "NAME" or "NAME:~a,b" or "NAME:x=y" */
static bool var_lookup(const char *spec, ob_t *o)
{
    char name[40];
    const char *colon = strchr(spec, ':');
    const char *v;
    size_t nl = colon ? (size_t)(colon - spec) : strlen(spec);
    if (!nl || nl >= sizeof name) return false;
    memcpy(name, spec, nl);
    name[nl] = 0;
    if (strchr(name, ' ') && !env_get(name)) return false;
    if (!(v = env_get(name))) return false;
    if (!colon) { ob_s(o, v); return true; }
    colon++;
    if (*colon == '~') {
        int len = (int)strlen(v), st = atoi(colon + 1), cnt = len;
        const char *comma = strchr(colon, ',');
        if (st < 0) st += len;
        if (st < 0) st = 0;
        if (st > len) st = len;
        if (comma) {
            cnt = atoi(comma + 1);
            if (cnt < 0) cnt = len - st + cnt;
        }
        if (cnt > len - st) cnt = len - st;
        for (int i = 0; i < cnt; i++) ob_c(o, v[st + i]);
        return true;
    } else {
        const char *eq = strchr(colon, '=');
        char from[40];
        size_t fl;
        bool star = false;
        if (!eq) { ob_s(o, v); return true; }
        if (*colon == '*') { star = true; colon++; }
        fl = (size_t)(eq - colon);
        if (fl >= sizeof from) fl = sizeof from - 1;
        memcpy(from, colon, fl);
        from[fl] = 0;
        if (!fl) { ob_s(o, v); return true; }
        if (star) {
            const char *f = stristr_(v, from);
            if (f) { ob_s(o, eq + 1); ob_s(o, f + fl); }
            else ob_s(o, v);
            return true;
        }
        while (*v) {
            if (!strnicmp_(v, from, fl)) { ob_s(o, eq + 1); v += fl; }
            else ob_c(o, *v++);
        }
        return true;
    }
}

/* %VAR% / %1 / %~dp0 expansion */
static void expand_vars(const char *in, char *out, int max, bool batch)
{
    ob_t o = {out, 0, max};
    while (*in) {
        if (*in == '%') {
            const char *j;
            if (batch && g_bc) {
                if (in[1] == '%') { ob_c(&o, '%'); in += 2; continue; }
                if (isdigit((unsigned char)in[1])) {
                    int k = in[1] - '0';
                    if (k < g_bc->nargs && g_bc->args[k]) ob_s(&o, g_bc->args[k]);
                    in += 2;
                    continue;
                }
                if (in[1] == '*') {
                    for (int k = 1; k < g_bc->nargs; k++) {
                        if (k > 1) ob_c(&o, ' ');
                        ob_s(&o, g_bc->args[k]);
                    }
                    in += 2;
                    continue;
                }
                if (in[1] == '~') {
                    const char *m = in + 2;
                    char mods[8];
                    int ml = 0;
                    while (*m && strchr("fdpnx", tolower((unsigned char)*m)) && ml < 7)
                        mods[ml++] = (char)tolower((unsigned char)*m++);
                    mods[ml] = 0;
                    if (isdigit((unsigned char)*m)) {
                        int k = *m - '0';
                        const char *a = (k < g_bc->nargs && g_bc->args[k]) ? g_bc->args[k] : "";
                        if (!ml) strip_quotes_to(a, &o);
                        else {
                            char *unq = malloc(LINE_MAX);
                            if (unq) {
                                ob_t u = {unq, 0, LINE_MAX};
                                strip_quotes_to(a, &u);
                                unq[u.n] = 0;
                                path_parts(unq, mods, &o);
                                free(unq);
                            }
                        }
                        in = m + 1;
                        continue;
                    }
                }
            }
            j = strchr(in + 1, '%');
            if (j && j > in + 1) {
                char spec[64];
                size_t l = (size_t)(j - in - 1);
                if (l < sizeof spec) {
                    memcpy(spec, in + 1, l);
                    spec[l] = 0;
                    if (var_lookup(spec, &o)) { in = j + 1; continue; }
                    if (batch && g_bc && !strchr(spec, ' ')) { in = j + 1; continue; }
                }
            }
            if (batch && g_bc && !j) { in++; continue; }
            ob_c(&o, *in++);
            continue;
        }
        ob_c(&o, *in++);
    }
    out[o.n] = 0;
}

/* !VAR! delayed expansion */
static void expand_delayed(const char *in, char *out, int max)
{
    ob_t o = {out, 0, max};
    while (*in) {
        if (*in == '!') {
            const char *j = strchr(in + 1, '!');
            if (j && j > in + 1 && j - in < 60) {
                char spec[64];
                memcpy(spec, in + 1, j - in - 1);
                spec[j - in - 1] = 0;
                var_lookup(spec, &o);
                in = j + 1;
                continue;
            }
            in++;
            continue;
        }
        ob_c(&o, *in++);
    }
    out[o.n] = 0;
}

/* ---------------------------------------------------------------- */
/* parsing helpers                                                   */
static char *match_paren(char *p)
{
    int d = 0;
    bool q = false;
    for (; *p; p++) {
        if (*p == '^' && p[1]) { p++; continue; }
        if (*p == '"') q = !q;
        else if (!q && *p == '(') d++;
        else if (!q && *p == ')') { if (--d == 0) return p; }
    }
    return NULL;
}

static bool starts_kw(const char *s, const char *kw)
{
    size_t l = strlen(kw);
    return !strnicmp_(s, kw, l) && (!s[l] || s[l] == ' ' || s[l] == '\t' || s[l] == '/' || s[l] == '(');
}

static void remove_carets(char *s)
{
    char *d = s;
    bool q = false;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        if (*s == '^' && !q && s[1]) { s++; }
        *d++ = *s;
    }
    *d = 0;
}

/* read one token (keeping quotes); returns pointer after token */
static char *read_tok(char *p, char *tok, int max, bool stop_eq)
{
    int n = 0;
    bool q = false;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) {
        if (*p == '"') q = !q;
        else if (!q && (*p == ' ' || *p == '\t')) break;
        else if (!q && stop_eq && p[0] == '=' && p[1] == '=') break;
        if (n < max - 1) tok[n++] = *p;
        p++;
    }
    tok[n] = 0;
    return p;
}

static int exec_seq(char *s);

/* The eZ80 stack is only 4KB (0xD1987E..0xD1A87E). Refuse to nest deeper
   when it is nearly used up instead of crashing the calculator. */
static bool stack_low(void)
{
#ifdef HOST
    static int depth_probe;
    (void)depth_probe;
    return false;
#else
    volatile char probe;
    return (uintptr_t)&probe < 0xD1987E + 1100;
#endif
}

static bool stack_check(void)
{
    if (!stack_low()) return true;
    err_out("Commands are nested too deeply.\n");
    g_batch_abort = true;
    return false;
}

int exec_line(const char *line, bool do_expand)
{
    char *buf;
    int r;
    if (!stack_check()) return 1;
    buf = malloc(BUFSZ);
    if (!buf) { err_out("Not enough memory.\n"); return 1; }
    if (do_expand) expand_vars(line, buf, BUFSZ, false);
    else { strncpy(buf, line, BUFSZ - 1); buf[BUFSZ - 1] = 0; }
    r = exec_seq(buf);
    free(buf);
    return r;
}

/* ---------------------------------------------------------------- */
/* running a single command                                          */
static const char *path_exts[] = {"", ".bat", ".cmd"};

static int find_program(const char *word)
{
    char *cand = malloc(LINE_MAX);
    const char *path;
    int n = -1;
    if (!cand) return -1;
    for (int pass = 0; pass < 2 && n < 0; pass++) {
        for (int e = 0; e < 3 && n < 0; e++) {
            snprintf(cand, LINE_MAX, "%s%s", word, path_exts[e]);
            n = fs_resolve(cand);
            if (n >= 0 && (fs_nodes[n].attr & FA_DIR)) n = -1;
            if (n >= 0 && e == 0) {
                const char *dot = strrchr(fs_nodes[n].name, '.');
                if (!dot) n = -1;
            }
        }
        if (strchr(word, '\\') || strchr(word, ':')) break;
        /* then search PATH */
        if (pass == 0 && (path = env_get("PATH"))) {
            const char *p = path;
            while (*p && n < 0) {
                char dir[96];
                int l = 0;
                while (*p && *p != ';') { if (l < 95) dir[l++] = *p; p++; }
                if (*p) p++;
                dir[l] = 0;
                if (!l) continue;
                for (int e = 0; e < 3 && n < 0; e++) {
                    snprintf(cand, LINE_MAX, "%s\\%s%s", dir, word, path_exts[e]);
                    n = fs_resolve(cand);
                    if (n >= 0 && (fs_nodes[n].attr & FA_DIR)) n = -1;
                    if (n >= 0 && e == 0 && !strrchr(fs_nodes[n].name, '.')) n = -1;
                }
            }
            break;
        }
    }
    free(cand);
    return n;
}

static bool ti_prgm_exists(const char *name)
{
    char up[9];
    uint8_t h;
    if (strlen(name) > 8 || !isalpha((unsigned char)name[0])) return false;
    strcpy(up, name);
    str_upper(up);
    for (const char *c = up; *c; c++) if (!isalnum((unsigned char)*c)) return false;
    if ((h = ti_OpenVar(up, "r", OS_TYPE_PRGM)) || (h = ti_OpenVar(up, "r", OS_TYPE_PROT_PRGM))) {
        ti_Close(h);
        return true;
    }
    return false;
}

static int run_external(char *word, char *rest)
{
    int n = find_program(word);
    if (n >= 0) {
        const char *dot = strrchr(fs_nodes[n].name, '.');
        if (dot && (!stricmp_(dot, ".bat") || !stricmp_(dot, ".cmd"))) {
            char *argv[10];
            char *abuf = xstrdup(rest);
            int argc;
            if (!abuf) return 1;
            argv[0] = word;
            argc = 1;
            {
                /* keep quotes for batch args, like CMD */
                char *s = abuf;
                while (*s && argc < 10) {
                    bool q = false;
                    while (*s == ' ' || *s == '\t' || *s == ',') s++;
                    if (!*s) break;
                    argv[argc++] = s;
                    while (*s && (q || (*s != ' ' && *s != '\t' && *s != ','))) { if (*s == '"') q = !q; s++; }
                    if (*s) *s++ = 0;
                }
            }
            n = run_batch_file((uint8_t)n, argc, argv);
            free(abuf);
            return n;
        }
        if (dot && (!stricmp_(dot, ".txt") || !stricmp_(dot, ".log") || !stricmp_(dot, ".ini") ||
                    !stricmp_(dot, ".md") || !stricmp_(dot, ".csv"))) {
            char *p = malloc(LINE_MAX);
            int r;
            if (!p) return 1;
            fs_path((uint8_t)n, p);
            r = edit_file(p);
            free(p);
            return r;
        }
        err_out("This file does not have an app associated with it.\n");
        return 1;
    }
    if (ti_prgm_exists(word)) {
        char *cmd = malloc(LINE_MAX);
        int r;
        if (!cmd) return 1;
        snprintf(cmd, LINE_MAX, "start %s", word);
        r = exec_line(cmd, false);
        free(cmd);
        return r;
    }
    if (strlen(word) == 2 && word[1] == ':' && isalpha((unsigned char)word[0])) {
        if (toupper((unsigned char)word[0]) == 'C') return 0;
        err_out("The system cannot find the drive specified.\n");
        return 1;
    }
    err_out("'");
    err_out(word);
    err_out("' is not recognized as an internal or external command,\noperable program or batch file.\n");
    return 9009;
}

static int macro_depth;

static int exec_simple(char *s)
{
    char *word, *raw, *wend;
    char *buf = NULL;
    const cmd_t *c;
    const char *mac;
    int r;

    while (*s == ' ' || *s == '\t' || *s == '@') s++;
    if (!*s) return EL_KEEP;

    /* !VAR! is expanded per simple command at run time, so blocks and
       IF/FOR bodies are left alone here and expanded when they run */
    if (g_delayed && strchr(s, '!') && *s != '(' && !starts_kw(s, "if") && !starts_kw(s, "for")) {
        buf = malloc(BUFSZ);
        if (!buf) return 1;
        expand_delayed(s, buf, BUFSZ);
        s = buf;
    }

    if (*s == '(') {
        char *e = match_paren(s);
        if (e) *e = 0;
        r = exec_line(s + 1, false);
        free(buf);
        return r;
    }

    /* the command word */
    word = s;
    wend = s;
    if (*wend == '"') {
        word = ++wend;
        while (*wend && *wend != '"') wend++;
    } else {
        while (*wend && *wend != ' ' && *wend != '\t') wend++;
    }

    /* builtin? the name ends at a delimiter like "cd..", "echo.", "dir/w" */
    {
        char name[16];
        int l = 0;
        const char *p = word;
        while (p < wend && !strchr("./\\(=,;+:", *p) && l < 15) name[l++] = *p++;
        name[l] = 0;
        c = l ? find_cmd(name) : NULL;
        if (c && (p == wend || strchr("./\\(=,;+", *p))) {
            char *argv[24];
            char *args;
            int argc;
            raw = (char *)p;
            if (*wend == '"') { /* quoted builtin name: unusual, ignore quote */ }
            if (c->fn == NULL) { free(buf); return 0; }
            args = malloc(BUFSZ);
            if (!args) { free(buf); return 1; }
            remove_carets(raw);
            strncpy(args, raw, BUFSZ - 1);
            args[BUFSZ - 1] = 0;
            argv[0] = name;
            argc = 1 + split_args(args, argv + 1, 23);
            if (argc > 1 && !strcmp(argv[1], "/?")) {
                out_puts(c->help);
                out_nl();
                r = 0;
            } else {
                r = c->fn(argc, argv, raw);
            }
            free(args);
            free(buf);
            return r;
        }
    }

    /* external command / batch file / doskey macro */
    {
        char *rest = wend;
        char *w;
        if (*rest == '"') rest++;
        if (*rest) *rest++ = 0;
        w = word;
        rest = trim(rest);
        if (!g_in_batch && macro_depth < 4 && (mac = macro_get(w))) {
            char *m = malloc(BUFSZ);
            ob_t o = {m, 0, BUFSZ};
            if (!m) { free(buf); return 1; }
            for (; *mac; mac++) {
                if (*mac == '$' && mac[1]) {
                    mac++;
                    if (*mac == '*') ob_s(&o, rest);
                    else if (*mac == 'T' || *mac == 't') ob_c(&o, '&');
                    else if (*mac == 'G' || *mac == 'g') ob_c(&o, '>');
                    else if (*mac == 'L' || *mac == 'l') ob_c(&o, '<');
                    else if (*mac == 'B' || *mac == 'b') ob_c(&o, '|');
                    else if (*mac == '$') ob_c(&o, '$');
                    else if (*mac >= '1' && *mac <= '9') {
                        int k = *mac - '1';
                        const char *a = rest;
                        while (k-- > 0 && *a) { while (*a && *a != ' ') a++; while (*a == ' ') a++; }
                        while (*a && *a != ' ') ob_c(&o, *a++);
                    }
                } else ob_c(&o, *mac);
            }
            m[o.n] = 0;
            macro_depth++;
            r = exec_line(m, true);
            macro_depth--;
            free(m);
            free(buf);
            return r;
        }
        remove_carets(rest);
        r = run_external(w, rest);
    }
    free(buf);
    return r;
}

/* ---------------------------------------------------------------- */
/* redirection and pipes                                             */
static const char *redir_err(int e)
{
    if (e == -1) return "The system cannot find the path specified.\n";
    if (e == -2) return "Access is denied.\n";
    return "There is not enough space on the disk.\n";
}

/* pull "> file", ">> file", "< file", "2>nul", "2>&1" out of s */
static void parse_redirs(char *s, char *out_f, bool *append, char *in_f, bool *err_nul, bool *err_out_)
{
    bool q = false, found = false;
    int d = 0;
    *out_f = *in_f = 0;
    for (char *p = s; *p; p++) {
        int fd = 1;
        char *start = p, *dst;
        if (*p == '^' && p[1]) { p++; continue; }
        if (*p == '"') { q = !q; continue; }
        if (q) continue;
        if (*p == '(') d++;
        if (*p == ')') d--;
        if (d > 0 || (*p != '>' && *p != '<')) continue;
        if (*p == '>' && p > s && (p[-1] == '1' || p[-1] == '2') && (p - 1 == s || p[-2] == ' ')) {
            fd = p[-1] - '0';
            start = p - 1;
        }
        dst = out_f;
        if (*p == '<') { dst = in_f; p++; }
        else {
            p++;
            if (*p == '>') { *append = true; p++; } else if (fd == 1) *append = false;
            if (*p == '&') {
                p++;
                if (*p == '1' && fd == 2) *err_out_ = true;
                if (*p) p++;
                memset(start, ' ', p - start);
                p--;
                found = true;
                continue;
            }
        }
        while (*p == ' ') p++;
        {
            int n = 0;
            bool fq = false;
            if (fd == 2) *err_nul = true; /* 2>anything: errors are dropped */
            while (*p && (fq || (*p != ' ' && *p != '>' && *p != '<' && *p != '|' && *p != '&'))) {
                if (*p == '"') fq = !fq;
                else if (fd != 2 && n < LINE_MAX - 1) dst[n++] = *p;
                p++;
            }
            if (fd != 2) dst[n] = 0;
        }
        memset(start, ' ', p - start);
        p--;
        found = true;
    }
    if (found) {
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    }
}

static int exec_pipeline(char *s)
{
    char *segs[6];
    int nseg = 0, r = 0;
    bool q = false, keep = false;
    int d = 0;
    sbuf_t pin, pout;
    sbuf_t *save_cap = out_cap, *save_stdin = g_stdin;
    size_t save_pos = g_stdin_pos;
    bool save_nul = out_nul, save_en = g_err_nul, save_eo = g_err_to_out;
    char *out_f, *in_f;

    segs[nseg++] = s;
    for (char *p = s; *p; p++) {
        if (*p == '^' && p[1]) { p++; continue; }
        if (*p == '"') q = !q;
        else if (!q && *p == '(') d++;
        else if (!q && *p == ')') d--;
        else if (!q && d <= 0 && *p == '|' && nseg < 6) { *p = 0; segs[nseg++] = p + 1; }
    }

    out_f = malloc(LINE_MAX);
    in_f = malloc(LINE_MAX);
    if (!out_f || !in_f) { free(out_f); free(in_f); err_out("Not enough memory.\n"); return 1; }
    sb_init(&pin);
    sb_init(&pout);

    for (int i = 0; i < nseg && !g_break && !g_goto; i++) {
        bool append = false, en = false, eo = false, capture, to_nul = false;
        sbuf_t inbuf;
        sb_init(&inbuf);
        parse_redirs(segs[i], out_f, &append, in_f, &en, &eo);
        if (*out_f && (!stricmp_(out_f, "nul") || !stricmp_(out_f, "nul:"))) { to_nul = true; *out_f = 0; }
        if (*out_f && !stricmp_(out_f, "con")) *out_f = 0;
        capture = (i < nseg - 1) || *out_f;

        /* stdin */
        if (*in_f) {
            int n = fs_resolve(in_f);
            if (n < 0 || (fs_nodes[n].attr & FA_DIR)) {
                err_out("The system cannot find the file specified.\n");
                r = 1;
                break;
            }
            inbuf.buf = fs_read((uint8_t)n, &inbuf.len);
            inbuf.cap = inbuf.len + 1;
            g_stdin = &inbuf;
        } else {
            g_stdin = i ? &pin : save_stdin;
        }
        g_stdin_pos = (g_stdin == save_stdin) ? save_pos : 0;

        if (capture) { sb_init(&pout); out_cap = &pout; out_nul = false; }
        if (to_nul) out_nul = true;
        if (en) g_err_nul = true;
        if (eo) g_err_to_out = true;

        r = exec_simple(segs[i]);
        if (r == EL_KEEP) { r = 0; keep = true; }
        else { g_errorlevel = r; keep = false; }

        out_cap = save_cap;
        out_nul = save_nul;
        g_err_nul = save_en;
        g_err_to_out = save_eo;
        sb_free(&inbuf);
        sb_free(&pin);

        if (*out_f) {
            int e = fs_write_path(out_f, pout.buf ? pout.buf : "", pout.len, append);
            if (e) { err_out(redir_err(e)); r = 1; }
            else if (pout.overflow) err_out("Warning: output truncated.\n");
            sb_free(&pout);
        } else if (capture) {
            pin = pout;
            sb_init(&pout);
        }
    }
    sb_free(&pin);
    sb_free(&pout);
    free(out_f);
    free(in_f);
    g_stdin = save_stdin;
    g_stdin_pos = save_pos;
    if (!keep) g_errorlevel = r;
    return r;
}

/* split "a & b && c || d" at top level and run */
static int exec_seq(char *s)
{
    char op = '&';
    bool ok = g_errorlevel == 0;
    int r = g_errorlevel;

    while (s && *s && !g_quit && !g_goto && !g_break && !g_batch_abort) {
        char *next = NULL;
        char nop = '&';
        bool run;
        while (*s == ' ' || *s == '\t' || *s == '@') s++;
        if (!*s) break;
        if (starts_kw(s, "rem") || (s[0] == ':' && s[1] == ':')) break;
        run = op == '&' || (op == 'A' && ok) || (op == 'O' && !ok);

        if (starts_kw(s, "if") || starts_kw(s, "for")) {
            if (run) {
                r = exec_simple(s);
                if (r == EL_KEEP) r = 0;
                else g_errorlevel = r;
                ok = r == 0;
            }
            break;
        }
        {
            bool q = false;
            int d = 0;
            for (char *p = s; *p; p++) {
                if (*p == '^' && p[1]) { p++; continue; }
                if (*p == '"') q = !q;
                else if (q) continue;
                else if (*p == '(') d++;
                else if (*p == ')') d--;
                else if (d <= 0 && *p == '&') {
                    *p = 0;
                    if (p[1] == '&') { nop = 'A'; next = p + 2; }
                    else next = p + 1;
                    break;
                } else if (d <= 0 && *p == '|' && p[1] == '|') {
                    *p = 0;
                    nop = 'O';
                    next = p + 2;
                    break;
                }
            }
        }
        if (run) { r = exec_pipeline(s); ok = r == 0; }
        op = nop;
        s = next;
    }
    return r;
}

/* ---------------------------------------------------------------- */
/* batch files                                                       */
static int paren_depth(const char *s)
{
    int d = 0;
    bool q = false;
    for (; *s; s++) {
        if (*s == '^' && s[1]) { s++; continue; }
        if (*s == '"') q = !q;
        else if (!q && *s == '(') d++;
        else if (!q && *s == ')') d--;
    }
    return d;
}

static int batch_run(bctx_t *bc)
{
    char *line = malloc(BUFSZ), *ex = malloc(BUFSZ);
    bctx_t *save = g_bc;
    bool save_ib = g_in_batch;

    if (!line || !ex) { free(line); free(ex); err_out("Not enough memory.\n"); return 1; }
    if (!stack_check() || ++batch_depth > 8) {
        if (batch_depth > 8) batch_depth--;
        err_out("Batch recursion too deep.\n");
        g_batch_abort = true;
        free(line); free(ex);
        return 1;
    }
    g_bc = bc;
    g_in_batch = true;
    bc->local_base = local_depth;

    while (!bc->done && bc->pc < bc->nlines && !g_quit && !g_batch_abort) {
        char *l;
        bool at = false;
        if (check_break()) { g_batch_abort = true; break; }
        l = bc->lines[bc->pc++];
        while (*l == ' ' || *l == '\t') l++;
        if (!*l || *l == ':') continue;
        strncpy(line, l, BUFSZ - 1);
        line[BUFSZ - 1] = 0;
        while (paren_depth(line) > 0 && bc->pc < bc->nlines) {
            char *nx = bc->lines[bc->pc++];
            while (*nx == ' ' || *nx == '\t') nx++;
            if (!*nx || (nx[0] == ':' && nx[1] == ':')) continue;
            if (strlen(line) + strlen(nx) + 4 >= BUFSZ) break;
            strcat(line, " & ");
            strcat(line, nx);
        }
        l = line;
        while (*l == ' ') l++;
        if (*l == '@') { at = true; l++; }
        expand_vars(l, ex, BUFSZ, true);
        if (g_echo && !at) {
            print_prompt();
            out_puts(ex);
            out_nl();
        }
        g_goto = false;
        exec_seq(ex);
        g_goto = false;
        con_render_lazy();
    }
    while (local_depth > bc->local_base) setlocal_pop();
    g_bc = save;
    g_in_batch = save_ib;
    batch_depth--;
    free(line);
    free(ex);
    return bc->done ? bc->code : g_errorlevel;
}

static void set_args(bctx_t *bc, int argc, char **argv)
{
    bc->nargs = argc > 10 ? 10 : argc;
    for (int i = 0; i < bc->nargs; i++) bc->args[i] = xstrdup(argv[i]);
}

static void free_args(bctx_t *bc)
{
    for (int i = 0; i < bc->nargs; i++) free(bc->args[i]);
    bc->nargs = 0;
}

int run_batch_file(uint8_t node, int argc, char **argv)
{
    bctx_t *bc = calloc(1, sizeof(bctx_t));
    size_t sz;
    int n = 1, r;
    char *p;

    if (!bc) return 1;
    bc->text = fs_read(node, &sz);
    if (!bc->text) { free(bc); err_out("Not enough memory.\n"); return 1; }
    for (p = bc->text; *p; p++) if (*p == '\n') n++;
    bc->lines = malloc(n * sizeof(char *));
    if (!bc->lines) { free(bc->text); free(bc); err_out("Not enough memory.\n"); return 1; }
    p = bc->text;
    bc->nlines = 0;
    while (p) {
        char *nl = strchr(p, '\n');
        bc->lines[bc->nlines++] = p;
        if (nl) {
            *nl = 0;
            if (nl > p && nl[-1] == '\r') nl[-1] = 0;
            p = nl + 1;
        } else p = NULL;
    }
    bc->owner = true;
    set_args(bc, argc, argv);
    r = batch_run(bc);
    free_args(bc);
    free(bc->lines);
    free(bc->text);
    free(bc);
    if (!g_bc) {
        if (g_batch_abort) out_puts("^C\n");
        g_batch_abort = false;
    }
    g_errorlevel = r;
    return r;
}

static int find_label(bctx_t *bc, const char *label)
{
    while (*label == ':') label++;
    for (int i = 0; i < bc->nlines; i++) {
        const char *l = bc->lines[i];
        size_t ll = strlen(label);
        while (*l == ' ' || *l == '\t') l++;
        if (*l != ':') continue;
        l++;
        if (!strnicmp_(l, label, ll) && (!l[ll] || l[ll] == ' ' || l[ll] == '\t')) return i;
    }
    return -1;
}

int batch_goto(const char *label)
{
    int i;
    if (!g_bc) return 0;
    while (*label == ':') label++;
    if (!stricmp_(label, "eof")) {
        g_bc->done = true;
        g_bc->code = g_errorlevel;
        g_goto = true;
        return g_errorlevel;
    }
    i = find_label(g_bc, label);
    if (i < 0) {
        err_out("The system cannot find the batch label specified - ");
        err_out(label);
        err_out("\n");
        g_bc->done = true;
        g_goto = true;
        return 1;
    }
    g_bc->pc = i + 1;
    g_goto = true;
    return 0;
}

int batch_call_label(const char *label, char *rest)
{
    bctx_t *bc;
    char *argv[10];
    int argc, i, r;
    if (!g_bc) {
        err_out("Invalid attempt to call batch label outside of batch script.\n");
        return 1;
    }
    i = find_label(g_bc, label);
    if (i < 0) {
        err_out("The system cannot find the batch label specified - ");
        err_out(label);
        err_out("\n");
        return 1;
    }
    bc = calloc(1, sizeof(bctx_t));
    if (!bc) return 1;
    bc->text = g_bc->text;
    bc->lines = g_bc->lines;
    bc->nlines = g_bc->nlines;
    bc->pc = i + 1;
    argv[0] = (char *)label;
    argc = 1 + split_args(rest, argv + 1, 9);
    set_args(bc, argc, argv);
    r = batch_run(bc);
    free_args(bc);
    free(bc);
    g_goto = false;
    g_errorlevel = r;
    return r;
}

void batch_shift(void)
{
    if (!g_bc || g_bc->nargs < 2) return;
    free(g_bc->args[1]);
    memmove(&g_bc->args[1], &g_bc->args[2], (g_bc->nargs - 2) * sizeof(char *));
    g_bc->nargs--;
}

int batch_exit(int code)
{
    if (g_bc) {
        g_bc->done = true;
        g_bc->code = code;
        g_goto = true;
    }
    return code;
}

/* ---------------------------------------------------------------- */
/* IF                                                                */
static void dexp(char *tok, int max)
{
    char *t;
    if (!g_delayed || !strchr(tok, '!') || !(t = malloc(max))) return;
    expand_delayed(tok, t, max);
    strcpy(tok, t);
    free(t);
}

static bool parse_long(const char *s, long *v)
{
    char *e;
    if (!*s) return false;
    *v = strtol(s, &e, 0);
    return !*e;
}

int cmd_if(int argc, char **argv, char *raw)
{
    char *p = raw, *body, *then_p, *else_p = NULL;
    char *t1, *t2, opw[8];
    bool icase = false, neg = false, res = false;
    const int TS = LINE_MAX / 2;
    (void)argc; (void)argv;

    t1 = malloc(TS * 3);
    if (!t1) return 1;
    t2 = t1 + TS;

    while (*p == ' ') p++;
    if (!strnicmp_(p, "/i ", 3)) { icase = true; p += 3; }
    while (*p == ' ') p++;
    if (starts_kw(p, "not")) { neg = true; p += 3; }
    while (*p == ' ') p++;

    if (starts_kw(p, "exist")) {
        char *unq = t2 + TS;
        int j = 0;
        p = read_tok(p + 5, t1, TS, false);
        dexp(t1, TS);
        for (char *c = t1; *c; c++) if (*c != '"') unq[j++] = *c;
        unq[j] = 0;
        if (has_wild(unq)) {
            uint8_t tmp[4];
            res = fs_glob(unq, tmp, 4, NULL) > 0;
        } else res = fs_resolve(unq) >= 0;
    } else if (starts_kw(p, "defined")) {
        p = read_tok(p + 7, t1, TS, false);
        res = env_get(t1) != NULL;
    } else if (starts_kw(p, "errorlevel")) {
        p = read_tok(p + 10, t1, TS, false);
        res = g_errorlevel >= atoi(t1);
    } else {
        p = read_tok(p, t1, TS, true);
        while (*p == ' ') p++;
        if (p[0] == '=' && p[1] == '=') {
            p += 2;
            strcpy(opw, "==");
        } else {
            p = read_tok(p, opw, sizeof opw, false);
        }
        p = read_tok(p, t2, TS, false);
        dexp(t1, TS);
        dexp(t2, TS);
        if (!strcmp(opw, "==")) {
            res = icase ? !stricmp_(t1, t2) : !strcmp(t1, t2);
        } else {
            long a, b;
            int cmp;
            if (parse_long(t1, &a) && parse_long(t2, &b)) cmp = a < b ? -1 : a > b;
            else cmp = icase ? stricmp_(t1, t2) : strcmp(t1, t2);
            if (is_switch(opw, "EQU")) res = cmp == 0;
            else if (is_switch(opw, "NEQ")) res = cmp != 0;
            else if (is_switch(opw, "LSS")) res = cmp < 0;
            else if (is_switch(opw, "LEQ")) res = cmp <= 0;
            else if (is_switch(opw, "GTR")) res = cmp > 0;
            else if (is_switch(opw, "GEQ")) res = cmp >= 0;
            else {
                err_out(opw);
                err_out(" was unexpected at this time.\n");
                free(t1);
                return 1;
            }
        }
    }
    free(t1);
    if (neg) res = !res;

    while (*p == ' ') p++;
    body = p;
    if (!*body) {
        err_out("The syntax of the command is incorrect.\n");
        return 1;
    }
    then_p = body;
    if (*body == '(') {
        char *e = match_paren(body);
        if (e) {
            *e = 0;
            then_p = body + 1;
            e++;
            while (*e == ' ') e++;
            if (starts_kw(e, "else")) {
                e += 4;
                while (*e == ' ') e++;
                else_p = e;
                if (*e == '(') {
                    char *e2 = match_paren(e);
                    if (e2) *e2 = 0;
                    else_p = e + 1;
                }
            }
        }
    }
    if (res) return exec_line(then_p, false);
    if (else_p) return exec_line(else_p, false);
    return EL_KEEP;
}

/* ---------------------------------------------------------------- */
/* FOR                                                               */
static int for_body(const char *body, char var, const char **vals, int nvals)
{
    char *b = malloc(BUFSZ);
    ob_t o = {b, 0, BUFSZ};
    int r;
    if (!b) return 1;
    for (const char *p = body; *p; p++) {
        if (*p == '%') {
            const char *q = p + 1;
            bool tilde = false;
            if (*q == '~') { tilde = true; q++; }
            if (*q && *q >= var && *q < var + nvals) {
                const char *v = vals[*q - var];
                if (tilde) strip_quotes_to(v, &o);
                else ob_s(&o, v);
                p = q;
                continue;
            }
        }
        ob_c(&o, *p);
    }
    b[o.n] = 0;
    r = exec_line(b, false);
    free(b);
    return r;
}

static int for_f_text(char *text, const char *body, char var, const char *opts)
{
    int skip = 0, ntok = 0, r = 0;
    int toks[8];
    bool star = false;
    char delims[12] = " \t";
    char eol = ';';
    const char *o;
    char *line, *nl;

    /* options */
    if ((o = stristr_(opts, "skip="))) skip = atoi(o + 5);
    if ((o = stristr_(opts, "eol="))) eol = o[4];
    if ((o = stristr_(opts, "delims="))) {
        int n = 0;
        o += 7;
        while (*o && *o != '"' && n < 11) {
            if (*o == ' ' && (o[1] == '"' || !o[1] || !strnicmp_(o + 1, "tokens", 6) ||
                              !strnicmp_(o + 1, "skip", 4) || !strnicmp_(o + 1, "eol", 3) ||
                              !strnicmp_(o + 1, "usebackq", 8)))
                break;
            delims[n++] = *o++;
        }
        delims[n] = 0;
    }
    if ((o = stristr_(opts, "tokens="))) {
        o += 7;
        while (*o && *o != ' ' && *o != '"' && ntok < 8) {
            if (*o == '*') { star = true; o++; break; }
            if (isdigit((unsigned char)*o)) {
                int a = atoi(o), b = a;
                while (isdigit((unsigned char)*o)) o++;
                if (*o == '-') { o++; b = atoi(o); while (isdigit((unsigned char)*o)) o++; }
                for (int k = a; k <= b && ntok < 8; k++) toks[ntok++] = k;
            }
            if (*o == ',') o++;
            else if (*o == '*') { star = true; o++; break; }
            else if (!isdigit((unsigned char)*o)) break;
        }
    }
    if (!ntok && !star) toks[ntok++] = 1;

    line = text;
    while (line && *line && !g_break && !g_goto) {
        const char *vals[9];
        char *work, *fields[24];
        int nf = 0, nv = 0;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        {
            size_t ll = strlen(line);
            if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
        }
        if (skip > 0) { skip--; goto next; }
        if (!*line || *line == eol) goto next;
        work = xstrdup(line);
        if (!work) break;
        {
            /* split fields; remember where each starts for '*' */
            char *s = work;
            int startoff[24];
            while (*s && nf < 24) {
                while (*s && *delims && strchr(delims, *s)) s++;
                if (!*s) break;
                startoff[nf] = (int)(s - work);
                fields[nf++] = s;
                while (*s && !(*delims && strchr(delims, *s))) s++;
                if (*s) *s++ = 0;
            }
            for (int k = 0; k < ntok; k++)
                vals[nv++] = (toks[k] >= 1 && toks[k] <= nf) ? fields[toks[k] - 1] : "";
            if (star) {
                int idx = ntok ? toks[ntok - 1] : 0;
                if (idx < nf) vals[nv++] = line + startoff[idx];
                else vals[nv++] = "";
            }
        }
        r = for_body(body, var, vals, nv);
        free(work);
    next:
        line = nl ? nl + 1 : NULL;
    }
    return r;
}

int cmd_for(int argc, char **argv, char *raw)
{
    char *p = raw, *set, *body, *e;
    char var = 0, mode = 0;
    char opts[64] = "";
    int r = 0;
    (void)argc; (void)argv;

    while (*p == ' ') p++;
    while (*p == '/') {
        mode = (char)tolower((unsigned char)p[1]);
        p += 2;
        while (*p == ' ') p++;
        if (mode == 'f' && *p == '"') {
            char *q = strchr(p + 1, '"');
            if (q) {
                size_t l = (size_t)(q - p - 1);
                if (l >= sizeof opts) l = sizeof opts - 1;
                memcpy(opts, p + 1, l);
                opts[l] = 0;
                p = q + 1;
            }
            while (*p == ' ') p++;
        }
    }
    if (*p == '%') {
        p++;
        if (*p == '%') p++;
        var = *p++;
    }
    while (*p == ' ') p++;
    if (!var || !starts_kw(p, "in")) goto syntax;
    p += 2;
    while (*p == ' ') p++;
    if (*p != '(' || !(e = match_paren(p))) goto syntax;
    *e = 0;
    set = p + 1;
    p = e + 1;
    while (*p == ' ') p++;
    if (!starts_kw(p, "do")) goto syntax;
    body = p + 2;
    while (*body == ' ') body++;
    if (!*body) goto syntax;

    if (mode == 'l') {
        long v[3] = {0, 0, 0};
        int k = 0;
        char *s = set;
        while (*s && k < 3) {
            while (*s == ' ' || *s == ',') s++;
            if (!*s) break;
            v[k++] = strtol(s, &s, 0);
        }
        if (!v[1]) return 0;
        for (long i = v[0]; v[1] > 0 ? i <= v[2] : i >= v[2]; i += v[1]) {
            char num[16];
            const char *vals[1] = {num};
            sprintf(num, "%ld", i);
            r = for_body(body, var, vals, 1);
            if (g_break || g_goto || g_quit) break;
        }
        return r;
    }

    if (mode == 'f') {
        char *s = trim(set);
        bool backq = stristr_(opts, "usebackq") != NULL;
        if ((!backq && *s == '"') || (backq && *s == '\'')) {
            char *t = xstrdup(s + 1);
            size_t l;
            if (!t) return 1;
            l = strlen(t);
            if (l && (t[l - 1] == '"' || t[l - 1] == '\'')) t[l - 1] = 0;
            r = for_f_text(t, body, var, opts);
            free(t);
        } else if ((!backq && *s == '\'') || (backq && *s == '`')) {
            sbuf_t cap, *save = out_cap;
            bool sn = out_nul;
            char *cmdl = xstrdup(s + 1);
            size_t l;
            if (!cmdl) return 1;
            l = strlen(cmdl);
            if (l && (cmdl[l - 1] == '\'' || cmdl[l - 1] == '`')) cmdl[l - 1] = 0;
            sb_init(&cap);
            out_cap = &cap;
            out_nul = false;
            exec_line(cmdl, false);
            out_cap = save;
            out_nul = sn;
            free(cmdl);
            if (cap.buf) r = for_f_text(cap.buf, body, var, opts);
            sb_free(&cap);
        } else {
            char *files[8];
            int nf = split_args(s, files, 8);
            for (int i = 0; i < nf && !g_break && !g_goto; i++) {
                int n = fs_resolve(files[i]);
                char *t;
                if (n < 0 || (fs_nodes[n].attr & FA_DIR)) {
                    err_out("The system cannot find the file ");
                    err_out(files[i]);
                    err_out(".\n");
                    return 1;
                }
                t = fs_read((uint8_t)n, NULL);
                if (!t) return 1;
                r = for_f_text(t, body, var, opts);
                free(t);
            }
        }
        return r;
    }

    /* plain set: words, wildcards match files (or dirs with /d) */
    {
        char *items[24];
        int ni = 0;
        char *s = set;
        while (*s && ni < 24) {
            bool q = false;
            while (*s == ' ' || *s == ',' || *s == ';' || *s == '\t') s++;
            if (!*s) break;
            items[ni++] = s;
            while (*s && (q || !strchr(" ,;\t", *s))) { if (*s == '"') q = !q; s++; }
            if (*s) *s++ = 0;
        }
        for (int i = 0; i < ni && !g_break && !g_goto && !g_quit; i++) {
            if (has_wild(items[i])) {
                uint8_t *m = malloc(FS_MAX + LINE_MAX + LINE_MAX / 2);
                int nm;
                char *pre, *val;
                char *bs;
                if (!m) return 1;
                pre = (char *)m + FS_MAX;
                val = pre + LINE_MAX / 2;
                strncpy(pre, items[i], LINE_MAX / 2 - 1);
                pre[LINE_MAX / 2 - 1] = 0;
                bs = strrchr(pre, '\\');
                if (!bs) bs = strrchr(pre, ':');
                if (bs) bs[1] = 0; else pre[0] = 0;
                nm = fs_glob(items[i], m, FS_MAX, NULL);
                for (int k = 0; k < nm && !g_break && !g_goto; k++) {
                    const char *vals[1] = {val};
                    bool isdir = (fs_nodes[m[k]].attr & FA_DIR) != 0;
                    if (isdir != (mode == 'd')) continue;
                    if (fs_nodes[m[k]].attr & FA_HIDDEN) continue;
                    sprintf(val, "%s%s", pre, fs_nodes[m[k]].name);
                    r = for_body(body, var, vals, 1);
                }
                free(m);
            } else if (mode != 'd') {
                const char *vals[1] = {items[i]};
                r = for_body(body, var, vals, 1);
            }
        }
    }
    return r;
syntax:
    err_out("The syntax of the command is incorrect.\n");
    return 1;
}

/* ---------------------------------------------------------------- */
/* prompt / main loop                                                */
void print_prompt(void)
{
    const char *p = env_get("PROMPT");
    char *path = malloc(LINE_MAX);
    if (!path) return;
    if (!p) p = "$P$G";
    for (; *p; p++) {
        if (*p != '$' || !p[1]) { out_putc(*p); continue; }
        switch (toupper((unsigned char)*++p)) {
        case 'P': fs_path(fs_cwd, path); out_puts(path); break;
        case 'G': out_putc('>'); break;
        case 'L': out_putc('<'); break;
        case 'B': out_putc('|'); break;
        case 'Q': out_putc('='); break;
        case 'S': out_putc(' '); break;
        case 'A': out_putc('&'); break;
        case 'C': out_putc('('); break;
        case 'F': out_putc(')'); break;
        case '$': out_putc('$'); break;
        case '_': out_nl(); break;
        case 'N': out_putc('C'); break;
        case 'D': out_puts(env_get("DATE")); break;
        case 'T': out_puts(env_get("TIME")); break;
        case 'V': out_puts("CE Command Prompt 10.0.84"); break;
        case 'H': out_putc('\b'); break;
        case '+': break;
        default: break;
        }
    }
    free(path);
}

void shell_banner(void)
{
    out_puts("CE Command Prompt [Version 10.0.84]\n");
    out_puts("(c) TI-84 Plus CE edition. HELP lists\ncommands, KEYS shows the keypad map.\n\n");
}

void shell_init(void)
{
    uint8_t s, m, h;
    boot_GetTime(&s, &m, &h);
    srandom(((uint32_t)h << 16) ^ ((uint32_t)m << 8) ^ s ^ (uint32_t)fs_used_bytes());
    env_set("PATH", "C:\\Windows\\System32;C:\\Windows");
    env_set("PATHEXT", ".BAT;.CMD");
    env_set("PROMPT", "$P$G");
    env_set("USERNAME", "User");
    env_set("USERPROFILE", "C:\\Users\\User");
    env_set("COMPUTERNAME", "TI84PCE");
    env_set("OS", "TI_NT");
    env_set("SYSTEMROOT", "C:\\Windows");
    env_set("WINDIR", "C:\\Windows");
    env_set("TEMP", "C:\\Temp");
    env_set("TMP", "C:\\Temp");
    env_set("PROCESSOR_ARCHITECTURE", "eZ80");
    env_set("COMSPEC", "C:\\Windows\\System32\\cmd.exe");
}

void shell_loop(void)
{
    char *line = malloc(LINE_MAX);
    int n;
    if (!line) return;

    /* autoexec */
    if ((n = fs_resolve("C:\\autoexec.bat")) >= 0 && !(fs_nodes[n].attr & FA_DIR)) {
        char *argv[1] = {"autoexec"};
        bool e = g_echo;
        g_echo = false;
        run_batch_file((uint8_t)n, 1, argv);
        g_echo = e;
        if (con_col()) out_nl();
    }

    while (!g_quit) {
        int r;
        out_cap = NULL;
        out_nul = false;
        print_prompt();
        line[0] = 0;
        r = read_line(line, LINE_MAX, true);
        if (r == -2) break;
        if (r == -1) { out_nl(); continue; }
        if (!*trim(line)) continue;
        g_goto = false;
        exec_line(line, true);
        if (g_break) {
            out_puts("^C");
            clear_break();
        }
        if (con_col()) out_nl();
        out_nl();
    }
    free(line);
}
