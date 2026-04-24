/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-io-internal.h
    Internal stream structure — not a public header.
**/

#ifndef AXL_IO_INTERNAL_H
#define AXL_IO_INTERNAL_H

#include <stddef.h>
#include <axl/axl-io.h>

struct AxlStream {
    void         *ctx;
    axl_ssize_t  (*read)(void *ctx, void *buf, size_t count);
    axl_ssize_t  (*write)(void *ctx, const void *buf, size_t count);
    axl_ssize_t  (*pread)(void *ctx, void *buf, size_t count, size_t offset);
    axl_ssize_t  (*pwrite)(void *ctx, const void *buf, size_t count, size_t offset);
    int          (*seek)(void *ctx, int64_t offset, int whence);
    int64_t      (*tell)(void *ctx);
    int          (*flush)(void *ctx);
    void         (*close)(void *ctx);
    bool         eof;
};

/* Allocate a stream with the given vtable. */
AxlStream *axl_stream_new(void);

/* Buffer backend — called from AxlIOBuf.c */
AxlStream *axl_bufopen_internal(void);

/* File backend — called from AxlIOFile.c */
AxlStream *axl_fopen_internal(const char *path, const char *mode);

/* File helpers for g_file_get/set_contents wrappers */
int axl_file_get_contents_internal(const char *path, void **buf, size_t *len);
int axl_file_set_contents_internal(const char *path, const void *buf, size_t len);

#endif /* AXL_IO_INTERNAL_H */
