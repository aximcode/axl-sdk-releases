/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-regex.h
    Regular-expression search — a compiled-pattern matcher over the
    @ref axl-find.h byte-source seam.

    A *compiled* pattern (`AxlRegex`): parse and compile once, then
    search many times — the right shape for find-all loops and matching
    one pattern across many lines or buffers. The matcher reads through
    the same `AxlByteReader` the literal find engine uses, so the same
    `AxlRegex` searches a flat buffer, an AxlTextBuffer, or an
    AxlPieceTree; a successful search reports the same `AxlMatch`.

    The engine is a Thompson NFA / Pike VM, NOT a backtracker, so match
    time is O(pattern × input) for every pattern — there is no
    catastrophic ("ReDoS") blowup. That guarantee is why backreferences
    are deliberately unsupported: they are not regular and would force
    backtracking.

    Supported syntax: literals; `.`; greedy and lazy quantifiers
    `* + ? *? +? ??`; bounded repetition `{n}` / `{n,}` / `{n,m}` / `{,m}`
    (counts clamp to 1024; a `{` that is not a valid interval is a literal);
    anchors `^ $`; alternation `|`; grouping and capture `( )`; character
    classes `[...]` / `[^...]` with `a-z` ranges; the escapes
    `\d \w \s \D \W \S \n \t \r \f \v` and backslash-escaped metacharacters.
    Bounded repetition desugars to the base quantifiers, so a capture group
    repeated by an interval resolves to its last match. Named groups are not
    in this version. Matching is byte-oriented and leftmost (Perl / `grep -P`
    priority, not POSIX leftmost-longest).

    @code
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("[0-9]+", AXL_REGEX_DEFAULT);
    AxlMatch m;
    if (re && axl_regex_search_buf(re, text, len, 0, AXL_REGEX_MATCH_DEFAULT, &m))
        printf("first number at %zu, %zu bytes\n", m.start, m.length);
    @endcode
**/

#ifndef AXL_REGEX_H
#define AXL_REGEX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-find.h>   /* AxlByteReader, AxlMatch, AxlMemReader */

#ifdef __cplusplus
extern "C" {
#endif

/// A compiled regular expression. Opaque; create with axl_regex_new,
/// release with axl_regex_free.
typedef struct AxlRegex AxlRegex;

/// Compile-time options (affect how the pattern is interpreted).
typedef enum {
    AXL_REGEX_DEFAULT   = 0,
    AXL_REGEX_CASELESS  = 1u << 0,  ///< ASCII case-insensitive matching
    AXL_REGEX_MULTILINE = 1u << 1,  ///< `^`/`$` also match at `\n` boundaries
    AXL_REGEX_DOTALL    = 1u << 2,  ///< `.` also matches `\n`
} AxlRegexFlags;

/// Match-time options, passed per search.
typedef enum {
    AXL_REGEX_MATCH_DEFAULT  = 0,
    AXL_REGEX_MATCH_ANCHORED = 1u << 0,  ///< match must start exactly at @p from_offset
} AxlRegexMatchFlags;

/// Detail for a failed compile (optional out param of axl_regex_new_full).
typedef struct {
    size_t      offset;   ///< byte offset in the pattern where parsing failed
    const char *message;  ///< static, human-readable reason (never freed)
} AxlRegexError;

// ---------------------------------------------------------------------------
// Compile / free
// ---------------------------------------------------------------------------

/**
 * @brief Compile a pattern.
 *
 * @return a compiled regex (free with axl_regex_free), or NULL if the
 *     pattern is malformed (the reason is logged) or on allocation
 *     failure. Use axl_regex_new_full to recover the error detail.
 */
AxlRegex *
axl_regex_new(
    const char *pattern,  ///< the regular-expression text (UTF-8 bytes)
    uint32_t    flags     ///< AxlRegexFlags
);

/**
 * @brief Compile a pattern, reporting compile-error detail.
 *
 * Identical to axl_regex_new but, on a malformed pattern, fills @p err
 * (when non-NULL) with the byte offset and a static reason string.
 * @p err is left untouched on success and on allocation failure.
 *
 * @return a compiled regex, or NULL (see axl_regex_new).
 */
AxlRegex *
axl_regex_new_full(
    const char    *pattern,  ///< the regular-expression text
    uint32_t       flags,    ///< AxlRegexFlags
    AxlRegexError *err        ///< [out] error detail on failure, or NULL
);

/**
 * @brief Free a compiled regex. NULL-safe.
 */
void
axl_regex_free(
    AxlRegex *re  ///< compiled regex (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlRegex, axl_regex_free)
#endif

/**
 * @brief Number of capture groups in the pattern (excluding group 0).
 *
 * Group 0 is always the overall match; this counts the `( )` groups,
 * which are numbered 1..N left-to-right by opening parenthesis. Use it
 * to size the @p groups array for axl_regex_search_captures.
 *
 * @return the capture-group count (0 if @p re is NULL).
 */
size_t
axl_regex_capture_count(
    const AxlRegex *re  ///< compiled regex
);

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

/**
 * @brief Find the leftmost match in a byte source.
 *
 * Scans @p reader from @p from_offset and reports the leftmost match
 * (start >= @p from_offset). With AXL_REGEX_MATCH_ANCHORED the match
 * must begin exactly at @p from_offset. Iterate by re-searching from
 * `m.start + (m.length ? m.length : 1)`.
 *
 * The matcher needs a contiguous view of the scanned region: it uses
 * the reader's zero-copy `peek` when available, otherwise it reads the
 * region `[from_offset, length)` into a temporary buffer (so for a
 * non-contiguous out-of-core source the cost is O(region) memory per
 * call — fine for editor-sized regions; windowed matching is future
 * work).
 *
 * @return true and fills @p out on a match; false if no match (or any
 *     required argument is NULL).
 */
AXL_WARN_UNUSED bool
axl_regex_search(
    const AxlRegex      *re,           ///< compiled regex
    const AxlByteReader *reader,       ///< byte source to scan
    size_t               from_offset,  ///< where to start scanning
    uint32_t             match_flags,  ///< AxlRegexMatchFlags
    AxlMatch            *out           ///< [out] overall match on success
);

/**
 * @brief Find the leftmost match in a contiguous buffer.
 *
 * Convenience over axl_regex_search for the common contiguous case;
 * the bytes are borrowed and need not outlive the call.
 *
 * @return true and fills @p out on a match; false otherwise.
 */
AXL_WARN_UNUSED bool
axl_regex_search_buf(
    const AxlRegex *re,           ///< compiled regex
    const void     *data,         ///< bytes to scan
    size_t          len,          ///< number of bytes
    size_t          from_offset,  ///< where to start scanning
    uint32_t        match_flags,  ///< AxlRegexMatchFlags
    AxlMatch       *out           ///< [out] overall match on success
);

/**
 * @brief Find the leftmost match and report capture groups.
 *
 * Like axl_regex_search, but also fills @p groups with the captured
 * sub-matches: `groups[0]` is the overall match and `groups[k]` is the
 * k-th `( )` group. At most @p n_groups entries are written (size it
 * via axl_regex_capture_count + 1). A group that did not participate in
 * the match (e.g. an unmatched optional or alternation branch) is
 * reported with `start == AXL_REGEX_NO_MATCH` and `length == 0`.
 *
 * @return true and fills @p groups on a match; false otherwise.
 */
AXL_WARN_UNUSED bool
axl_regex_search_captures(
    const AxlRegex      *re,           ///< compiled regex
    const AxlByteReader *reader,       ///< byte source to scan
    size_t               from_offset,  ///< where to start scanning
    uint32_t             match_flags,  ///< AxlRegexMatchFlags
    AxlMatch            *groups,       ///< [out] group array (groups[0] = overall)
    size_t               n_groups      ///< capacity of @p groups
);

/// Sentinel `start` for a capture group that did not participate.
#define AXL_REGEX_NO_MATCH ((size_t)-1)

#ifdef __cplusplus
}
#endif

#endif /* AXL_REGEX_H */
