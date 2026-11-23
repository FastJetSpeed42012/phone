/*
 * fs.c - virtual C:\ drive.
 *
 * Directory tree metadata lives in the AppVar "CMDFSYS".
 * Each file's contents live in their own AppVar "CMDFxx" (xx = node id),
 * so files can be large and are archived (flash) when CMD exits.
 */
#include "cmd.h"
#include <fileioc.h>
#include <ti/vars.h>
#include <sys/rtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define META_NAME "CMDFSYS"
static const char META_MAGIC[4] = {'C', 'F', 'S', '2'};

node_t fs_nodes[FS_MAX];
uint8_t fs_cwd;
char fs_label[12] = "TI84CE";

static void data_name(uint8_t n, char *out)
{
    sprintf(out, "CMDF%02X", n);
}

void now_packed(uint16_t *d, uint16_t *t)
{
    uint8_t day, mon, s, m, h;
    uint16_t yr;
    boot_GetDate(&day, &mon, &yr);
    boot_GetTime(&s, &m, &h);
    if (yr < 1980) yr = 1980;
    *d = (uint16_t)(((yr - 1980) << 9) | (mon << 5) | day);
    *t = (uint16_t)((h << 11) | (m << 5) | (s >> 1));
}

void fmt_date(uint16_t d, char *out)
{
    sprintf(out, "%02u/%02u/%02u", (d >> 5) & 15, d & 31, ((d >> 9) + 1980) % 100);
}

void fmt_time(uint16_t t, char *out)
{
    sprintf(out, "%02u:%02u", t >> 11, (t >> 5) & 63);
}

void fs_touch(uint8_t n)
{
    now_packed(&fs_nodes[n].date, &fs_nodes[n].time);
}

void fs_save(void)
{
    uint8_t h = ti_Open(META_NAME, "w");
    if (!h) return;
    ti_Write(META_MAGIC, 4, 1, h);
    ti_Write(fs_label, sizeof fs_label, 1, h);
    ti_Write(&fs_cwd, 1, 1, h);
    ti_Write(fs_nodes, sizeof fs_nodes, 1, h);
    ti_Close(h);
}

void fs_archive_all(void)
{
    char nm[10];
    uint8_t h;
    for (int i = -1; i < FS_MAX; i++) {
        if (i < 0) strcpy(nm, META_NAME);
        else {
            if (!(fs_nodes[i].attr & FA_USED) || (fs_nodes[i].attr & FA_DIR) || !fs_nodes[i].size) continue;
            data_name((uint8_t)i, nm);
        }
        if ((h = ti_Open(nm, "r"))) {
            if (!ti_IsArchived(h)) ti_SetArchiveStatus(true, h);
            ti_Close(h);
        }
    }
}

static int new_node(uint8_t parent, const char *name, uint8_t attr)
{
    for (int i = 1; i < FS_MAX; i++) {
        if (!(fs_nodes[i].attr & FA_USED)) {
            node_t *n = &fs_nodes[i];
            memset(n, 0, sizeof *n);
            strncpy(n->name, name, FS_NAMELEN - 1);
            n->parent = parent;
            n->attr = attr | FA_USED;
            fs_touch((uint8_t)i);
            return i;
        }
    }
    return -1;
}

static void put_file(const char *path, const char *text)
{
    char name[FS_NAMELEN];
    int p = fs_split(path, name);
    int n;
    if (p < 0) return;
    n = fs_create((uint8_t)p, name, false);
    if (n >= 0) fs_write((uint8_t)n, text, strlen(text));
}

static const char readme_txt[] =
    "Welcome to CE Command Prompt!\n"
    "\n"
    "This is a Windows CMD style shell for\n"
    "the TI-84 Plus CE. Your files live on\n"
    "the virtual C: drive and are saved to\n"
    "archive memory when you EXIT.\n"
    "\n"
    "Try:  help   keys   dir /s   tree /f\n"
    "      demo   guess   set /a 6*7\n"
    "      echo hi > a.txt & type a.txt\n"
    "      dir | find \"bat\"\n";

static const char demo_bat[] =
    "@echo off\n"
    "title Batch demo\n"
    "echo Hello, %USERNAME%! Today is %DATE%.\n"
    "set /a n=0\n"
    ":loop\n"
    "set /a n+=1\n"
    "set /a sq=n*n\n"
    "echo %n% squared is %sq%\n"
    "if %n% LSS 5 goto loop\n"
    "for %%c in (red green blue) do echo color: %%c\n"
    "for /l %%i in (1,1,3) do echo count %%i\n"
    "call :greet World\n"
    "echo Files here:\n"
    "dir /b\n"
    "title Command Prompt\n"
    "goto :eof\n"
    ":greet\n"
    "echo Subroutine says hello, %1!\n"
    "exit /b 0\n";

static const char guess_bat[] =
    "@echo off\n"
    "setlocal\n"
    "set /a secret=%RANDOM% %% 100 + 1\n"
    "set /a tries=0\n"
    "echo I'm thinking of a number 1-100.\n"
    ":ask\n"
    "set g=\n"
    "set /p g=Your guess: \n"
    "if \"%g%\"==\"\" goto end\n"
    "set /a tries+=1\n"
    "if %g% LSS %secret% echo Higher!& goto ask\n"
    "if %g% GTR %secret% echo Lower!& goto ask\n"
    "echo Correct in %tries% tries!\n"
    ":end\n"
    "endlocal\n";

void fs_format(void)
{
    char nm[10];
    for (int i = 0; i < FS_MAX; i++) {
        data_name((uint8_t)i, nm);
        ti_Delete(nm);
    }
    memset(fs_nodes, 0, sizeof fs_nodes);
    fs_nodes[0].attr = FA_USED | FA_DIR;
    fs_touch(0);
    fs_cwd = 0;
    strcpy(fs_label, "TI84CE");
    fs_mkdirs("C:\\Windows\\System32");
    fs_mkdirs("C:\\Program Files");
    fs_mkdirs("C:\\Users\\Public");
    fs_mkdirs("C:\\Users\\User\\Documents");
    fs_mkdirs("C:\\Temp");
    put_file("C:\\Users\\User\\readme.txt", readme_txt);
    put_file("C:\\Users\\User\\demo.bat", demo_bat);
    put_file("C:\\Windows\\System32\\guess.bat", guess_bat);
    fs_cwd = (uint8_t)fs_resolve("C:\\Users\\User");
    fs_save();
}

void fs_init(void)
{
    uint8_t h = ti_Open(META_NAME, "r");
    char mg[4];
    if (h && ti_GetSize(h) == 4 + sizeof fs_label + 1 + sizeof fs_nodes) {
        ti_Read(mg, 4, 1, h);
        if (!memcmp(mg, META_MAGIC, 4)) {
            ti_Read(fs_label, sizeof fs_label, 1, h);
            ti_Read(&fs_cwd, 1, 1, h);
            ti_Read(fs_nodes, sizeof fs_nodes, 1, h);
            ti_Close(h);
            if (fs_cwd >= FS_MAX || !(fs_nodes[fs_cwd].attr & FA_DIR)) fs_cwd = 0;
            return;
        }
    }
    if (h) ti_Close(h);
    fs_format();
}

bool fs_valid_name(const char *name)
{
    size_t l = strlen(name);
    if (!l || l >= FS_NAMELEN) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (; *name; name++)
        if (strchr("\\/:*?\"<>|", *name) || (unsigned char)*name < 32) return false;
    return true;
}

int fs_child(uint8_t parent, const char *name)
{
    for (int i = 1; i < FS_MAX; i++)
        if ((fs_nodes[i].attr & FA_USED) && fs_nodes[i].parent == parent &&
            !stricmp_(fs_nodes[i].name, name))
            return i;
    return -1;
}

static bool is_sep(char c) { return c == '\\' || c == '/'; }

int fs_resolve(const char *path)
{
    char comp[FS_NAMELEN + 8];
    int n = fs_cwd;
    const char *p = path;

    if (p[0] && p[1] == ':') {
        if (toupper((unsigned char)p[0]) != 'C') return -1;
        p += 2;
    }
    if (is_sep(*p)) n = 0;
    while (*p) {
        size_t l = 0;
        while (is_sep(*p)) p++;
        if (!*p) break;
        while (*p && !is_sep(*p)) {
            if (l < sizeof comp - 1) comp[l++] = *p;
            p++;
        }
        comp[l] = 0;
        while (l && comp[l - 1] == ' ') comp[--l] = 0;
        if (!(fs_nodes[n].attr & FA_DIR)) return -1;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { n = fs_nodes[n].parent; continue; }
        if ((n = fs_child((uint8_t)n, comp)) < 0) return -1;
    }
    return n;
}

int fs_split(const char *path, char *name)
{
    char *buf = malloc(LINE_MAX);
    int i, last = -1, start = 0, p;

    if (!buf) return -1;
    strncpy(buf, path, LINE_MAX - 1);
    buf[LINE_MAX - 1] = 0;
    if (buf[0] && buf[1] == ':') start = 2;
    /* strip trailing separators like "dir\" (but keep a lone root "\") */
    i = (int)strlen(buf);
    while (i > start + 1 && is_sep(buf[i - 1])) buf[--i] = 0;
    for (i = start; buf[i]; i++) if (is_sep(buf[i])) last = i;
    if (last < 0) {
        strncpy(name, buf + start, FS_NAMELEN - 1);
        name[FS_NAMELEN - 1] = 0;
        buf[start] = 0;
    } else {
        strncpy(name, buf + last + 1, FS_NAMELEN - 1);
        name[FS_NAMELEN - 1] = 0;
        buf[last == start ? last + 1 : last] = 0;
    }
    p = fs_resolve(buf);
    free(buf);
    if (p < 0 || !(fs_nodes[p].attr & FA_DIR)) return -1;
    return p;
}

int fs_create(uint8_t parent, const char *name, bool dir)
{
    int n;
    if (!fs_valid_name(name) || fs_child(parent, name) >= 0) return -1;
    n = new_node(parent, name, dir ? FA_DIR : FA_ARCH);
    if (n >= 0) fs_save();
    return n;
}

int fs_mkdirs(const char *path)
{
    char comp[FS_NAMELEN + 8];
    int n = fs_cwd;
    const char *p = path;

    if (p[0] && p[1] == ':') {
        if (toupper((unsigned char)p[0]) != 'C') return -1;
        p += 2;
    }
    if (is_sep(*p)) n = 0;
    while (*p) {
        size_t l = 0;
        int c;
        while (is_sep(*p)) p++;
        if (!*p) break;
        while (*p && !is_sep(*p)) {
            if (l < sizeof comp - 1) comp[l++] = *p;
            p++;
        }
        comp[l] = 0;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { n = fs_nodes[n].parent; continue; }
        c = fs_child((uint8_t)n, comp);
        if (c < 0) {
            if (!fs_valid_name(comp)) return -1;
            c = new_node((uint8_t)n, comp, FA_DIR);
            if (c < 0) return -1;
        } else if (!(fs_nodes[c].attr & FA_DIR)) return -1;
        n = c;
    }
    fs_save();
    return n;
}

bool fs_remove(uint8_t n)
{
    char nm[10];
    if (n == 0) return false;
    if (fs_nodes[n].attr & FA_DIR) {
        for (int i = 1; i < FS_MAX; i++)
            if ((fs_nodes[i].attr & FA_USED) && fs_nodes[i].parent == n) return false;
    } else {
        data_name(n, nm);
        ti_Delete(nm);
    }
    fs_nodes[n].attr = 0;
    if (fs_cwd == n) fs_cwd = 0;
    fs_save();
    return true;
}

bool fs_is_ancestor(uint8_t anc, uint8_t n)
{
    for (;;) {
        if (n == anc) return true;
        if (n == 0) return false;
        n = fs_nodes[n].parent;
    }
}

void fs_path(uint8_t n, char *out)
{
    uint8_t chain[24];
    int depth = 0;
    strcpy(out, "C:\\");
    while (n != 0 && depth < 24) {
        chain[depth++] = n;
        n = fs_nodes[n].parent;
    }
    while (depth--) {
        strcat(out, fs_nodes[chain[depth]].name);
        if (depth) strcat(out, "\\");
    }
}

char *fs_read(uint8_t n, size_t *size)
{
    char nm[10];
    uint8_t h;
    size_t sz = 0;
    char *buf;

    data_name(n, nm);
    h = ti_Open(nm, "r");
    if (h) sz = ti_GetSize(h);
    buf = malloc(sz + 1);
    if (!buf) {
        if (h) ti_Close(h);
        return NULL;
    }
    if (h) {
        if (sz) ti_Read(buf, sz, 1, h);
        ti_Close(h);
    }
    buf[sz] = 0;
    if (size) *size = sz;
    return buf;
}

bool fs_write(uint8_t n, const char *data, size_t size)
{
    char nm[10];
    uint8_t h;
    bool ok = true;

    data_name(n, nm);
    if (!size) {
        ti_Delete(nm);
    } else {
        h = ti_Open(nm, "w");
        if (!h) ok = false;
        else {
            if (ti_Write(data, size, 1, h) != 1) ok = false;
            ti_Close(h);
        }
        if (!ok) { ti_Delete(nm); size = 0; }
    }
    fs_nodes[n].size = (uint16_t)size;
    fs_nodes[n].attr |= FA_ARCH;
    fs_touch(n);
    fs_save();
    return ok;
}

bool fs_append(uint8_t n, const char *data, size_t size)
{
    char nm[10];
    uint8_t h;
    bool ok = false;

    if (!size) return true;
    if ((uint32_t)fs_nodes[n].size + size > 65000) return false;
    data_name(n, nm);
    h = ti_Open(nm, "a");
    if (h) {
        ok = ti_Write(data, size, 1, h) == 1;
        fs_nodes[n].size = ti_GetSize(h);
        ti_Close(h);
    }
    fs_touch(n);
    fs_save();
    return ok;
}

int fs_list(uint8_t dir, uint8_t *out, int max)
{
    int cnt = 0;
    for (int i = 1; i < FS_MAX && cnt < max; i++) {
        if ((fs_nodes[i].attr & FA_USED) && fs_nodes[i].parent == dir) {
            int j = cnt++;
            while (j > 0 && stricmp_(fs_nodes[out[j - 1]].name, fs_nodes[i].name) > 0) {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = (uint8_t)i;
        }
    }
    return cnt;
}

uint32_t fs_free_arc(void)
{
    os_ArcChk();
    return os_TempFreeArc;
}

int fs_used_nodes(void)
{
    int c = 0;
    for (int i = 0; i < FS_MAX; i++) if (fs_nodes[i].attr & FA_USED) c++;
    return c;
}

uint32_t fs_used_bytes(void)
{
    uint32_t t = 0;
    for (int i = 0; i < FS_MAX; i++)
        if ((fs_nodes[i].attr & (FA_USED | FA_DIR)) == FA_USED) t += fs_nodes[i].size;
    return t;
}

static bool wm(const char *p, const char *s)
{
    for (;;) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (; *s; s++) if (wm(p, s)) return true;
            return wm(p, s);
        }
        if (!*s) {
            /* "name." pattern style: trailing '.' matches no extension */
            while (*p == '.' || *p == '*') p++;
            return !*p;
        }
        if (*p != '?' && toupper((unsigned char)*p) != toupper((unsigned char)*s)) return false;
        p++;
        s++;
    }
}

bool wild_match(const char *pat, const char *name)
{
    if (!strcmp(pat, "*.*") || !strcmp(pat, "*")) return true;
    return wm(pat, name);
}

bool has_wild(const char *s)
{
    return strchr(s, '*') || strchr(s, '?');
}

int fs_glob(const char *pattern, uint8_t *out, int max, uint8_t *dir_out)
{
    char name[FS_NAMELEN];
    uint8_t *kids;
    int p, n, cnt = 0;

    if (!has_wild(pattern)) {
        n = fs_resolve(pattern);
        if (n < 0) return 0;
        if (dir_out) *dir_out = fs_nodes[n].parent;
        out[0] = (uint8_t)n;
        return 1;
    }
    p = fs_split(pattern, name);
    if (p < 0 || !(kids = malloc(FS_MAX))) return 0;
    if (dir_out) *dir_out = (uint8_t)p;
    n = fs_list((uint8_t)p, kids, FS_MAX);
    for (int i = 0; i < n && cnt < max; i++)
        if (wild_match(name, fs_nodes[kids[i]].name)) out[cnt++] = kids[i];
    free(kids);
    return cnt;
}

int fs_write_path(const char *path, const char *data, size_t len, bool append)
{
    char name[FS_NAMELEN];
    int n = fs_resolve(path);
    if (n < 0) {
        int p = fs_split(path, name);
        if (p < 0) return -1;
        n = fs_create((uint8_t)p, name, false);
        if (n < 0) return -1;
    }
    if (fs_nodes[n].attr & (FA_DIR | FA_RO)) return -2;
    if (append ? !fs_append((uint8_t)n, data, len) : !fs_write((uint8_t)n, data, len)) return -3;
    return 0;
}
