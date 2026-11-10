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

    Scope: the WRITER emits ustar — regular files and directories, names
    up to 256 bytes via the name/prefix split; a name that won't fit the
    ustar name(100)+prefix(155) split is rejected rather than silently
    truncated. The READER also takes the long names other tars write
    (GNU tar's default `gnu` format and bsdtar's / `--format=posix` pax):
    a GNU `L` record or a pax `x` `path` names the member that follows,
    and a pax `size` overrides the header's. A long name over
    `AXL_TAR_NAME_MAX - 1` bytes is never cut: the member is reported
    with AxlTarEntry.name_refused. Sparse files and GNU base-256 sizes
    are out of scope.
**/

#ifndef AXL_TAR_H
#define AXL_TAR_H

#include <stdbool.h>
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
#define AXL_TAR_TYPE_GLOBAL 'g'  ///< pax global header: reported, carries no member

/// One archive entry, as reported by axl_tar_reader_next.
typedef struct {
    char      name[AXL_TAR_NAME_MAX];  ///< full path: a GNU `L` / pax `path` long
                                       ///<   name when the archive carries one, else
                                       ///<   the ustar prefix joined with name; EMPTY
                                       ///<   when @ref name_refused is set
    uint64_t  size;                    ///< data byte count (a pax `size` wins over
                                       ///<   the header's; 0 for directories)
    uint32_t  mode;                    ///< permission bits (low 12)
    char      type;                    ///< AXL_TAR_TYPE_FILE / _DIR / _GLOBAL / raw
                                       ///<   typeflag (e.g. '1' hardlink, '2' symlink)
    bool      name_refused;            ///< the archive names this member with a name
                                       ///<   the reader will not report whole -- a long
                                       ///<   name over AXL_TAR_NAME_MAX - 1 bytes, or a
                                       ///<   name field with bytes after its NUL. The
                                       ///<   caller should skip the member; the next
                                       ///<   axl_tar_reader_next steps over its data
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
 * GNU `L` (long name) and pax `x` (extended header) records are consumed
 * here and applied to the member that follows them: its name becomes the
 * long name, and a pax `size` replaces the header's. Other pax keys are
 * ignored. A GNU `K` (long link name) is consumed and dropped: the link
 * member after it is reported with its raw typeflag. A pax `g` global
 * header is reported as an entry of type AXL_TAR_TYPE_GLOBAL (its data is
 * the records) so the caller can note it; it names no member, and a
 * pending `L`/`x` is not applied to it. A long name or name field the
 * reader cannot report whole sets AxlTarEntry.name_refused with an empty
 * name -- never a cut one. A malformed pax record set, or a pax `size`
 * over 2^63 - 1 (off_t's maximum, which also keeps size + padding from
 * wrapping), refuses the member the same way and drops every size the
 * set gave: its data is stepped over by the header's size. A WELL-FORMED
 * set refused only for its name (too long, or holding a NUL) keeps its
 * `size`: that is still the member's real length -- a pax size is how a
 * member over the 8 GiB a header can state is recorded -- so @p out's
 * size and the data stepped over are the pax size. The
 * ustar prefix field is read only from a POSIX header (its `ustar` magic
 * NUL-terminated); GNU headers keep other data there.
 *
 * A real end is told apart from a stream that stops early, as GNU tar
 * tells them apart: a zero block (one is enough), or the stream ending
 * exactly at a header boundary, is the end; the stream ending inside a
 * header, a member's data or its padding, or inside an `L` / `K` / `x`
 * record or before the member one introduced (a `K`, or an `x` of
 * ignored keys, included), is a truncated archive. (GNU tar
 * treats a stream ending part-way into a header as an end, losing that
 * member silently; libarchive reports it, and so does this reader.) Once
 * a call returns anything but AXL_OK, every later call returns the same
 * value; a truncation axl_tar_reader_read() met is reported here too.
 *
 * @return AXL_OK with @p out populated;
 *     AXL_ERR at the end of the archive, or for a NULL argument;
 *     AXL_INCOMPLETE when the stream ended early -- a truncated archive;
 *     AXL_INVALID for a header that fails its checksum (damage, or not a
 *     tar archive at all);
 *     AXL_IO_ERROR when the stream failed.
 *     @p out is untouched on every value but AXL_OK.
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
 * A stream that ends before the declared size -- a truncated archive --
 * is an error, never a clean end: the bytes it holds are returned, then
 * -1, so a caller cannot mistake a short member for a whole one.
 *
 * @return bytes read (0 once the whole entry has been read), or -1 on a
 *     stream error or when the stream ends with entry bytes still owed.
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
