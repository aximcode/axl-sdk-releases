/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tar.h
    POSIX ustar archive reader and writer over AxlStream.

    A small, dependency-light tar codec: write a sequence of named
    byte blobs into a ustar archive (or read them back), streaming
    through any AxlStream — an in-memory buffer (`axl_bufopen`), a file
    (`axl_fopen`), or anything else that backs the stream API. Used by
    the `tar` tool and by mkfixture's HTTP write target (which builds a
    fixture tarball in memory and POSTs it).

    Scope: the ustar format — regular files and directories, names up to
    255 bytes via the name/prefix split. GNU/PAX long-name and sparse
    extensions are out of scope; a name that won't fit the ustar
    name(100)+prefix(155) split is rejected rather than silently
    truncated.
**/

#ifndef AXL_TAR_H
#define AXL_TAR_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/// ustar block size; every header and the data of each entry is padded
/// to a multiple of this.
#define AXL_TAR_BLOCK   512u

/// Max entry-name length: ustar name(100) + '/' + prefix(155), + NUL.
#define AXL_TAR_NAME_MAX  257u

/// Entry type, from the ustar typeflag byte.
#define AXL_TAR_TYPE_FILE  '0'   ///< regular file (also accepts '\0')
#define AXL_TAR_TYPE_DIR   '5'   ///< directory

/// One archive entry, as reported by axl_tar_reader_next.
typedef struct {
    char      name[AXL_TAR_NAME_MAX];  ///< full path (prefix joined with name)
    uint64_t  size;                    ///< data byte count (0 for directories)
    uint32_t  mode;                    ///< permission bits (low 12)
    char      type;                    ///< AXL_TAR_TYPE_FILE / _DIR / raw typeflag
} AxlTarEntry;

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

/// Opaque tar writer. Created by axl_tar_writer_new, destroyed by
/// axl_tar_writer_free. Does NOT own or close the underlying stream.
typedef struct AxlTarWriter AxlTarWriter;

/**
 * @brief Create a tar writer that emits to @p out.
 *
 * The writer borrows @p out — the caller keeps ownership and closes it
 * after `axl_tar_writer_finish` + `axl_tar_writer_free`.
 *
 * @return a writer, or NULL on allocation failure or if @p out is NULL.
 */
AxlTarWriter *
axl_tar_writer_new(
    AxlStream  *out   ///< destination stream (borrowed)
);

/**
 * @brief Append a regular-file entry with in-memory contents.
 *
 * Writes a ustar header for @p name (split across the name/prefix
 * fields when longer than 100 bytes) followed by @p len data bytes
 * zero-padded to the block size.
 *
 * @return AXL_OK on success, AXL_ERR if @p w / @p name is NULL, @p data
 *     is NULL with non-zero @p len, the name is too long for ustar, or
 *     the stream write fails.
 */
int
axl_tar_writer_add(
    AxlTarWriter  *w,      ///< writer
    const char    *name,   ///< entry path (e.g. "acpi/facp.dat")
    uint32_t       mode,    ///< permission bits (e.g. 0644)
    const void    *data,    ///< file contents (may be NULL iff @p len is 0)
    size_t         len      ///< content length in bytes
);

/**
 * @brief Append a directory entry (no data).
 *
 * Optional — extracting tools create parent directories implicitly, so
 * archives built for the HTTP sink omit these. Provided for the `tar`
 * tool's fidelity.
 *
 * @return AXL_OK on success, AXL_ERR on bad args / name too long / write
 *     failure.
 */
int
axl_tar_writer_add_dir(
    AxlTarWriter  *w,      ///< writer
    const char    *name,   ///< directory path (a trailing '/' is added if absent)
    uint32_t       mode     ///< permission bits (e.g. 0755)
);

/**
 * @brief Write the end-of-archive marker (two zero blocks).
 *
 * Call once after the last entry, before freeing. The stream is flushed
 * by the caller (or by closing it).
 *
 * @return AXL_OK on success, AXL_ERR on write failure.
 */
int
axl_tar_writer_finish(
    AxlTarWriter  *w
);

/// Free a tar writer. NULL-safe. Does not close the stream.
void
axl_tar_writer_free(
    AxlTarWriter  *w
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlTarWriter, axl_tar_writer_free)
#endif

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

/// Opaque tar reader. Created by axl_tar_reader_new, destroyed by
/// axl_tar_reader_free. Does NOT own or close the underlying stream.
typedef struct AxlTarReader AxlTarReader;

/**
 * @brief Create a tar reader over @p in.
 *
 * The reader borrows @p in (which must be positioned at the start of an
 * archive). @return a reader, or NULL on allocation failure / NULL @p in.
 */
AxlTarReader *
axl_tar_reader_new(
    AxlStream  *in   ///< source stream (borrowed)
);

/**
 * @brief Advance to the next archive entry.
 *
 * Skips any unread data (and its padding) from the previous entry, reads
 * and parses the next 512-byte header into @p out, and leaves the stream
 * positioned at the entry's data — read it with `axl_tar_reader_read`.
 *
 * @return AXL_OK with @p out populated, or AXL_ERR at the end-of-archive
 *     marker, on a malformed/short header, or on a stream error (@p out
 *     untouched). A clean end and a truncated archive both stop
 *     iteration the same way.
 */
int
axl_tar_reader_next(
    AxlTarReader  *r,
    AxlTarEntry   *out
);

/**
 * @brief Read up to @p len bytes of the current entry's data.
 *
 * Reads no further than the current entry's declared size; returns 0
 * once the entry is exhausted. Call repeatedly to stream large entries.
 * The entry size is taken from the header and trusted to the stream's
 * EOF behavior — a truncated archive that over-declares a size simply
 * short-reads here (and the next advance stops), it cannot over-read.
 *
 * @return bytes read (0 at end of the current entry), or -1 on a stream
 *     error.
 */
axl_ssize_t
axl_tar_reader_read(
    AxlTarReader  *r,
    void          *buf,
    size_t         len
);

/// Free a tar reader. NULL-safe. Does not close the stream.
void
axl_tar_reader_free(
    AxlTarReader  *r
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlTarReader, axl_tar_reader_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_TAR_H */
