/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file paste.c
    Print the AXL clipboard to standard output (pbpaste / `xclip -o`).

    Build with axl-cc:
      axl-cc paste.c -o paste.efi

    Usage:
      paste                 # write the clipboard bytes to stdout
      paste > out.bin       # ... or to a file
      paste --mime          # print the stored MIME type instead

    The clipboard is AXL shared memory (axl-shm), so this prints what an
    earlier `clip` (even from a separate invocation) put there in this
    boot. Output is raw bytes (binary-clean) via axl_stdout_raw.
**/

#include <axl.h>

static const AxlArgDesc flags[] = {
    { .name = "mime", .short_name = 'm', .type = AXL_ARG_BOOL,
      .help = "Print the clipboard's MIME type instead of its contents" },
    {0}
};

static int
run_paste(AxlArgs *a)
{
    size_t      len = 0;
    const char *mime = NULL;
    const void *data = axl_clipboard_get(&len, &mime);

    if (axl_args_get_bool(a, "mime")) {
        if (mime != NULL) {
            axl_printf("%s\n", mime);
        }
        return 0;
    }

    if (axl_stdout_raw == NULL) {
        axl_printerr("paste: no binary output stream\n");
        return 1;
    }
    if (data != NULL && len > 0) {
        if (axl_write(axl_stdout_raw, data, len) != (axl_ssize_t)len) {
            axl_printerr("paste: short write to stdout\n");
            return 1;
        }
    }
    return 0;
}

AXL_TOOL_MAIN(paste)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name    = "paste",
        .help    = "Print the AXL clipboard to standard output (pbpaste-style)",
        .flags   = flags,
        .handler = run_paste,
    });
}
