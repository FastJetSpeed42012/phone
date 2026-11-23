/*
 * CE Command Prompt - a Windows-CMD-style shell for the TI-84 Plus CE
 * Shared declarations.
 */
#ifndef CMD_H
#define CMD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------- screen / console (ui.c) ---------------- */
#define COLS      40
#define ROWS      27
#define SB_LINES  200
#define LINE_MAX  240

/* palette slots 0..15 are the classic 16 console colors */
#define PAL_TITLE_BG  16
#define PAL_TITLE_FG  17
#define PAL_STAT_BG   18
#define PAL_STAT_FG   19
#define PAL_STAT_HI   20

extern uint8_t con_fg, con_bg;
extern char con_title[48];
extern bool g_break;           /* set when [ON] is pressed during a command */

extern uint8_t ui_mode;              /* 0 abc, 1 ABC, 2 123 */
extern bool ui_second;
extern void (*ui_render_hook)(void);  /* alternate full-screen renderer */
bool ui_cursor_on(void);
void ui_draw_title(void);
void ui_draw_status(const char *extra);
uint8_t key_poll(void);
int  start_program(const char *name);
int  cmd_if(int argc, char **argv, char *raw);
int  cmd_for(int argc, char **argv, char *raw);
void ui_init(void);
void ui_end(void);
void con_putc(char c);
void con_clear(void);
void con_render(void);
void con_render_lazy(void);
int  con_col(void);

/* key handling */
#define KEY_ON 0x80
uint8_t key_wait(void);             /* blocking, returns sk_* code or KEY_ON */
bool    check_break(void);          /* polls [ON]; sets g_break */
void    clear_break(void);
int     wait_any_key(void);         /* returns sk code */
int     key_to_char(uint8_t sk, bool second, uint8_t mode);

/* line input. returns 0 ok, -1 break([ON]), -2 quit (2nd+MODE) */
int  read_line(char *buf, int max, bool use_history);
void history_add(const char *s);
int  history_count(void);
const char *history_get(int i);

/* ---------------- output routing ---------------- */
typedef struct {
    char  *buf;
    size_t len, cap;
    bool   overflow;
} sbuf_t;

extern sbuf_t *out_cap;     /* capture target (pipes/redirection) or NULL */
extern bool    out_nul;     /* discard output */
void out_putc(char c);
void out_puts(const char *s);
void out_printf(const char *fmt, ...);
void out_nl(void);
void sb_init(sbuf_t *b);
void sb_free(sbuf_t *b);
bool sb_putc(sbuf_t *b, char c);
bool sb_puts(sbuf_t *b, const char *s);

/* pager used by MORE and DIR /P */
void pager_reset(bool on);
bool pager_line(void);      /* call after each printed line; false => user quit */

/* ---------------- virtual filesystem (fs.c) ---------------- */
#define FS_MAX      160
#define FS_NAMELEN  24
#define FS_MAXFILE  12000

#define FA_USED   0x01
#define FA_DIR    0x02
#define FA_RO     0x04
#define FA_HIDDEN 0x08
#define FA_SYS    0x10
#define FA_ARCH   0x20

typedef struct {
    char     name[FS_NAMELEN];
    uint8_t  parent;
    uint8_t  attr;
    uint16_t size;
    uint16_t date;
    uint16_t time;
} node_t;

extern node_t fs_nodes[FS_MAX];
extern uint8_t fs_cwd;
extern char fs_label[12];

void fs_init(void);
void fs_format(void);
void fs_save(void);
void fs_archive_all(void);
int  fs_child(uint8_t parent, const char *name);
int  fs_resolve(const char *path);                  /* node or -1 */
int  fs_split(const char *path, char *name);        /* parent of last component, fills name */
int  fs_create(uint8_t parent, const char *name, bool dir);
int  fs_mkdirs(const char *path);
bool fs_remove(uint8_t n);
void fs_path(uint8_t n, char *out);
bool fs_is_ancestor(uint8_t anc, uint8_t n);
char *fs_read(uint8_t n, size_t *size);             /* malloc'd, NUL terminated */
bool fs_write(uint8_t n, const char *data, size_t size);
bool fs_append(uint8_t n, const char *data, size_t size);
int  fs_list(uint8_t dir, uint8_t *out, int max);   /* sorted children */
bool fs_valid_name(const char *name);
void fs_touch(uint8_t n);
uint32_t fs_free_arc(void);
int  fs_used_nodes(void);
uint32_t fs_used_bytes(void);
bool wild_match(const char *pat, const char *name);
bool has_wild(const char *s);
int  fs_glob(const char *pattern, uint8_t *out, int max, uint8_t *dir_out);
int  fs_write_path(const char *path, const char *data, size_t len, bool append);
void fmt_date(uint16_t d, char *out);
void fmt_time(uint16_t t, char *out);
void now_packed(uint16_t *d, uint16_t *t);

/* ---------------- shell core (shell.c) ---------------- */
extern int  g_errorlevel;
extern bool g_echo;
extern bool g_quit;
extern sbuf_t *g_stdin;
extern size_t  g_stdin_pos;
extern bool g_in_batch;

void shell_init(void);
void shell_banner(void);
void shell_loop(void);
int  exec_line(const char *line, bool expand);
void print_prompt(void);

const char *env_get(const char *name);
bool env_set(const char *name, const char *val);
void env_list(const char *prefix);

int  split_args(char *s, char **argv, int max);
bool read_stdin_line(char *buf, int max);
char *trim(char *s);
int  stricmp_(const char *a, const char *b);
int  strnicmp_(const char *a, const char *b, size_t n);
const char *stristr_(const char *h, const char *n);
void str_upper(char *s);
bool is_switch(const char *a, const char *sw);
bool ask_yn(const char *q);
void err_out(const char *s);
int  run_batch_file(uint8_t node, int argc, char **argv);
int  batch_goto(const char *label);
int  batch_call_label(const char *label, char *rest);
void batch_shift(void);
int  batch_exit(int code);
void setlocal_push(bool delayed);
void setlocal_pop(void);
bool cmd_exists(const char *name);
extern bool g_delayed;

/* doskey macros */
const char *macro_get(const char *name);
bool macro_set(const char *name, const char *val);
void macro_list(void);

/* ---------------- commands (cmds.c) ---------------- */
/* a command returning EL_KEEP succeeded but leaves %ERRORLEVEL% alone */
#define EL_KEEP (-32000)
typedef int (*cmd_fn)(int argc, char **argv, char *raw);
typedef struct {
    const char *name;
    cmd_fn fn;
    const char *help;
} cmd_t;
extern const cmd_t commands[];
const cmd_t *find_cmd(const char *name);
int complete_word(const char *word, bool first, int nth, char *out, int max);

/* ---------------- editor (edit.c) ---------------- */
int edit_file(const char *path);

/* ---------------- expressions / regex (expr.c) ---------------- */
int  eval_set_a(const char *expr, long *result);   /* 0 ok, else error code */
bool re_search(const char *re, const char *text, bool icase);

#endif
