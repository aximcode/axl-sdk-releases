/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hexdump.c
    Formatted hex+ASCII dump with configurable grouping.
    Supports direct console output and AxlLog integration.

    Migrated from AxlHexDump.c(EDK2-style) to GLib-style API.
**/

#include <axl/axl-hexdump.h>
#include <axl/axl-log.h>
#include <axl/axl-io.h>
#include <axl/axl-str.h>
#include <axl/axl-format.h>

AXL_LOG_DOMAIN("util");

#define DEFAULT_BPL    16
#define DEFAULT_GROUP  AXL_HEX_GROUP_WORD
#define LINE_BUF_SIZE  256

// ---------------------------------------------------------------------------
// BufCtx adapter for axl_vformat (same pattern as axl-log.c)
// ---------------------------------------------------------------------------

typedef struct {
    char   *buf;
    size_t  pos;
    size_t  size;
} BufCtx;

static void
buf_write(const char *data, size_t len, void *ctx)
{
    BufCtx *bc = (BufCtx *)ctx;
    size_t avail = bc->size - bc->pos - 1;
    if (len > avail) {
        len = avail;
    }
    axl_memcpy(bc->buf + bc->pos, data, len);
    bc->pos += len;
    bc->buf[bc->pos] = '\0';
}

static size_t
buf_sprintf(char *buf, size_t buf_size, const char *fmt, ...)
{
    BufCtx bc = { buf, 0, buf_size };
    va_list args;

    buf[0] = '\0';
    va_start(args, fmt);
    axl_vformat(buf_write, &bc, fmt, args);
    va_end(args);
    return bc.pos;
}

// ---------------------------------------------------------------------------
// Callback type for output -- either console or log
// ---------------------------------------------------------------------------

typedef void (*hex_output_fn)(void *ctx, const char *line);

typedef struct {
    int          level;
    const char  *domain;
    const char  *func;
    int          line;
} LogCtx;

static void
output_to_console(void *ctx, const char *line)
{
    (void)ctx;
    axl_print("%s\n", line);
}

static void
output_to_log(void *ctx, const char *line)
{
    LogCtx *lc = (LogCtx *)ctx;
    axl_log_full(lc->level, lc->domain, lc->func, lc->line, "%s", line);
}

// ---------------------------------------------------------------------------
// Shared formatting core
// ---------------------------------------------------------------------------

static void
format_hexdump(const char *name, const void *data, size_t size,
               size_t bytes_per_line, size_t group_size,
               hex_output_fn output, void *output_ctx)
{
    const unsigned char *bytes;
    size_t offset;
    size_t count;
    size_t i;
    char line_buf[LINE_BUF_SIZE];
    size_t pos;
    unsigned char ch;

    if (data == NULL || size == 0) {
        return;
    }

    if (bytes_per_line == 0) {
        bytes_per_line = DEFAULT_BPL;
    }
    if (bytes_per_line > 64) {
        bytes_per_line = 64;
    }
    if (group_size == 0) {
        group_size = DEFAULT_GROUP;
    }
    if (group_size > bytes_per_line) {
        group_size = bytes_per_line;
    }

    /* Truncate oversized dumps */
    if (size > AXL_HEXDUMP_MAX_SIZE) {
        buf_sprintf(line_buf, sizeof (line_buf),
                    "hex dump truncated: %u bytes(max %u)",
                    (unsigned)size, (unsigned)AXL_HEXDUMP_MAX_SIZE);
        output(output_ctx, line_buf);
        size = AXL_HEXDUMP_MAX_SIZE;
    }

    /* Header */
    if (name != NULL) {
        buf_sprintf(line_buf, sizeof (line_buf),
                    "=== %s(%p, %u bytes) ===",
                    name, data, (unsigned)size);
        output(output_ctx, line_buf);
    }

    bytes = (const unsigned char *)data;

    for (offset = 0; offset < size; offset += bytes_per_line) {
        count = size - offset;
        if (count > bytes_per_line) {
            count = bytes_per_line;
        }

        /* Line offset */
        pos = buf_sprintf(line_buf, sizeof (line_buf), "  %04x: ",
                          (unsigned)offset);

        /* Hex bytes with grouping */
        for (i = 0; i < bytes_per_line; i++) {
            if (i < count) {
                pos += buf_sprintf(line_buf + pos,
                                   sizeof (line_buf) - pos,
                                   "%02x", bytes[offset + i]);
            } else {
                if (pos + 2 < sizeof (line_buf)) {
                    line_buf[pos++] = ' ';
                    line_buf[pos++] = ' ';
                    line_buf[pos] = '\0';
                }
            }
            /* Space after each group boundary */
            if ((i + 1) % group_size == 0 && i + 1 < bytes_per_line) {
                if (pos < sizeof (line_buf) - 1) {
                    line_buf[pos++] = ' ';
                    line_buf[pos] = '\0';
                }
            }
        }

        /* ASCII column */
        if (pos + 3 < sizeof (line_buf)) {
            line_buf[pos++] = ' ';
            line_buf[pos++] = ' ';
            line_buf[pos++] = '|';
        }
        for (i = 0; i < count; i++) {
            if (pos >= sizeof (line_buf) - 1) {
                break;
            }
            ch = bytes[offset + i];
            line_buf[pos++] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.';
        }
        if (pos < sizeof (line_buf) - 1) {
            line_buf[pos++] = '|';
        }
        line_buf[pos] = '\0';

        output(output_ctx, line_buf);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_hexdump(const char *name, const void *data, size_t size,
            size_t bytes_per_line, size_t group_size)
{
    format_hexdump(name, data, size, bytes_per_line, group_size,
                   output_to_console, NULL);
}

void
axl_hexdump_to_log(int level, const char *domain, const char *func,
                   int line, const char *name, const void *data,
                   size_t size, size_t bytes_per_line, size_t group_size)
{
    LogCtx ctx;

    ctx.level  = level;
    ctx.domain = domain;
    ctx.func   = func;
    ctx.line   = line;

    format_hexdump(name, data, size, bytes_per_line, group_size,
                   output_to_log, &ctx);
}
