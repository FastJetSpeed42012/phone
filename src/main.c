/*
 * CE Command Prompt
 * A Windows CMD style command shell for the TI-84 Plus CE.
 */
#include "cmd.h"
#include <fileioc.h>
#include <ti/vars.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static int run(bool resumed, int retval);

/* entry point used when a program launched with START finishes */
static int resume_cb(void *data, int retval)
{
    (void)data;
    return run(true, retval);
}

int start_program(const char *name)
{
    char up[9];
    uint8_t h;
    int r;

    if (strlen(name) > 8) goto missing;
    strcpy(up, name);
    str_upper(up);
    if (!stricmp_(up, "CMD")) { err_out("CMD is already running.\n"); return 1; }
    h = ti_OpenVar(up, "r", OS_TYPE_PRGM);
    if (!h) h = ti_OpenVar(up, "r", OS_TYPE_PROT_PRGM);
    if (!h) goto missing;
    ti_Close(h);

    fs_save();
    ui_end();
    r = os_RunPrgm(up, NULL, 0, resume_cb);
    /* only returns on failure */
    ui_init();
    out_printf("Could not start %s (error %d).\n", up, r);
    return 1;

missing:
    err_out("The system cannot find the file ");
    err_out(name);
    err_out(".\n");
    return 1;
}

static int run(bool resumed, int retval)
{
    ui_init();
    fs_init();
    shell_init();
    if (resumed) out_printf("Program exited with code %d.\n\n", retval);
    else shell_banner();
    shell_loop();
    ui_end();
    fs_save();
    fs_archive_all();
    return 0;
}

#ifdef HOST
#define main host_main
#endif
int main(void)
{
    return run(false, 0);
}
