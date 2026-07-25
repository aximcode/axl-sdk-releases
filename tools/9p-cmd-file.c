/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-cmd-file.c - the `9p` one-shot verbs: ls, get, put.
 *
 * Each opens a synchronous Axl9pClient session, does one thing, and
 * disconnects - no driver, no residency, nothing left behind. That is the
 * whole difference from serve/mount, which deploy resident DXE drivers
 * (9p-cmd-serve.c, 9p-cmd-mount.c).
 *
 * Server addresses are written host[:port]; the port defaults to the 9P
 * well-known 564. The splitter lives in 9p-common.c because `mount` names
 * a server too and must accept the identical form.
 *
 * Diagnostic convention, because every verb here straddles the wire: an
 * unqualified "9p: cannot <verb> %s" names a LOCAL path, and the "%s on the
 * server" form names a REMOTE one. `get` and `put` each emit one of each.
 */

#include <axl.h>

#include "9p-common.h"

// ---------------------------------------------------------------------------
// Flags and positionals - non-static; 9p.c builds the verb tree from them
// ---------------------------------------------------------------------------

const AxlArgDesc axl9p_file_flags[] = {
    AXL_9P_NET_ARG_NIC,
    { 0 }
};

const AxlArgDesc axl9p_ls_positional[] = {
    { .name = "server", .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { .name = "path",   .type = AXL_ARG_STRING, .default_value = "/",
      .help = "directory on the server" },
    { 0 }
};

const AxlArgDesc axl9p_get_positional[] = {
    { .name = "server",  .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { .name = "path",    .type = AXL_ARG_STRING, .required = true,
      .help = "file on the server" },
    { .name = "outfile", .type = AXL_ARG_STRING,
      .help = "local destination (default: write to stdout)" },
    { 0 }
};

const AxlArgDesc axl9p_put_positional[] = {
    { .name = "infile", .type = AXL_ARG_STRING, .required = true,
      .help = "local file to send" },
    { .name = "server", .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { .name = "path",   .type = AXL_ARG_STRING, .required = true,
      .help = "destination path on the server" },
    { 0 }
};

// ---------------------------------------------------------------------------
// Session helpers - used only by this file's three verbs
// ---------------------------------------------------------------------------

/* Bring the NIC up and open a session. Returns NULL after reporting the
   failure itself, so every verb's error path is one `if`. */
static Axl9pClient *
open_session(
    AxlArgs    *a,
    const char *spec
)
{
    char         host[AXL_9P_HOST_MAX];
    /* Seeds axl9p_split_host_port's IN/OUT port: these verbs have no
       --port flag, so an address without an inline ":port" lands on the
       9P well-known port. */
    uint16_t     port = AXL_9P_PORT_DEFAULT;
    Axl9pClient *c    = NULL;

    if (!axl9p_split_host_port(spec, host, sizeof(host), &port)) {
        axl_printerr("9p: bad server address '%s' (want host or host:port)\n",
                     spec != NULL ? spec : "");
        return NULL;
    }
    if (axl_net_init(axl_args_get_uint(a, "nic"), 10) != AXL_OK) {
        axl_printerr("9p: could not bring a NIC online\n");
        return NULL;
    }
    if (axl_9p_connect(host, port, "", NULL, &c) != AXL_OK) {
        axl_printerr("9p: connect to %s:%u failed\n", host, (unsigned)port);
        return NULL;
    }
    return c;
}

// ---------------------------------------------------------------------------
// Verbs
// ---------------------------------------------------------------------------

int
axl9p_ls_handler(
    AxlArgs *a
)
{
    Axl9pClient *c;
    AxlArray    *entries = NULL;
    const char  *path    = axl_args_get_string(a, "path");
    size_t       i;

    c = open_session(a, axl_args_get_string(a, "server"));
    if (c == NULL) {
        return 1;
    }
    if (axl_9p_list(c, path, &entries) != AXL_OK) {
        axl_printerr("9p: cannot list %s\n", path);
        axl_9p_disconnect(c);
        return 1;
    }
    for (i = 0; i < axl_array_len(entries); i++) {
        const AxlFsEntry *e = (const AxlFsEntry *)axl_array_get(entries, i);
        axl_printf("%c %llu %s\n",
                   axl_fs_entry_is_dir(e) ? 'd' : 'f',
                   (unsigned long long)e->size, e->name);
    }
    axl_array_free(entries);
    axl_9p_disconnect(c);
    return 0;
}

int
axl9p_get_handler(
    AxlArgs *a
)
{
    Axl9pClient *c;
    AxlBytes    *data    = NULL;
    const char  *path    = axl_args_get_string(a, "path");
    const char  *outfile = axl_args_get_string(a, "outfile");
    const void  *buf;
    size_t       len     = 0;
    int          rc      = 0;

    c = open_session(a, axl_args_get_string(a, "server"));
    if (c == NULL) {
        return 1;
    }
    /* "on the server" is load-bearing: `put`'s failure to read its LOCAL
       infile prints the unqualified form. One message for both sides of the
       wire would leave a human unable to tell which end failed. */
    if (axl_9p_read_file(c, path, &data) != AXL_OK) {
        axl_printerr("9p: cannot read %s on the server\n", path);
        axl_9p_disconnect(c);
        return 1;
    }
    buf = axl_bytes_get_data(data, &len);
    if (outfile != NULL && outfile[0] != '\0') {
        /* A zero-byte remote file leaves axl_bytes' data pointer NULL, which
           axl_file_set_contents rejects outright -- hand it a valid empty
           buffer so an empty remote file still produces an empty local one. */
        if (axl_file_set_contents(outfile, buf != NULL ? buf : "", len) == AXL_OK) {
            axl_printf("9p: wrote %llu bytes to %s\n",
                       (unsigned long long)len, outfile);
        } else {
            axl_printerr("9p: cannot write %s\n", outfile);
            rc = 1;
        }
    } else if (len > 0) {
        /* Byte-exact, like cat.c: the payload is not NUL-terminated and may
           contain NULs, so a "%.*s" here would both scan past the allocation
           and silently truncate a binary file at its first zero byte. */
        if (axl_write(axl_stdout, buf, len) != (axl_ssize_t)len) {
            axl_printerr("9p: cannot write %s to stdout\n", path);
            rc = 1;
        }
    }
    axl_bytes_unref(data);
    axl_9p_disconnect(c);
    return rc;
}

int
axl9p_put_handler(
    AxlArgs *a
)
{
    Axl9pClient *c;
    const char  *infile = axl_args_get_string(a, "infile");
    const char  *path   = axl_args_get_string(a, "path");
    void        *buf    = NULL;
    size_t       len    = 0;
    int          rc     = 0;

    /* Unqualified: this is the LOCAL infile. `get`'s remote-read failure
       says "on the server" - see the note there. */
    if (axl_file_get_contents(infile, &buf, &len) != AXL_OK) {
        axl_printerr("9p: cannot read %s\n", infile);
        return 1;
    }
    c = open_session(a, axl_args_get_string(a, "server"));
    if (c == NULL) {
        axl_free(buf);
        return 1;
    }
    if (axl_9p_write_file(c, path, buf, len) == AXL_OK) {
        axl_printf("9p: put %llu bytes to %s\n",
                   (unsigned long long)len, path);
    } else {
        axl_printerr("9p: cannot write %s on the server\n", path);
        rc = 1;
    }
    axl_free(buf);
    axl_9p_disconnect(c);
    return rc;
}
