/**
 * console-readline-selftest.c — drive axl_console_readline / _ex and the
 * interactive axl_stdin fallback over a serial console, so a QEMU
 * serial-stdin harness can verify the echoed, Backspace-edited,
 * Enter-terminated line contract end-to-end.
 *
 * Sequence (one line fed by the harness per step):
 *   1. Flush any startup type-ahead, print "READLINE-READY".
 *   2. axl_console_readline           → "LINE1=[...]"   (basic line)
 *   3. axl_console_readline           → "LINE2=[...]"   (Backspace edit)
 *   4. axl_console_readline           → "LINE3=[...]"   (empty line)
 *   5. axl_console_readline_ex(echo=0)→ "PASS4=[...]"   (hidden entry)
 *   6. axl_readline(axl_stdin)        → "FALLBACK=[...]" (transparent
 *                                        console line-editing fallback)
 *   7. Print "READLINE-DONE".
 *
 * Each interactive step uses a finite whole-line deadline so a mis-fed
 * key can never wedge the run indefinitely (the outer QEMU timeout is the
 * final backstop); on -1 it prints "LINEn=<TIMEOUT>" and continues.
 *
 * Run via test/integration/test-console-readline-qemu.sh.
 */

#include <axl.h>

/* Generous per-line budget: the harness injects a full line within a
   second or two, so 30 s only ever fires on a genuinely broken feed. */
#define LINE_TIMEOUT_MS  30000ULL

static void
read_and_print(
    const char  *marker,
    uint64_t     max_len,
    bool         echo
    )
{
    char *line = NULL;
    int   rc   = axl_console_readline_ex(LINE_TIMEOUT_MS, (size_t)max_len,
                                         echo, &line);
    if (rc != AXL_OK) {
        axl_printf("%s=<TIMEOUT>\n", marker);
        return;
    }
    axl_printf("%s=[%s]\n", marker, line);
    axl_free(line);
}

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    /* Eat the Enter that launched us / any startup.nsh type-ahead. */
    axl_console_flush_input();
    axl_print("READLINE-READY\n");

    read_and_print("LINE1", 0, true);    /* basic echoed line          */
    read_and_print("LINE2", 0, true);    /* Backspace mid-line edit    */
    read_and_print("LINE3", 0, true);    /* immediate Enter -> ""      */
    read_and_print("PASS4", 0, false);   /* hidden (password) entry    */

    /* Transparent fallback: reading axl_stdin on an interactive console
       must line-cook via the same editor. axl_readline returns the line
       WITH its trailing '\n' (as it does for piped input); trim it so
       the marker shows the exact typed text. */
    char *fb = axl_readline(axl_stdin);
    if (fb == NULL) {
        axl_print("FALLBACK=<EOF>\n");
    } else {
        size_t n = axl_strlen(fb);
        if (n > 0 && fb[n - 1] == '\n') {
            fb[n - 1] = '\0';
        }
        axl_printf("FALLBACK=[%s]\n", fb);
        axl_free(fb);
    }

    /* Also confirm the predicate reports interactive on this console. */
    axl_printf("INTERACTIVE=%s\n",
               axl_stdin_is_interactive() ? "true" : "false");

    axl_print("READLINE-DONE\n");
    return 0;
}
