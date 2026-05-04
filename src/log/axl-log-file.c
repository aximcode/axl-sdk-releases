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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define FILE_BUF_SIZE   4096
#define FLUSH_THRESHOLD (FILE_BUF_SIZE * 3 / 4)

// ---------------------------------------------------------------------------
// Position tracker for manual line building
// ---------------------------------------------------------------------------

typedef struct {
    char   *buf;
    size_t  pos;
    size_t  size;
} BufCtx;

// ---------------------------------------------------------------------------
// Global State (single file handler)
// ---------------------------------------------------------------------------

static AxlFileHandle      mFileHandle  = NULL;
static char              *mFileBuf     = NULL;
static size_t             mFileBufPos  = 0;

// ---------------------------------------------------------------------------
// Level Tags
// ---------------------------------------------------------------------------

static const char *mFileLevelTag[] = {
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG",
    "TRACE"
};

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

static size_t
format_timestamp(char *buf, size_t buf_size)
{
    AxlTime time;

    if (axl_backend_get_time(&time) != AXL_OK) {
        axl_strlcpy(buf, "0000-00-00T00:00:00.000000 ", buf_size);
        return axl_strlen(buf);
    }

    unsigned y = time.year, mo = time.month, d = time.day;
    unsigned h = time.hour, mi = time.minute, s = time.second;
    unsigned us = time.nanosecond / 1000;

    int p = 0;
    buf[p++] = '0' + (y / 1000) % 10;
    buf[p++] = '0' + (y / 100) % 10;
    buf[p++] = '0' + (y / 10) % 10;
    buf[p++] = '0' + y % 10;
    buf[p++] = '-';
    buf[p++] = '0' + mo / 10;
    buf[p++] = '0' + mo % 10;
    buf[p++] = '-';
    buf[p++] = '0' + d / 10;
    buf[p++] = '0' + d % 10;
    buf[p++] = 'T';
    buf[p++] = '0' + h / 10;
    buf[p++] = '0' + h % 10;
    buf[p++] = ':';
    buf[p++] = '0' + mi / 10;
    buf[p++] = '0' + mi % 10;
    buf[p++] = ':';
    buf[p++] = '0' + s / 10;
    buf[p++] = '0' + s % 10;
    buf[p++] = '.';
    buf[p++] = '0' + (us / 100000) % 10;
    buf[p++] = '0' + (us / 10000) % 10;
    buf[p++] = '0' + (us / 1000) % 10;
    buf[p++] = '0' + (us / 100) % 10;
    buf[p++] = '0' + (us / 10) % 10;
    buf[p++] = '0' + us % 10;
    buf[p++] = ' ';
    buf[p] = '\0';

    return (size_t)p;
}

static void
flush_buffer(void)
{
    if (mFileHandle == NULL || mFileBuf == NULL || mFileBufPos == 0) {
        return;
    }

    size_t write_size = mFileBufPos;
    axl_backend_file_write(mFileHandle, &write_size, mFileBuf);
    mFileBufPos = 0;
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
        size_t write_size = len;
        if (mFileHandle != NULL) {
            axl_backend_file_write(mFileHandle, &write_size, data);
        }
        return;
    }

    axl_memcpy(mFileBuf + mFileBufPos, data, len);
    mFileBufPos += len;

    if (mFileBufPos >= FLUSH_THRESHOLD) {
        flush_buffer();
    }
}

static void
file_handler(int level, const char *domain, const char *message, void *data)
{
    char ts_buf[32];
    char line_buf[640];

    (void)data;

    format_timestamp(ts_buf, sizeof (ts_buf));

    BufCtx bc = { line_buf, 0, sizeof (line_buf) };

    /* Build the line manually: timestamp [LEVEL] domain: message\n */
    size_t ts_len = axl_strlen(ts_buf);
    axl_memcpy(line_buf + bc.pos, ts_buf, ts_len);
    bc.pos += ts_len;

    if (level >= 0 && level <= AXL_LOG_TRACE) {
        line_buf[bc.pos++] = '[';
        const char *tag = mFileLevelTag[level];
        size_t tag_len = axl_strlen(tag);
        axl_memcpy(line_buf + bc.pos, tag, tag_len);
        bc.pos += tag_len;
        line_buf[bc.pos++] = ']';
        line_buf[bc.pos++] = ' ';
    }

    if (domain != NULL) {
        size_t dlen = axl_strlen(domain);
        if (bc.pos + dlen + 2 < sizeof (line_buf) - 2) {
            axl_memcpy(line_buf + bc.pos, domain, dlen);
            bc.pos += dlen;
            line_buf[bc.pos++] = ':';
            line_buf[bc.pos++] = ' ';
        }
    }

    if (message != NULL) {
        size_t mlen = axl_strlen(message);
        size_t avail = sizeof (line_buf) - bc.pos - 2;
        if (mlen > avail) {
            mlen = avail;
        }
        axl_memcpy(line_buf + bc.pos, message, mlen);
        bc.pos += mlen;
    }

    line_buf[bc.pos++] = '\n';
    line_buf[bc.pos] = '\0';

    append_to_buffer(line_buf, bc.pos);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_log_file_attach(const char *path)
{
    if (path == NULL) {
        return AXL_ERR;
    }

    // Close existing file handler if any
    if (mFileHandle != NULL) {
        flush_buffer();
        axl_log_remove_handler(file_handler);
        axl_backend_file_close(&mFileHandle);
    }

    if (mFileBuf == NULL) {
        mFileBuf = axl_backend_alloc_zero(FILE_BUF_SIZE);
        if (mFileBuf == NULL) {
            return AXL_ERR;
        }
    }

    mFileBufPos = 0;

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
    flush_buffer();
}
