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
#include <axl/axl-fs.h>
#include "../fs/axl-file-gen.h"
#include "../fs/axl-fs-internal.h"
#include "../runtime/axl-signal-internal.h"
#include "axl-stream-eof.h"
AXL_LOG_DOMAIN("stream");

/* Size of the zero block a gap is filled from (see file_fill_gap): one
   firmware write per 64 KiB. 4 KiB chunks cost 90 ms per 8 MiB on OVMF
   against 39 ms. */
#define FILE_GAP_CHUNK  65536u

typedef struct {
    AxlFileHandle  handle;
    bool           writable;  /* opened for write: has state worth flushing */
    uint32_t       gen_key;   /* write registry slot for this path (writable only) */
    /* The position may lie past end of file: set by a seek or this
       stream's own shrink, cleared by the next sequential write, which
       checks for a gap once. */
    bool           gap_possible;
    /* The registry generation of gen_key as this stream last left it. Any
       other AXL writer -- a truncate by path, another stream's
       axl_ftruncate -- moves it, and may have shrunk the file below this
       stream's position, so a moved generation arms the gap check too. */
    uint32_t       gen_seen;
    /* The stream's position, tracked here rather than asked of the
       firmware: an AxlFsProvider volume does not support GetPosition at
       all, and every read, write and seek of this stream goes through this
       file, so the count is exact for a stream used as a stream. */
    uint64_t       pos;
    /* A restore of the handle to pos failed: it is somewhere else, and
       must be re-seeked before the next read or write. */
    bool           handle_off;
} FileCtx;

/* Record that this stream's file changed. THE choke point for stream
   writes: every buffered write, printf, raw write and positional write in
   AXL lands in file_write or file_pwrite, so one call in each is the whole
   coverage — no per-caller bump to forget. The key is computed once at
   axl_fopen, so this allocates nothing and cannot fail. */
static void
file_touched(FileCtx *f)
{
    if (f->writable) {
        axl_file_gen_bump_key(f->gen_key);
        f->gen_seen = axl_file_gen_read(f->gen_key);
    }
}

/* Make a write that starts at @p pos land on zeros, not on whatever the
   volume held before, when @p pos is past end of file.

   POSIX reads a hole left by writing past the end as zeros. The UEFI spec
   says nothing, and the EDK2-derived FAT driver does not do it: a WRITE
   past the end goes through FatGrowEof (ReadWrite.c), which allocates the
   clusters and moves the end of file but never writes the gap, so the
   bytes of a previously deleted file read back from it. Only its SetInfo
   path (FatExpandOFile) zero-fills. Writing the zeros here instead of
   growing through SetInfo makes the guarantee hold on every filesystem,
   an `AxlFsProvider` volume included, rather than on that one driver.

   On return the handle may be anywhere; the caller seeks to where it
   writes. A file whose length cannot be read is not filled -- the write
   goes ahead as it would have without this check, rather than failing. */
static int
file_fill_gap(FileCtx *f, uint64_t pos)
{
    int64_t end = axl_backend_file_get_size(f->handle);

    if (end < 0) {
        return AXL_OK;
    }
    if ((uint64_t)end >= pos) {
        return AXL_OK;
    }
    if (axl_backend_file_set_position(f->handle, (uint64_t)end) != AXL_OK) {
        return AXL_ERR;
    }

    /* Allocated per fill, not kept: gaps are rare, and a static block this
       size would be 64 KiB of every image that opens a file. */
    uint64_t gap   = pos - (uint64_t)end;
    size_t   chunk = (gap < FILE_GAP_CHUNK) ? (size_t)gap : FILE_GAP_CHUNK;
    uint8_t *zeros = axl_calloc(1, chunk);
    int      rc    = (zeros != NULL) ? AXL_OK : AXL_ERR;

    for (uint64_t left = gap; rc == AXL_OK && left > 0; ) {
        /* A large gap is a large synchronous write; stay Ctrl-C
           responsive, as the other whole-file operations do. */
        _axl_poll_break();
        size_t want = (left < chunk) ? (size_t)left : chunk;
        size_t size = want;
        rc = axl_backend_file_write(f->handle, &size, zeros);
        file_touched(f);   /* even a failed write may have moved bytes */
        if (rc != AXL_OK || size != want) {
            rc = AXL_ERR;
        }
        left -= want;
    }
    axl_free(zeros);
    return rc;
}

// ---------------------------------------------------------------------------
// File vtable
// ---------------------------------------------------------------------------

/* Put the handle back at the stream's position. If even that fails, say
   so, and the next read or write re-seeks before it moves a byte -- the
   alternative is landing it wherever the failure left the handle. */
static void
file_restore(FileCtx *f)
{
    f->handle_off = axl_backend_file_set_position(f->handle, f->pos) != AXL_OK;
}

/* Re-seek a handle a failed restore left elsewhere. */
static int
file_sync_handle(FileCtx *f)
{
    if (f->handle_off) {
        file_restore(f);
    }
    return f->handle_off ? AXL_ERR : AXL_OK;
}

static axl_ssize_t
file_write(void *ctx, const void *data, size_t count)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    if (file_sync_handle(f) != AXL_OK) {
        return -1;
    }

    /* Only after a seek or a shrink can the position be past the end, so
       only then is the length worth asking for. A shrink by another AXL
       writer shows as a generation this stream did not leave. (One by a
       writer outside this image does not: there is no change notification
       in UEFI.) */
    if ((f->gap_possible || axl_file_gen_read(f->gen_key) != f->gen_seen)
        && count > 0) {
        int fill = file_fill_gap(f, f->pos);
        file_restore(f);
        if (fill != AXL_OK || f->handle_off) {
            axl_debug("zero-filling the gap before a write failed");
            return -1;
        }
        f->gap_possible = false;
    }

    rc = axl_backend_file_write(f->handle, &size, data);
    file_touched(f);   /* even a failed write may have moved bytes */
    if (rc != 0) {
        /* Some bytes may have landed and moved the handle; the stream
           counts none of them, so a retry must start where it says. */
        file_restore(f);
        axl_debug("write failed");
        return -1;
    }
    f->pos += size;
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_read(void *ctx, void *data, size_t count)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    if (file_sync_handle(f) != AXL_OK) {
        return -1;
    }
    rc = axl_backend_file_read(f->handle, &size, data);
    if (rc != 0) {
        file_restore(f);
        axl_debug("read failed");
        return -1;
    }
    f->pos += size;
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_pread(void *ctx, void *data, size_t count, size_t offset)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    rc = axl_backend_file_set_position(f->handle, (uint64_t)offset);
    if (rc == 0) {
        rc = axl_backend_file_read(f->handle, &size, data);
    }
    file_restore(f);
    if (rc != 0) {
        axl_debug("pread failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_pwrite(void *ctx, const void *data, size_t count, size_t offset)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc = AXL_OK;

    /* Every exit puts the handle back where the stream is: the gap fill
       and the seek both move it, whether or not the write follows. */
    if (count > 0) {
        rc = file_fill_gap(f, (uint64_t)offset);
    }
    if (rc == AXL_OK) {
        rc = axl_backend_file_set_position(f->handle, (uint64_t)offset);
    }
    if (rc == AXL_OK) {
        rc = axl_backend_file_write(f->handle, &size, data);
        file_touched(f);   /* even a failed write may have moved bytes */
    }
    file_restore(f);
    if (rc != 0) {
        axl_debug("pwrite failed");
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
        if (offset < 0 && (uint64_t)(-offset) > f->pos) {
            return -1;
        }
        pos = (uint64_t)((int64_t)f->pos + offset);
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

    if (axl_backend_file_set_position(f->handle, pos) != AXL_OK) {
        return -1;
    }
    /* A later write checks for a gap; the seek itself extends nothing, as
       with lseek(2). */
    f->pos          = pos;
    f->handle_off   = false;
    f->gap_possible = true;
    return AXL_OK;
}

static int64_t
file_tell(void *ctx)
{
    return (int64_t)((FileCtx *)ctx)->pos;
}

static int
file_flush(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;

    /* A read-only handle holds no dirty state of ours, and the firmware
       answers a flush on one with EFI_ACCESS_DENIED — so flushing a read
       stream is a no-op success, the way stdio treats it. */
    if (!f->writable) {
        return AXL_OK;
    }
    return axl_backend_file_flush(f->handle);
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

/* The file backend's operations. Built here rather than assigned inline so
   the block stays driven by AXL_STREAM_OPS_INIT — a slot added to
   AxlStreamOps starts out NULL rather than uninitialised, whatever the
   struct grows into. There is only one caller today; the buffer backend's
   twin has two, because a stream-keyed accessor has to name the same set. */
static AxlStreamOps
file_ops(void)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;

    ops.read   = file_read;
    ops.write  = file_write;
    ops.pread  = file_pread;
    ops.pwrite = file_pwrite;
    ops.seek   = file_seek;
    ops.tell   = file_tell;
    ops.flush  = file_flush;
    ops.close  = file_close;
    return ops;
}

AxlStream *
axl_fopen(const char *path, const char *mode)
{
    AxlStreamOps ops = file_ops();
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
        axl_debug("utf8_to_ucs2 failed: %s", path);
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
        axl_debug("open failed: %s", path);
        return NULL;
    }

    f = axl_new(FileCtx);
    if (f == NULL) {
        axl_debug("allocation failed");
        axl_backend_file_close(&handle);
        return NULL;
    }
    f->handle   = handle;
    f->writable = (open_mode & AXL_FILE_MODE_WRITE) != 0u;
    f->gen_key  = axl_file_gen_key(path);
    f->gen_seen = axl_file_gen_read(f->gen_key);
    if (f->writable) {
        /* The open itself is a namespace change when the file did not
           exist (write modes carry CREATE), so a reader that has been
           watching an absent path learns about it here rather than on the
           first byte written. */
        file_touched(f);
    }

    /* For append mode, start at the end. The stream must KNOW where that
       is -- it tracks its own position -- so a length that cannot be read
       refuses the open rather than starting at 0 with the handle at the
       end. */
    if (mode[0] == 'a') {
        int64_t end = axl_backend_file_get_size(handle);
        if (end < 0
            || axl_backend_file_set_position(handle, (uint64_t)end) != AXL_OK) {
            axl_debug("append open: the file's length cannot be read: %s", path);
            file_close(f);
            return NULL;
        }
        f->pos = (uint64_t)end;
    }

    s = axl_stream_open_custom(f, &ops, "file");
    if (s == NULL) {
        /* A refused open never calls `close`, so the context is still ours
           to release — the same contract a consumer gets. file_close IS that
           release and nothing more (shut the handle, free the struct), so
           calling it directly cannot drift from what close does. */
        axl_debug("allocation failed");
        file_close(f);
        return NULL;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Handle forms of the AxlFs metadata calls (declared in axl-fs.h)
// ---------------------------------------------------------------------------

/* This stream's FileCtx, or NULL for any stream axl_fopen did not return --
   buffer, console, custom, or a wrapper built over a file stream. */
static FileCtx *
file_ctx(AxlStream *s)
{
    AxlStreamOps ops = file_ops();
    return (FileCtx *)axl_stream_ctx(s, &ops);
}

int
axl_ftruncate(AxlStream *s, uint64_t size)
{
    FileCtx *f;
    int64_t  before;
    int      rc;

    if (s == NULL) {
        return AXL_ERR;
    }
    f = file_ctx(s);
    if (f == NULL) {
        return AXL_UNSUPPORTED;
    }
    if (!f->writable) {
        return AXL_DENIED;
    }
    /* Buffered bytes first: written after the resize they would land past
       a shrink, or over a grow's zeros. */
    if (axl_fflush(s) != AXL_OK) {
        return AXL_ERR;
    }

    before = axl_backend_file_get_size(f->handle);
    rc     = axl_fs_handle_truncate(f->handle, size);
    file_touched(f);          /* even a failed resize may have changed it */
    f->gap_possible = true;   /* a shrink can leave the position past the end */
    if (rc == AXL_OK && before >= 0 && size > (uint64_t)before
        && axl_feof(s)) {
        /* The sticky end-of-file flag described the old, shorter file.
           Clear it alone: axl_fseek would also drop transcode bytes still
           pending, and axl_clearerr the error flag. */
        _axl_stream_clear_eof(s);
    }
    return rc;
}

int
axl_fstat(AxlStream *s, AxlFsEntry *entry)
{
    FileCtx *f;

    if (entry == NULL) {
        return AXL_ERR;
    }
    axl_memset(entry, 0, sizeof(*entry));
    if (s == NULL) {
        return AXL_ERR;
    }
    f = file_ctx(s);
    if (f == NULL) {
        return AXL_UNSUPPORTED;
    }
    return axl_fs_handle_stat(f->handle, entry);
}

int
axl_fset_mtime(AxlStream *s, uint64_t mtime_unix)
{
    FileCtx *f;

    if (s == NULL) {
        return AXL_ERR;
    }
    if (mtime_unix < AXL_FS_MTIME_MIN || mtime_unix > AXL_FS_MTIME_MAX) {
        return AXL_INVALID;
    }
    f = file_ctx(s);
    if (f == NULL) {
        return AXL_UNSUPPORTED;
    }
    if (!f->writable) {
        return AXL_DENIED;
    }
    /* Buffered bytes first: reaching the file afterwards, they would stamp
       the current time over the one set here. */
    if (axl_fflush(s) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_fs_handle_set_mtime(f->handle, mtime_unix);
}
