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
#include "axl-file-gen.h"

struct AxlFileWriter {
    AxlFileHandle handle;
    uint64_t      written;   /* bytes written (includes initial size on APPEND) */
    bool          failed;    /* a write failed; further writes are rejected */
    uint32_t      gen_key;   /* write registry slot for the target path */
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

    /* Bump HERE, not after the writer is built. The open above already
       mutated the file -- it carries CREATE, and the replace branch below
       truncates it to empty -- so every early return between this point
       and the return of `w` leaves a file that changed. Bumping late meant
       an OOM, or the append branch's set_position failure, left an open
       view serving the pre-truncate length and bytes. Unconditional and
       before the success check, same as every other write path. */
    uint32_t gen_key = axl_file_gen_key(path);
    axl_file_gen_bump_key(gen_key);

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
    w->gen_key = gen_key;
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
    axl_file_gen_bump_key(w->gen_key);   /* a partial write still moved bytes */
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
    /* Flush EXPLICITLY, then close. Close's own flush cannot report a
       failure — EFI_FILE_PROTOCOL.Close is specified to return only
       EFI_SUCCESS — so a durability-sensitive caller (a PUT handler
       deciding 201 vs 5xx) would have had nothing to check. The real
       flush primitive does report media/full/write-protected errors. */
    bool failed = w->failed;
    int  frc    = axl_backend_file_flush(w->handle);
    int  rc     = axl_backend_file_close(&w->handle);
    axl_free(w);
    return (failed || frc != AXL_OK || rc != AXL_OK) ? AXL_ERR : AXL_OK;
}
