/*
 * cmds.c - built-in commands
 */
#include "cmd.h"
#include <fileioc.h>
#include <ti/vars.h>
#include <ti/info.h>
#include <ti/getcsc.h>
#include <sys/rtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>


#define SERIAL "84CE-0C2B"

/* ---------------------------------------------------------------- */
/* helpers                                                           */
static void fmt_num(uint32_t v, char *out)
{
    char tmp[16];
    int l, o = 0;
    sprintf(tmp, "%lu", (unsigned long)v);
    l = (int)strlen(tmp);
    for (int i = 0; i < l; i++) {
        out[o++] = tmp[i];
        if ((l - i - 1) % 3 == 0 && i != l - 1) out[o++] = ',';
    }
    out[o] = 0;
}

/* pull up to max unsigned numbers out of s (any separators) */
static int parse_nums(const char *s, unsigned *v, int max)
{
    int n = 0;
    while (*s && n < max) {
        if (isdigit((unsigned char)*s)) {
            unsigned x = 0;
            while (isdigit((unsigned char)*s)) x = x * 10 + (unsigned)(*s++ - '0');
            v[n++] = x;
        } else s++;
    }
    return n;
}

/* scratch path buffer for recursive walkers (keeps the 4KB stack free) */
static char walk_path[LINE_MAX];

static void not_found(void) { err_out("The system cannot find the file specified.\n"); }
static void path_not_found(void) { err_out("The system cannot find the path specified.\n"); }
static void syntax_err(void) { err_out("The syntax of the command is incorrect.\n"); }

/* collect non-switch args */
static int plain_args(int argc, char **argv, char **out, int max)
{
    int n = 0;
    for (int i = 1; i < argc && n < max; i++)
        if (argv[i][0] != '/') out[n++] = argv[i];
    return n;
}

static bool has_sw(int argc, char **argv, const char *sw)
{
    for (int i = 1; i < argc; i++) if (is_switch(argv[i], sw)) return true;
    return false;
}

static char *raw_rest(char *raw)
{
    /* skip the delimiter right after a command name */
    if (*raw == ' ' || *raw == '\t' || *raw == '.' || *raw == ',' || *raw == ';' || *raw == '=') raw++;
    return trim(raw);
}

static void unquote(char *s)
{
    char *d = s;
    for (; *s; s++) if (*s != '"') *d++ = *s;
    *d = 0;
}

static bool protected_var(const char *name)
{
    return !stricmp_(name, "CMD") || !strnicmp_(name, "CMDF", 4) || !strnicmp_(name, "CMDFSYS", 7);
}

/* print text buffer line by line, honouring [ON] and an optional pager */
static void print_text(const char *t)
{
    for (; *t; t++) {
        out_putc(*t);
        if (*t == '\n' && !pager_line()) return;
    }
}

/* ---------------------------------------------------------------- */
/* basic commands                                                    */
static int c_echo(int argc, char **argv, char *raw)
{
    char *t;
    (void)argc; (void)argv;
    if (*raw && strchr(".:;/\\(+[]", *raw)) {
        out_puts(raw + 1);
        out_nl();
        return EL_KEEP;
    }
    t = raw;
    if (*t == ' ' || *t == '\t') t++;
    {
        char tmp[8];
        char *tt;
        strncpy(tmp, t, 7);
        tmp[7] = 0;
        tt = trim(tmp);
        if (!*trim(t) ) {
            out_printf("ECHO is %s.\n", g_echo ? "on" : "off");
            return EL_KEEP;
        }
        if (!stricmp_(tt, "on") && strlen(trim(t)) == 2) { g_echo = true; return EL_KEEP; }
        if (!stricmp_(tt, "off") && strlen(trim(t)) == 3) { g_echo = false; return EL_KEEP; }
    }
    out_puts(t);
    out_nl();
    return EL_KEEP;
}

static int c_cls(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    if (!out_cap) con_clear();
    return EL_KEEP;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int c_color(int argc, char **argv, char *raw)
{
    int b, f;
    (void)raw;
    if (argc < 2) { con_fg = 7; con_bg = 0; return 0; }
    if (strlen(argv[1]) == 1) { b = con_bg; f = hexv(argv[1][0]); }
    else { b = hexv(argv[1][0]); f = hexv(argv[1][1]); }
    if (b < 0 || f < 0 || b == f) return 1;
    con_fg = (uint8_t)f;
    con_bg = (uint8_t)b;
    return 0;
}

static int c_title(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv;
    strncpy(con_title, raw_rest(raw), sizeof con_title - 1);
    con_title[sizeof con_title - 1] = 0;
    return EL_KEEP;
}

static int c_prompt(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    (void)argc; (void)argv;
    env_set("PROMPT", *r ? r : "$P$G");
    return EL_KEEP;
}

static int c_ver(int argc, char **argv, char *raw)
{
    const system_info_t *si = os_GetSystemInfo();
    (void)argc; (void)argv; (void)raw;
    out_printf("\nCE Command Prompt [Version 10.0.84]\nTI-OS %u.%u.%u.%04u\n",
               si->osMajorVersion, si->osMinorVersion, si->osRevisionVersion, si->osBuildVersion);
    return 0;
}

static int c_vol(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    out_printf(" Volume in drive C is %s\n Volume Serial Number is " SERIAL "\n", fs_label);
    return 0;
}

static int c_label(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    char buf[12];
    (void)argc; (void)argv;
    if (!strnicmp_(r, "c:", 2)) r = trim(r + 2);
    if (!*r) {
        c_vol(0, NULL, "");
        out_puts("Volume label (11 characters, ENTER for none)? ");
        buf[0] = 0;
        if (read_line(buf, sizeof buf, false)) return 1;
        r = buf;
    }
    strncpy(fs_label, r, sizeof fs_label - 1);
    fs_label[sizeof fs_label - 1] = 0;
    str_upper(fs_label);
    fs_save();
    return 0;
}

static int c_date(int argc, char **argv, char *raw)
{
    char buf[16];
    unsigned v[3], m, d, y;
    (void)raw;
    if (has_sw(argc, argv, "/t")) { out_printf("%s\n", env_get("DATE")); return 0; }
    if (argc < 2) {
        out_printf("The current date is: %s\nEnter the new date: (mm-dd-yyyy) ", env_get("DATE"));
        buf[0] = 0;
        if (read_line(buf, sizeof buf, false) || !buf[0]) return 0;
    } else {
        strncpy(buf, argv[1], 15);
        buf[15] = 0;
    }
    if (parse_nums(buf, v, 3) != 3 || !(m = v[0]) || m > 12 || !(d = v[1]) || d > 31) {
        err_out("The system cannot accept the date entered.\n");
        return 1;
    }
    y = v[2];
    if (y < 100) y += 2000;
    boot_SetDate((uint8_t)d, (uint8_t)m, (uint16_t)y);
    return 0;
}

static int c_time(int argc, char **argv, char *raw)
{
    char buf[16];
    unsigned v[3] = {0, 0, 0}, h, m, s;
    (void)raw;
    if (has_sw(argc, argv, "/t")) {
        uint8_t ss, mm, hh;
        boot_GetTime(&ss, &mm, &hh);
        out_printf("%02u:%02u\n", hh, mm);
        return 0;
    }
    if (argc < 2) {
        out_printf("The current time is: %s\nEnter the new time: ", env_get("TIME"));
        buf[0] = 0;
        if (read_line(buf, sizeof buf, false) || !buf[0]) return 0;
    } else {
        strncpy(buf, argv[1], 15);
        buf[15] = 0;
    }
    if (parse_nums(buf, v, 3) < 2 || (h = v[0]) > 23 || (m = v[1]) > 59 || (s = v[2]) > 59) {
        err_out("The system cannot accept the time entered.\n");
        return 1;
    }
    boot_SetTime((uint8_t)s, (uint8_t)m, (uint8_t)h);
    return 0;
}

static int c_pause(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    out_puts("Press any key to continue . . . ");
    if (!out_cap) wait_any_key();
    out_nl();
    return EL_KEEP;
}

static int c_timeout(int argc, char **argv, char *raw)
{
    long secs = -1;
    bool nobreak = has_sw(argc, argv, "/nobreak");
    clock_t start;
    long left;
    (void)raw;
    for (int i = 1; i < argc; i++) {
        if (is_switch(argv[i], "/t") && i + 1 < argc) secs = atol(argv[++i]);
        else if (argv[i][0] != '/') secs = atol(argv[i]);
    }
    if (secs < -1) { syntax_err(); return 1; }
    start = clock();
    left = -2;
    for (;;) {
        long el = (long)((clock() - start) / CLOCKS_PER_SEC);
        uint8_t k;
        if (secs >= 0 && el >= secs) break;
        if (secs >= 0 && secs - el != left) {
            left = secs - el;
            con_putc('\r');
            out_printf("Waiting for %2ld seconds, press a key ", left);
            con_render();
        } else if (secs < 0 && left == -2) {
            out_puts("Press any key to continue ...");
            left = -1;
            con_render();
        }
        k = key_poll();
        if (k == KEY_ON || check_break()) { clear_break(); out_nl(); return 1; }
        if (k && !nobreak && k != sk_2nd && k != sk_Alpha) break;
    }
    if (secs >= 0) { con_putc('\r'); out_puts("Waiting for  0 seconds, press a key "); }
    out_nl();
    return 0;
}

static int c_choice(int argc, char **argv, char *raw)
{
    const char *choices = "YN", *msg = NULL;
    bool nolist = false;
    long tmo = -1;
    char def = 0;
    char lst[40];
    clock_t st;
    (void)raw;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strnicmp_(a, "/c", 2)) choices = a[2] == ':' ? a + 3 : (a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : "YN"));
        else if (!strnicmp_(a, "/m", 2)) msg = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : "");
        else if (is_switch(a, "/n")) nolist = true;
        else if (!strnicmp_(a, "/t", 2)) tmo = atol(a[2] == ':' ? a + 3 : (a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : "0")));
        else if (!strnicmp_(a, "/d", 2)) def = *(a[2] == ':' ? a + 3 : (a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : "")));
    }
    if (msg) out_puts(msg);
    if (!nolist) {
        int n = 0;
        lst[n++] = '[';
        for (const char *c = choices; *c && n < 36; c++) {
            if (c != choices) lst[n++] = ',';
            lst[n++] = (char)toupper((unsigned char)*c);
        }
        lst[n++] = ']';
        lst[n++] = '?';
        lst[n] = 0;
        if (msg) out_putc(' ');
        out_puts(lst);
    }
    con_render();
    st = clock();
    for (;;) {
        uint8_t k = key_poll();
        int ch = 0;
        if (tmo >= 0 && def && (clock() - st) / CLOCKS_PER_SEC >= (unsigned long)tmo) ch = def;
        if (k == KEY_ON) { clear_break(); out_nl(); return 0; }
        if (k == sk_Alpha) { ui_mode = (ui_mode + 1) % 3; con_render(); continue; }
        if (k == sk_Enter && def) ch = def;
        if (k && !ch) ch = key_to_char(k, false, ui_mode);
        if (ch) {
            for (int i = 0; choices[i]; i++) {
                if (toupper((unsigned char)choices[i]) == toupper(ch)) {
                    out_putc((char)toupper(ch));
                    out_nl();
                    return i + 1;
                }
            }
        }
    }
}

static int c_rem(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    return EL_KEEP;
}

static int c_exit(int argc, char **argv, char *raw)
{
    int code = 0;
    bool b = false;
    (void)raw;
    for (int i = 1; i < argc; i++) {
        if (is_switch(argv[i], "/b")) b = true;
        else code = atoi(argv[i]);
    }
    if (b && g_in_batch) return batch_exit(code);
    if (b) return code;
    g_quit = true;
    return code;
}

static int c_goto(int argc, char **argv, char *raw)
{
    (void)raw;
    if (argc < 2) { err_out("No batch label specified to GOTO command.\n"); return 1; }
    if (!g_in_batch) return 0;
    return batch_goto(argv[1]);
}

static int c_call(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    (void)argc; (void)argv;
    if (!*r) return 0;
    if (*r == ':') {
        char *sp = r;
        char lbl[32];
        int n = 0;
        while (*sp && *sp != ' ' && n < 31) lbl[n++] = *sp++;
        lbl[n] = 0;
        {
            char *rest = malloc(LINE_MAX);
            int rc;
            if (!rest) return 1;
            strncpy(rest, trim(sp), LINE_MAX - 1);
            rest[LINE_MAX - 1] = 0;
            rc = batch_call_label(lbl, rest);
            free(rest);
            return rc;
        }
    }
    return exec_line(r, false);
}

static int c_shift(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    batch_shift();
    return EL_KEEP;
}

static int c_setlocal(int argc, char **argv, char *raw)
{
    (void)raw;
    if (!g_in_batch) return EL_KEEP;
    setlocal_push(argc > 1 && !stricmp_(argv[1], "EnableDelayedExpansion"));
    return EL_KEEP;
}

static int c_endlocal(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    if (g_in_batch) setlocal_pop();
    return EL_KEEP;
}

static int c_set(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    char *eq;
    (void)argc; (void)argv;

    if (!*r) { env_list(NULL); return 0; }
    if (!strnicmp_(r, "/a", 2) && (r[2] == ' ' || r[2] == '"' || !r[2])) {
        long v;
        int e;
        char *x = trim(r + 2);
        unquote(x);
        e = eval_set_a(x, &v);
        if (e == 1) { err_out("Missing operand.\n"); return 1; }
        if (e == 2) { err_out("Divide by zero error.\n"); return 1; }
        if (e) { err_out("Invalid number or expression.\n"); return 1; }
        if (!g_in_batch) out_printf("%ld\n", v);
        return 0;
    }
    if (!strnicmp_(r, "/p", 2) && (r[2] == ' ' || r[2] == '"')) {
        char *x = trim(r + 2), *val;
        size_t l = strlen(x);
        if (*x == '"' && l > 1 && x[l - 1] == '"') { x[l - 1] = 0; x++; }
        eq = strchr(x, '=');
        if (!eq) { syntax_err(); return 1; }
        *eq = 0;
        val = malloc(LINE_MAX);
        if (!val) return 1;
        val[0] = 0;
        if (g_stdin) {
            if (!read_stdin_line(val, LINE_MAX)) { free(val); return 1; }
        } else {
            out_puts(eq + 1);
            if (read_line(val, LINE_MAX, false)) { free(val); return 1; }
        }
        if (*val) env_set(trim(x), val);
        free(val);
        return 0;
    }
    if (*r == '"') {
        char *q = strrchr(r + 1, '"');
        r++;
        if (q) *q = 0;
    }
    eq = strchr(r, '=');
    if (!eq) {
        /* list variables starting with the given prefix */
        sbuf_t tmp, *save = out_cap;
        bool any;
        sb_init(&tmp);
        out_cap = &tmp;
        env_list(r);
        out_cap = save;
        any = tmp.len != 0;
        if (any) out_puts(tmp.buf);
        sb_free(&tmp);
        if (!any) {
            err_out("Environment variable ");
            err_out(r);
            err_out(" not defined\n");
            return 1;
        }
        return 0;
    }
    *eq = 0;
    if (!*r) { syntax_err(); return 1; }
    if (!env_set(r, eq + 1)) { err_out("Not enough environment space.\n"); return 1; }
    return 0;
}

static int c_path(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    (void)argc; (void)argv;
    if (!*r) {
        const char *p = env_get("PATH");
        out_printf("PATH=%s\n", p ? p : "(null)");
        return 0;
    }
    env_set("PATH", strcmp(r, ";") ? r : NULL);
    return 0;
}

/* ---------------------------------------------------------------- */
/* directories                                                       */
static uint8_t dir_stack[8];
static int dir_sp;

static int c_cd(int argc, char **argv, char *raw)
{
    char *r = raw;
    int n;
    char *path;
    (void)argc; (void)argv;
    if (*r == ' ') r++;
    r = trim(r);
    if (!strnicmp_(r, "/d", 2) && (r[2] == ' ' || !r[2])) r = trim(r + 2);
    unquote(r);
    if (!*r) {
        path = malloc(LINE_MAX);
        if (!path) return 1;
        fs_path(fs_cwd, path);
        out_printf("%s\n", path);
        free(path);
        return 0;
    }
    if (strlen(r) == 2 && r[1] == ':') return 0;
    n = fs_resolve(r);
    if (n < 0) { path_not_found(); return 1; }
    if (!(fs_nodes[n].attr & FA_DIR)) { err_out("The directory name is invalid.\n"); return 1; }
    fs_cwd = (uint8_t)n;
    return 0;
}

static int c_pushd(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    int n;
    (void)argc; (void)argv;
    unquote(r);
    if (!*r) {
        for (int i = dir_sp - 1; i >= 0; i--) {
            fs_path(dir_stack[i], walk_path);
            out_printf("%s\n", walk_path);
        }
        return 0;
    }
    n = fs_resolve(r);
    if (n < 0 || !(fs_nodes[n].attr & FA_DIR)) { path_not_found(); return 1; }
    if (dir_sp < 8) dir_stack[dir_sp++] = fs_cwd;
    fs_cwd = (uint8_t)n;
    return 0;
}

static int c_popd(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    if (dir_sp) {
        uint8_t n = dir_stack[--dir_sp];
        if (fs_nodes[n].attr & FA_DIR) fs_cwd = n;
    }
    return 0;
}

static int c_md(int argc, char **argv, char *raw)
{
    char *names[8];
    int n = plain_args(argc, argv, names, 8), rc = 0;
    (void)raw;
    if (!n) { syntax_err(); return 1; }
    for (int i = 0; i < n; i++) {
        if (fs_resolve(names[i]) >= 0) {
            err_out("A subdirectory or file ");
            err_out(names[i]);
            err_out(" already exists.\n");
            rc = 1;
        } else if (fs_mkdirs(names[i]) < 0) {
            err_out("The filename, directory name, or volume label syntax is incorrect.\n");
            rc = 1;
        }
    }
    return rc;
}

static int rm_tree(uint8_t n)
{
    int cnt = 0;
    for (int i = 1; i < FS_MAX; i++)
        if ((fs_nodes[i].attr & FA_USED) && fs_nodes[i].parent == n && i != n)
            cnt += rm_tree((uint8_t)i);
    fs_remove(n);
    return cnt + 1;
}

static int c_rd(int argc, char **argv, char *raw)
{
    char *names[8];
    bool s = has_sw(argc, argv, "/s"), q = has_sw(argc, argv, "/q");
    int n = plain_args(argc, argv, names, 8), rc = 0;
    (void)raw;
    if (!n) { syntax_err(); return 1; }
    for (int i = 0; i < n; i++) {
        int d = fs_resolve(names[i]);
        bool empty = true;
        if (d < 0) { not_found(); rc = 1; continue; }
        if (!(fs_nodes[d].attr & FA_DIR)) { err_out("The directory name is invalid.\n"); rc = 1; continue; }
        if (d == 0 || fs_is_ancestor((uint8_t)d, fs_cwd)) {
            err_out("The process cannot access the file because it is being used by another process.\n");
            rc = 1;
            continue;
        }
        for (int k = 1; k < FS_MAX; k++)
            if ((fs_nodes[k].attr & FA_USED) && fs_nodes[k].parent == d) { empty = false; break; }
        if (!empty && !s) { err_out("The directory is not empty.\n"); rc = 1; continue; }
        if (!empty && !q) {
            char msg[80];
            snprintf(msg, sizeof msg, "%s, Are you sure", names[i]);
            if (!ask_yn(msg)) continue;
        }
        rm_tree((uint8_t)d);
    }
    return rc;
}

/* ---------------------------------------------------------------- */
/* DIR                                                               */
typedef struct {
    bool bare, wide, recurse, all, page, dirs_only, files_only;
    uint32_t files, bytes, dirs;
    uint32_t tfiles, tbytes;
    bool header_done;
} diropt_t;

static bool dir_show(diropt_t *o, uint8_t n)
{
    uint8_t a = fs_nodes[n].attr;
    if ((a & (FA_HIDDEN | FA_SYS)) && !o->all) return false;
    if (o->dirs_only && !(a & FA_DIR)) return false;
    if (o->files_only && (a & FA_DIR)) return false;
    return true;
}

static void dir_line(diropt_t *o, const char *name, uint8_t n, bool dir, int *col)
{
    char ds[10], ts[8], sz[16];
    if (o->wide) {
        char w[16];
        if (dir) snprintf(w, sizeof w, "[%.11s]", name);
        else snprintf(w, sizeof w, "%.12s", name);
        out_printf("%-13s", w);
        if (++*col == 3) { *col = 0; out_nl(); pager_line(); }
        return;
    }
    fmt_date(fs_nodes[n].date, ds);
    fmt_time(fs_nodes[n].time, ts);
    if (dir) strcpy(sz, "<DIR>    ");
    else { fmt_num(fs_nodes[n].size, sz); }
    out_printf("%s %s %9s %s\n", ds, ts, sz, name);
    pager_line();
}

static bool dir_list(diropt_t *o, uint8_t d, const char *pat)
{
    uint8_t *kids = malloc(FS_MAX);
    char *path = malloc(LINE_MAX);
    int n, col = 0;
    bool any = false;
    if (!kids || !path) { free(kids); free(path); return false; }
    n = fs_list(d, kids, FS_MAX);
    fs_path(d, path);
    o->files = o->bytes = o->dirs = 0;

    for (int i = 0; i < n; i++)
        if (dir_show(o, kids[i]) && wild_match(pat, fs_nodes[kids[i]].name)) { any = true; break; }
    if (!any && !(!strcmp(pat, "*") && !o->recurse)) goto sub;

    if (!o->bare) {
        out_printf("\n Directory of %s\n\n", path);
        pager_line(); pager_line(); pager_line();
        if (d != 0 && !strcmp(pat, "*") && !o->files_only) {
            dir_line(o, ".", d, true, &col);
            dir_line(o, "..", fs_nodes[d].parent, true, &col);
            o->dirs += 2;
        }
    }
    for (int i = 0; i < n && !g_break; i++) {
        uint8_t k = kids[i];
        bool isdir = fs_nodes[k].attr & FA_DIR;
        if (!dir_show(o, k) || !wild_match(pat, fs_nodes[k].name)) continue;
        if (o->bare) {
            if (o->recurse) {
                fs_path(k, path);
                out_printf("%s\n", path);
            } else out_printf("%s\n", fs_nodes[k].name);
            pager_line();
        } else dir_line(o, fs_nodes[k].name, k, isdir, &col);
        if (isdir) o->dirs++;
        else { o->files++; o->bytes += fs_nodes[k].size; }
    }
    if (col) out_nl();
    if (!o->bare) {
        char b[16];
        fmt_num(o->bytes, b);
        out_printf("%8lu File(s) %12s bytes\n", (unsigned long)o->files, b);
        pager_line();
        if (!o->recurse) {
            fmt_num(fs_free_arc(), b);
            out_printf("%8lu Dir(s)  %12s bytes free\n", (unsigned long)o->dirs, b);
            pager_line();
        }
    }
    o->tfiles += o->files;
    o->tbytes += o->bytes;
sub:
    if (o->recurse) {
        for (int i = 0; i < n && !g_break; i++)
            if ((fs_nodes[kids[i]].attr & FA_DIR) && (o->all || !(fs_nodes[kids[i]].attr & FA_HIDDEN)))
                if (dir_list(o, kids[i], pat)) any = true;
    }
    free(kids);
    free(path);
    return any;
}

static int c_dir(int argc, char **argv, char *raw)
{
    diropt_t o;
    char *pats[6];
    int np, rc = 0;
    (void)raw;
    memset(&o, 0, sizeof o);
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/') continue;
        for (char *c = a + 1; *c; c++) {
            switch (tolower((unsigned char)*c)) {
            case 'b': o.bare = true; break;
            case 'w': o.wide = true; break;
            case 's': o.recurse = true; break;
            case 'p': o.page = true; break;
            case 'a':
                o.all = true;
                if (c[1] == ':') c++;
                if (c[1] == '-' && tolower((unsigned char)c[2]) == 'd') { o.files_only = true; c += 2; }
                else if (tolower((unsigned char)c[1]) == 'd') { o.dirs_only = true; c++; }
                else if (tolower((unsigned char)c[1]) == 'h') c++;
                break;
            case 'o': if (c[1] == ':') c++; if (c[1] == '-') c++; if (c[1]) c++; break;
            default: break;
            }
        }
    }
    if (o.wide && o.bare) o.wide = false;
    np = plain_args(argc, argv, pats, 6);
    if (!np) { pats[0] = "."; np = 1; }
    pager_reset(o.page);
    if (!o.bare) {
        out_printf(" Volume in drive C is %s\n Volume Serial Number is " SERIAL "\n", fs_label);
        pager_line(); pager_line();
    }
    for (int i = 0; i < np; i++) {
        int n = fs_resolve(pats[i]);
        char name[FS_NAMELEN];
        if (n >= 0 && (fs_nodes[n].attr & FA_DIR)) {
            dir_list(&o, (uint8_t)n, "*");
        } else {
            int p = fs_split(pats[i], name);
            if (p < 0) { path_not_found(); rc = 1; continue; }
            if (!dir_list(&o, (uint8_t)p, name)) {
                if (!o.bare) out_puts("File Not Found\n");
                else err_out("File Not Found\n");
                rc = 1;
            }
        }
    }
    if (o.recurse && !o.bare) {
        char b[16];
        fmt_num(o.tbytes, b);
        out_printf("\n     Total Files Listed:\n%8lu File(s) %12s bytes\n", (unsigned long)o.tfiles, b);
        fmt_num(fs_free_arc(), b);
        out_printf("%17s%12s bytes free\n", "", b);
    }
    return rc;
}

/* ---------------------------------------------------------------- */
/* TREE                                                              */
static void tree_walk(uint8_t d, char *prefix, bool files)
{
    uint8_t *kids = malloc(FS_MAX);
    int n, shown = 0, last = -1, lastf = -1;
    size_t pl = strlen(prefix);
    if (!kids) return;
    n = fs_list(d, kids, FS_MAX);
    for (int i = 0; i < n; i++) {
        if (fs_nodes[kids[i]].attr & FA_HIDDEN) continue;
        if (fs_nodes[kids[i]].attr & FA_DIR) last = i;
        else lastf = i;
    }
    if (files) {
        for (int i = 0; i < n && !g_break; i++) {
            uint8_t k = kids[i];
            if ((fs_nodes[k].attr & (FA_DIR | FA_HIDDEN))) continue;
            out_printf("%s%s   %s\n", prefix, last >= 0 ? "|" : " ", fs_nodes[k].name);
            shown++;
            if (!pager_line()) break;
        }
        if (shown) { out_printf("%s%s\n", prefix, last >= 0 ? "|" : ""); pager_line(); }
    }
    (void)lastf;
    for (int i = 0; i < n && !g_break; i++) {
        uint8_t k = kids[i];
        if (!(fs_nodes[k].attr & FA_DIR) || (fs_nodes[k].attr & FA_HIDDEN)) continue;
        out_printf("%s%s---%s\n", prefix, i == last ? "\\" : "+", fs_nodes[k].name);
        if (!pager_line()) break;
        if (pl + 4 < 60) {
            strcat(prefix, i == last ? "    " : "|   ");
            tree_walk(k, prefix, files);
            prefix[pl] = 0;
        }
    }
    free(kids);
}

static int c_tree(int argc, char **argv, char *raw)
{
    char *p[2];
    int n = plain_args(argc, argv, p, 2), d;
    char *buf = malloc(LINE_MAX);
    char prefix[64] = "";
    (void)raw;
    if (!buf) return 1;
    d = n ? fs_resolve(p[0]) : fs_cwd;
    if (d < 0 || !(fs_nodes[d].attr & FA_DIR)) { free(buf); err_out("Invalid path\n"); return 1; }
    pager_reset(true);
    fs_path((uint8_t)d, buf);
    out_printf("Folder PATH listing for volume %s\nVolume serial number is " SERIAL "\n%s\n", fs_label, buf);
    tree_walk((uint8_t)d, prefix, has_sw(argc, argv, "/f"));
    free(buf);
    return 0;
}

/* ---------------------------------------------------------------- */
/* TYPE / MORE / FIND / FINDSTR / SORT / FC                          */
static int c_type(int argc, char **argv, char *raw)
{
    char *names[8];
    int n = plain_args(argc, argv, names, 8), rc = 0;
    uint8_t *m;
    (void)raw;
    if (!n) {
        if (g_stdin && g_stdin->buf) { out_puts(g_stdin->buf + g_stdin_pos); return 0; }
        syntax_err();
        return 1;
    }
    m = malloc(FS_MAX);
    if (!m) return 1;
    for (int i = 0; i < n; i++) {
        int c;
        if (!stricmp_(names[i], "nul")) continue;
        c = fs_glob(names[i], m, FS_MAX, NULL);
        if (!c) { not_found(); err_out("Error occurred while processing: "); err_out(names[i]); err_out(".\n"); rc = 1; continue; }
        for (int k = 0; k < c && !g_break; k++) {
            char *t;
            if (fs_nodes[m[k]].attr & FA_DIR) {
                if (c == 1) { err_out("Access is denied.\n"); rc = 1; }
                continue;
            }
            if (c > 1 || n > 1) out_printf("\n%s\n\n\n", fs_nodes[m[k]].name);
            t = fs_read(m[k], NULL);
            if (!t) { err_out("Not enough memory.\n"); rc = 1; break; }
            pager_reset(false);
            print_text(t);
            free(t);
        }
    }
    free(m);
    return rc;
}

/* iterate lines of a buffer */
static char *next_line(char **p, char *buf, int max)
{
    int n = 0;
    char *s = *p;
    if (!s || !*s) return NULL;
    while (*s && *s != '\n') {
        if (*s != '\r' && n < max - 1) buf[n++] = *s;
        s++;
    }
    if (*s == '\n') s++;
    buf[n] = 0;
    *p = s;
    return buf;
}

/* load either named file or stdin into a malloc'd buffer */
static char *load_src(const char *name, bool *ok)
{
    *ok = true;
    if (name) {
        int n = fs_resolve(name);
        if (n < 0 || (fs_nodes[n].attr & FA_DIR)) {
            err_out("File not found - ");
            err_out(name);
            err_out("\n");
            *ok = false;
            return NULL;
        }
        return fs_read((uint8_t)n, NULL);
    }
    if (g_stdin && g_stdin->buf) {
        char *c = malloc(g_stdin->len - g_stdin_pos + 1);
        if (c) strcpy(c, g_stdin->buf + g_stdin_pos);
        return c;
    }
    {
        char *c = malloc(1);
        if (c) *c = 0;
        return c;
    }
}

static int c_more(int argc, char **argv, char *raw)
{
    char *names[1];
    bool ok;
    char *t;
    (void)raw;
    t = load_src(plain_args(argc, argv, names, 1) ? names[0] : NULL, &ok);
    if (!ok) return 1;
    if (!t) return 1;
    pager_reset(true);
    print_text(t);
    if (*t && t[strlen(t) - 1] != '\n') out_nl();
    free(t);
    return 0;
}

static int c_find(int argc, char **argv, char *raw)
{
    bool v = false, cnt = false, num = false, ic = false;
    char *str = NULL, *files[6];
    int nf = 0, found = 0;
    char *line = malloc(LINE_MAX);
    (void)raw;
    if (!line) return 1;
    for (int i = 1; i < argc; i++) {
        if (is_switch(argv[i], "/v")) v = true;
        else if (is_switch(argv[i], "/c")) cnt = true;
        else if (is_switch(argv[i], "/n")) num = true;
        else if (is_switch(argv[i], "/i")) ic = true;
        else if (!str) str = argv[i];
        else if (nf < 6) files[nf++] = argv[i];
    }
    if (!str) { free(line); err_out("FIND: Parameter format not correct\n"); return 2; }
    for (int f = 0; f < (nf ? nf : 1); f++) {
        bool ok;
        char *t = load_src(nf ? files[f] : NULL, &ok), *p;
        int ln = 0, c = 0;
        if (!ok || !t) { found = -1000; free(t); continue; }
        if (nf) {
            char up[FS_NAMELEN + 8];
            strncpy(up, files[f], sizeof up - 1);
            up[sizeof up - 1] = 0;
            str_upper(up);
            out_printf("\n---------- %s%s", up, cnt ? ": " : "\n");
        }
        p = t;
        while (next_line(&p, line, LINE_MAX) && !g_break) {
            bool m = ic ? stristr_(line, str) != NULL : strstr(line, str) != NULL;
            ln++;
            if (m == v) continue;
            c++;
            if (!cnt) {
                if (num) out_printf("[%d]", ln);
                out_printf("%s\n", line);
            }
        }
        if (cnt) out_printf("%d\n", c);
        found += c;
        free(t);
    }
    free(line);
    return found > 0 ? 0 : 1;
}

static int c_findstr(int argc, char **argv, char *raw)
{
    bool v = false, num = false, ic = false, lit = false, beg = false, end = false, monly = false;
    char *pats[6], *files[6];
    int np = 0, nf = 0, found = 0;
    char *line = malloc(LINE_MAX), *re = malloc(LINE_MAX);
    (void)raw;
    if (!line || !re) { free(line); free(re); return 1; }
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '/' && a[1]) {
            if (!strnicmp_(a, "/c:", 3)) { if (np < 6) pats[np++] = a + 3; continue; }
            for (char *c = a + 1; *c; c++) {
                switch (tolower((unsigned char)*c)) {
                case 'v': v = true; break;
                case 'n': num = true; break;
                case 'i': ic = true; break;
                case 'l': lit = true; break;
                case 'r': lit = false; break;
                case 'b': beg = true; break;
                case 'e': end = true; break;
                case 'm': monly = true; break;
                default: break;
                }
            }
        } else if (!np) {
            /* space separated words are alternatives */
            char *s = a;
            while (*s && np < 6) {
                while (*s == ' ') s++;
                if (!*s) break;
                pats[np++] = s;
                while (*s && *s != ' ') s++;
                if (*s) *s++ = 0;
            }
        } else if (nf < 6) files[nf++] = a;
    }
    if (!np) { free(line); free(re); err_out("FINDSTR: Bad command line\n"); return 2; }
    for (int f = 0; f < (nf ? nf : 1); f++) {
        bool ok;
        uint8_t *m = NULL;
        int mc = 1;
        if (nf && has_wild(files[f])) {
            m = malloc(FS_MAX);
            if (!m) break;
            mc = fs_glob(files[f], m, FS_MAX, NULL);
        }
        for (int fi = 0; fi < mc; fi++) {
            const char *fname = nf ? (m ? fs_nodes[m[fi]].name : files[f]) : NULL;
            char *t, *p;
            int ln = 0;
            bool fileshown = false;
            if (m) {
                if (fs_nodes[m[fi]].attr & FA_DIR) continue;
                t = fs_read(m[fi], NULL);
                ok = t != NULL;
            } else t = load_src(fname, &ok);
            if (!ok || !t) { free(t); continue; }
            p = t;
            while (next_line(&p, line, LINE_MAX) && !g_break) {
                bool hit = false;
                ln++;
                for (int k = 0; k < np && !hit; k++) {
                    if (lit) {
                        size_t pl = strlen(pats[k]), ll = strlen(line);
                        if (beg) hit = ic ? !strnicmp_(line, pats[k], pl) : !strncmp(line, pats[k], pl);
                        else if (end) hit = ll >= pl && (ic ? !stricmp_(line + ll - pl, pats[k]) : !strcmp(line + ll - pl, pats[k]));
                        else hit = ic ? stristr_(line, pats[k]) != NULL : strstr(line, pats[k]) != NULL;
                    } else {
                        snprintf(re, LINE_MAX, "%s%s%s", beg ? "^" : "", pats[k], end ? "$" : "");
                        hit = re_search(re, line, ic);
                    }
                }
                if (hit == v) continue;
                found++;
                if (monly) {
                    if (!fileshown && fname) out_printf("%s\n", fname);
                    fileshown = true;
                    break;
                }
                if (nf > 1 || m) out_printf("%s:", fname);
                if (num) out_printf("%d:", ln);
                out_printf("%s\n", line);
            }
            free(t);
        }
        free(m);
    }
    free(line);
    free(re);
    return found ? 0 : 1;
}

static int line_cmp(const void *a, const void *b)
{
    return stricmp_(*(char *const *)a, *(char *const *)b);
}

static int c_sort(int argc, char **argv, char *raw)
{
    char *names[1];
    bool ok, rev = has_sw(argc, argv, "/r");
    char *t, **lines;
    int n = 0, cap;
    (void)raw;
    t = load_src(plain_args(argc, argv, names, 1) ? names[0] : NULL, &ok);
    if (!ok || !t) return 1;
    cap = 1;
    for (char *c = t; *c; c++) if (*c == '\n') cap++;
    lines = malloc(cap * sizeof(char *));
    if (!lines) { free(t); return 1; }
    for (char *s = t; s && *s;) {
        char *nl = strchr(s, '\n');
        lines[n++] = s;
        if (nl) { *nl = 0; if (nl > s && nl[-1] == '\r') nl[-1] = 0; s = nl + 1; }
        else s = NULL;
    }
    qsort(lines, n, sizeof(char *), line_cmp);
    for (int i = 0; i < n && !g_break; i++) {
        out_puts(lines[rev ? n - 1 - i : i]);
        out_nl();
    }
    free(lines);
    free(t);
    return 0;
}

static int c_fc(int argc, char **argv, char *raw)
{
    char *names[2];
    bool ok1, ok2, ic = has_sw(argc, argv, "/c"), numl = has_sw(argc, argv, "/n");
    char *a, *b, *pa, *pb;
    char *la = malloc(LINE_MAX), *lb = malloc(LINE_MAX);
    int diffs = 0, ln = 0;
    (void)raw;
    if (!la || !lb) { free(la); free(lb); return 1; }
    if (plain_args(argc, argv, names, 2) < 2) { free(la); free(lb); err_out("FC: Insufficient number of file specifications\n"); return 2; }
    a = load_src(names[0], &ok1);
    b = load_src(names[1], &ok2);
    if (!ok1 || !ok2 || !a || !b) { free(a); free(b); free(la); free(lb); return 2; }
    out_printf("Comparing files %s and %s\n", names[0], names[1]);
    pa = a;
    pb = b;
    for (;;) {
        char *x = next_line(&pa, la, LINE_MAX), *y = next_line(&pb, lb, LINE_MAX);
        ln++;
        if (!x && !y) break;
        if (x && y && !(ic ? stricmp_(x, y) : strcmp(x, y))) continue;
        diffs++;
        out_printf("***** %s\n", names[0]);
        if (x) { if (numl) out_printf("%5d:  ", ln); out_printf("%s\n", x); }
        out_printf("***** %s\n", names[1]);
        if (y) { if (numl) out_printf("%5d:  ", ln); out_printf("%s\n", y); }
        out_puts("*****\n\n");
        if (g_break || diffs > 50) break;
    }
    if (!diffs) out_puts("FC: no differences encountered\n");
    free(a); free(b); free(la); free(lb);
    return diffs ? 1 : 0;
}

/* ---------------------------------------------------------------- */
/* COPY / XCOPY / MOVE / REN / DEL / ATTRIB                          */
static bool copy_node(uint8_t src, uint8_t dparent, const char *dname, bool overwrite_ok)
{
    size_t sz;
    char *t;
    int d = fs_child(dparent, dname);
    if (d == src) { err_out("The file cannot be copied onto itself.\n"); return false; }
    if (d >= 0) {
        if (fs_nodes[d].attr & (FA_DIR | FA_RO)) { err_out("Access is denied.\n"); return false; }
        if (!overwrite_ok) {
            char q[64];
            snprintf(q, sizeof q, "Overwrite %s", dname);
            if (!ask_yn(q)) return false;
        }
    } else {
        d = fs_create(dparent, dname, false);
        if (d < 0) { err_out("Cannot create file (directory full?).\n"); return false; }
    }
    t = fs_read(src, &sz);
    if (!t) { err_out("Not enough memory.\n"); return false; }
    if (!fs_write((uint8_t)d, t, sz)) { free(t); err_out("There is not enough space on the disk.\n"); return false; }
    free(t);
    return true;
}

/* resolve destination: returns parent dir + name to use */
static bool dest_for(const char *dst, const char *srcname, uint8_t *dp, char *dname)
{
    int n;
    if (!dst) { *dp = fs_cwd; strcpy(dname, srcname); return true; }
    n = fs_resolve(dst);
    if (n >= 0 && (fs_nodes[n].attr & FA_DIR)) { *dp = (uint8_t)n; strcpy(dname, srcname); return true; }
    n = fs_split(dst, dname);
    if (n < 0 || !fs_valid_name(dname)) { path_not_found(); return false; }
    *dp = (uint8_t)n;
    return true;
}

static int copy_con(const char *dst)
{
    sbuf_t b;
    char *line = malloc(LINE_MAX);
    int e;
    if (!line) return 1;
    sb_init(&b);
    out_puts("(type ^Z on its own line or press ON to end)\n");
    for (;;) {
        line[0] = 0;
        if (read_line(line, LINE_MAX, false)) break;
        if (!stricmp_(line, "^z")) break;
        sb_puts(&b, line);
        sb_putc(&b, '\n');
    }
    free(line);
    e = fs_write_path(dst, b.buf ? b.buf : "", b.len, false);
    sb_free(&b);
    if (e) { err_out("Cannot write file.\n"); return 1; }
    out_puts("        1 file(s) copied.\n");
    return 0;
}

static int c_copy(int argc, char **argv, char *raw)
{
    char *p[2];
    int np = plain_args(argc, argv, p, 2), copied = 0;
    bool y = has_sw(argc, argv, "/y") || g_in_batch;
    uint8_t *m;
    (void)raw;
    if (!np) { syntax_err(); return 1; }
    if (!stricmp_(p[0], "con")) {
        if (np < 2) { syntax_err(); return 1; }
        return copy_con(p[1]);
    }
    if (strchr(p[0], '+')) {
        /* concatenation: copy a+b+c dest */
        sbuf_t b;
        char *s = p[0];
        int e;
        sb_init(&b);
        while (s && *s) {
            char *plus = strchr(s, '+');
            int n;
            if (plus) *plus = 0;
            n = fs_resolve(s);
            if (n < 0 || (fs_nodes[n].attr & FA_DIR)) { sb_free(&b); not_found(); return 1; }
            {
                char *t = fs_read((uint8_t)n, NULL);
                if (t) { sb_puts(&b, t); free(t); }
            }
            s = plus ? plus + 1 : NULL;
        }
        e = fs_write_path(np > 1 ? p[1] : p[0], b.buf ? b.buf : "", b.len, false);
        sb_free(&b);
        if (e) { err_out("Cannot write file.\n"); return 1; }
        out_puts("        1 file(s) copied.\n");
        return 0;
    }
    m = malloc(FS_MAX);
    if (!m) return 1;
    {
        int c = fs_glob(p[0], m, FS_MAX, NULL);
        int n0 = fs_resolve(p[0]);
        if (n0 >= 0 && (fs_nodes[n0].attr & FA_DIR)) {
            /* copying a directory copies its files */
            c = 0;
            for (int i = 1; i < FS_MAX; i++)
                if ((fs_nodes[i].attr & (FA_USED | FA_DIR)) == FA_USED && fs_nodes[i].parent == n0) m[c++] = (uint8_t)i;
        }
        if (!c) { free(m); not_found(); out_puts("        0 file(s) copied.\n"); return 1; }
        for (int i = 0; i < c && !g_break; i++) {
            uint8_t dp;
            char dname[FS_NAMELEN];
            if (fs_nodes[m[i]].attr & FA_DIR) continue;
            if (!dest_for(np > 1 ? p[1] : NULL, fs_nodes[m[i]].name, &dp, dname)) break;
            if (c > 1) { char pp[LINE_MAX]; fs_path(m[i], pp); out_printf("%s\n", pp); }
            if (copy_node(m[i], dp, dname, y)) copied++;
        }
    }
    free(m);
    out_printf("%9d file(s) copied.\n", copied);
    return copied ? 0 : 1;
}

static int xcopy_dir(uint8_t src, uint8_t dst, bool sub, bool quiet, bool y)
{
    int cnt = 0;
    uint8_t *kids = malloc(FS_MAX);
    int n;
    if (!kids) return 0;
    n = fs_list(src, kids, FS_MAX);
    for (int i = 0; i < n && !g_break; i++) {
        uint8_t k = kids[i];
        if (fs_nodes[k].attr & FA_DIR) {
            int d;
            if (!sub || fs_is_ancestor(k, dst)) continue;
            d = fs_child(dst, fs_nodes[k].name);
            if (d < 0) d = fs_create(dst, fs_nodes[k].name, true);
            if (d >= 0 && (fs_nodes[d].attr & FA_DIR)) cnt += xcopy_dir(k, (uint8_t)d, sub, quiet, y);
        } else {
            if (!quiet) { fs_path(k, walk_path); out_puts(walk_path); out_nl(); }
            if (copy_node(k, dst, fs_nodes[k].name, y)) cnt++;
        }
    }
    free(kids);
    return cnt;
}

static int c_xcopy(int argc, char **argv, char *raw)
{
    char *p[2];
    int np = plain_args(argc, argv, p, 2), s, d, cnt;
    bool sub = has_sw(argc, argv, "/s") || has_sw(argc, argv, "/e");
    bool q = has_sw(argc, argv, "/q"), y = has_sw(argc, argv, "/y") || g_in_batch;
    (void)raw;
    if (!np) { syntax_err(); return 4; }
    s = fs_resolve(p[0]);
    if (s < 0) { err_out("File not found - "); err_out(p[0]); err_out("\n"); return 4; }
    if (!(fs_nodes[s].attr & FA_DIR)) return c_copy(argc, argv, raw);
    d = np > 1 ? fs_resolve(p[1]) : fs_cwd;
    if (d < 0) d = fs_mkdirs(p[1]);
    if (d < 0 || !(fs_nodes[d].attr & FA_DIR)) { err_out("Invalid path\n"); return 4; }
    if (fs_is_ancestor((uint8_t)s, (uint8_t)d) && sub) { err_out("Cannot perform a cyclic copy\n"); return 4; }
    cnt = xcopy_dir((uint8_t)s, (uint8_t)d, sub, q, y);
    out_printf("%d File(s) copied\n", cnt);
    return 0;
}

static int c_move(int argc, char **argv, char *raw)
{
    char *p[2];
    int np = plain_args(argc, argv, p, 2), moved = 0, c;
    uint8_t *m;
    (void)raw;
    if (np < 2) { syntax_err(); return 1; }
    m = malloc(FS_MAX);
    if (!m) return 1;
    c = fs_glob(p[0], m, FS_MAX, NULL);
    if (!c) { free(m); not_found(); return 1; }
    for (int i = 0; i < c; i++) {
        uint8_t dp;
        char dname[FS_NAMELEN];
        int ex;
        if (!dest_for(p[1], fs_nodes[m[i]].name, &dp, dname)) break;
        if ((fs_nodes[m[i]].attr & FA_DIR) && fs_is_ancestor(m[i], dp)) {
            err_out("The process cannot access the file because it is being used by another process.\n");
            continue;
        }
        ex = fs_child(dp, dname);
        if (ex >= 0 && ex != m[i]) {
            if (fs_nodes[ex].attr & FA_DIR) { err_out("Access is denied.\n"); continue; }
            if (!g_in_batch && !has_sw(argc, argv, "/y")) {
                char q[64];
                snprintf(q, sizeof q, "Overwrite %s", dname);
                if (!ask_yn(q)) continue;
            }
            fs_remove((uint8_t)ex);
        }
        fs_nodes[m[i]].parent = dp;
        strcpy(fs_nodes[m[i]].name, dname);
        moved++;
    }
    fs_save();
    free(m);
    out_printf("%9d file(s) moved.\n", moved);
    return moved ? 0 : 1;
}

/* apply CMD-style wildcard rename: "*.bak", "new?.txt" ... */
static void wild_rename(const char *src, const char *pat, char *out)
{
    int o = 0;
    const char *s = src;
    for (const char *p = pat; *p && o < FS_NAMELEN - 1; p++) {
        if (*p == '*') {
            const char *stop = strchr(p + 1, '.') ? strrchr(s, '.') : NULL;
            if (p[1] == '.' && stop) { while (s < stop && o < FS_NAMELEN - 1) out[o++] = *s++; }
            else if (!p[1]) { while (*s && o < FS_NAMELEN - 1) out[o++] = *s++; }
            else { while (*s && *s != p[1] && o < FS_NAMELEN - 1) out[o++] = *s++; }
        } else if (*p == '?') {
            if (*s && *s != '.') out[o++] = *s++;
        } else {
            out[o++] = *p;
            if (*s && *s != '.') s++;
            else if (*s == '.' && *p == '.') s++;
        }
    }
    out[o] = 0;
}

static int c_ren(int argc, char **argv, char *raw)
{
    char *p[2];
    int c, done = 0;
    uint8_t *m, dir;
    (void)raw;
    if (plain_args(argc, argv, p, 2) < 2) { syntax_err(); return 1; }
    if (strchr(p[1], '\\') || strchr(p[1], ':')) { syntax_err(); return 1; }
    m = malloc(FS_MAX);
    if (!m) return 1;
    c = fs_glob(p[0], m, FS_MAX, &dir);
    if (!c) { free(m); not_found(); return 1; }
    for (int i = 0; i < c; i++) {
        char nn[FS_NAMELEN];
        if (has_wild(p[1])) wild_rename(fs_nodes[m[i]].name, p[1], nn);
        else { strncpy(nn, p[1], FS_NAMELEN - 1); nn[FS_NAMELEN - 1] = 0; }
        if (!fs_valid_name(nn)) { syntax_err(); continue; }
        if (fs_child(fs_nodes[m[i]].parent, nn) >= 0 && stricmp_(nn, fs_nodes[m[i]].name)) {
            err_out("A duplicate file name exists, or the file cannot be found.\n");
            continue;
        }
        strcpy(fs_nodes[m[i]].name, nn);
        done++;
    }
    fs_save();
    free(m);
    return done ? 0 : 1;
}

static int del_in(uint8_t dir, const char *pat, bool sub, bool force, bool quiet_print, int *cnt)
{
    uint8_t *kids = malloc(FS_MAX);
    int n, rc = 0;
    if (!kids) return 1;
    n = fs_list(dir, kids, FS_MAX);
    for (int i = 0; i < n && !g_break; i++) {
        uint8_t k = kids[i];
        if (fs_nodes[k].attr & FA_DIR) {
            if (sub) del_in(k, pat, sub, force, quiet_print, cnt);
            continue;
        }
        if (!wild_match(pat, fs_nodes[k].name)) continue;
        if ((fs_nodes[k].attr & FA_RO) && !force) {
            fs_path(k, walk_path);
            err_out("Access is denied - ");
            err_out(walk_path);
            err_out("\n");
            rc = 1;
            continue;
        }
        if (sub && !quiet_print) {
            fs_path(k, walk_path);
            out_printf("Deleted file - %s\n", walk_path);
        }
        fs_remove(k);
        (*cnt)++;
    }
    free(kids);
    return rc;
}

static int c_del(int argc, char **argv, char *raw)
{
    char *names[8];
    int n = plain_args(argc, argv, names, 8), rc = 0;
    bool q = has_sw(argc, argv, "/q"), s = has_sw(argc, argv, "/s"), f = has_sw(argc, argv, "/f");
    (void)raw;
    if (!n) { syntax_err(); return 1; }
    for (int i = 0; i < n; i++) {
        int t = fs_resolve(names[i]), cnt = 0;
        char pat[FS_NAMELEN];
        int dir;
        if (t >= 0 && (fs_nodes[t].attr & FA_DIR)) {
            char *path = malloc(LINE_MAX);
            if (!path) return 1;
            fs_path((uint8_t)t, path);
            strcat(path, "\\*");
            if (!q && !g_in_batch && !ask_yn(path)) { free(path); continue; }
            free(path);
            dir = t;
            strcpy(pat, "*");
        } else {
            dir = fs_split(names[i], pat);
            if (dir < 0) { path_not_found(); rc = 1; continue; }
            if (!q && (!strcmp(pat, "*") || !strcmp(pat, "*.*")) && !g_in_batch && !ask_yn("Are you sure")) continue;
        }
        rc |= del_in((uint8_t)dir, pat, s, f, q, &cnt);
        if (!cnt && !rc) {
            char *path = malloc(LINE_MAX);
            if (path) {
                fs_path((uint8_t)dir, path);
                if (path[strlen(path) - 1] != '\\') strcat(path, "\\");
                strcat(path, pat);
                err_out("Could Not Find ");
                err_out(path);
                err_out("\n");
                free(path);
            }
            rc = 1;
        }
    }
    return rc;
}

static int c_attrib(int argc, char **argv, char *raw)
{
    uint8_t set = 0, clr = 0;
    char *names[6];
    int nn = 0, rc = 0;
    bool sub = false;
    uint8_t *m;
    (void)raw;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        uint8_t bit = 0;
        if ((a[0] == '+' || a[0] == '-') && a[1] && !a[2]) {
            switch (toupper((unsigned char)a[1])) {
            case 'R': bit = FA_RO; break;
            case 'H': bit = FA_HIDDEN; break;
            case 'S': bit = FA_SYS; break;
            case 'A': bit = FA_ARCH; break;
            }
            if (!bit) { err_out("Parameter format not correct - "); err_out(a); err_out("\n"); return 1; }
            if (a[0] == '+') set |= bit; else clr |= bit;
        } else if (is_switch(a, "/s")) sub = true;
        else if (a[0] != '/' && nn < 6) names[nn++] = a;
    }
    (void)sub;
    if (!nn) names[nn++] = "*";
    m = malloc(FS_MAX);
    if (!m) return 1;
    for (int i = 0; i < nn; i++) {
        int c = fs_glob(names[i], m, FS_MAX, NULL);
        if (!c) { err_out("File not found - "); err_out(names[i]); err_out("\n"); rc = 1; continue; }
        for (int k = 0; k < c && !g_break; k++) {
            node_t *nd = &fs_nodes[m[k]];
            if (set | clr) {
                nd->attr = (uint8_t)((nd->attr | set) & ~clr);
            } else {
                char pp[LINE_MAX];
                fs_path(m[k], pp);
                out_printf("%c  %c%c%c  %s\n", nd->attr & FA_ARCH ? 'A' : ' ', nd->attr & FA_SYS ? 'S' : ' ',
                           nd->attr & FA_HIDDEN ? 'H' : ' ', nd->attr & FA_RO ? 'R' : ' ', pp);
            }
        }
    }
    if (set | clr) fs_save();
    free(m);
    return rc;
}

/* ---------------------------------------------------------------- */
/* misc / system                                                     */
static int c_mem(int argc, char **argv, char *raw)
{
    void *fp;
    char a[16], b[16], c[16];
    (void)argc; (void)argv; (void)raw;
    fmt_num(os_MemChk(&fp), a);
    fmt_num(fs_free_arc(), b);
    fmt_num(fs_used_bytes(), c);
    out_printf("RAM free     : %12s bytes\n", a);
    out_printf("Archive free : %12s bytes\n", b);
    out_printf("Drive C: used: %12s bytes\n", c);
    out_printf("Drive C: nodes %d of %d\n", fs_used_nodes(), FS_MAX);
    return 0;
}

static int c_systeminfo(int argc, char **argv, char *raw)
{
    const system_info_t *si = os_GetSystemInfo();
    void *fp;
    char a[16], b[16];
    (void)argc; (void)argv; (void)raw;
    fmt_num(os_MemChk(&fp), a);
    fmt_num(fs_free_arc(), b);
    pager_reset(true);
    out_printf("Host Name:      %s\n", env_get("COMPUTERNAME"));
    out_printf("OS Name:        CE Command Prompt\n");
    out_printf("OS Version:     10.0.84\n");
    out_printf("TI-OS Version:  %u.%u.%u.%04u\n", si->osMajorVersion, si->osMinorVersion,
               si->osRevisionVersion, si->osBuildVersion);
    out_printf("Boot Code:      %u.%u.%u.%04u\n", si->bootMajorVersion, si->bootMinorVersion,
               si->bootRevisionVersion, si->bootBuildVersion);
    out_printf("Manufacturer:   Texas Instruments\n");
    out_printf("System Model:   %s\n", si->hardwareType ? "TI-83 Premium CE" : "TI-84 Plus CE");
    out_printf("Hardware Rev:   %u\n", si->hardwareVersion);
    out_printf("Processor:      Zilog eZ80 @ 48 MHz\n");
    out_printf("Total RAM:      154 KB (user)\n");
    out_printf("Available RAM:  %s bytes\n", a);
    out_printf("Archive Free:   %s bytes\n", b);
    out_printf("Calc ID:        %02X%02X%02X%02X%02X%02X%02X%02X\n", si->calcid[0], si->calcid[1],
               si->calcid[2], si->calcid[3], si->calcid[4], si->calcid[5], si->calcid[6], si->calcid[7]);
    out_printf("Display:        320x240, 16-bit\n");
    out_printf("Uptime:         since last [ON]\n");
    return 0;
}

static int c_hostname(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    out_printf("%s\n", env_get("COMPUTERNAME"));
    return 0;
}

static int c_whoami(int argc, char **argv, char *raw)
{
    char h[24], u[24];
    (void)argc; (void)argv; (void)raw;
    strncpy(h, env_get("COMPUTERNAME"), 23); h[23] = 0;
    strncpy(u, env_get("USERNAME") ? env_get("USERNAME") : "user", 23); u[23] = 0;
    for (char *c = h; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = u; *c; c++) *c = (char)tolower((unsigned char)*c);
    out_printf("%s\\%s\n", h, u);
    return 0;
}

/* list calculator variables like processes */
static int c_tasklist(int argc, char **argv, char *raw)
{
    void *vp = NULL;
    char *name;
    uint8_t type;
    int n = 0;
    (void)argc; (void)argv; (void)raw;
    pager_reset(true);
    out_puts("Image Name  Type        Size  Mem\n=========== ======= ========= ===\n");
    while ((name = ti_DetectAny(&vp, NULL, &type))) {
        const char *ts;
        uint8_t h;
        uint16_t sz = 0;
        bool arc = false;
        char szs[16];
        unsigned char hdr[2] = {0, 0};
        if (type != OS_TYPE_PRGM && type != OS_TYPE_PROT_PRGM && type != OS_TYPE_APPVAR) continue;
        if (!isalpha((unsigned char)name[0])) continue;
        if (type == OS_TYPE_APPVAR && !strncmp(name, "CMDF", 4)) continue; /* drive C: storage */
        if ((h = ti_OpenVar(name, "r", type))) {
            sz = ti_GetSize(h);
            arc = ti_IsArchived(h);
            if (sz >= 2) ti_Read(hdr, 2, 1, h);
            ti_Close(h);
        }
        if (type == OS_TYPE_APPVAR) ts = "AppVar";
        else if (hdr[0] == 0xEF && hdr[1] == 0x7B) ts = "ASM/C";
        else ts = type == OS_TYPE_PROT_PRGM ? "Basic*" : "Basic";
        fmt_num(sz, szs);
        out_printf("%-11s %-7s %9s %s\n", name, ts, szs, arc ? "ARC" : "RAM");
        n++;
        if (!pager_line()) break;
    }
    out_printf("%d item(s)\n", n);
    return 0;
}

static int c_taskkill(int argc, char **argv, char *raw)
{
    char *name = NULL;
    bool force = has_sw(argc, argv, "/f");
    char up[10];
    int r;
    (void)raw;
    for (int i = 1; i < argc; i++)
        if (is_switch(argv[i], "/im") && i + 1 < argc) name = argv[++i];
        else if (argv[i][0] != '/') name = argv[i];
    if (!name) { err_out("ERROR: Invalid syntax. Use /IM name\n"); return 1; }
    if (protected_var(name)) { err_out("ERROR: The process cannot be terminated (critical).\n"); return 1; }
    strncpy(up, name, 8);
    up[8] = 0;
    if (!force) {
        char q[48];
        snprintf(q, sizeof q, "Delete calculator variable %s", name);
        if (!ask_yn(q)) return 1;
    }
    str_upper(up);
    r = ti_DeleteVar(up, OS_TYPE_PRGM) || ti_DeleteVar(up, OS_TYPE_PROT_PRGM) ||
        ti_DeleteVar(name, OS_TYPE_APPVAR);
    if (!r) { out_printf("ERROR: The process \"%s\" not found.\n", name); return 128; }
    out_printf("SUCCESS: The process \"%s\" has been\nterminated.\n", name);
    return 0;
}

static int c_start(int argc, char **argv, char *raw)
{
    char *p[1];
    (void)raw;
    if (!plain_args(argc, argv, p, 1)) {
        shell_banner();
        return 0;
    }
    {
        int n = fs_resolve(p[0]);
        if (n >= 0 && !(fs_nodes[n].attr & FA_DIR)) return exec_line(p[0], false);
    }
    return start_program(p[0]);
}

static int c_export(int argc, char **argv, char *raw)
{
    char *p[2];
    int n;
    uint8_t h;
    size_t sz;
    char *t;
    (void)raw;
    if (plain_args(argc, argv, p, 2) < 2) { out_puts("Usage: export <file> <AppVarName>\n"); return 1; }
    if (strlen(p[1]) > 8 || protected_var(p[1])) { err_out("Invalid AppVar name.\n"); return 1; }
    n = fs_resolve(p[0]);
    if (n < 0 || (fs_nodes[n].attr & FA_DIR)) { not_found(); return 1; }
    t = fs_read((uint8_t)n, &sz);
    if (!t) return 1;
    h = ti_Open(p[1], "w");
    if (!h || (sz && ti_Write(t, sz, 1, h) != 1)) {
        if (h) ti_Close(h);
        free(t);
        err_out("Not enough memory.\n");
        return 1;
    }
    ti_Close(h);
    free(t);
    out_printf("Exported %u bytes to AppVar %s.\n", (unsigned)sz, p[1]);
    return 0;
}

static int c_import(int argc, char **argv, char *raw)
{
    char *p[2];
    uint8_t h;
    size_t sz;
    char *t;
    int e;
    bool prg = has_sw(argc, argv, "/p");
    (void)raw;
    if (plain_args(argc, argv, p, 2) < 2) { out_puts("Usage: import <AppVarName> <file> [/p]\n"); return 1; }
    if (prg) {
        char up[10];
        strncpy(up, p[0], 8); up[8] = 0;
        str_upper(up);
        h = ti_OpenVar(up, "r", OS_TYPE_PRGM);
        if (!h) h = ti_OpenVar(up, "r", OS_TYPE_PROT_PRGM);
    } else h = ti_Open(p[0], "r");
    if (!h) { err_out("Calculator variable not found.\n"); return 1; }
    sz = ti_GetSize(h);
    if (sz > FS_MAXFILE) { ti_Close(h); err_out("Variable too large.\n"); return 1; }
    t = malloc(sz + 1);
    if (!t) { ti_Close(h); err_out("Not enough memory.\n"); return 1; }
    if (sz) ti_Read(t, sz, 1, h);
    ti_Close(h);
    e = fs_write_path(p[1], t, sz, false);
    free(t);
    if (e) { err_out("Cannot write file.\n"); return 1; }
    out_printf("Imported %u bytes.\n", (unsigned)sz);
    return 0;
}

static int c_where(int argc, char **argv, char *raw)
{
    char *p[1];
    int found = 0;
    const char *path;
    char *cand = malloc(LINE_MAX);
    static const char *ex[] = {"", ".bat", ".cmd"};
    (void)raw;
    if (!cand) return 1;
    if (!plain_args(argc, argv, p, 1)) { free(cand); syntax_err(); return 2; }
    path = env_get("PATH");
    for (int pass = 0; pass < 12; pass++) {
        char dir[96] = ".";
        if (pass) {
            int idx = pass - 1, l = 0;
            const char *s = path;
            if (!s) break;
            while (idx > 0 && *s) { if (*s++ == ';') idx--; }
            if (!*s) break;
            while (*s && *s != ';' && l < 95) dir[l++] = *s++;
            dir[l] = 0;
        }
        for (int e = 0; e < 3; e++) {
            int n;
            snprintf(cand, LINE_MAX, "%s\\%s%s", dir, p[0], ex[e]);
            n = fs_resolve(cand);
            if (n >= 0 && !(fs_nodes[n].attr & FA_DIR)) {
                fs_path((uint8_t)n, cand);
                out_printf("%s\n", cand);
                found++;
            }
        }
    }
    if (find_cmd(p[0])) { out_printf("(internal command) %s\n", p[0]); found++; }
    free(cand);
    if (!found) { err_out("INFO: Could not find files for the given pattern(s).\n"); return 1; }
    return 0;
}

static int c_doskey(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw), *eq;
    (void)argc; (void)argv;
    if (!*r) return 0;
    if (!stricmp_(r, "/history")) {
        for (int i = 0; i < history_count(); i++) out_printf("%s\n", history_get(i));
        return 0;
    }
    if (!stricmp_(r, "/macros")) { macro_list(); return 0; }
    eq = strchr(r, '=');
    if (!eq) return 0;
    *eq = 0;
    if (!macro_set(trim(r), eq + 1)) { err_out("Too many macros.\n"); return 1; }
    return 0;
}

static int c_history(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    for (int i = 0; i < history_count(); i++) out_printf("%3d  %s\n", i + 1, history_get(i));
    return EL_KEEP;
}

static int c_format(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    out_puts("WARNING, ALL DATA ON DRIVE C:\nWILL BE LOST!\n");
    if (!ask_yn("Proceed with Format")) return 1;
    out_puts("Formatting...\n");
    con_render();
    fs_format();
    out_puts("Format complete.\n");
    return 0;
}

static int c_cmd(int argc, char **argv, char *raw)
{
    char *r = raw_rest(raw);
    (void)argc; (void)argv;
    if (!strnicmp_(r, "/c", 2) || !strnicmp_(r, "/k", 2)) {
        r = trim(r + 2);
        if (*r == '"') {
            size_t l = strlen(r);
            if (l > 1 && r[l - 1] == '"') { r[l - 1] = 0; r++; }
        }
        return exec_line(r, false);
    }
    shell_banner();
    return 0;
}

static int c_edit(int argc, char **argv, char *raw)
{
    char *p[1];
    (void)raw;
    if (!plain_args(argc, argv, p, 1)) p[0] = "untitled.txt";
    return edit_file(p[0]);
}

static int c_shutdown(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    out_puts("Shutting down. Saving drive C:...\n");
    con_render();
    g_quit = true;
    return 0;
}

static int c_keys(int argc, char **argv, char *raw)
{
    (void)argc; (void)argv; (void)raw;
    pager_reset(true);
    print_text(
        "KEYPAD MAP\n"
        "[alpha]   cycle abc -> ABC -> 123\n"
        "[2nd][alpha]  toggle abc/ABC\n"
        "[2nd]     next key types a symbol\n"
        "abc mode: letters as printed on keys,\n"
        "  0=space .=: (-)=? +=\" 3=_\n"
        "123 mode: digits + - * / ^ ( ) , .\n"
        "  (-)=- sto=> math..cos=a..f x2=x\n"
        "Any mode:\n"
        "  X,T,0,n=space  window=\\  zoom=.\n"
        "  trace=/  graph=\"  vars=%  stat=*\n"
        "  mode==\n"
        "2nd+key symbols:\n"
        "  0 =  1 !  2 @  3 #  4 $  5 %\n"
        "  6 &  7 <  8 >  9 |  . :  (-) ~\n"
        "  / \\  * *  - _  + +  ( [  ) ]\n"
        "  , ;  sto '  ^ {  x2 }  x-1 `\n"
        "  math ?  log ^  ln #\n"
        "Editing:\n"
        "  enter=run  del=backspace\n"
        "  2nd+del=delete  clear=clear line\n"
        "  left/right move, 2nd+left/right\n"
        "  = home/end, up/down = history\n"
        "  2nd+up/down = scroll back\n"
        "  y= = TAB (complete name)\n"
        "  2nd+enter = recall last command\n"
        "  ON = Ctrl+C (break)\n"
        "  2nd+mode (QUIT) = exit CMD\n");
    return EL_KEEP;
}

static int c_help(int argc, char **argv, char *raw)
{
    (void)raw;
    if (argc > 1) {
        const cmd_t *c = find_cmd(argv[1]);
        if (!c) { out_puts("This command is not supported by the\nhelp utility.\n"); return 1; }
        out_puts(c->help);
        out_nl();
        return 0;
    }
    pager_reset(true);
    out_puts("For more information on a specific\ncommand, type HELP command-name\n");
    pager_line(); pager_line();
    for (const cmd_t *c = commands; c->name; c++) {
        char line[41];
        const char *h = c->help;
        int n = snprintf(line, sizeof line, "%-8s ", c->name);
        for (char *u = line; *u && *u != ' '; u++) *u = (char)toupper((unsigned char)*u);
        while (*h && *h != '\n' && n < 39) line[n++] = *h++;
        line[n] = 0;
        out_printf("%s\n", line);
        if (!pager_line()) break;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
const cmd_t commands[] = {
    {"attrib", c_attrib, "Displays or changes file attributes.\nATTRIB [+R|-R] [+H|-H] [+S|-S] [+A|-A]\n       [files]"},
    {"call", c_call, "Calls a batch file or :label.\nCALL [path]file [args]\nCALL :label [args]"},
    {"cd", c_cd, "Displays/changes the current dir.\nCD [/D] [path]   CD ..   CD \\"},
    {"chdir", c_cd, "Same as CD."},
    {"choice", c_choice, "Prompts the user to pick a key.\nCHOICE [/C keys] [/N] [/T secs /D key]\n       [/M text]\nERRORLEVEL = index of key pressed."},
    {"cls", c_cls, "Clears the screen."},
    {"cmd", c_cmd, "Starts a command interpreter.\nCMD [/C command]"},
    {"color", c_color, "Sets console colors.\nCOLOR [attr]  attr = 2 hex digits\nbackground then foreground:\n0 Black  1 Blue   2 Green  3 Aqua\n4 Red    5 Purple 6 Yellow 7 White\n8 Gray   9 LBlue  A LGreen B LAqua\nC LRed   D LPurp  E LYell  F BWhite"},
    {"copy", c_copy, "Copies files.\nCOPY [/Y] source [dest]\nCOPY a+b dest      COPY CON file"},
    {"date", c_date, "Shows or sets the date.\nDATE [/T | mm-dd-yyyy]"},
    {"del", c_del, "Deletes files.\nDEL [/Q] [/S] [/F] names\n/S subdirs /Q quiet /F read-only too"},
    {"dir", c_dir, "Lists files and subdirectories.\nDIR [path] [/A[[:]D|-D|H]] [/B] [/P]\n    [/S] [/W]"},
    {"doskey", c_doskey, "Edits command macros/history.\nDOSKEY name=text ($* $1-$9 $T)\nDOSKEY /MACROS   DOSKEY /HISTORY"},
    {"echo", c_echo, "Displays messages, or toggles echo.\nECHO [ON|OFF]  ECHO text  ECHO."},
    {"edit", c_edit, "Full-screen text editor.\nEDIT [file]"},
    {"endlocal", c_endlocal, "Ends localization of environment\nchanges in a batch file."},
    {"erase", c_del, "Same as DEL."},
    {"exit", c_exit, "Quits CMD (saves drive C:) or a\nbatch script.\nEXIT [/B] [exitcode]"},
    {"export", c_export, "Copies a file to a calculator AppVar.\nEXPORT file APPVAR"},
    {"fc", c_fc, "Compares two files line by line.\nFC [/C] [/N] file1 file2"},
    {"find", c_find, "Searches for a text string.\nFIND [/V] [/C] [/N] [/I] \"str\" [files]"},
    {"findstr", c_findstr, "Searches with regular expressions.\nFINDSTR [/I /V /N /L /R /B /E /M]\n  [/C:str] strings [files]\nRegex: . * + ? ^ $ [abc] [^a-z] \\x"},
    {"for", cmd_for, "Runs a command for each item.\nFOR %v IN (set) DO cmd\nFOR /L %v IN (start,step,end) DO cmd\nFOR /D %v IN (*) DO cmd\nFOR /F \"opts\" %v IN (file|\"str\"|'cmd')\n    DO cmd   opts: tokens= delims=\n    skip= eol= usebackq\nUse %%v inside batch files."},
    {"format", c_format, "Erases drive C: (asks first)."},
    {"goto", c_goto, "Jumps to a :label in a batch file.\nGOTO label   GOTO :EOF"},
    {"help", c_help, "Provides help for commands.\nHELP [command]  or  command /?"},
    {"history", c_history, "Shows command history."},
    {"hostname", c_hostname, "Prints the computer name."},
    {"if", cmd_if, "Conditional processing.\nIF [/I] [NOT] a==b cmd\nIF [NOT] EXIST file cmd\nIF [NOT] DEFINED var cmd\nIF [NOT] ERRORLEVEL n cmd\nIF a EQU|NEQ|LSS|LEQ|GTR|GEQ b cmd\nIF cond (cmd) ELSE (cmd)"},
    {"import", c_import, "Copies a calculator AppVar (or\nprogram with /P) into a file.\nIMPORT VARNAME file [/P]"},
    {"keys", c_keys, "Shows the keypad map."},
    {"label", c_label, "Changes the volume label.\nLABEL [name]"},
    {"md", c_md, "Creates directories (and parents).\nMD path [path...]"},
    {"mem", c_mem, "Shows memory usage."},
    {"mkdir", c_md, "Same as MD."},
    {"more", c_more, "Shows output one screen at a time.\nMORE [file]   cmd | MORE\nENTER=line, CLEAR=quit, other=page"},
    {"move", c_move, "Moves/renames files and folders.\nMOVE [/Y] source dest"},
    {"notepad", c_edit, "Same as EDIT."},
    {"path", c_path, "Shows or sets the search path.\nPATH [dir;dir...]   PATH ;"},
    {"pause", c_pause, "Waits for a key press."},
    {"popd", c_popd, "Returns to the dir saved by PUSHD."},
    {"prompt", c_prompt, "Changes the prompt.\nPROMPT [text]  $P path $G > $D date\n$T time $_ newline $N drive $$ $"},
    {"pushd", c_pushd, "Saves the current dir, then CDs.\nPUSHD [path]"},
    {"rd", c_rd, "Removes a directory.\nRD [/S] [/Q] path"},
    {"rem", c_rem, "Comment line in a batch file."},
    {"ren", c_ren, "Renames files.\nREN name newname   REN *.txt *.bak"},
    {"rename", c_ren, "Same as REN."},
    {"rmdir", c_rd, "Same as RD."},
    {"set", c_set, "Shows/sets environment variables.\nSET [var[=value]]\nSET /A expression  (+-*/% << >> & |\n  ^ ~ ! () and += -= *= /= etc.)\nSET /P var=prompt\n%var:~start,len%  %var:old=new%"},
    {"setlocal", c_setlocal, "Begins local env in a batch file.\nSETLOCAL [EnableDelayedExpansion]"},
    {"shift", c_shift, "Shifts batch parameters %1..%9."},
    {"shutdown", c_shutdown, "Saves drive C: and exits."},
    {"sort", c_sort, "Sorts input lines.\nSORT [/R] [file]   cmd | SORT"},
    {"start", c_start, "Runs a calculator program or file.\nSTART PRGMNAME   (returns to CMD\nwhen the program ends)"},
    {"systeminfo", c_systeminfo, "Shows system information."},
    {"taskkill", c_taskkill, "Deletes a calculator program or\nAppVar.  TASKKILL /IM name [/F]"},
    {"tasklist", c_tasklist, "Lists calculator programs/AppVars."},
    {"time", c_time, "Shows or sets the time.\nTIME [/T | hh:mm[:ss]]"},
    {"timeout", c_timeout, "Waits a number of seconds.\nTIMEOUT /T secs [/NOBREAK]"},
    {"title", c_title, "Sets the window title.\nTITLE text"},
    {"tree", c_tree, "Graphically shows folder structure.\nTREE [path] [/F]"},
    {"type", c_type, "Displays text files.\nTYPE file [file...]"},
    {"ver", c_ver, "Shows the version."},
    {"vol", c_vol, "Shows the volume label and serial."},
    {"where", c_where, "Locates programs on the PATH.\nWHERE name"},
    {"whoami", c_whoami, "Shows the current user."},
    {"xcopy", c_xcopy, "Copies files and directory trees.\nXCOPY src [dest] [/S] [/E] [/Q] [/Y]"},
    {NULL, NULL, NULL},
};

const cmd_t *find_cmd(const char *name)
{
    for (const cmd_t *c = commands; c->name; c++)
        if (!stricmp_(c->name, name)) return c;
    return NULL;
}

bool cmd_exists(const char *name) { return find_cmd(name) != NULL; }

/* TAB completion: nth candidate for word */
int complete_word(const char *word, bool first, int nth, char *out, int max)
{
    char dirpart[LINE_MAX / 2], pre[FS_NAMELEN + 2];
    const char *sep = NULL;
    size_t pl;
    int d, n;
    uint8_t *kids;

    for (const char *c = word; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') sep = c;
    if (first && !sep) {
        pl = strlen(word);
        for (const cmd_t *c = commands; c->name; c++) {
            if (!strnicmp_(c->name, word, pl)) {
                if (nth-- == 0) { strncpy(out, c->name, max - 1); out[max - 1] = 0; return 1; }
            }
        }
    }
    if (sep) {
        size_t dl = (size_t)(sep - word) + 1;
        if (dl >= sizeof dirpart) return 0;
        memcpy(dirpart, word, dl);
        dirpart[dl] = 0;
        strncpy(pre, sep + 1, sizeof pre - 1);
    } else {
        dirpart[0] = 0;
        strncpy(pre, word, sizeof pre - 1);
    }
    pre[sizeof pre - 1] = 0;
    d = dirpart[0] ? fs_resolve(dirpart) : fs_cwd;
    if (d < 0 || !(fs_nodes[d].attr & FA_DIR)) return 0;
    kids = malloc(FS_MAX);
    if (!kids) return 0;
    n = fs_list((uint8_t)d, kids, FS_MAX);
    pl = strlen(pre);
    for (int i = 0; i < n; i++) {
        if (fs_nodes[kids[i]].attr & FA_HIDDEN) continue;
        if (!strnicmp_(fs_nodes[kids[i]].name, pre, pl)) {
            if (nth-- == 0) {
                snprintf(out, max, "%s%s", dirpart, fs_nodes[kids[i]].name);
                free(kids);
                return 1;
            }
        }
    }
    free(kids);
    return 0;
}
