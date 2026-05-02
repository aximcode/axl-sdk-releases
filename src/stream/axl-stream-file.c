/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream-file.c
    File-backed AxlStream — vtable that bridges axl_fopen to the
    AxlBackend file API. Path-based filesystem operations (read-
    whole-file, dir walk, volume enumerate, etc.) live in
    `src/fs/axl-fs.c`; this file is the stream factory only.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include "axl-stream-internal.h"
AXL_LOG_DOMAIN("stream");

typedef struct {
    AxlFileHandle  handle;
} FileCtx;

// ---------------------------------------------------------------------------
// File vtable
// ---------------------------------------------------------------------------

static axl_ssize_t
file_write(void *ctx, const void *data, size_t count)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    rc = axl_backend_file_write(f->handle, &size, data);
    if (rc != 0) {
        axl_warning("write failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_read(void *ctx, void *data, size_t count)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    rc = axl_backend_file_read(f->handle, &size, data);
    if (rc != 0) {
        axl_warning("read failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_pread(void *ctx, void *data, size_t count, size_t offset)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t saved_pos;
    size_t size = count;
    int rc;

    /* Save current position */
    axl_backend_file_get_position(f->handle, &saved_pos);

    /* Seek to offset */
    rc = axl_backend_file_set_position(f->handle, (uint64_t)offset);
    if (rc != 0) {
        axl_warning("seek failed");
        return -1;
    }

    rc = axl_backend_file_read(f->handle, &size, data);

    /* Restore position */
    axl_backend_file_set_position(f->handle, saved_pos);

    if (rc != 0) {
        axl_warning("pread failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_pwrite(void *ctx, const void *data, size_t count, size_t offset)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t saved_pos;
    size_t size = count;
    int rc;

    axl_backend_file_get_position(f->handle, &saved_pos);

    rc = axl_backend_file_set_position(f->handle, (uint64_t)offset);
    if (rc != 0) {
        axl_warning("seek failed");
        return -1;
    }

    rc = axl_backend_file_write(f->handle, &size, data);

    axl_backend_file_set_position(f->handle, saved_pos);

    if (rc != 0) {
        axl_warning("pwrite failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static int
file_seek(void *ctx, int64_t offset, int whence)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t pos;

    if (whence == AXL_SEEK_SET) {
        if (offset < 0) {
            return -1;
        }
        pos = (uint64_t)offset;
    } else if (whence == AXL_SEEK_CUR) {
        uint64_t cur;
        if (axl_backend_file_get_position(f->handle, &cur) != 0) {
            return -1;
        }
        if (offset < 0 && (uint64_t)(-offset) > cur) {
            return -1;
        }
        pos = (uint64_t)((int64_t)cur + offset);
    } else if (whence == AXL_SEEK_END) {
        int64_t file_size = axl_backend_file_get_size(f->handle);
        if (file_size < 0) {
            return -1;
        }
        if (offset < 0 && (uint64_t)(-offset) > (uint64_t)file_size) {
            return -1;
        }
        pos = (uint64_t)(file_size + offset);
    } else {
        return -1;
    }

    return axl_backend_file_set_position(f->handle, pos);
}

static int64_t
file_tell(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t pos;

    if (axl_backend_file_get_position(f->handle, &pos) != 0) {
        return -1;
    }
    return (int64_t)pos;
}

static int
file_flush(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t zero = 0;

    /* UEFI: WriteFile with size 0 flushes */
    return axl_backend_file_write(f->handle, &zero, NULL);
}

static void
file_close(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;
    axl_backend_file_close(&f->handle);
    axl_free(f);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AxlStream *
axl_fopen(const char *path, const char *mode)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    uint64_t open_mode;
    int rc;
    FileCtx *f;
    AxlStream *s;

    if (path == NULL || mode == NULL) {
        return NULL;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_warning("utf8_to_ucs2 failed: %s", path);
        return NULL;
    }

    if (mode[0] == 'r') {
        open_mode = AXL_FILE_MODE_READ;
    } else if (mode[0] == 'w' || mode[0] == 'a') {
        /* 'a' currently behaves the same as 'w' — no true append
           mode yet. Caller-visible semantics remain consistent. */
        open_mode = AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE;
    } else {
        axl_free(wide_path);
        return NULL;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path, open_mode, 0, &handle);
    axl_free(wide_path);

    if (rc != 0) {
        axl_warning("open failed: %s", path);
        return NULL;
    }

    f = axl_new(FileCtx);
    if (f == NULL) {
        axl_warning("allocation failed");
        axl_backend_file_close(&handle);
        return NULL;
    }
    f->handle = handle;

    /* For append mode, seek to end */
    if (mode[0] == 'a') {
        axl_backend_file_set_position(handle, 0xFFFFFFFFFFFFFFFFULL);
    }

    s = axl_stream_new();
    if (s == NULL) {
        axl_warning("allocation failed");
        axl_backend_file_close(&handle);
        axl_free(f);
        return NULL;
    }

    s->ctx    = f;
    s->read   = file_read;
    s->write  = file_write;
    s->pread  = file_pread;
    s->pwrite = file_pwrite;
    s->seek   = file_seek;
    s->tell   = file_tell;
    s->flush  = file_flush;
    s->close  = file_close;

    return s;
}
