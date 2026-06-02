/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file clip.c
    Copy standard input to the AXL clipboard (pbcopy / `xclip` equivalent).

    Build with axl-cc:
      axl-cc clip.c -o clip.efi

    Usage:
      some-tool | clip            # copy stdin to the clipboard
      clip -m text/plain < file   # copy a file, tagging a MIME type
      clip --clear                # empty the clipboard

    The clipboard is AXL shared memory (axl-shm): it survives this tool
    exiting, so a later `paste` in the same boot prints it back. Bytes are
    taken raw from stdin (binary-clean), so it carries text or binary.
**/

#include <axl.h>

static const AxlArgDesc flags[] = {
    { .name = "mime", .short_name = 'm', .type = AXL_ARG_STRING,
      .help = "Tag the copied data with a MIME type (e.g. text/plain)" },
    { .name = "clear", .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Empty the clipboard instead of copying stdin" },
    {0}
};

/* Read all of @in into a growable buffer. Returns AXL_OK and sets
   *out_buf / *out_len (caller frees), or AXL_ERR on OOM. */
static int
read_all(AxlStream *in, void **out_buf, size_t *out_len)
{
    uint8_t *buf = NULL;
    size_t   len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            size_t nc = cap ? cap * 2 : 4096;
            if (nc <= cap) {            /* capacity doubling overflowed */
                axl_free(buf);
                return AXL_ERR;
            }
            uint8_t *nb = axl_realloc(buf, nc);
            if (nb == NULL) {
                axl_free(buf);
                return AXL_ERR;
            }
            buf = nb;
            cap = nc;
        }
        axl_ssize_t r = axl_read(in, buf + len, cap - len);
        if (r < 0) {
            axl_free(buf);
            return AXL_ERR;
        }
        if (r == 0) {
            break;          /* EOF */
        }
        len += (size_t)r;
    }
    *out_buf = buf;
    *out_len = len;
    return AXL_OK;
}

static int
run_clip(AxlArgs *a)
{
    if (axl_args_get_bool(a, "clear")) {
        axl_clipboard_clear();
        return 0;
    }

    if (axl_stdin == NULL) {
        axl_printerr("clip: no stdin (run as the right side of a pipe)\n");
        return 1;
    }

    void  *buf = NULL;
    size_t len = 0;
    if (read_all(axl_stdin, &buf, &len) != AXL_OK) {
        axl_printerr("clip: out of memory reading stdin\n");
        return 1;
    }

    const char *mime = axl_args_get_string(a, "mime");
    int rc = axl_clipboard_set(buf, len, mime);
    axl_free(buf);
    if (rc != AXL_OK) {
        axl_printerr("clip: failed to set the clipboard\n");
        return 1;
    }
    return 0;
}

AXL_TOOL_MAIN(clip)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name    = "clip",
        .help    = "Copy standard input to the AXL clipboard (pbcopy-style)",
        .flags   = flags,
        .handler = run_clip,
    });
}
