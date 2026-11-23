/*
 * ui.c - screen, console scrollback, keypad handling and line input
 */
#include "cmd.h"
#include <graphx.h>
#include <keypadc.h>
#include <ti/getcsc.h>
#include <sys/rtc.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#define TOP_Y   12
#define STAT_Y  (TOP_Y + ROWS * 8)

uint8_t con_fg = 7, con_bg = 0;
char con_title[48] = "Command Prompt";
bool g_break;

uint8_t ui_mode;        /* 0 = abc, 1 = ABC, 2 = 123 */
bool ui_second;

static char sb[SB_LINES][COLS];
static int con_abs, con_top, ccol;
static int view_scroll;
static bool in_active, blink_on;
static int in_abs, in_col;
static clock_t last_render;
static bool no_lazy;
static uint8_t prev_keys[8];
static uint8_t held_sk;
static clock_t held_t;
static bool held_rep, on_prev;


/* ---------------------------------------------------------------- */
/* palette: 16 console colors + UI colors                           */
static const uint8_t pal_rgb[21][3] = {
    {12, 12, 12},   {0, 55, 218},   {19, 161, 14},  {58, 150, 221},
    {197, 15, 31},  {136, 23, 152}, {193, 156, 0},  {204, 204, 204},
    {118, 118, 118},{59, 120, 255}, {22, 198, 12},  {97, 214, 214},
    {231, 72, 86},  {180, 0, 158},  {249, 241, 165},{242, 242, 242},
    {255, 255, 255},{0, 0, 0},      {36, 36, 36},   {200, 200, 200},
    {255, 196, 0},
};

void ui_init(void)
{
    gfx_Begin();
    for (uint8_t i = 0; i < 21; i++)
        gfx_palette[i] = gfx_RGBTo1555(pal_rgb[i][0], pal_rgb[i][1], pal_rgb[i][2]);
    gfx_SetDrawBuffer();
    gfx_SetMonospaceFont(8);
    gfx_SetTextTransparentColor(254);
    memset(sb, 0, sizeof sb);
    con_abs = con_top = ccol = 0;
#ifndef HOST
    /* ignore keys still held from launching the program (e.g. [enter]) */
    kb_Scan();
    for (uint8_t g = 1; g <= 7; g++) prev_keys[g] = kb_Data[g];
#endif
}

void ui_end(void)
{
    gfx_End();
}

static char *line_at(int abs)
{
    return sb[(unsigned)abs % SB_LINES];
}

int con_col(void) { return ccol; }

static void con_newline(void)
{
    con_abs++;
    memset(line_at(con_abs), 0, COLS);
    if (con_abs - con_top >= SB_LINES)
        con_top++;
    ccol = 0;
    if (!no_lazy)
        con_render_lazy();
}

void con_putc(char c)
{
    if (c == '\n') { con_newline(); return; }
    if (c == '\r') { ccol = 0; return; }
    if (c == '\t') {
        do con_putc(' '); while (ccol % 8);
        return;
    }
    if (ccol >= COLS) con_newline();
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    line_at(con_abs)[ccol++] = c;
}

void con_clear(void)
{
    memset(sb, 0, sizeof sb);
    con_top = con_abs;
    ccol = 0;
    view_scroll = 0;
}

static void draw_text(int x, int y, const char *s)
{
    gfx_SetTextXY(x, y);
    gfx_PrintString(s);
}

void ui_draw_title(void)
{
    char t[33];
    gfx_SetColor(PAL_TITLE_BG);
    gfx_FillRectangle_NoClip(0, 0, 320, TOP_Y);
    gfx_SetColor(0);
    gfx_FillRectangle_NoClip(3, 2, 10, 8);
    gfx_SetTextFGColor(15);
    gfx_SetTextBGColor(0);
    draw_text(4, 2, ">");
    gfx_SetTextFGColor(PAL_TITLE_FG);
    gfx_SetTextBGColor(PAL_TITLE_BG);
    strncpy(t, con_title, 31);
    t[31] = 0;
    draw_text(17, 2, t);
    gfx_SetColor(PAL_TITLE_FG);
    gfx_HorizLine_NoClip(272, 7, 8);
    gfx_Rectangle_NoClip(288, 2, 8, 8);
    gfx_Line_NoClip(304, 2, 311, 9);
    gfx_Line_NoClip(304, 9, 311, 2);
    gfx_SetColor(PAL_STAT_BG);
    gfx_HorizLine_NoClip(0, TOP_Y - 1, 320);
}

void ui_draw_status(const char *extra)
{
    static const char *modes[3] = {"abc", "ABC", "123"};
    uint8_t s, m, h;
    char clk[8];

    gfx_SetColor(PAL_STAT_BG);
    gfx_FillRectangle_NoClip(0, STAT_Y, 320, 240 - STAT_Y);
    gfx_SetTextBGColor(PAL_STAT_BG);
    gfx_SetTextFGColor(PAL_STAT_HI);
    draw_text(2, STAT_Y + 2, modes[ui_mode]);
    if (ui_second) draw_text(34, STAT_Y + 2, "2nd");
    gfx_SetTextFGColor(PAL_STAT_FG);
    if (extra) draw_text(66, STAT_Y + 2, extra);
    boot_GetTime(&s, &m, &h);
    sprintf(clk, "%02u:%02u", h, m);
    draw_text(278, STAT_Y + 2, clk);
}

void con_render(void)
{
    int last = con_abs;
    int top, r, c;

    if (in_active && in_abs > last) last = in_abs;
    top = last - ROWS + 1 - view_scroll;
    if (top < con_top) top = con_top;
    if (view_scroll && top + ROWS - 1 > last) top = last - ROWS + 1;
    if (top < con_top) top = con_top;

    gfx_SetColor(con_bg);
    gfx_FillRectangle_NoClip(0, TOP_Y, 320, ROWS * 8);
    gfx_SetTextFGColor(con_fg);
    gfx_SetTextBGColor(con_bg);
    for (r = 0; r < ROWS; r++) {
        int abs = top + r;
        const char *l;
        if (abs > last) break;
        l = line_at(abs);
        for (c = 0; c < COLS; c++) {
            char ch = l[c];
            if (ch && ch != ' ') {
                gfx_SetTextXY(c * 8, TOP_Y + r * 8);
                gfx_PrintChar(ch);
            }
        }
    }
    if (in_active && blink_on && !view_scroll) {
        int row = in_abs - top;
        if (row >= 0 && row < ROWS) {
            gfx_SetColor(con_fg);
            gfx_FillRectangle_NoClip(in_col * 8, TOP_Y + row * 8 + 6, 8, 2);
        }
    }
    ui_draw_title();
    ui_draw_status(view_scroll ? "-- scrollback --" : NULL);
    gfx_SwapDraw();
    last_render = clock();
}

void con_render_lazy(void)
{
    check_break();
    if ((clock() - last_render) > CLOCKS_PER_SEC / 10)
        con_render();
}

/* ---------------------------------------------------------------- */
/* output routing                                                    */
sbuf_t *out_cap;
bool out_nul;

void sb_init(sbuf_t *b) { b->buf = NULL; b->len = b->cap = 0; b->overflow = false; }
void sb_free(sbuf_t *b) { free(b->buf); sb_init(b); }

bool sb_putc(sbuf_t *b, char c)
{
    if (b->len + 2 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        char *nb;
        if (nc > 9000) nc = 9000;
        if (b->len + 2 > nc) { b->overflow = true; return false; }
        nb = realloc(b->buf, nc);
        if (!nb) { b->overflow = true; return false; }
        b->buf = nb;
        b->cap = nc;
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = 0;
    return true;
}

bool sb_puts(sbuf_t *b, const char *s)
{
    while (*s) if (!sb_putc(b, *s++)) return false;
    return true;
}

void out_putc(char c)
{
    if (out_nul) return;
    if (out_cap) sb_putc(out_cap, c);
    else con_putc(c);
}

void out_puts(const char *s)
{
    while (*s) out_putc(*s++);
}

void out_nl(void) { out_putc('\n'); }

void out_printf(const char *fmt, ...)
{
    static char buf[256];   /* static: keeps the 4KB stack free */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_puts(buf);
}

/* ---------------------------------------------------------------- */
/* keypad                                                            */

#ifdef HOST
/* host test build: keys come from the test harness */
uint8_t host_next_key(void);
uint8_t key_poll(void) { return host_next_key(); }
static void kb_unused(void) { (void)prev_keys; (void)held_sk; (void)held_t; (void)held_rep; }
void con_dump(void (*line)(const char *s))
{
    char b[COLS + 1];
    (void)kb_unused;
    for (int a = con_top; a <= con_abs; a++) {
        memcpy(b, line_at(a), COLS);
        b[COLS] = 0;
        line(b);
    }
}
#else
uint8_t key_poll(void)
{
    uint8_t cur[8];
    uint8_t sk = 0;
    bool on = boot_CheckOnPressed();

    if (on && !on_prev) { on_prev = true; return KEY_ON; }
    on_prev = on;

    kb_Scan();
    for (uint8_t g = 1; g <= 7; g++) {
        uint8_t nb;
        cur[g] = kb_Data[g];
        nb = cur[g] & ~prev_keys[g];
        if (nb && !sk) {
            uint8_t bit = 0;
            while (!(nb & (1 << bit))) bit++;
            sk = (7 - g) * 8 + bit + 1;
        }
    }
    memcpy(prev_keys + 1, cur + 1, 7);
    if (sk) {
        held_sk = sk;
        held_t = clock();
        held_rep = false;
        return sk;
    }
    if (held_sk) {
        uint8_t g = 7 - (held_sk - 1) / 8, bit = (held_sk - 1) % 8;
        if (!(cur[g] & (1 << bit))) {
            held_sk = 0;
        } else {
            clock_t now = clock();
            clock_t lim = held_rep ? CLOCKS_PER_SEC / 18 : CLOCKS_PER_SEC * 2 / 5;
            if (now - held_t > lim) {
                held_t = now;
                held_rep = true;
                if (held_sk != sk_2nd && held_sk != sk_Alpha && held_sk != sk_Enter &&
                    held_sk != sk_Mode && held_sk != sk_Clear && held_sk != sk_Yequ)
                    return held_sk;
            }
        }
    }
    return 0;
}
#endif

void (*ui_render_hook)(void);

bool ui_cursor_on(void) { return blink_on; }

static void do_render(void)
{
    if (ui_render_hook) ui_render_hook();
    else con_render();
}

uint8_t key_wait(void)
{
    clock_t bt = clock();
    uint8_t k;
    blink_on = true;
    do_render();
    for (;;) {
        k = key_poll();
        if (k) break;
        if (clock() - bt > CLOCKS_PER_SEC / 2) {
            bt = clock();
            blink_on = !blink_on;
            do_render();
        }
    }
    blink_on = true;
    return k;
}

int wait_any_key(void)
{
    uint8_t k;
    do k = key_wait(); while (k == sk_2nd || k == sk_Alpha);
    if (k == KEY_ON) clear_break();
    return k;
}

bool check_break(void)
{
    if (boot_CheckOnPressed()) g_break = true;
    return g_break;
}

void clear_break(void)
{
    while (boot_CheckOnPressed()) ;
    on_prev = false;
    g_break = false;
}

/* keypad -> character maps, indexed by sk_* scan code */
static const char map_alpha[57] = {
    [sk_Math] = 'a', [sk_Apps] = 'b', [sk_Prgm] = 'c',
    [sk_Recip] = 'd', [sk_Sin] = 'e', [sk_Cos] = 'f', [sk_Tan] = 'g', [sk_Power] = 'h',
    [sk_Square] = 'i', [sk_Comma] = 'j', [sk_LParen] = 'k', [sk_RParen] = 'l', [sk_Div] = 'm',
    [sk_Log] = 'n', [sk_7] = 'o', [sk_8] = 'p', [sk_9] = 'q', [sk_Mul] = 'r',
    [sk_Ln] = 's', [sk_4] = 't', [sk_5] = 'u', [sk_6] = 'v', [sk_Sub] = 'w',
    [sk_Store] = 'x', [sk_1] = 'y', [sk_2] = 'z', [sk_3] = '_', [sk_Add] = '"',
    [sk_0] = ' ', [sk_DecPnt] = ':', [sk_Chs] = '?',
};
static const char map_num[57] = {
    [sk_0] = '0', [sk_1] = '1', [sk_2] = '2', [sk_3] = '3', [sk_4] = '4',
    [sk_5] = '5', [sk_6] = '6', [sk_7] = '7', [sk_8] = '8', [sk_9] = '9',
    [sk_DecPnt] = '.', [sk_Chs] = '-', [sk_Add] = '+', [sk_Sub] = '-',
    [sk_Mul] = '*', [sk_Div] = '/', [sk_Power] = '^', [sk_LParen] = '(',
    [sk_RParen] = ')', [sk_Comma] = ',', [sk_Store] = '>',
    [sk_Math] = 'a', [sk_Apps] = 'b', [sk_Prgm] = 'c', [sk_Recip] = 'd',
    [sk_Sin] = 'e', [sk_Cos] = 'f', [sk_Square] = 'x',
};
static const char map_2nd[57] = {
    [sk_0] = '=', [sk_1] = '!', [sk_2] = '@', [sk_3] = '#', [sk_4] = '$',
    [sk_5] = '%', [sk_6] = '&', [sk_7] = '<', [sk_8] = '>', [sk_9] = '|',
    [sk_DecPnt] = ':', [sk_Chs] = '~', [sk_Div] = '\\', [sk_Mul] = '*',
    [sk_Sub] = '_', [sk_Add] = '+', [sk_LParen] = '[', [sk_RParen] = ']',
    [sk_Comma] = ';', [sk_Store] = '\'', [sk_Power] = '{', [sk_Square] = '}',
    [sk_Recip] = '`', [sk_Math] = '?', [sk_Log] = '^', [sk_Ln] = '#',
    [sk_GraphVar] = '\t',
};
/* keys that type the same thing in every mode */
static const char map_any[57] = {
    [sk_GraphVar] = ' ', [sk_Window] = '\\', [sk_Zoom] = '.', [sk_Trace] = '/',
    [sk_Graph] = '"', [sk_Vars] = '%', [sk_Stat] = '*', [sk_Mode] = '=',
};

int key_to_char(uint8_t sk, bool second, uint8_t mode)
{
    char c;
    if (sk >= 57) return 0;
    if (second) return map_2nd[sk];
    if ((c = map_any[sk])) return c;
    if (mode == 2) return map_num[sk];
    c = map_alpha[sk];
    if (mode == 1 && c >= 'a' && c <= 'z') c -= 32;
    return c;
}

/* ---------------------------------------------------------------- */
/* history                                                           */
#define HIST_N 12
static char hist[HIST_N][LINE_MAX];
static int hist_n;

void history_add(const char *s)
{
    if (!*s) return;
    if (hist_n && !strcmp(hist[(hist_n - 1) % HIST_N], s)) return;
    strncpy(hist[hist_n % HIST_N], s, LINE_MAX - 1);
    hist[hist_n % HIST_N][LINE_MAX - 1] = 0;
    hist_n++;
}

int history_count(void) { return hist_n < HIST_N ? hist_n : HIST_N; }

const char *history_get(int i)
{
    int base = hist_n < HIST_N ? 0 : hist_n - HIST_N;
    return hist[(base + i) % HIST_N];
}

/* ---------------------------------------------------------------- */
/* line input                                                        */
static int rl_start_abs, rl_start_col, rl_max_abs;

static void rl_redraw(const char *buf, int len, int cur)
{
    int a, p;
    no_lazy = true;
    for (a = rl_start_abs; a <= rl_max_abs; a++) {
        char *l = line_at(a);
        if (a == rl_start_abs) memset(l + rl_start_col, 0, COLS - rl_start_col);
        else memset(l, 0, COLS);
    }
    con_abs = rl_start_abs;
    ccol = rl_start_col;
    for (int i = 0; i < len; i++) con_putc(buf[i]);
    if (con_abs > rl_max_abs) rl_max_abs = con_abs;
    p = rl_start_col + cur;
    in_abs = rl_start_abs + p / COLS;
    in_col = p % COLS;
    no_lazy = false;
}

static int word_start(const char *buf, int cur)
{
    bool q = false;
    int ws = 0;
    for (int i = 0; i < cur; i++) {
        if (buf[i] == '"') { q = !q; if (q) ws = i; }
        else if (!q && (buf[i] == ' ' || buf[i] == '|' || buf[i] == '&' || buf[i] == '>' || buf[i] == '<'))
            ws = i + 1;
    }
    return ws;
}

int read_line(char *buf, int max, bool use_history)
{
    int len = (int)strlen(buf), cur = len;
    int hidx = history_count();
    int tab_n = -1, tab_ws = 0;
    char tab_orig[64];
    int ret = 0;

    rl_start_abs = rl_max_abs = con_abs;
    rl_start_col = ccol;
    in_active = true;
    view_scroll = 0;
    rl_redraw(buf, len, cur);

    for (;;) {
        uint8_t k = key_wait();
        bool sec = ui_second;
        int ch;

        if (k == sk_2nd) { ui_second = !ui_second; continue; }
        ui_second = false;
        if (k != sk_Yequ) tab_n = -1;

        if (sec && (k == sk_Up || k == sk_Down)) {
            int lines = con_abs - con_top;
            view_scroll += (k == sk_Up) ? ROWS - 1 : -(ROWS - 1);
            if (view_scroll > lines - ROWS + 1) view_scroll = lines - ROWS + 1;
            if (view_scroll < 0) view_scroll = 0;
            continue;
        }
        view_scroll = 0;

        if (k == KEY_ON) {
            clear_break();
            ret = -1;
            break;
        }
        if (k == sk_Alpha) {
            if (sec) ui_mode = ui_mode == 1 ? 0 : 1;
            else ui_mode = (ui_mode + 1) % 3;
            continue;
        }
        if (k == sk_Mode && sec) { ret = -2; break; }
        if (k == sk_Enter) {
            if (sec && use_history && history_count()) {
                strncpy(buf, history_get(history_count() - 1), max - 1);
                buf[max - 1] = 0;
                len = cur = (int)strlen(buf);
            } else break;
        } else if (k == sk_Clear) {
            len = cur = 0;
        } else if (k == sk_Del) {
            if (sec) {
                if (cur < len) { memmove(buf + cur, buf + cur + 1, len - cur); len--; }
            } else if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur + 1);
                cur--; len--;
            }
        } else if (k == sk_Left) {
            cur = sec ? 0 : (cur > 0 ? cur - 1 : 0);
        } else if (k == sk_Right) {
            cur = sec ? len : (cur < len ? cur + 1 : len);
        } else if ((k == sk_Up || k == sk_Down) && use_history) {
            int n = history_count();
            if (k == sk_Up && hidx > 0) hidx--;
            else if (k == sk_Down && hidx < n) hidx++;
            if (hidx < n) strncpy(buf, history_get(hidx), max - 1);
            else buf[0] = 0;
            buf[max - 1] = 0;
            len = cur = (int)strlen(buf);
        } else if (k == sk_Yequ && use_history) {
            char out[80];
            bool first;
            int i, ol;
            if (tab_n < 0) {
                tab_ws = word_start(buf, cur);
                i = tab_ws;
                if (buf[i] == '"') i++;
                ol = cur - i;
                if (ol > 63) ol = 63;
                memcpy(tab_orig, buf + i, ol);
                tab_orig[ol] = 0;
                tab_n = 0;
            } else tab_n++;
            first = true;
            for (i = 0; i < tab_ws; i++) if (buf[i] != ' ' && buf[i] != '@') { first = false; break; }
            if (!complete_word(tab_orig, first, tab_n, out, sizeof out)) {
                if (tab_n == 0 || !complete_word(tab_orig, first, (tab_n = 0), out, sizeof out))
                    continue;
            }
            {
                char rep[84];
                int rl;
                if (strchr(out, ' ')) sprintf(rep, "\"%s\"", out);
                else strcpy(rep, out);
                rl = (int)strlen(rep);
                if (tab_ws + rl + (len - cur) >= max) continue;
                memmove(buf + tab_ws + rl, buf + cur, len - cur + 1);
                memcpy(buf + tab_ws, rep, rl);
                len = tab_ws + rl + (len - cur);
                cur = tab_ws + rl;
            }
        } else if ((ch = key_to_char(k, sec, ui_mode))) {
            if (ch == '\t') ch = ' ';
            if (len < max - 1) {
                memmove(buf + cur + 1, buf + cur, len - cur + 1);
                buf[cur++] = (char)ch;
                len++;
            }
        }
        buf[len] = 0;
        rl_redraw(buf, len, cur);
    }
    buf[len] = 0;
    rl_redraw(buf, len, len);   /* leaves the console positioned after the text */
    in_active = false;
    if (ret == -1) { con_putc('^'); con_putc('C'); }
    if (ret == 0 && use_history) history_add(buf);
    con_putc('\n');
    return ret;
}

/* ---------------------------------------------------------------- */
/* pager                                                             */
static bool pg_on;
static int pg_lines;

void pager_reset(bool on)
{
    pg_on = on && !out_cap && !out_nul;
    pg_lines = 0;
}

bool pager_line(void)
{
    int k;
    if (!pg_on) return !g_break;
    if (g_break) return false;
    if (++pg_lines < ROWS - 1) return true;
    {
        const char *m = "-- More --";
        while (*m) con_putc(*m++);
    }
    k = wait_any_key();
    memset(line_at(con_abs), 0, COLS);
    ccol = 0;
    if (k == sk_Clear || k == KEY_ON) return false;
    pg_lines = (k == sk_Enter) ? ROWS - 2 : 0;
    return true;
}
