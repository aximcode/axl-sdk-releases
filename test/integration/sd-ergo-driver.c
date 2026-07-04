/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* sd-ergo-driver.c — resident driver built ENTIRELY from AXL_SHARED_DRIVER.
 * Proves the turnkey ergonomics: app-logic-only run() reads stdin, writes
 * stdout, and arms an exit status; the SDK owns publish/bridge/exit-status.
 *
 * run() gets the launcher's argv verbatim (the canonical `int main`
 * contract — argv[0] is the program name), so the verb is argv[1]. */
#include <axl.h>

static int ergo_init(void)   { return 0; }   /* no heavy setup for the test */
static int ergo_unload(void) { return 0; }

static int
ergo_run(int argc, char **argv)
{
    if (argc >= 2 && argv[1] != NULL && axl_strcmp(argv[1], "echotext") == 0) {
        AxlStream *t = axl_stdin_text();
        char *l = (t != NULL) ? axl_readline(t) : NULL;
        if (l != NULL) {
            size_t n = axl_strlen(l);
            while (n > 0 && (l[n-1] == '\n' || l[n-1] == '\r')) { l[--n] = '\0'; }
        }
        axl_printf("ERGO:%s\n", l != NULL ? l : "<EOF>");
        axl_free(l);
        if (t != NULL) { axl_fclose(t); }
        return 0;
    }
    if (argc >= 2 && argv[1] != NULL && axl_strcmp(argv[1], "status") == 0) {
        axl_set_exit_status((AxlEfiStatus)0x12345678);   /* bit-63 clear: observable via %lasterror% */
        axl_printf("ERGOSTAT\n");
        return 0;
    }
    axl_printf("ERGO:<BADVERB>\n");
    return 1;
}

AXL_SHARED_DRIVER("axl/sd-ergo", ergo_init, ergo_run, ergo_unload)
