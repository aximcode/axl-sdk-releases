/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-file.c
    File log handler — writes log messages to a file on the filesystem.
    Buffers output and flushes periodically or on demand.
**/

#include <stddef.h>
#include <stdbool.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
#include "../fs/axl-file-gen.h"
#include "axl-log-line.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define FILE_BUF_SIZE   4096
#define FLUSH_THRESHOLD (FILE_BUF_SIZE * 3 / 4)

// ---------------------------------------------------------------------------
// Global State (single file handler)
// ---------------------------------------------------------------------------

static AxlFileHandle      mFileHandle  = NULL;
static char              *mFileBuf     = NULL;
static size_t             mFileBufPos  = 0;
static uint32_t           mFileGenKey  = 0;   /* write registry slot for the log path */

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

/* THE one place log records reach the file handle. The buffered drain and
   the oversized-record bypass both go through here so that the write
   registry bump (which lets an AxlFileView open on the log notice the
   append) cannot be wired into one and forgotten on the other -- they are
   two copies of the same three lines, and that shape has bitten this tree
   before. The write status is dropped for the reason flush_buffer
   documents below: there is no actor for a log-write failure. */
static void
log_file_write_raw(const void *data, size_t len)
{
    if (mFileHandle == NULL || data == NULL || len == 0) {
        return;
    }
    size_t write_size = len;
    axl_backend_file_write(mFileHandle, &write_size, data);
    axl_file_gen_bump_key(mFileGenKey);
}

/* Drain the in-memory buffer into the file HANDLE. This is the hot path
   (it runs every FLUSH_THRESHOLD bytes), so it deliberately stops there:
   pushing the firmware's own buffers out to the media on every threshold
   would put a synchronous media write in the middle of ordinary logging.
   flush_to_media() is the durability step. The write status is dropped
   because there is no actor for it -- reporting a log failure through the
   log is a loop, and the bytes are gone either way. */
static void
flush_buffer(void)
{
    if (mFileBuf == NULL || mFileBufPos == 0) {
        return;
    }

    log_file_write_raw(mFileBuf, mFileBufPos);
    mFileBufPos = 0;
}

/* Drain the buffer AND push the firmware's through to the volume. Writing
   to a file handle is not durability: a close cannot report a failure
   (EFI_FILE_PROTOCOL.Close is specified to return only EFI_SUCCESS), so
   without an explicit flush a log written right up to a crash or a power
   loss -- which is the situation logs exist for -- could be missing its
   last records entirely. Used by the two places that mean "the log must
   be on disk now": the public axl_log_flush and handler teardown. */
static void
flush_to_media(void)
{
    flush_buffer();
    if (mFileHandle != NULL) {
        axl_backend_file_flush(mFileHandle);
    }
}

static void
append_to_buffer(const char *data, size_t len)
{
    if (mFileBuf == NULL) {
        return;
    }

    if (mFileBufPos + len >= FILE_BUF_SIZE) {
        flush_buffer();
    }

    if (len >= FILE_BUF_SIZE) {
        log_file_write_raw(data, len);
        return;
    }

    axl_memcpy(mFileBuf + mFileBufPos, data, len);
    mFileBufPos += len;

    if (mFileBufPos >= FLUSH_THRESHOLD) {
        flush_buffer();
    }
}

static void
file_handler(int level, const char *domain, const char *message,
             const AxlRealtime *stamp, void *data)
{
    char line_buf[640];

    (void)data;

    size_t len = axl_log_format_line(line_buf, sizeof(line_buf), level,
                                     domain, message, stamp, "\n");
    if (len == 0) {
        return;
    }
    append_to_buffer(line_buf, len);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/* Tear down whatever axl_log_file_attach last installed. NULL-safe by
   construction — flush_buffer / axl_log_remove_handler / file_close all
   no-op when their state is unset, so calling this against an
   already-detached (or never-attached) state runs harmlessly. */
static void
detach_file_handler(void)
{
    flush_to_media();
    axl_log_remove_handler(file_handler);
    if (mFileHandle != NULL) {
        axl_backend_file_close(&mFileHandle);
        mFileHandle = NULL;
    }
}

int
axl_log_file_attach(const char *path)
{
    if (path == NULL) {
        return AXL_ERR;
    }

    // Close existing file handler if any
    if (mFileHandle != NULL) {
        detach_file_handler();
    }

    if (mFileBuf == NULL) {
        mFileBuf = axl_backend_alloc_zero(FILE_BUF_SIZE);
        if (mFileBuf == NULL) {
            return AXL_ERR;
        }
    }

    mFileBufPos = 0;
    mFileGenKey = axl_file_gen_key(path);

    unsigned short *wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    int rc = axl_backend_file_open(
        (const unsigned short *)wide_path,
        AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE,
        0,
        &mFileHandle);

    axl_free(wide_path);

    if (rc != AXL_OK) {
        mFileHandle = NULL;
        return AXL_ERR;
    }

    /* The open carries CREATE, so it is a namespace change when the log
       did not exist -- the identical reason axl_fopen bumps at open for a
       write mode. Without this a view watching a not-yet-existing log path
       learns nothing until the first drain. */
    axl_file_gen_bump_key(mFileGenKey);

    if (axl_log_add_handler(file_handler, NULL) != AXL_OK) {
        /* Handler table full — file is open, but messages won't reach it.
         * Caller should know so they can decide to abort or carry on. */
        return AXL_ERR;
    }
    return AXL_OK;
}

void
axl_log_flush(void)
{
    flush_to_media();
}

void
axl_log_file_detach(void)
{
    detach_file_handler();
}
