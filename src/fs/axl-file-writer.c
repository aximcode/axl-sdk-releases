/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-file-writer.c
    Streaming (out-of-core) file writer — the write peer of AxlFileView.

    Opens a backend file handle once and writes straight through to it,
    so a multi-GB payload (a WebDAV PUT, an uploaded ISO) never has to
    fit in memory. Replace (truncate-in-place) by default, append or
    exclusive-create via flags.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-fs.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>   /* axl_utf8_to_ucs2 */

struct AxlFileWriter {
    AxlFileHandle handle;
    uint64_t      written;   /* bytes written (includes initial size on APPEND) */
    bool          failed;    /* a write failed; further writes are rejected */
};

AxlFileWriter *
axl_file_writer_open(
    const char *path,
    uint32_t    flags
    )
{
    if (path == NULL) {
        return NULL;
    }

    bool append = (flags & AXL_FILE_WRITER_APPEND) != 0u;
    bool excl   = (flags & AXL_FILE_WRITER_EXCL) != 0u;

    /* Exclusive create: fail if the path already exists. Single-threaded
       UEFI file I/O makes the probe-then-create window benign. */
    if (excl) {
        AxlFsEntry existing;
        if (axl_file_info(path, &existing) == AXL_OK) {
            return NULL;
        }
    }

    unsigned short *wide = axl_utf8_to_ucs2(path);
    if (wide == NULL) {
        return NULL;
    }

    AxlFileHandle handle;
    int rc = axl_backend_file_open(
        (const unsigned short *)wide,
        AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE,
        0, &handle);
    axl_free(wide);
    if (rc != AXL_OK) {
        return NULL;
    }

    uint64_t start = 0;
    if (append) {
        /* Continue from the current end of file. */
        int64_t sz = axl_backend_file_get_size(handle);
        if (sz > 0) {
            if (axl_backend_file_set_position(handle, (uint64_t)sz) != AXL_OK) {
                axl_backend_file_close(&handle);
                return NULL;
            }
            start = (uint64_t)sz;
        }
    } else {
        /* Replace: truncate any existing content to empty so a shorter
           new payload leaves no stale tail. */
        if (axl_backend_file_set_size(handle, 0) != AXL_OK) {
            axl_backend_file_close(&handle);
            return NULL;
        }
        axl_backend_file_set_position(handle, 0);
    }

    AxlFileWriter *w = axl_calloc(1, sizeof(*w));
    if (w == NULL) {
        axl_backend_file_close(&handle);
        return NULL;
    }
    w->handle  = handle;
    w->written = start;
    w->failed  = false;
    return w;
}

int
axl_file_writer_write(
    AxlFileWriter *w,
    const void    *buf,
    size_t         len
    )
{
    if (w == NULL || (buf == NULL && len > 0)) {
        return AXL_ERR;
    }
    if (w->failed) {
        return AXL_ERR;
    }
    if (len == 0) {
        return AXL_OK;
    }

    size_t n  = len;
    int    rc = axl_backend_file_write(w->handle, &n, buf);
    w->written += n;              /* count even a partial advance */
    if (rc != AXL_OK || n != len) {
        w->failed = true;
        return AXL_ERR;
    }
    return AXL_OK;
}

uint64_t
axl_file_writer_tell(
    const AxlFileWriter *w
    )
{
    return (w != NULL) ? w->written : 0;
}

int
axl_file_writer_close(
    AxlFileWriter *w
    )
{
    if (w == NULL) {
        return AXL_OK;
    }
    bool failed = w->failed;
    int  rc     = axl_backend_file_close(&w->handle);   /* flush + close */
    axl_free(w);
    return (failed || rc != AXL_OK) ? AXL_ERR : AXL_OK;
}
