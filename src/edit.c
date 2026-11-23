/*
 * edit.c - full-screen text editor (EDIT / NOTEPAD)
 *
 * [y=]    save          [graph] quit
 * arrows  move          2nd+left/right  home/end
 * 2nd+up/down  page     del / 2nd+del   backspace / delete
 */
#include "cmd.h"
#include <graphx.h>
#include <ti/getcsc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define ED_TOP 12
#define ED_MAX FS_MAXFILE

static char *ed_text;
static int ed_len, ed_cur, ed_top, ed_left, ed_want = -1;
static bool ed_dirty, ed_ro;
static char ed_msg[32];
static char ed_name[FS_NAMELEN];

static int line_start(int pos)
{
    while (pos > 0 && ed_text[pos - 1] != '\n') pos--;
    return pos;
}

static int line_end(int pos)
{
    while (pos < ed_len && ed_text[pos] != '\n') pos++;
    return pos;
}

static int line_no(int pos)
{
    int n = 0;
    for (int i = 0; i < pos; i++) if (ed_text[i] == '\n') n++;
    return n;
}

static void ed_render(void)
{
    int ln = line_no(ed_cur), col = ed_cur - line_start(ed_cur);
    int pos = 0, l = 0;
    char st[32];

    if (ln < ed_top) ed_top = ln;
    if (ln >= ed_top + ROWS) ed_top = ln - ROWS + 1;
    if (col < ed_left) ed_left = col;
    if (col >= ed_left + COLS) ed_left = col - COLS + 1;

    while (l < ed_top && pos < ed_len) { if (ed_text[pos++] == '\n') l++; }

    gfx_SetColor(1);
    gfx_FillRectangle_NoClip(0, ED_TOP, 320, ROWS * 8);
    gfx_SetTextBGColor(1);
    gfx_SetTextFGColor(15);
    for (int r = 0; r < ROWS; r++) {
        int le, c;
        if (pos > ed_len || (pos == ed_len && r > 0 && (ed_len == 0 || ed_text[ed_len - 1] != '\n'))) break;
        le = line_end(pos);
        for (c = 0; c < COLS; c++) {
            int i = pos + ed_left + c;
            char ch;
            if (i >= le) break;
            ch = ed_text[i];
            if (ch != ' ') {
                if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?';
                gfx_SetTextXY(c * 8, ED_TOP + r * 8);
                gfx_PrintChar(ch);
            }
        }
        if (ed_left && le > pos) {
            gfx_SetTextFGColor(14);
            gfx_SetTextXY(0, ED_TOP + r * 8);
            gfx_PrintChar('<');
            gfx_SetTextFGColor(15);
        }
        if (le >= ed_len) { pos = ed_len + 1; continue; }
        pos = le + 1;
    }
    if (ui_cursor_on()) {
        gfx_SetColor(14);
        gfx_FillRectangle_NoClip((col - ed_left) * 8, ED_TOP + (ln - ed_top) * 8 + 6, 8, 2);
    }
    ui_draw_title();
    if (ed_msg[0]) snprintf(st, sizeof st, "%s", ed_msg);
    else snprintf(st, sizeof st, "%d:%d%s y=Save graph=Quit", ln + 1, col + 1, ed_dirty ? "*" : "");
    ui_draw_status(st);
    gfx_SwapDraw();
}

static bool ed_insert(char c)
{
    if (ed_len >= ED_MAX) { strcpy(ed_msg, "File too large!"); return false; }
    memmove(ed_text + ed_cur + 1, ed_text + ed_cur, ed_len - ed_cur);
    ed_text[ed_cur++] = c;
    ed_len++;
    ed_dirty = true;
    return true;
}

static bool ed_save(const char *path)
{
    int e;
    if (ed_ro) { strcpy(ed_msg, "Read-only file!"); return false; }
    e = fs_write_path(path, ed_text, ed_len, false);
    if (e) {
        strcpy(ed_msg, e == -3 ? "Disk full!" : "Save failed!");
        return false;
    }
    ed_dirty = false;
    snprintf(ed_msg, sizeof ed_msg, "Saved %d bytes", ed_len);
    return true;
}

int edit_file(const char *path)
{
    char save_title[sizeof con_title];
    char *fpath;
    int n, rc = 0;

    if (out_cap || out_nul) { err_out("EDIT needs the screen.\n"); return 1; }
    fpath = malloc(LINE_MAX);
    ed_text = malloc(ED_MAX + 1);
    if (!fpath || !ed_text) {
        free(fpath);
        free(ed_text);
        err_out("Not enough memory.\n");
        return 1;
    }
    strncpy(fpath, path, LINE_MAX - 1);
    fpath[LINE_MAX - 1] = 0;
    ed_len = ed_cur = ed_top = ed_left = 0;
    ed_dirty = ed_ro = false;
    ed_msg[0] = 0;
    ed_want = -1;

    n = fs_resolve(fpath);
    if (n >= 0) {
        size_t sz;
        char *t;
        if (fs_nodes[n].attr & FA_DIR) {
            free(fpath); free(ed_text);
            err_out("Access is denied.\n");
            return 1;
        }
        t = fs_read((uint8_t)n, &sz);
        if (!t || sz > ED_MAX) {
            free(t); free(fpath); free(ed_text);
            err_out("File too large to edit.\n");
            return 1;
        }
        memcpy(ed_text, t, sz);
        ed_len = (int)sz;
        free(t);
        ed_ro = (fs_nodes[n].attr & FA_RO) != 0;
        strncpy(ed_name, fs_nodes[n].name, FS_NAMELEN - 1);
    } else {
        char nm[FS_NAMELEN];
        if (fs_split(fpath, nm) < 0 || !fs_valid_name(nm)) {
            free(fpath); free(ed_text);
            err_out("The system cannot find the path specified.\n");
            return 1;
        }
        strcpy(ed_name, nm);
        strcpy(ed_msg, "New file");
    }
    ed_name[FS_NAMELEN - 1] = 0;

    strcpy(save_title, con_title);
    snprintf(con_title, sizeof con_title, "Edit - %s%s", ed_name, ed_ro ? " [RO]" : "");
    ui_render_hook = ed_render;

    for (;;) {
        uint8_t k = key_wait();
        bool sec = ui_second;
        int ch;
        if (k == sk_2nd) { ui_second = !ui_second; continue; }
        ui_second = false;
        if (k != sk_Yequ) ed_msg[0] = 0;
        if (k != sk_Up && k != sk_Down) ed_want = -1;

        if (k == sk_Alpha) {
            if (sec) ui_mode = ui_mode == 1 ? 0 : 1;
            else ui_mode = (ui_mode + 1) % 3;
        } else if (k == sk_Yequ) {
            ed_save(fpath);
        } else if (k == sk_Graph || (k == sk_Mode && sec) || k == KEY_ON) {
            if (k == KEY_ON) clear_break();
            if (!ed_dirty) break;
            strcpy(ed_msg, "Save? enter=Y del=N clr=back");
            ed_render();
            for (;;) {
                uint8_t a = key_wait();
                if (a == sk_Enter) { if (ed_save(fpath)) goto done; break; }
                if (a == sk_Del) goto done;
                if (a == sk_Clear || a == KEY_ON) { ed_msg[0] = 0; break; }
            }
        } else if (k == sk_Left) {
            if (sec) ed_cur = line_start(ed_cur);
            else if (ed_cur > 0) ed_cur--;
        } else if (k == sk_Right) {
            if (sec) ed_cur = line_end(ed_cur);
            else if (ed_cur < ed_len) ed_cur++;
        } else if (k == sk_Up || k == sk_Down) {
            int reps = sec ? ROWS - 1 : 1;
            int col = ed_cur - line_start(ed_cur);
            if (ed_want < 0) ed_want = col;
            while (reps--) {
                int ls = line_start(ed_cur);
                if (k == sk_Up) {
                    int pls;
                    if (ls == 0) break;
                    pls = line_start(ls - 1);
                    ed_cur = pls + ed_want < ls - 1 ? pls + ed_want : ls - 1;
                } else {
                    int le = line_end(ed_cur), nle;
                    if (le >= ed_len) break;
                    nle = line_end(le + 1);
                    ed_cur = le + 1 + ed_want < nle ? le + 1 + ed_want : nle;
                }
            }
        } else if (k == sk_Enter) {
            ed_insert('\n');
        } else if (k == sk_Del) {
            if (ed_ro) continue;
            if (sec) {
                if (ed_cur < ed_len) {
                    memmove(ed_text + ed_cur, ed_text + ed_cur + 1, ed_len - ed_cur - 1);
                    ed_len--;
                    ed_dirty = true;
                }
            } else if (ed_cur > 0) {
                memmove(ed_text + ed_cur - 1, ed_text + ed_cur, ed_len - ed_cur);
                ed_cur--;
                ed_len--;
                ed_dirty = true;
            }
        } else if (k == sk_Clear) {
            /* clear: delete to end of line */
            int le = line_end(ed_cur);
            if (ed_ro || le == ed_cur) continue;
            memmove(ed_text + ed_cur, ed_text + le, ed_len - le);
            ed_len -= le - ed_cur;
            ed_dirty = true;
        } else if ((ch = key_to_char(k, sec, ui_mode))) {
            if (ed_ro) continue;
            if (ch == '\t') { for (int i = 0; i < 4; i++) ed_insert(' '); }
            else ed_insert((char)ch);
        }
    }
done:
    ui_render_hook = NULL;
    strcpy(con_title, save_title);
    free(ed_text);
    free(fpath);
    ed_text = NULL;
    con_render();
    return rc;
}
