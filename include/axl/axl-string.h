/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-string.h:
 *
 * Mutable auto-growing string builder, like GLib's GString.
 * All strings are UTF-8 (char *).
 *
 * Format specifiers follow standard C printf conventions:
 *   %s  — char * string
 *   %d  — signed int
 *   %u  — unsigned int
 *   %x  — hex (lowercase), %X hex (uppercase)
 *   %lu — unsigned long
 *   %llu — unsigned long long (uint64_t)
 *   %zu — size_t
 *   %c  — char
 *   %p  — pointer
 */

#ifndef AXL_STRING_H
#define AXL_STRING_H

#include <stddef.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlString AxlString;

/**
 * @brief Create a new string builder.
 *
 * If @a init is non-NULL, the string is initialized with that content.
 * Pass NULL for an empty string builder.
 *
 * @return a new AxlString, or NULL on allocation failure.
 *     Free with axl_string_free().
 */
AxlString *
axl_string_new(
    const char *init  ///< initial content (NULL for empty)
);

/**
 * @brief Create a new string builder with pre-reserved capacity.
 *
 * @return a new AxlString, or NULL on allocation failure.
 */
AxlString *
axl_string_new_size(
    size_t reserve  ///< initial capacity in bytes
);

/**
 * @brief Append a string. NULL is treated as empty.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_append(
    AxlString  *b, ///< string builder
    const char *s  ///< NUL-terminated string to append
);

/**
 * @brief Append exactly @a len bytes from @a data.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_append_len(
    AxlString  *b,    ///< string builder
    const char *data, ///< bytes to append (not necessarily NUL-terminated)
    size_t      len   ///< number of bytes
);

/**
 * @brief Append formatted text. Auto-grows the buffer as needed.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_append_printf(
    AxlString  *b,   ///< string builder
    const char *fmt, ///< printf-style format string
    ...
) __attribute__((format(printf, 2, 3)));

/**
 * @brief Append a single character.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_append_c(
    AxlString *b, ///< string builder
    char       c  ///< character to append
);

/**
 * @brief Prepend a NUL-terminated string.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_prepend(
    AxlString  *b, ///< string builder
    const char *s  ///< NUL-terminated string to prepend
);

/**
 * @brief Prepend exactly @a len bytes from @a s.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_prepend_len(
    AxlString  *b,   ///< string builder
    const char *s,   ///< bytes to prepend
    size_t      len  ///< number of bytes
);

/**
 * @brief Prepend a single character.
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_prepend_c(
    AxlString *b, ///< string builder
    char       c  ///< character to prepend
);

/**
 * @brief Insert a NUL-terminated string at @a pos.
 *
 * If @a pos >= current length, equivalent to append.
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_insert(
    AxlString  *b,   ///< string builder
    size_t      pos, ///< byte offset to insert at
    const char *s    ///< NUL-terminated string to insert
);

/**
 * @brief Remove @a len bytes starting at @a pos.
 *
 * If @a pos >= current length, no-op. If pos + len exceeds the
 * length, erases to end.
 *
 * @return AXL_OK on success, AXL_ERR if b is NULL.
 */
int
axl_string_erase(
    AxlString *b,   ///< string builder
    size_t     pos, ///< byte offset to start erasing
    size_t     len  ///< number of bytes to erase
);

/**
 * @brief Truncate the string to @a len bytes.
 *
 * If @a len >= current length, no-op (does not grow).
 *
 * @return AXL_OK on success, AXL_ERR if b is NULL.
 */
int
axl_string_truncate(
    AxlString *b,  ///< string builder
    size_t     len ///< new length
);

/**
 * @brief Overwrite content at @a pos with @a s.
 *
 * If pos + strlen(s) exceeds the current length, the string is
 * grown to accommodate.
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_string_overwrite(
    AxlString  *b,   ///< string builder
    size_t      pos, ///< byte offset to start overwriting
    const char *s    ///< NUL-terminated replacement string
);

/**
 * @brief Get the current string content.
 *
 * The returned pointer is owned by the builder and becomes invalid
 * after modification or free.
 *
 * @return NUL-terminated string, or "" if builder is NULL.
 */
const char *
axl_string_str(
    AxlString *b  ///< string builder
);

/**
 * @brief Get current string length.
 *
 * @return current string length (not counting NUL terminator).
 */
size_t
axl_string_len(
    AxlString *b  ///< string builder
);

/**
 * @brief Transfer ownership of the internal string to the caller.
 *
 * The builder is left empty (len=0). Caller frees with axl_free().
 *
 * @return the string, or NULL if empty/allocation failed.
 */
char *
axl_string_steal(
    AxlString *b  ///< string builder
);

/**
 * @brief Reset to empty string. Keeps the allocated buffer for reuse.
 */
void
axl_string_clear(
    AxlString *b  ///< string builder
);

/**
 * @brief Free the builder and its internal buffer. NULL-safe.
 */
void
axl_string_free(
    AxlString *b  ///< string builder, or NULL
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlString, axl_string_free)
#endif

/**
 * @brief Format a string into a newly allocated buffer.
 *
 * Like GLib's g_strdup_printf(). Caller frees with axl_free().
 *
 * @return formatted string, or NULL on failure.
 */
char *
axl_asprintf(
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* AXL_STRING_H */
