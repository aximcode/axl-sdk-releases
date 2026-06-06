/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-text-buffer.h:
 *
 * A growable, editable byte buffer with an integral line index, tuned
 * for an interactive text editor: load a file once, then many small
 * inserts/deletes near a moving cursor, and — every keystroke and every
 * repaint — map between byte offsets and line numbers.
 *
 * Storage is a gap buffer (O(1) amortized insert/delete at the gap;
 * moving the gap to the next edit site is a single memmove). Because the
 * bytes are not contiguous, callers read ranges out via
 * axl_text_buffer_get rather than holding a pointer.
 *
 * The store is byte-oriented and encoding-agnostic: '\n' (0x0A) is the
 * only special byte (the line delimiter). Any UTF-8 / codepoint policy
 * is the caller's to apply on top.
 *
 * Line index: a sorted index of newline offsets, maintained
 * incrementally on every edit (never a full rescan). Offset->line and
 * line->bounds are O(log n) binary searches — the editor calls them on
 * every keystroke and once per visible line per frame.
 */

#ifndef AXL_TEXT_BUFFER_H
#define AXL_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-find.h>     /* AxlFindFlags, AxlMatch */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlTextBuffer AxlTextBuffer;

/**
 * @brief Create an empty text buffer.
 *
 * @p initial_capacity is a hint for the initial gap size (0 = a small
 * default); the buffer grows as needed.
 *
 * @return new buffer (length 0, line count 1), or NULL on OOM.
 *     Free with axl_text_buffer_free().
 */
AxlTextBuffer *
axl_text_buffer_new(
    size_t initial_capacity  ///< initial byte capacity hint (0 = default)
);

/**
 * @brief Free a text buffer and its storage. NULL-safe.
 */
void
axl_text_buffer_free(
    AxlTextBuffer *tb  ///< buffer (NULL-safe)
);

/**
 * @brief Replace the entire contents with @p data.
 *
 * Rebuilds the line index from scratch (this is the one O(n) operation —
 * it is a full load, not an incremental edit).
 *
 * @return AXL_OK on success, AXL_ERR on OOM or invalid args.
 */
AXL_WARN_UNUSED int
axl_text_buffer_set_bytes(
    AxlTextBuffer *tb,    ///< buffer
    const char    *data,  ///< replacement bytes (may be NULL iff @p len is 0)
    size_t         len    ///< number of bytes
);

/**
 * @brief Total byte length of the buffer's contents.
 */
size_t
axl_text_buffer_length(
    const AxlTextBuffer *tb  ///< buffer
);

/**
 * @brief Insert @p len bytes at byte @p offset.
 *
 * @p offset is clamped to the current length (so length is an append).
 * The line index is updated incrementally.
 *
 * @return AXL_OK on success, AXL_ERR on OOM or invalid args (NULL @p data
 *     with @p len > 0).
 */
AXL_WARN_UNUSED int
axl_text_buffer_insert(
    AxlTextBuffer *tb,      ///< buffer
    size_t         offset,  ///< byte offset to insert at (clamped to length)
    const char    *data,    ///< bytes to insert
    size_t         len      ///< number of bytes
);

/**
 * @brief Delete @p len bytes starting at byte @p offset.
 *
 * @p offset past the end deletes nothing; @p len is clamped so the range
 * never extends past the end. The line index is updated incrementally.
 *
 * @return AXL_OK on success (including the no-op cases), AXL_ERR on
 *     invalid args.
 */
AXL_WARN_UNUSED int
axl_text_buffer_delete(
    AxlTextBuffer *tb,      ///< buffer
    size_t         offset,  ///< byte offset to delete from
    size_t         len      ///< number of bytes to delete
);

/**
 * @brief Copy a byte range out into @p out.
 *
 * Copies up to min(@p len, @p cap) bytes of [offset, offset+len),
 * clamped to the buffer length. A gap buffer is not contiguous, so this
 * copy-out is how callers read content.
 *
 * @return number of bytes copied (may be < @p len at end-of-buffer or
 *     when @p cap is the limit).
 */
size_t
axl_text_buffer_get(
    const AxlTextBuffer *tb,      ///< buffer
    size_t               offset,  ///< byte offset to read from
    size_t               len,     ///< bytes requested
    char                *out,     ///< destination buffer
    size_t               cap      ///< capacity of @p out
);

/**
 * @brief Read a single byte.
 *
 * @return the byte (0–255) at @p offset, or -1 if @p offset is out of
 *     range.
 */
int
axl_text_buffer_byte_at(
    const AxlTextBuffer *tb,     ///< buffer
    size_t               offset  ///< byte offset
);

/**
 * @brief Number of lines.
 *
 * Defined as (newline count + 1): an empty buffer is 1 line, and a
 * trailing '\n' yields a real (empty) final line.
 */
size_t
axl_text_buffer_line_count(
    const AxlTextBuffer *tb  ///< buffer
);

/**
 * @brief The line number (0-based) containing byte @p offset.
 *
 * @p offset is clamped to the buffer length. A '\n' byte belongs to the
 * line it terminates. O(log n).
 *
 * @return 0-based line number.
 */
size_t
axl_text_buffer_line_of_offset(
    const AxlTextBuffer *tb,     ///< buffer
    size_t               offset  ///< byte offset
);

/**
 * @brief Byte range [start, end) of line @p line.
 *
 * @p end excludes the line's terminating '\n' (so it is the offset of
 * that '\n', or the buffer length for the last line). O(log n).
 *
 * @return AXL_OK if @p line is valid, AXL_ERR if @p line >= line count.
 */
AXL_WARN_UNUSED int
axl_text_buffer_line_bounds(
    const AxlTextBuffer *tb,     ///< buffer
    size_t               line,   ///< 0-based line number
    size_t              *start,  ///< [out] first byte of the line
    size_t              *end     ///< [out] one past the last byte, excluding '\n'
);

/**
 * @brief Copy a byte range out into a fresh NUL-terminated buffer.
 *
 * The allocating counterpart of axl_text_buffer_get (mirrors
 * axl_piece_tree_get_alloc): allocates (clamped length + 1) bytes, copies
 * [offset, offset+len) clamped to the buffer, and NUL-terminates. Raw
 * bytes (embedded NULs preserved; a C-string view stops at the first).
 *
 * @return newly allocated buffer (free with axl_free; an empty range
 *     yields a 1-byte ""), or NULL on OOM / NULL @p tb.
 */
char *
axl_text_buffer_get_alloc(
    const AxlTextBuffer *tb,      ///< buffer
    size_t               offset,  ///< byte offset
    size_t               len      ///< bytes requested (clamped to the buffer)
);

/**
 * @brief Round @p offset down to the start of the UTF-8 codepoint that
 *     contains it (mirror of axl_piece_tree_cp_align).
 *
 * Returns @p offset unchanged on a codepoint boundary (or at 0 / the
 * buffer end); otherwise steps back over continuation bytes. Use it to
 * snap an arbitrary byte offset to a valid caret position.
 */
size_t
axl_text_buffer_cp_align(
    const AxlTextBuffer *tb,     ///< buffer
    size_t               offset  ///< byte offset
);

/**
 * @brief Offset of the next UTF-8 codepoint boundary after @p offset
 *     (caret "move right"; clamped at the buffer end).
 */
size_t
axl_text_buffer_cp_next(
    const AxlTextBuffer *tb,     ///< buffer
    size_t               offset  ///< byte offset
);

/**
 * @brief Offset of the previous UTF-8 codepoint boundary before @p offset
 *     (caret "move left"; clamped at 0).
 */
size_t
axl_text_buffer_cp_prev(
    const AxlTextBuffer *tb,     ///< buffer
    size_t               offset  ///< byte offset
);

/**
 * @brief Search for a byte substring (mirror of axl_piece_tree_find).
 *
 * Finds @p needle starting the scan at @p from_offset. Forward (default)
 * returns the lowest match with start >= @p from_offset; @c AXL_FIND_BACKWARD
 * returns the highest match with start <= @p from_offset.
 * @c AXL_FIND_CASE_INSENSITIVE folds ASCII case; @c AXL_FIND_WHOLE_WORD
 * requires non-word bytes (anything but `[A-Za-z0-9_]`) on both sides.
 * Matches that straddle the internal gap are handled. Wrap-around is the
 * caller's job.
 *
 * Thin wrapper over axl_find_in_source (see @ref AxlByteReader).
 *
 * @return true and fills @p out on a match; false if not found (or
 *     @p needle_len is 0).
 */
AXL_WARN_UNUSED bool
axl_text_buffer_find(
    AxlTextBuffer *tb,           ///< buffer
    const char    *needle,       ///< bytes to find
    size_t         needle_len,   ///< length of @p needle
    size_t         from_offset,  ///< where to start scanning
    uint32_t       flags,        ///< AxlFindFlags
    AxlMatch      *out           ///< [out] match on success
);

/// Opaque compiled regex (see axl-regex.h).
typedef struct AxlRegex AxlRegex;

/**
 * @brief Search the buffer for a compiled regular expression.
 *
 * Regex analog of axl_text_buffer_find. Uses the buffer's zero-copy
 * peek when the searched span is contiguous (gap at the end — the
 * common case after appends); otherwise the matcher materializes the
 * buffer into a temporary, O(n) per call. For find-all over a large
 * buffer, materialize once and use axl_regex_search_buf.
 *
 * @return true and fills @p out on a match; false otherwise.
 */
AXL_WARN_UNUSED bool
axl_text_buffer_find_regex(
    AxlTextBuffer  *tb,           ///< buffer
    const AxlRegex *re,           ///< compiled regex
    size_t          from_offset,  ///< where to start scanning
    uint32_t        match_flags,  ///< AxlRegexMatchFlags
    AxlMatch       *out           ///< [out] match on success
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlTextBuffer, axl_text_buffer_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_TEXT_BUFFER_H */
