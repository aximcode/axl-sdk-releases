/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* io-streams.c — emits one marker per stream; driven under redirect
 * operators by test-io-redirect-qemu.sh to prove sink separation.
 *
 *   io-streams.efi        the three text markers, then 8 raw bytes on stderr
 *   io-streams.efi text   the three text markers only (the sink cases, whose
 *                         subject is the text path)
 *   io-streams.efi raw    8 raw bytes on stdout, then 8 on stderr, each taken
 *                         from a longer buffer with NO NUL after them: the
 *                         Shell's console file prints what it is given as a
 *                         NUL-terminated UCS-2 string, so a raw write handed
 *                         to it as-is reads on past the bytes into the
 *                         filler -- on the screen as garbage.
 *   io-streams.efi split  UTF-16LE text in two raw stdout writes: the mark and
 *                         "ab", then "cd" and a newline with no mark -- the
 *                         way `cat --raw` hands over a UTF-16 file in
 *                         buffer-sized pieces. */
#include <axl.h>

AXL_LOG_DOMAIN("io-streams");

/* Not const and not string literals: no NUL anywhere near the bytes. */
static char gRawOut[24] = { 'R', 'A', 'W', ':', 'o', 'u', 't', '\n',
                            '#', '#', '#', '#', '#', '#', '#', '#',
                            '#', '#', '#', '#', '#', '#', '#', '#' };
static char gRawErr[24] = { 'R', 'A', 'W', ':', 'e', 'r', 'r', '\n',
                            '#', '#', '#', '#', '#', '#', '#', '#',
                            '#', '#', '#', '#', '#', '#', '#', '#' };

static char gSplitA[] = { (char)0xFF, (char)0xFE, 'a', 0, 'b', 0 };
static char gSplitB[] = { 'c', 0, 'd', 0, '\r', 0, '\n', 0 };

int
main(int argc, char **argv)
{
    if (argc > 1 && axl_strcmp(argv[1], "split") == 0) {
        axl_write(axl_stdout_raw, gSplitA, sizeof(gSplitA));
        axl_write(axl_stdout_raw, gSplitB, sizeof(gSplitB));
        return 0;
    }
    if (argc > 1 && axl_strcmp(argv[1], "raw") == 0) {
        axl_write(axl_stdout_raw, gRawOut, 8);
        axl_write(axl_stderr_raw, gRawErr, 8);
        return 0;
    }
    axl_print("OUT:stdout\n");        /* -> ConOut (stdout) */
    axl_printerr("ERR:stderr\n");     /* -> stderr sink */
    axl_warning("LOG:warn");          /* diagnostic log */
    if (argc > 1 && axl_strcmp(argv[1], "text") == 0) {
        return 0;
    }
    axl_write(axl_stderr_raw, gRawErr, 8);   /* binary -> shell StdErr handle */
    return 0;
}
