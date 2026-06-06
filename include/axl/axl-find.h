/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-find.h:
 *
 * Byte-substring search over an abstract random-access byte source.
 *
 * The search engine (Boyer-Moore-Horspool, with case-insensitive and
 * whole-word variants) reads through an `AxlByteReader` — a tiny
 * function-table over whatever holds the bytes — so the same engine
 * drives a flat memory block, an AxlTextBuffer (gap buffer), and an
 * AxlPieceTree (out-of-core piece table). The reader's `read` pulls
 * windowed chunks (with overlap to catch matches spanning the source's
 * internal boundaries); an optional `peek` lets a contiguous source be
 * scanned in place with no copy.
 *
 * `axl_find_in_source` is the single engine; the per-type
 * `axl_*_find` wrappers (axl_text_buffer_find, axl_piece_tree_find)
 * build the appropriate reader and call it. A successful find reports
 * an `AxlMatch` (start + length); length is carried explicitly so the
 * result shape already fits variable-length matchers (a future regex /
 * fuzzy engine slots in behind the same reader without reshaping
 * callers).
 *
 * Byte-oriented; the only "word" notion is for AXL_FIND_WHOLE_WORD,
 * where a word byte is `[A-Za-z0-9_]`. Single-threaded (UEFI).
 */

#ifndef AXL_FIND_H
#define AXL_FIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Flags for axl_find_in_source and the axl_*_find wrappers.
typedef enum {
    AXL_FIND_DEFAULT          = 0,
    AXL_FIND_CASE_INSENSITIVE = 1u << 0,  ///< ASCII case fold
    AXL_FIND_BACKWARD         = 1u << 1,  ///< search toward offset 0
    AXL_FIND_WHOLE_WORD       = 1u << 2,  ///< match only at word boundaries
} AxlFindFlags;

/// A successful match: the bytes `[start, start + length)`. `length`
/// equals the needle length for a literal find, but is reported
/// explicitly so the result also fits variable-length matchers.
typedef struct {
    size_t start;   ///< byte offset of the match
    size_t length;  ///< match length in bytes
} AxlMatch;

/// Random-access byte source the search engine reads through. An
/// implementation fills the function pointers and `ctx`; the engine
/// never inspects `ctx` itself.
typedef struct AxlByteReader AxlByteReader;
struct AxlByteReader {
    /// Total number of bytes the reader can serve.
    size_t (*length)(const AxlByteReader *r);

    /// Copy up to @p len bytes starting at logical @p offset into
    /// @p buf, returning the number actually copied (fewer than @p len
    /// only when @p offset + len runs past the end). Always present.
    size_t (*read)(
        const AxlByteReader *r,
        size_t               offset,
        size_t               len,
        void                *buf
    );

    /// OPTIONAL zero-copy fast path: if `[offset, offset + len)` is
    /// stored contiguously, return a direct pointer to those bytes;
    /// otherwise return NULL. NULL is always a safe answer — the engine
    /// falls back to @ref read. May itself be NULL (no fast path).
    const char *(*peek)(
        const AxlByteReader *r,
        size_t               offset,
        size_t               len
    );

    void *ctx;  ///< implementation data (opaque to the engine)
};

/// Built-in `AxlByteReader` over a flat, contiguous memory block. The
/// reader supports the zero-copy `peek` path, so searching a memory
/// block performs no copying. Initialize with axl_mem_reader_init and
/// pass `&mem.reader` to axl_find_in_source. The bytes are borrowed —
/// @p data must outlive the search.
typedef struct {
    AxlByteReader reader;   ///< pass &reader to axl_find_in_source
    const char   *data;
    size_t        len;
} AxlMemReader;

/// Initialize a contiguous in-memory reader over @p data / @p len.
void
axl_mem_reader_init(
    AxlMemReader *mem,    ///< reader to initialize (caller-owned)
    const void   *data,   ///< borrowed bytes (must outlive the search)
    size_t        len     ///< number of bytes
);

/// Search @p reader for the @p needle_len bytes at @p needle, scanning
/// from @p from_offset. Forward (default) returns the lowest match with
/// start >= @p from_offset; @c AXL_FIND_BACKWARD returns the highest
/// match with start <= @p from_offset. @c AXL_FIND_CASE_INSENSITIVE
/// folds ASCII case; @c AXL_FIND_WHOLE_WORD requires non-word bytes on
/// both sides. Matches spanning the source's internal boundaries are
/// handled. Wrap-around is the caller's job.
///
/// @return true and fills @p out on a match; false if not found (or
///     @p needle_len is 0, or any required argument is NULL).
AXL_WARN_UNUSED bool
axl_find_in_source(
    const AxlByteReader *reader,       ///< byte source to scan
    const char          *needle,       ///< bytes to find
    size_t               needle_len,   ///< length of @p needle
    size_t               from_offset,  ///< where to start scanning
    uint32_t             flags,        ///< AxlFindFlags
    AxlMatch            *out           ///< [out] match on success
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FIND_H */
