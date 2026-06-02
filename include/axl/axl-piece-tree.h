/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-piece-tree.h:
 *
 * An out-of-core, editable text buffer for large files.
 *
 * The original file is never loaded whole — its bytes are read on demand
 * through AxlFileView — while edits accumulate in an append-only add
 * buffer. A balanced tree of pieces (spans into the original or the add
 * buffer, held in an AxlRBTree augmented with subtree byte and newline
 * sums) stitches the two into one logical document with O(log n)
 * offset<->line mapping and O(log n) edits. Editing a multi-gigabyte
 * file therefore costs memory proportional to the edits, not the file.
 *
 * This is the structure VS Code calls a "piece tree". For a
 * memory-resident editable store (small/medium buffers, form fields)
 * use AxlTextBuffer (a gap buffer) instead — AxlPieceTree is the
 * large-file tool.
 *
 * Byte-oriented and encoding-agnostic: '\n' (0x0A) is the only special
 * byte (line delimiter). Line semantics match AxlTextBuffer exactly (a
 * '\n' belongs to the line it terminates; line bounds exclude the
 * trailing '\n'; an empty document is one line), so the two are
 * interchangeable for a renderer. Content is read out via
 * axl_piece_tree_get (the document is virtual — pieces, not a contiguous
 * buffer). Read-only on the original; never writes back until
 * axl_piece_tree_save.
 *
 * Single-threaded (UEFI).
 */

#ifndef AXL_PIECE_TREE_H
#define AXL_PIECE_TREE_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-stream.h>   /* AxlEncoding */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlPieceTree AxlPieceTree;
typedef struct AxlPageCache AxlPageCache;   /* <axl/axl-page-cache.h> */

/// Flags for axl_piece_tree_find.
typedef enum {
    AXL_FIND_DEFAULT          = 0,
    AXL_FIND_CASE_INSENSITIVE = 1u << 0,  ///< ASCII case fold
    AXL_FIND_BACKWARD         = 1u << 1,  ///< search toward offset 0
    AXL_FIND_WHOLE_WORD       = 1u << 2,  ///< match only at word boundaries
} AxlFindFlags;

/// Line-ending (EOL) style. LF / CRLF / CR are concrete terminators;
/// AXL_EOL_MIXED is only ever *returned* by axl_piece_tree_detect_eol (a
/// document mixing more than one style) and is not a valid argument to
/// axl_piece_tree_set_eol.
typedef enum {
    AXL_EOL_LF = 0,   ///< "\n" (Unix)
    AXL_EOL_CRLF,     ///< "\r\n" (DOS / Windows)
    AXL_EOL_CR,       ///< "\r" (classic Mac)
    AXL_EOL_MIXED,    ///< (detect only) more than one style present
} AxlEol;

/// One edit for axl_piece_tree_apply_edits: delete @c del_len bytes at
/// @c offset, then insert @c ins_len bytes. Offsets are in the
/// document's ORIGINAL coordinates (before any edit in the batch).
typedef struct {
    size_t      offset;
    size_t      del_len;
    const char *ins;
    size_t      ins_len;
} AxlEdit;

/// Forward line iterator (see axl_piece_tree_line_iter_init). Fields are
/// private.
typedef struct {
    AxlPieceTree *pt;
    void         *node;        ///< current piece (AxlRBNode*)
    size_t        node_base;   ///< doc offset of @c node's first byte
    size_t        intra;       ///< bytes consumed within @c node
    size_t        nl_idx;      ///< next newline index in node's source array
    size_t        line_start;  ///< doc offset of the pending line's start
    size_t        doc_len;
    bool          done;
} AxlPieceLineIter;

/**
 * @brief Open a file for out-of-core editing.
 *
 * Opens an AxlFileView over @p path and performs a single streaming scan
 * to index newline positions (the one O(file) step — sequential,
 * constant-memory via the page cache — that every editor pays to show
 * line numbers on a large file). No file content is held resident.
 *
 * @p page_size / @p max_frames are the AxlFileView cache parameters
 * (0 / 0 select defaults).
 *
 * @return new piece tree, or NULL on open / OOM failure.
 *     Free with axl_piece_tree_free().
 */
AxlPieceTree *
axl_piece_tree_open(
    const char *path,        ///< file path (UTF-8)
    size_t      page_size,   ///< AxlFileView page size (0 = default)
    size_t      max_frames   ///< AxlFileView resident frames (0 = default)
);

/**
 * @brief Open a file out-of-core sharing a page cache across documents.
 *
 * Like axl_piece_tree_open, but the underlying AxlFileView borrows @p cache
 * (created with axl_page_cache_new_shared) instead of allocating its own
 * frames, so one bounded frame budget is shared across many open documents
 * (an editor with many files). The page size comes from the cache (which
 * must be a power of two). The cache is caller-owned and NOT freed when the
 * document is freed — free it after all documents sharing it are freed.
 *
 * @return new piece tree, or NULL on open / OOM failure.
 *     Free with axl_piece_tree_free().
 */
AxlPieceTree *
axl_piece_tree_open_cached(
    const char   *path,   ///< file path (UTF-8)
    AxlPageCache *cache   ///< shared cache to borrow (caller-owned)
);

/**
 * @brief Create an empty document (no backing file; add buffer only).
 *
 * @return new empty piece tree (length 0, line count 1), or NULL on OOM.
 */
AxlPieceTree *
axl_piece_tree_new(void);

/**
 * @brief Free a piece tree and all its resources. NULL-safe.
 */
void
axl_piece_tree_free(
    AxlPieceTree *pt  ///< piece tree (NULL-safe)
);

/**
 * @brief Total byte length of the logical document.
 */
size_t
axl_piece_tree_length(
    const AxlPieceTree *pt  ///< piece tree
);

/**
 * @brief Number of lines (newline count + 1; empty document = 1).
 */
size_t
axl_piece_tree_line_count(
    const AxlPieceTree *pt  ///< piece tree
);

/**
 * @brief Insert @p len bytes at byte @p offset.
 *
 * @p offset is clamped to the current length. O(log pieces).
 *
 * @return AXL_OK on success, AXL_ERR on OOM or invalid args (NULL
 *     @p data with @p len > 0).
 */
AXL_WARN_UNUSED int
axl_piece_tree_insert(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset,  ///< byte offset (clamped to length)
    const char   *data,    ///< bytes to insert
    size_t        len      ///< number of bytes
);

/**
 * @brief Delete @p len bytes starting at byte @p offset.
 *
 * @p offset past the end deletes nothing; @p len is clamped to the
 * document end. O(log pieces) per affected piece boundary.
 *
 * @return AXL_OK on success (including the no-op cases), AXL_ERR on OOM
 *     (a mid-piece split may allocate).
 */
AXL_WARN_UNUSED int
axl_piece_tree_delete(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset,  ///< byte offset
    size_t        len      ///< number of bytes
);

/**
 * @brief Copy a logical byte range out.
 *
 * Copies up to min(@p len, @p cap) bytes of [offset, offset+len),
 * clamped to the document length, spanning pieces and reading original
 * bytes through AxlFileView as needed.
 *
 * @return number of bytes copied.
 */
size_t
axl_piece_tree_get(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset,  ///< byte offset
    size_t        len,     ///< bytes requested
    char         *out,     ///< destination buffer
    size_t        cap      ///< capacity of @p out
);

/**
 * @brief Copy a logical byte range out into a fresh NUL-terminated buffer.
 *
 * Convenience over axl_piece_tree_get for the common copy-out-then-use
 * cases (selection to clipboard, measuring a line): allocates
 * (clamped length + 1) bytes, copies [offset, offset+len) clamped to the
 * document, and NUL-terminates. The copy holds the raw bytes — embedded
 * NULs are preserved but a C-string view of the result stops at the first.
 *
 * @return newly allocated buffer (free with axl_free; an empty range yields
 *     a 1-byte ""), or NULL on OOM / NULL @p pt.
 */
char *
axl_piece_tree_get_alloc(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset,  ///< byte offset
    size_t        len      ///< bytes requested (clamped to the document)
);

// ---------------------------------------------------------------------------
// UTF-8 codepoint navigation
// ---------------------------------------------------------------------------

/**
 * @brief Round @p offset down to the start of the UTF-8 codepoint that
 *     contains it.
 *
 * Returns @p offset unchanged if it already sits on a codepoint boundary
 * (or at 0 / the document end); otherwise steps back over UTF-8
 * continuation bytes. Use it to snap an arbitrary byte offset (e.g. from a
 * click-to-offset hit test) to a valid caret position. O(log n).
 */
size_t
axl_piece_tree_cp_align(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset   ///< byte offset
);

/**
 * @brief Offset of the next UTF-8 codepoint boundary after @p offset.
 *
 * Advances past the codepoint containing @p offset — caret "move right".
 * Clamped at the document end. O(log n).
 */
size_t
axl_piece_tree_cp_next(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset   ///< byte offset
);

/**
 * @brief Offset of the previous UTF-8 codepoint boundary before @p offset.
 *
 * The start of the codepoint immediately before @p offset — caret "move
 * left". Clamped at 0. O(log n).
 */
size_t
axl_piece_tree_cp_prev(
    AxlPieceTree *pt,      ///< piece tree
    size_t        offset   ///< byte offset
);

/**
 * @brief The 0-based line number containing byte @p offset.
 *
 * @p offset is clamped to the document length. O(log n).
 */
size_t
axl_piece_tree_line_of_offset(
    const AxlPieceTree *pt,     ///< piece tree
    size_t              offset  ///< byte offset
);

/**
 * @brief Byte range [start, end) of line @p line (end excludes '\n').
 *
 * A trailing CR (`\\r`) immediately before the line's terminating '\n' (a
 * CRLF pair) is also excluded, so the returned range is the line's content
 * without either terminator byte. O(log n).
 *
 * @return AXL_OK if @p line is valid, AXL_ERR if @p line >= line count.
 */
AXL_WARN_UNUSED int
axl_piece_tree_line_bounds(
    const AxlPieceTree *pt,     ///< piece tree
    size_t              line,   ///< 0-based line number
    size_t             *start,  ///< [out] first byte of the line
    size_t             *end     ///< [out] one past last byte, excluding '\n'
);

/**
 * @brief Crash-safely write the current document to a file.
 *
 * Streams every piece (reading original bytes through AxlFileView) to a
 * temporary sibling, flushes, then replaces @p path by rename — the
 * document is never fully materialized in memory. Same crash-safety as
 * axl_file_write_atomic (the target is never left half-written).
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
/**
 * @brief Search for a byte substring across the (virtual) document.
 *
 * Finds @p needle starting the scan at @p from_offset. Forward (default)
 * returns the lowest match offset >= @p from_offset; @c AXL_FIND_BACKWARD
 * returns the highest match offset <= @p from_offset. Matches spanning
 * piece boundaries are handled. @c AXL_FIND_CASE_INSENSITIVE folds ASCII
 * case; @c AXL_FIND_WHOLE_WORD requires non-word bytes (anything but
 * `[A-Za-z0-9_]`) on both sides. Wrap-around is the caller's job.
 *
 * @return true and sets @p *out_offset on a match; false if not found
 *     (or @p needle_len is 0).
 */
AXL_WARN_UNUSED bool
axl_piece_tree_find(
    AxlPieceTree *pt,           ///< piece tree
    const char   *needle,       ///< bytes to find
    size_t        needle_len,   ///< length of @p needle
    size_t        from_offset,  ///< where to start scanning
    uint32_t      flags,        ///< AxlFindFlags
    size_t       *out_offset    ///< [out] match offset on success
);

/**
 * @brief Whether the document differs from the last saved state.
 *
 * Save-point aware: cleared by save / open / new, set by edits, and
 * cleared again if undo/redo returns the document to the saved state.
 */
bool
axl_piece_tree_is_modified(
    const AxlPieceTree *pt  ///< piece tree
);

/**
 * @brief Apply a batch of edits as a single undo step.
 *
 * Edits use original (pre-batch) offsets and may be given in any order;
 * they are applied with correct offset adjustment so later edits land
 * where the caller intended (replace-all, multi-cursor, column edit).
 * The whole batch is one undo group.
 *
 * @return AXL_OK on success, AXL_ERR on OOM or invalid args (a partial
 *     batch may have applied — undo it to recover).
 */
AXL_WARN_UNUSED int
axl_piece_tree_apply_edits(
    AxlPieceTree  *pt,    ///< piece tree
    const AxlEdit *edits, ///< edits (original coordinates)
    size_t         n      ///< number of edits
);

/**
 * @brief Start a forward line iterator (one O(n) pass over all lines).
 *
 * Cheaper than repeated axl_piece_tree_line_bounds when visiting every
 * line (find-all, re-layout, export). Do not edit the document while
 * iterating.
 */
void
axl_piece_tree_line_iter_init(
    AxlPieceTree     *pt,   ///< piece tree
    AxlPieceLineIter *it    ///< [out] iterator
);

/**
 * @brief Start a line iterator at a given line (skip earlier lines in
 *     O(log n) instead of O(@p start_line)).
 *
 * Like axl_piece_tree_line_iter_init but the first axl_piece_tree_line_iter_next
 * returns line @p start_line — so a renderer can iterate a viewport deep in
 * a huge file without walking every preceding line. @p start_line == 0 is
 * identical to init; @p start_line >= the line count leaves the iterator
 * exhausted (next returns false).
 */
void
axl_piece_tree_line_iter_init_at(
    AxlPieceTree     *pt,         ///< piece tree
    AxlPieceLineIter *it,         ///< [out] iterator
    size_t            start_line  ///< 0-based line to start at
);

/**
 * @brief Advance the line iterator.
 *
 * Sets @p *start / @p *end to the next line's byte range (end excludes
 * the terminating '\n' and a preceding CR (`\\r`), matching
 * axl_piece_tree_line_bounds).
 *
 * @return true if a line was returned, false at end of document.
 */
bool
axl_piece_tree_line_iter_next(
    AxlPieceLineIter *it,     ///< iterator
    size_t           *start,  ///< [out] line start
    size_t           *end     ///< [out] line end (excludes '\n')
);

AXL_WARN_UNUSED int
axl_piece_tree_save(
    AxlPieceTree *pt,    ///< piece tree
    const char   *path   ///< destination path (UTF-8)
);

// ---------------------------------------------------------------------------
// Encoding-aware load / save
// ---------------------------------------------------------------------------

/**
 * @brief Open a text file, detecting and decoding its encoding.
 *
 * Sniffs the leading bytes with axl_detect_encoding. A plain UTF-8 file
 * with no BOM opens out-of-core exactly like axl_piece_tree_open (no
 * materialization, @p page_size / @p max_frames are the AxlFileView cache
 * parameters). Every other case — a UTF-8 BOM to strip, or UTF-16 LE/BE
 * (reported as the @c AXL_ENC_UCS2_LE / @c AXL_ENC_UCS2_BE wire codes) —
 * is read in full, transcoded to UTF-8 (surrogate-aware; BOM stripped),
 * and held in a memory-resident document (axl_piece_tree_new).
 *
 * Either way the document is UTF-8 internally and starts clean (no undo
 * history; axl_piece_tree_is_modified is false). The detected encoding
 * and BOM presence are reported so a later axl_piece_tree_save_encoded
 * can round-trip them.
 *
 * @return new piece tree, or NULL on open / read / transcode / OOM
 *     failure. Free with axl_piece_tree_free().
 */
AxlPieceTree *
axl_piece_tree_load_encoded(
    const char  *path,        ///< file path (UTF-8)
    size_t       page_size,   ///< AxlFileView page size, UTF-8 path (0 = default)
    size_t       max_frames,  ///< AxlFileView resident frames, UTF-8 path (0 = default)
    AxlEncoding *out_enc,      ///< [out, optional] detected encoding
    bool        *out_has_bom   ///< [out, optional] whether a BOM was present
);

/**
 * @brief Write the document to a file in a chosen encoding.
 *
 * Transcodes the (UTF-8) document to @p enc — one of @c AXL_ENC_UTF8,
 * @c AXL_ENC_UCS2_LE, or @c AXL_ENC_UCS2_BE (the UCS-2 codes are written
 * as surrogate-aware UTF-16) — optionally prepending the matching BOM,
 * then writes it crash-safely (axl_file_write_atomic). The save point is
 * advanced (axl_piece_tree_is_modified becomes false) on success.
 *
 * Plain UTF-8 with @p write_bom false is equivalent to (and delegates to)
 * the streaming axl_piece_tree_save — it never materializes the document.
 * Any other encoding materializes the whole document for the transcode,
 * which is acceptable for the rare "save as a different encoding" case.
 *
 * @return AXL_OK on success, AXL_ERR on an unsupported @p enc, transcode,
 *     write, or OOM failure.
 */
AXL_WARN_UNUSED int
axl_piece_tree_save_encoded(
    AxlPieceTree *pt,         ///< piece tree
    const char   *path,       ///< destination path (UTF-8)
    AxlEncoding   enc,        ///< target wire encoding
    bool          write_bom   ///< prepend the encoding's BOM
);

// ---------------------------------------------------------------------------
// Line endings (EOL)
// ---------------------------------------------------------------------------

/**
 * @brief Detect the document's line-ending style.
 *
 * Scans the whole document (O(n)) and classifies its line terminators:
 * each "\r\n" is CRLF, each lone "\n" is LF, each lone "\r" is CR. The
 * result is that single style if the document is uniform, AXL_EOL_MIXED if
 * more than one style is present, and AXL_EOL_LF if there are no line
 * terminators at all (the conventional default).
 *
 * @return the detected AxlEol.
 */
AxlEol
axl_piece_tree_detect_eol(
    AxlPieceTree *pt  ///< piece tree
);

/**
 * @brief Set the line ending that axl_piece_tree_save writes.
 *
 * By default (no call) save preserves the document's bytes verbatim. Once
 * set, every save (and axl_piece_tree_save_encoded) normalizes each line
 * terminator — "\r\n", a lone "\r", or a lone "\n" — to @p eol while
 * streaming, so line endings convert without materializing the document.
 *
 * Note this does not rewrite the in-memory document; it only governs the
 * bytes written on save. @p eol must be a concrete style — AXL_EOL_MIXED
 * (or any out-of-range value) is rejected.
 *
 * @return AXL_OK, or AXL_ERR for AXL_EOL_MIXED / an out-of-range @p eol.
 */
AXL_WARN_UNUSED int
axl_piece_tree_set_eol(
    AxlPieceTree *pt,   ///< piece tree
    AxlEol        eol   ///< target line ending (LF / CRLF / CR)
);

// ---------------------------------------------------------------------------
// Read-only mode
// ---------------------------------------------------------------------------

/**
 * @brief Make the document reject content mutations.
 *
 * While read-only, axl_piece_tree_insert / _delete / _apply_edits return
 * AXL_ERR without changing the document. Reads (axl_piece_tree_get, find,
 * line queries) and save are unaffected. Clear it to allow edits again.
 *
 * This governs the editor's own mutators only; it is independent of the
 * file's on-disk permissions.
 */
void
axl_piece_tree_set_read_only(
    AxlPieceTree *pt,        ///< piece tree
    bool          read_only  ///< true to reject mutations
);

/// @brief Whether the document is currently read-only.
bool
axl_piece_tree_is_read_only(
    const AxlPieceTree *pt  ///< piece tree
);

// ---------------------------------------------------------------------------
// Backing file
// ---------------------------------------------------------------------------

/**
 * @brief Whether the backing file changed on disk since it was opened.
 *
 * Compares the current size / modification time of the file this document
 * was opened from (axl_piece_tree_open, or load_encoded's out-of-core
 * path) against the values captured at open. An out-of-core document reads
 * original bytes lazily through that file, so an external change can
 * corrupt reads — call this (e.g. when the editor regains focus) to detect
 * it and offer a reload.
 *
 * Returns false for a document with no backing file (axl_piece_tree_new,
 * or a load that was transcoded fully into memory). Note mtime has
 * filesystem granularity (FAT is 2 s), so a same-size change within that
 * window can be missed.
 *
 * @return true if the file's size or mtime differs from open (or the file
 *     is now missing / inaccessible); false otherwise.
 */
bool
axl_piece_tree_backing_changed(
    AxlPieceTree *pt  ///< piece tree
);

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

/**
 * @brief Undo the most recent edit (or edit group).
 *
 * Reverses the last recorded insert/delete — or, if it was inside a
 * group (see axl_piece_tree_undo_group_begin), the whole group — and
 * moves it onto the redo stack. Reversal reuses the original / add-buffer
 * bytes (no copy) and is atomic: on OOM the document is left unchanged.
 *
 * Reports where the change landed so an editor can put the caret /
 * selection at the edit site: @p affected_offset receives the document
 * offset of the reverted edit and @p affected_len the byte length of the
 * content now present there — non-zero when bytes were re-inserted (select
 * that range), zero for a net deletion (collapse the caret there). For a
 * multi-edit group it is the last sub-edit applied. Both out-params are
 * optional (NULL to ignore) and are set to 0 on AXL_ERR.
 *
 * @return AXL_OK if something was undone, AXL_ERR if there is nothing to
 *     undo (or an allocation failed).
 */
AXL_WARN_UNUSED int
axl_piece_tree_undo(
    AxlPieceTree *pt,              ///< piece tree
    size_t       *affected_offset, ///< [out, optional] offset of the change
    size_t       *affected_len     ///< [out, optional] bytes now at that offset
);

/**
 * @brief Redo the most recently undone edit (or edit group).
 *
 * Reports the affected range exactly like axl_piece_tree_undo (see there):
 * @p affected_offset / @p affected_len locate the redone change for caret /
 * selection placement; both are optional.
 *
 * @return AXL_OK if something was redone, AXL_ERR if there is nothing to
 *     redo (or an allocation failed).
 */
AXL_WARN_UNUSED int
axl_piece_tree_redo(
    AxlPieceTree *pt,              ///< piece tree
    size_t       *affected_offset, ///< [out, optional] offset of the change
    size_t       *affected_len     ///< [out, optional] bytes now at that offset
);

/// @brief Whether an undo is available.
bool
axl_piece_tree_can_undo(
    const AxlPieceTree *pt  ///< piece tree
);

/// @brief Whether a redo is available.
bool
axl_piece_tree_can_redo(
    const AxlPieceTree *pt  ///< piece tree
);

/**
 * @brief Set how many edit records to retain.
 *
 * @c SIZE_MAX (the default) keeps unlimited history; @c 0 disables
 * undo (records are not kept); @c N keeps the most recent N records,
 * dropping the oldest. Note that unlimited history means unbounded
 * memory growth (the add buffer never shrinks and the log grows).
 */
void
axl_piece_tree_set_undo_limit(
    AxlPieceTree *pt,        ///< piece tree
    size_t        max_edits  ///< retained record count (SIZE_MAX = unlimited)
);

/**
 * @brief Begin an undo group; edits until the matching end undo together.
 *
 * Nestable — only the outermost begin/end pair forms the group. Use to
 * coalesce a run of keystrokes into a single undo step (the grouping
 * policy is the editor's; the mechanism is here).
 */
void
axl_piece_tree_undo_group_begin(
    AxlPieceTree *pt  ///< piece tree
);

/// @brief End the current undo group (see axl_piece_tree_undo_group_begin).
void
axl_piece_tree_undo_group_end(
    AxlPieceTree *pt  ///< piece tree
);

/**
 * @brief Break the undo run; subsequent edits start a new group.
 *
 * Sugar for the "accumulate-until-break" editing model (no explicit
 * begin/end bracketing): once any checkpoint has been set, consecutive
 * edits coalesce into one undo step until the next checkpoint. The editor
 * calls this at its chosen boundaries — a typing pause, a cursor jump, a
 * type↔delete switch, a word/line boundary, an N-keystroke cap — to get
 * VS Code-style "smart" grouping; the buffer supplies the mechanism, the
 * editor the policy (the buffer has no clock or cursor of its own).
 *
 * Equivalent to slicing one long group at this point. Edits made before
 * the first checkpoint (and with no active group) still undo
 * individually.
 */
void
axl_piece_tree_undo_checkpoint(
    AxlPieceTree *pt  ///< piece tree
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlPieceTree, axl_piece_tree_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_PIECE_TREE_H */
