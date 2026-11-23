/*
 * Host (Linux) stand-ins for the TI-84 Plus CE OS / libraries, plus a
 * keystroke feeder so the real shell code can be exercised by scripts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../src/cmd.h"
#include <fileioc.h>
#include <ti/vars.h>
#include <ti/info.h>
#include <ti/getcsc.h>
#include <sys/rtc.h>

uint16_t host_palette[256];
uint32_t os_TempFreeArc = 2500000;

/* ---- in-memory variable store ---- */
typedef struct { char name[9]; uint8_t type; uint8_t *data; size_t size; bool used, arc; } var_t;
static var_t vars[400];
typedef struct { int v; size_t pos; bool open; } hnd_t;
static hnd_t hs[8];

static int vfind(const char *n, uint8_t t)
{
    for (int i = 0; i < 400; i++) if (vars[i].used && vars[i].type == t && !strcmp(vars[i].name, n)) return i;
    return -1;
}
static int vnew(const char *n, uint8_t t)
{
    for (int i = 0; i < 400; i++) if (!vars[i].used) {
        memset(&vars[i], 0, sizeof vars[i]);
        strncpy(vars[i].name, n, 8); vars[i].type = t; vars[i].used = true; return i;
    }
    return -1;
}
uint8_t ti_OpenVar(const char *name, const char *mode, uint8_t type)
{
    int v = vfind(name, type), h;
    if (mode[0] == 'r' && v < 0) return 0;
    if (mode[0] == 'w') { if (v >= 0) { free(vars[v].data); vars[v].used = false; } v = vnew(name, type); }
    if (mode[0] == 'a' && v < 0) v = vnew(name, type);
    if (v < 0) return 0;
    if (mode[0] != 'r' || mode[1] == '+') vars[v].arc = false;
    for (h = 1; h < 8; h++) if (!hs[h].open) break;
    if (h == 8) { fprintf(stderr, "HOST: out of file handles!\n"); exit(3); }
    hs[h].v = v; hs[h].open = true; hs[h].pos = mode[0] == 'a' ? vars[v].size : 0;
    return (uint8_t)h;
}
uint8_t ti_Open(const char *name, const char *mode) { return ti_OpenVar(name, mode, OS_TYPE_APPVAR); }
int ti_Close(uint8_t h) { hs[h].open = false; return 1; }
size_t ti_Read(void *d, size_t s, size_t c, uint8_t h)
{
    var_t *v = &vars[hs[h].v];
    size_t n = s * c;
    if (hs[h].pos + n > v->size) return 0;
    memcpy(d, v->data + hs[h].pos, n); hs[h].pos += n; return c;
}
size_t ti_Write(const void *d, size_t s, size_t c, uint8_t h)
{
    var_t *v = &vars[hs[h].v];
    size_t n = s * c;
    if (hs[h].pos + n > 65505) return 0;
    if (hs[h].pos + n > v->size) { v->data = realloc(v->data, hs[h].pos + n); v->size = hs[h].pos + n; }
    memcpy(v->data + hs[h].pos, d, n); hs[h].pos += n; return c;
}
uint16_t ti_GetSize(uint8_t h) { return (uint16_t)vars[hs[h].v].size; }
int ti_IsArchived(uint8_t h) { return vars[hs[h].v].arc; }
int ti_SetArchiveStatus(bool a, uint8_t h) { vars[hs[h].v].arc = a; return 1; }
int ti_DeleteVar(const char *name, uint8_t type)
{
    int v = vfind(name, type);
    if (v < 0) return 0;
    free(vars[v].data); vars[v].used = false; return 1;
}
int ti_Delete(const char *name) { return ti_DeleteVar(name, OS_TYPE_APPVAR); }
char *ti_DetectAny(void **vp, const char *det, uint8_t *type)
{
    intptr_t i = (intptr_t)*vp;
    (void)det;
    for (; i < 400; i++) if (vars[i].used) { *type = vars[i].type; *vp = (void *)(i + 1); return vars[i].name; }
    return NULL;
}

size_t os_MemChk(void **f) { (void)f; return 98765; }
void os_ArcChk(void) {}
int os_RunPrgm(const char *p, void *d, size_t s, os_runprgm_callback_t cb)
{
    (void)d; (void)s; (void)cb;
    printf("HOST: os_RunPrgm(%s)\n", p);
    return -1;
}
static system_info_t si = {sizeof si, 7, 0, 9, 5, 8, 2, 12, 5, 3, 6, 17, {0}, {1,2,3,4,5,6,7,8}, {'T','I'}, 0x109};
const system_info_t *os_GetSystemInfo(void) { return &si; }

static uint8_t dd = 23, mo = 11, hh = 20, mi = 4, ss = 5;
static uint16_t yy = 2026;
void boot_SetDate(uint8_t d, uint8_t m, uint16_t y) { dd = d; mo = m; yy = y; }
void boot_GetDate(uint8_t *d, uint8_t *m, uint16_t *y) { *d = dd; *m = mo; *y = yy; }
void boot_SetTime(uint8_t s, uint8_t m, uint8_t h) { ss = s; mi = m; hh = h; }
void boot_GetTime(uint8_t *s, uint8_t *m, uint8_t *h) { *s = ss; *m = mi; *h = hh; }

/* ---- keystroke feeder ---- */
static uint8_t q[20000];
static int qh, qt, sim_mode;

static void push(uint8_t k) { q[qt++] = k; }

static void type_char(char c)
{
    /* find a (mode, 2nd, key) that yields c; prefer current mode, no 2nd */
    for (int pass = 0; pass < 2; pass++) {
        for (int dm = 0; dm < 3; dm++) {
            int m = (sim_mode + dm) % 3;
            for (int k = 1; k < 57; k++) {
                if (key_to_char((uint8_t)k, pass, (uint8_t)m) == c) {
                    while (sim_mode != m && !pass) { push(sk_Alpha); sim_mode = (sim_mode + 1) % 3; }
                    if (pass) push(sk_2nd);
                    push((uint8_t)k);
                    return;
                }
            }
            if (pass) break;
        }
    }
    fprintf(stderr, "HOST: cannot type '%c'\n", c);
    exit(2);
}

static void feed_line(const char *s)
{
    bool enter = true;
    while (*s) {
        if (*s == '{') {
            const char *e = strchr(s, '}');
            char tok[16];
            size_t l = (size_t)(e - s - 1);
            memcpy(tok, s + 1, l); tok[l] = 0;
            if (!strcmp(tok, "ENTER")) push(sk_Enter);
            else if (!strcmp(tok, "Y=")) push(sk_Yequ);
            else if (!strcmp(tok, "GRAPH")) push(sk_Graph);
            else if (!strcmp(tok, "CLEAR")) push(sk_Clear);
            else if (!strcmp(tok, "DEL")) push(sk_Del);
            else if (!strcmp(tok, "UP")) push(sk_Up);
            else if (!strcmp(tok, "DOWN")) push(sk_Down);
            else if (!strcmp(tok, "LEFT")) push(sk_Left);
            else if (!strcmp(tok, "RIGHT")) push(sk_Right);
            else if (!strcmp(tok, "2ND")) push(sk_2nd);
            else if (!strcmp(tok, "ON")) push(KEY_ON);
            else if (!strcmp(tok, "QUIT")) { push(sk_2nd); push(sk_Mode); }
            else if (!strcmp(tok, "NOENTER")) enter = false;
            else { fprintf(stderr, "HOST: bad token %s\n", tok); exit(2); }
            s = e + 1;
            continue;
        }
        type_char(*s++);
    }
    if (enter) push(sk_Enter);
}

static void print_line(const char *s)
{
    char b[COLS + 1];
    int n = (int)strlen(s);
    strcpy(b, s);
    for (int i = 0; i < n; i++) if (!b[i]) b[i] = ' ';
    while (n && (b[n - 1] == ' ' || !b[n - 1])) b[--n] = 0;
    printf("|%s\n", b);
}

void con_dump(void (*line)(const char *s));

static void finish(void)
{
    con_dump(print_line);
    fflush(stdout);
}

uint8_t host_next_key(void)
{
    if (qh == qt) {
        /* script done: quit CMD normally */
        static bool quitting;
        if (quitting) exit(0);
        quitting = true;
        push(sk_2nd); push(sk_Mode);
    }
    return q[qh++];
}

int host_main(void);
int main(int argc, char **argv)
{
    char line[512];
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) { perror("script"); return 1; }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' && line[1] == ' ') continue;
        feed_line(line);
    }
    atexit(finish);
    return host_main();
}
