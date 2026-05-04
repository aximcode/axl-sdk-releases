/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-str.h:
 *
 * String utilities. All char * functions operate on UTF-8 strings
 * (which are a superset of ASCII). Case-insensitive operations
 * (axl_strcasecmp, axl_strcasestr) fold ASCII letters only — they
 * do not handle full Unicode case mapping.
 *
 * UCS-2 (unsigned short *) functions are in the _w section at the
 * bottom of this file — these are for UEFI internal use. Consumer
 * code should use UTF-8 throughout.
 *
 * All allocated results are freed with axl_free().
 */

#ifndef AXL_STR_H
#define AXL_STR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// UTF-8 string/memory operations (freestanding — no <string.h> needed)
// ---------------------------------------------------------------------------

/**
 * @brief Get string length. NULL returns 0.
 */
size_t
axl_strlen(
    const char *s  ///< NUL-terminated string, or NULL
);

/**
 * @brief Compare two strings.
 *
 * @return <0, 0, or >0.
 */
int
axl_strcmp(
    const char *a,  ///< first string
    const char *b   ///< second string
);

/**
 * @brief Test if two strings are equal. NULL-safe.
 *
 * Shorthand for `axl_strcmp(a, b) == 0`.
 *
 * @return true if equal.
 */
static inline bool
axl_streql(
    const char *a,
    const char *b
    )
{
    if (a == b) { return true; }
    if (a == NULL || b == NULL) { return false; }
    return axl_strcmp(a, b) == 0;
}

/**
 * @brief Compare at most @n bytes of two strings.
 *
 * @return <0, 0, or >0.
 */
int
axl_strncmp(
    const char *a,  ///< first string
    const char *b,  ///< second string
    size_t      n   ///< max bytes to compare
);

/**
 * @brief Case-insensitive string comparison (ASCII case folding only).
 *
 * @return <0, 0, or >0.
 */
int
axl_strcasecmp(
    const char *a,  ///< first string
    const char *b   ///< second string
);

/**
 * @brief Bounded string length. Like POSIX strnlen().
 *
 * Returns the number of bytes in @a s before the first NUL, capped
 * at @a maxlen. NULL-safe (returns 0). Useful for length-bounded
 * scans of fixed-width buffers where running off the end would be a
 * read past the buffer.
 */
size_t
axl_strnlen(
    const char *s,        ///< NUL-terminated string, or NULL
    size_t      maxlen    ///< maximum bytes to scan
);

// ---------------------------------------------------------------------------
// ASCII character classification (locale-free, inline)
//
// These mirror the C `<ctype.h>` predicates but apply only to
// 7-bit ASCII — UTF-8 high bytes always test false, never undefined
// behavior. Suitable for tokenizers, hex/JSON parsers, header-name
// case-folding, etc. Take `int` to match the libc shape (so EOF /
// negative values pass safely).
// ---------------------------------------------------------------------------

/** True for ASCII '0'..'9'. */
static inline bool
axl_isdigit(int c)
{
    return (unsigned)c - '0' < 10u;
}

/** True for ASCII '0'..'9', 'a'..'f', 'A'..'F'. */
static inline bool
axl_isxdigit(int c)
{
    return ((unsigned)c - '0' < 10u)
        || ((unsigned)(c | 0x20) - 'a' < 6u);
}

/** True for ASCII 'a'..'z' or 'A'..'Z'. */
static inline bool
axl_isalpha(int c)
{
    return (unsigned)(c | 0x20) - 'a' < 26u;
}

/** True for ASCII alphanumeric. */
static inline bool
axl_isalnum(int c)
{
    return axl_isalpha(c) || axl_isdigit(c);
}

/** True for ASCII whitespace: space, tab, LF, CR, VT, FF. */
static inline bool
axl_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\v' || c == '\f';
}

/** Lowercase one ASCII char; non-letters returned unchanged. */
static inline int
axl_tolower(int c)
{
    return ((unsigned)c - 'A' < 26u) ? c + ('a' - 'A') : c;
}

/** Uppercase one ASCII char; non-letters returned unchanged. */
static inline int
axl_toupper(int c)
{
    return ((unsigned)c - 'a' < 26u) ? c - ('a' - 'A') : c;
}

/**
 * @brief Decode one ASCII hex digit to its integer value.
 *
 * Accepts '0'..'9', 'a'..'f', 'A'..'F'. Returns -1 for any other
 * input — callers can use the return type as both a value and a
 * validity check.
 */
static inline int
axl_hex_nibble(int c)
{
    if ((unsigned)c - '0' < 10u) return c - '0';
    unsigned lo = (unsigned)(c | 0x20) - 'a';
    if (lo < 6u) return (int)lo + 10;
    return -1;
}

/**
 * @brief Copy @n bytes from @a src to @a dst.
 *
 * Regions must not overlap. NULL-safe: returns @a dst if either is NULL.
 *
 * @return @a dst.
 */
void *
axl_memcpy(
    void       *dst,  ///< destination
    const void *src,  ///< source
    size_t      n     ///< byte count
);

/**
 * @brief Fill @a n bytes of @a dst with byte @a c.
 *
 * @return @a dst.
 */
void *
axl_memset(
    void  *dst,  ///< destination
    int    c,    ///< fill byte
    size_t n     ///< byte count
);

/**
 * @brief Copy @a n bytes, handling overlapping regions. Like memmove().
 *
 * @return @a dst.
 */
void *
axl_memmove(
    void       *dst,  ///< destination
    const void *src,  ///< source
    size_t      n     ///< byte count
);

/**
 * @brief Compare @a n bytes of memory. Like memcmp().
 *
 * @return <0, 0, or >0.
 */
int
axl_memcmp(
    const void *a,  ///< first buffer
    const void *b,  ///< second buffer
    size_t      n   ///< byte count
);

/**
 * @brief Find the first occurrence of @p needle in @p haystack.
 *
 * Like the GNU `memmem` extension. Linear byte-by-byte scan;
 * fine for the sizes we typically search (firmware tables,
 * rom images, signature blocks) but not designed for
 * megabyte-scale inputs.
 *
 * @return pointer to the first match inside @p haystack, or NULL
 *     if @p needle is not present, any input is NULL, or
 *     @p needle_len is 0 / larger than @p haystack_len.
 */
void *
axl_memmem(
    const void *haystack,        ///< buffer to search
    size_t      haystack_len,    ///< buffer length in bytes
    const void *needle,          ///< pattern to find
    size_t      needle_len       ///< pattern length in bytes
);

/**
 * @brief Format into a fixed buffer. Like snprintf().
 *
 * Uses AXL's own printf engine (standard C format specifiers).
 * Always NUL-terminates if size > 0.
 *
 * @return number of bytes that would have been written (excluding NUL),
 *     regardless of buffer size (allows truncation detection).
 */
int
axl_snprintf(
    char       *buf,   ///< output buffer
    size_t      size,  ///< buffer size
    const char *fmt,   ///< printf-style format string
    ...
) __attribute__((format(printf, 3, 4)));

/**
 * @brief Format a byte count into a human-readable string.
 *
 * Picks the largest IEC binary unit (KiB / MiB / GiB / TiB) that
 * yields a non-fractional integer (12884901888 → "12 GiB"). For
 * sizes that don't divide evenly, falls back to the largest unit
 * whose floor is non-zero and appends the raw byte count in
 * parentheses (e.g. 1500 → "1 KiB (1500 B)"). Sub-KiB values are
 * always rendered in bytes (512 → "512 B").
 *
 * Standard snprintf-style return semantics: the value returned is
 * the length the formatted string would have if @p buf_size were
 * unlimited. If that value is >= @p buf_size, the output was
 * truncated and the buffer holds a NUL-terminated prefix.
 *
 * @param value     byte count to format.
 * @param buf       output buffer; NUL-terminated on return when
 *                  @p buf_size > 0.
 * @param buf_size  buffer capacity in bytes.
 *
 * @return length the formatted string would occupy excluding NUL
 *     (== or < @p buf_size on success, > @p buf_size on truncation),
 *     or -1 if @p buf is NULL or @p buf_size is 0.
 */
int
axl_format_bytes(
    uint64_t  value,
    char     *buf,
    size_t    buf_size
);

// ---------------------------------------------------------------------------
// Safe string copy/concat (like strlcpy/strlcat)
// ---------------------------------------------------------------------------

/**
 * @brief Copy @a src into @a dst, guaranteeing NUL-termination.
 *
 * At most dst_size-1 characters are copied. Like BSD strlcpy / g_strlcpy.
 *
 * @return length of @a src (allows truncation detection:
 *     if return >= dst_size, output was truncated).
 */
size_t
axl_strlcpy(
    char       *dst,       ///< destination buffer
    const char *src,       ///< source string
    size_t      dst_size   ///< total size of @a dst (including NUL)
);

/**
 * @brief Append @a src to @a dst, guaranteeing NUL-termination.
 *
 * Like BSD strlcat / g_strlcat.
 *
 * @return attempted total length (dst_len + src_len).
 *     If return >= dst_size, output was truncated.
 */
size_t
axl_strlcat(
    char       *dst,       ///< destination buffer (must be NUL-terminated)
    const char *src,       ///< string to append
    size_t      dst_size   ///< total size of @a dst (including NUL)
);

// ---------------------------------------------------------------------------
// String duplication
// ---------------------------------------------------------------------------

/**
 * @brief Duplicate at most @a n bytes of @a s into a new NUL-terminated string.
 *
 * Caller frees with axl_free(). NULL-safe: returns NULL if @a s is NULL.
 *
 * @return new string, or NULL on failure.
 */
char *
axl_strndup(
    const char *s,  ///< source string
    size_t      n   ///< maximum bytes to copy
);

// ---------------------------------------------------------------------------
// String splitting, joining, trimming
// ---------------------------------------------------------------------------

/**
 * @brief Split a string by a delimiter character.
 *
 * Returns a NULL-terminated array of newly allocated strings.
 * Free the result with axl_strfreev().
 *
 * @return array of strings, or NULL on failure.
 */
char **
axl_strsplit(
    const char *str,        ///< string to split
    char        delimiter   ///< delimiter character
);

/**
 * @brief Free a NULL-terminated string array from axl_strsplit.
 */
void
axl_strfreev(
    char **arr  ///< array to free (NULL-safe)
);

/**
 * @brief Join a NULL-terminated string array with a separator.
 *
 * Caller frees the result with axl_free().
 *
 * @return new string, or NULL on failure.
 */
char *
axl_strjoin(
    const char  *separator, ///< separator between elements
    const char **arr        ///< NULL-terminated array of strings
);

/**
 * @brief Trim leading and trailing ASCII whitespace in place.
 *
 * Modifies the string by shifting content and NUL-terminating.
 * Returns the input pointer for convenience.
 *
 * @return @a str (same pointer).
 */
char *
axl_strstrip(
    char *str  ///< string to trim (modified in place)
);

/**
 * @brief Find first occurrence of character @a c in @a s.
 *
 * Like strchr(). Returns pointer to matching character, or NULL.
 *
 * @return pointer to first @a c in @a s, or NULL if not found.
 */
char *
axl_strchr(
    const char *s,  ///< string to search
    int         c   ///< character to find
);

/**
 * @brief Find first occurrence of @a needle in @a haystack.
 *
 * Like strstr(). Searches the entire NUL-terminated string.
 *
 * @return pointer to match, or NULL if not found.
 */
char *
axl_strstr(
    const char *haystack,  ///< string to search
    const char *needle     ///< substring to find
);

/**
 * @brief Copy @a src to @a dst, NUL-padding to @a n bytes.
 *
 * Like strncpy(). If @a src is shorter than @a n, the remainder
 * is filled with NUL bytes. Does NOT guarantee NUL-termination
 * if @a src is longer than @a n. Prefer axl_strlcpy() for safe
 * copying with guaranteed NUL-termination.
 *
 * @return @a dst.
 */
char *
axl_strncpy(
    char       *dst,  ///< destination buffer
    const char *src,  ///< source string
    size_t      n     ///< max bytes to write
);

// ---------------------------------------------------------------------------
// String searching (bounded)
// ---------------------------------------------------------------------------

/**
 * @brief Find first occurrence of @a needle in @a haystack.
 *
 * Searches at most @a haystack_len bytes. Pass -1 to search the
 * entire NUL-terminated string.
 *
 * @return pointer to match, or NULL if not found.
 */
char *
axl_strstr_len(
    const char *haystack,      ///< string to search
    long long   haystack_len,  ///< max bytes to search (-1 for all)
    const char *needle         ///< substring to find
);

/**
 * @brief Find last occurrence of @a needle in @a haystack.
 *
 * @return pointer to match, or NULL if not found.
 */
char *
axl_strrstr(
    const char *haystack,  ///< string to search
    const char *needle     ///< substring to find
);

/**
 * @brief Find last occurrence of @a needle in first @a haystack_len bytes.
 *
 * @return pointer to match, or NULL if not found.
 */
char *
axl_strrstr_len(
    const char *haystack,      ///< string to search
    long long   haystack_len,  ///< max bytes to search (-1 for all)
    const char *needle         ///< substring to find
);

/**
 * @brief Case-insensitive substring search (ASCII case folding only).
 *
 * Finds the first occurrence of @a needle in @a haystack, ignoring
 * ASCII letter case. NULL-safe: returns NULL if either argument is NULL.
 *
 * @return pointer to match, or NULL if not found.
 */
char *
axl_strcasestr(
    const char *haystack,  ///< string to search
    const char *needle     ///< substring to find (case-insensitive)
);

/**
 * @brief Length-bounded case-insensitive substring search.
 *
 * Like @ref axl_strcasestr but treats @a haystack as a byte slice
 * of @a haystack_len bytes — the haystack does NOT need to be
 * NUL-terminated. Pass `-1` for @a haystack_len to default to
 * NUL-terminated semantics (mirror of @ref axl_strstr_len).
 *
 * Useful for searching slices into a larger buffer (e.g. lines
 * delivered by `AxlLineReader` that point into a working buffer
 * without their own NUL terminator).
 *
 * @return pointer to match, or NULL if not found.
 */
char *
axl_strcasestr_len(
    const char *haystack,
    long long   haystack_len,
    const char *needle
);

// ---------------------------------------------------------------------------
// String testing
// ---------------------------------------------------------------------------

/**
 * @brief Glob-style pattern matching.
 *
 * Matches @a string against @a pattern using shell glob rules:
 * `*` matches zero or more characters, `?` matches exactly one,
 * `[abc]` matches a character class, `[a-z]` matches a range.
 * Matching is case-sensitive. NULL-safe: returns false if either
 * argument is NULL.
 *
 * @return true if @a string matches @a pattern.
 */
bool
axl_fnmatch(
    const char *pattern,  ///< glob pattern
    const char *string    ///< string to match against
);

/**
 * @brief Test if @a str starts with @a prefix.
 *
 * @return true if @a str begins with @a prefix.
 */
bool
axl_str_has_prefix(
    const char *str,     ///< string to test
    const char *prefix   ///< prefix to check for
);

/**
 * @brief Test if @a str ends with @a suffix.
 *
 * @return true if @a str ends with @a suffix.
 */
bool
axl_str_has_suffix(
    const char *str,     ///< string to test
    const char *suffix   ///< suffix to check for
);

/**
 * @brief Test if string is pure ASCII (all bytes 0x00-0x7F).
 *
 * @return true if all bytes are ASCII.
 */
bool
axl_str_is_ascii(
    const char *str  ///< string to test
);

/**
 * @brief NULL-safe string comparison.
 *
 * Two NULLs are equal. NULL sorts before non-NULL.
 *
 * @return negative, zero, or positive (like strcmp).
 */
int
axl_strcmp0(
    const char *str1,  ///< first string (may be NULL)
    const char *str2   ///< second string (may be NULL)
);

/**
 * @brief Byte-by-byte string equality test. (GLib: g_str_equal)
 *
 * Parameters are void* so this can be used directly as a hash table
 * equality function. Both strings must be non-NULL.
 *
 * @return true if strings are equal.
 */
bool
axl_str_equal(
    const void *v1,  ///< first string (cast to const char *)
    const void *v2   ///< second string (cast to const char *)
);

/**
 * @brief FNV-1a hash of a NUL-terminated string. (GLib: g_str_hash)
 *
 * Void-pointer signature matches AxlHashFunc so it can be handed
 * directly to axl_hash_table_new.
 *
 * @return hash value.
 */
size_t
axl_str_hash(
    const void *key  ///< NUL-terminated string (cast to const char *)
);

/**
 * @brief Case-insensitive comparison, length-bounded (ASCII only).
 *
 * Compares at most @a n bytes. ASCII letters only (A-Z, a-z).
 *
 * @return negative, zero, or positive (like strncmp).
 */
int
axl_strncasecmp(
    const char *s1, ///< first string
    const char *s2, ///< second string
    size_t      n   ///< max bytes to compare
);

/**
 * @brief Check if a NULL-terminated string array contains @a str.
 *
 * @return true if @a str is found in @a strv.
 */
bool
axl_strv_contains(
    const char *const *strv,  ///< NULL-terminated string array
    const char        *str    ///< string to search for
);

/**
 * @brief Check if two NULL-terminated string arrays are identical.
 *
 * Both arrays must be non-NULL. Compares element-by-element.
 *
 * @return true if arrays have the same elements in the same order.
 */
bool
axl_strv_equal(
    const char *const *strv1,  ///< first array
    const char *const *strv2   ///< second array
);

// ---------------------------------------------------------------------------
// UTF-8 <-> UCS-2 conversion
// ---------------------------------------------------------------------------

/**
 * @brief Convert a UTF-8 string to UCS-2.
 *
 * Handles BMP characters (U+0000..U+FFFF). Caller frees with axl_free().
 * The returned type (unsigned short *) matches UEFI's CHAR16.
 *
 * @return newly allocated UCS-2 string, or NULL if @a s is NULL
 *     or allocation fails.
 */
unsigned short *
axl_utf8_to_ucs2(
    const char *s  ///< UTF-8 string, or NULL
);

/**
 * @brief Widen an ASCII/UTF-8 string to UCS-2 in a caller-provided buffer.
 *
 * No allocation. Copies each byte as a 16-bit character. Suitable for
 * stack buffers in performance-sensitive code (logging, console output).
 *
 * @return number of characters written (excluding NUL terminator).
 */
size_t
axl_utf8_to_ucs2_buf(
    const char     *src,       ///< UTF-8 source string
    unsigned short *dst,       ///< destination UCS-2 buffer
    size_t          dst_count  ///< capacity of @a dst in characters (including NUL)
);

/**
 * @brief Convert a UCS-2 string to UTF-8.
 *
 * Caller frees with axl_free().
 *
 * @return newly allocated UTF-8 string, or NULL if @a s is NULL
 *     or allocation fails.
 */
char *
axl_ucs2_to_utf8(
    const unsigned short *s  ///< UCS-2 (unsigned short *) string, or NULL
);

/**
 * @brief Narrow a UCS-2 string to UTF-8 in a caller-provided buffer.
 *
 * No allocation. Encodes BMP characters with full UTF-8 multi-byte
 * sequences. Truncates cleanly at @a dst_size with a NUL terminator
 * (never writes a partial multi-byte sequence). Suitable for
 * stack buffers in performance-sensitive code (variable names,
 * console paths) — companion to axl_utf8_to_ucs2_buf.
 *
 * @return number of bytes written to @a dst (excluding NUL).
 */
size_t
axl_ucs2_to_utf8_buf(
    const unsigned short *src,       ///< UCS-2 source string
    char                 *dst,       ///< destination UTF-8 buffer
    size_t                dst_size   ///< capacity of @a dst in bytes (including NUL)
);

// ---------------------------------------------------------------------------
// Base64 (RFC 4648)
// ---------------------------------------------------------------------------

/**
 * @brief Base64-encode binary data.
 *
 * Caller frees with axl_free().
 *
 * @return NUL-terminated base64 string, or NULL on failure.
 */
char *
axl_base64_encode(
    const void *data,  ///< input bytes
    size_t      len    ///< input length
);

/**
 * @brief Decode a base64 string.
 *
 * @return AXL_OK on success, AXL_ERR on invalid input.
 */
int
axl_base64_decode(
    const char *b64,      ///< base64 string
    void      **out,      ///< (out): pointer to decoded data (caller frees with axl_free)
    size_t     *out_len   ///< (out): decoded data length
);

// ---------------------------------------------------------------------------
// Number parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parse an unsigned 64-bit integer with overflow detection.
 *
 * Skips leading whitespace, then accepts an optional "0x"/"0X" hex
 * prefix. The @p base parameter selects the radix (2-36, or 0 to
 * auto-detect: "0x"/"0X" prefix → base 16, otherwise base 10 — this
 * does NOT decode leading "0" as octal, unlike C strtol). A leading
 * '+' is accepted; a leading '-' is rejected for unsigned variants.
 *
 * Returns -1 on syntax error (no digits, invalid digit for base, sign
 * for unsigned), or out-of-range for the target type. On success,
 * @p out receives the value and @p endptr (if non-NULL) receives a
 * pointer just past the last consumed character. On error, @p out
 * is unchanged and *@p endptr (if non-NULL) is set to @p nptr.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_str_to_u64(
    const char  *nptr,    ///< number string
    int          base,    ///< 0 (auto), or 2-36
    uint64_t    *out,     ///< [out] parsed value
    const char **endptr   ///< [out, optional] past last consumed char
);

/// Like axl_str_to_u64 but the value must fit in uint32_t.
int
axl_str_to_u32(
    const char  *nptr,
    int          base,
    uint32_t    *out,
    const char **endptr
);

/// Like axl_str_to_u64 but the value must fit in uint16_t.
int
axl_str_to_u16(
    const char  *nptr,
    int          base,
    uint16_t    *out,
    const char **endptr
);

/// Like axl_str_to_u64 but the value must fit in uint8_t.
int
axl_str_to_u8(
    const char  *nptr,
    int          base,
    uint8_t     *out,
    const char **endptr
);

/**
 * @brief Parse a signed 64-bit integer with overflow detection.
 *
 * Same shape and rules as axl_str_to_u64, plus an optional leading
 * '-' for negatives. Returns -1 on syntax error or out-of-range.
 */
int
axl_str_to_s64(
    const char  *nptr,
    int          base,
    int64_t     *out,
    const char **endptr
);

/// Like axl_str_to_s64 but the value must fit in int32_t.
int
axl_str_to_s32(
    const char  *nptr,
    int          base,
    int32_t     *out,
    const char **endptr
);

/// Like axl_str_to_s64 but the value must fit in int16_t.
int
axl_str_to_s16(
    const char  *nptr,
    int          base,
    int16_t     *out,
    const char **endptr
);

/// Like axl_str_to_s64 but the value must fit in int8_t.
int
axl_str_to_s8(
    const char  *nptr,
    int          base,
    int8_t      *out,
    const char **endptr
);

/**
 * @brief Parse an unsigned 64-bit integer (legacy, lossy).
 *
 * @deprecated Prefer axl_str_to_u64. This wrapper preserves the
 *     previous "best effort, return 0 on any failure" semantics for
 *     existing callers — overflow wraps silently, "abc" returns 0
 *     indistinguishable from "0", and partial parses ("123abc"
 *     returns 123) are accepted.
 *
 * Handles "0x" prefix for hex. Returns 0 on NULL or invalid input.
 */
uint64_t
axl_strtou64(
    const char *s  ///< number string (decimal or "0x" hex)
);

/**
 * @brief Parse a hex/decimal value with an optional `+offset` suffix.
 *
 * Common pattern in CLI tools that take an address with an offset
 * (e.g. `do crb tag+offset reg`, `do rb physAddr+offset count`).
 * Accepts:
 *
 *   - "0x100"           → base = 0x100, offset = 0
 *   - "256"             → base = 256, offset = 0
 *   - "0x100+0x10"      → base = 0x100, offset = 0x10
 *   - "256+16"          → base = 256, offset = 16
 *   - "0x1000+0xFF"     → 0x1000 + 0xFF = 0x10FF
 *
 * Strict: any unconsumed characters after the optional offset cause
 * a parse error. Overflow on either component or on the sum returns
 * -1. Leading whitespace is allowed (matches `axl_str_to_u64`) but
 * not whitespace around the `+`. NULL input returns -1.
 *
 * @return AXL_OK on success (with @a out populated), AXL_ERR on parse error.
 */
int
axl_strtou64_with_offset(
    const char *s,    ///< number string with optional "+offset" suffix
    uint64_t   *out   ///< [out] parsed value (base + offset)
);

// ---------------------------------------------------------------------------
// AxlStrReader — cursor-based string parser
//
// Symmetric counterpart to AxlString (the builder). A reader BORROWS a
// `const char *` (no allocation, no ownership) and tracks a cursor with
// a sticky-error flag. Operations short-circuit when `ok` is false, so
// chains compose naturally without per-call error checking:
//
//     AxlStrReader r;
//     uint64_t v;
//     axl_str_reader_init(&r, "N[03A8]");
//     axl_str_reader_consume_char(&r, 'N');
//     axl_str_reader_consume_char(&r, '[');
//     axl_str_reader_take_u64(&r, 16, &v);
//     axl_str_reader_consume_char(&r, ']');
//     if (!r.ok || !axl_str_reader_eof(&r)) { ...parse failed... }
//
// For one-shot fixed-pattern parses, axl_sscanf below is a convenience
// wrapper built on top of this same primitive.
// ---------------------------------------------------------------------------

/// Cursor parser state. Initialize via axl_str_reader_init / _init_n.
/// Fields are exposed for convenience inspection (`r.ok`, `r.p`); don't
/// mutate them directly outside the helpers.
typedef struct {
    const char  *p;     ///< Current position (may equal end at EOF)
    const char  *end;   ///< One-past-last byte (sentinel)
    bool         ok;    ///< Sticky-error: false ⇒ a prior op failed,
                        ///< all subsequent ops short-circuit no-op
} AxlStrReader;

/**
 * @brief Initialize a reader from a NUL-terminated string.
 *
 * NULL @a s yields a reader at EOF with @a ok = true (consumers see
 * "nothing to parse, no error" — typically distinguished by checking
 * eof() AND remaining() before relying on a successful parse).
 */
void
axl_str_reader_init(
    AxlStrReader  *r,
    const char    *s
);

/**
 * @brief Initialize a reader from a length-bounded buffer.
 *
 * Use when the input is not NUL-terminated (slice into a larger buffer,
 * embedded NULs allowed). NULL @a s with @a n > 0 is a programming error
 * and yields an at-EOF reader with @a ok = false.
 */
void
axl_str_reader_init_n(
    AxlStrReader  *r,
    const char    *s,
    size_t         n
);

/// True iff the cursor is at end-of-input. NULL @a r returns true.
bool
axl_str_reader_eof(
    const AxlStrReader *r
);

/// Bytes between the cursor and end. 0 at EOF or NULL @a r.
size_t
axl_str_reader_remaining(
    const AxlStrReader *r
);

/// Peek the next byte without advancing. Returns 0 at EOF or when the
/// reader is in error state — distinguish via eof() if a 0 byte is
/// legitimate input.
char
axl_str_reader_peek(
    const AxlStrReader *r
);

/**
 * @brief Skip whitespace (' ', '\t', '\r', '\n', '\f', '\v').
 *
 * Always succeeds (skipping zero bytes is fine). Doesn't touch ok.
 * Use this between fields where the input grammar allows whitespace.
 */
bool
axl_str_reader_skip_ws(
    AxlStrReader *r
);

/**
 * @brief Consume a literal character.
 *
 * On match, advances past it and returns true. On mismatch (or EOF, or
 * prior error), sets @a ok = false and returns false; the cursor is not
 * advanced. Idempotent in the sense that ok-false reader stays ok-false.
 */
bool
axl_str_reader_consume_char(
    AxlStrReader  *r,
    char           c
);

/**
 * @brief Consume an exact literal string.
 *
 * Sets @a ok = false on mismatch (no partial consume). NULL or empty
 * @a literal is a no-op that returns true.
 */
bool
axl_str_reader_consume_str(
    AxlStrReader  *r,
    const char    *literal
);

/**
 * @brief Take bytes up to (but not including) @a delim, then consume @a delim.
 *
 * On success, @a *out points into the input (no allocation; valid for
 * the lifetime of the source string) and @a *out_len is the slice
 * length, which may be 0 (e.g. the input begins with @a delim).
 * If @a delim is not found before EOF, sets ok = false.
 */
bool
axl_str_reader_take_until(
    AxlStrReader   *r,
    char            delim,
    const char    **out,      ///< [out, optional] start of the slice
    size_t         *out_len   ///< [out, optional] slice length
);

/**
 * @brief Take bytes while @a pred returns true.
 *
 * Always succeeds; a zero-length take is not an error (returns true with
 * @a *out_len = 0). Use the result to distinguish "matched nothing" from
 * "matched something" if your grammar requires at least one char.
 */
bool
axl_str_reader_take_while(
    AxlStrReader   *r,
    bool          (*pred)(char),
    const char    **out,      ///< [out, optional] start of the slice
    size_t         *out_len   ///< [out, optional] slice length
);

/**
 * @brief Parse a u64 literal at the cursor.
 *
 * Behavior matches `axl_str_to_u64`:
 *   - @a base = 0: auto-detect "0x"/"0X" hex prefix, else decimal.
 *   - @a base = 16: accepts an optional "0x"/"0X" prefix and otherwise
 *     reads hex digits.
 *   - @a base in [2..36] (other than 16): reads digits only, no prefix
 *     handling.
 *
 * Stops at the first non-digit and advances the cursor past the
 * consumed digits (and prefix, if any). Sets @a ok = false and leaves
 * the cursor unchanged if no digits are present or the value
 * overflows u64.
 */
bool
axl_str_reader_take_u64(
    AxlStrReader  *r,
    int            base,    ///< 0 (auto-detect), 16 (with optional 0x), or 2..36
    uint64_t      *out      ///< [out] parsed value
);

/**
 * @brief Take a C identifier: `[A-Za-z_][A-Za-z0-9_]*`.
 *
 * @a *out points into the input; @a *out_len is the identifier length.
 * Sets ok = false when the leading char isn't a valid identifier start
 * (a digit, punctuation, EOF).
 */
bool
axl_str_reader_take_ident(
    AxlStrReader   *r,
    const char    **out,     ///< [out, optional]
    size_t         *out_len  ///< [out, optional]
);

// ---------------------------------------------------------------------------
// axl_sscanf — printf's symmetric partner, built on AxlStrReader.
//
// Subset of the C99 sscanf format-string grammar with the conversions
// AXL consumers actually use. Returns the number of successful
// conversions assigned (may be 0), or -1 on a malformed format string.
//
// Whitespace in the format matches any run of input whitespace
// (including none). A literal char in the format must match the input
// exactly; mismatch terminates the scan and returns the count so far.
//
// Conversion specifiers:
//
//   %%          literal '%'
//   %c          one char (no width modifier; %Nc takes the next N chars)
//   %d, %i      signed decimal / "auto" (with 0x detection for %i)
//   %u          unsigned decimal
//   %o, %x, %X  unsigned octal / hex (case-insensitive on the hex)
//   %s          run of non-whitespace; REQUIRES a width specifier (%Ns)
//               so the destination buffer can be bounded — the SDK does
//               not allow unbounded %s.
//   %[set]      run of chars matching the set (^set negates)
//   %n          assigns the number of bytes consumed so far (no input read)
//
// Length modifiers: hh, h, l, ll, z (for size_t), j (for intmax_t).
// For unsigned types, the same modifiers apply.
//
// Suppressing assignment with '*' (e.g. %*d) is supported and the
// conversion is performed but not stored.
//
// Returns count of successfully completed *and assigned* conversions,
// or -1 on a malformed format (e.g. %s without width, unrecognized
// conversion). NULL @a str returns -1.
// ---------------------------------------------------------------------------

#include <stdarg.h>

/**
 * @brief Scan @a str against @a fmt and assign to the listed pointers.
 *
 * @return number of conversions stored, or -1 on malformed format.
 */
int
axl_sscanf(
    const char  *str,
    const char  *fmt,
    ...
) __attribute__((format(scanf, 2, 3)));

/// va_list variant of axl_sscanf.
int
axl_vsscanf(
    const char  *str,
    const char  *fmt,
    va_list      ap
);

// ---------------------------------------------------------------------------
// Wide-string (UCS-2) utilities — UEFI internal use.
// Consumer code should use UTF-8. Convert with axl_ucs2_to_utf8().
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// UCS-2 primitive operations
// ---------------------------------------------------------------------------

/**
 * @brief Get UCS-2 string length. NULL returns 0.
 *
 * @return number of characters (not including NUL).
 */
size_t
axl_wcslen(
    const unsigned short *s  ///< UCS-2 string, or NULL
);

/**
 * @brief Compare two UCS-2 strings.
 *
 * NULL-safe: NULL sorts before non-NULL. Two NULLs are equal.
 *
 * @return <0, 0, or >0.
 */
int
axl_wcscmp(
    const unsigned short *a,  ///< first string
    const unsigned short *b   ///< second string
);

/**
 * @brief Test if two UCS-2 strings are equal. NULL-safe.
 *
 * Shorthand for `axl_wcscmp(a, b) == 0`.
 *
 * @return true if equal.
 */
static inline bool
axl_wcseql(
    const unsigned short *a,
    const unsigned short *b
    )
{
    if (a == b) { return true; }
    if (a == NULL || b == NULL) { return false; }
    return axl_wcscmp(a, b) == 0;
}

/**
 * @brief Copy UCS-2 string with size limit. Guarantees NUL-termination.
 */
void
axl_wcscpy(
    unsigned short       *dst,        ///< destination buffer
    const unsigned short *src,        ///< source string
    size_t                dst_count   ///< destination buffer size in characters
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_STR_H */
