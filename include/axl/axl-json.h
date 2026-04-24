/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-json.h:
 *
 * JSON parser (JSMN-based) and builder (fixed buffer).
 */

#ifndef AXL_JSON_H
#define AXL_JSON_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// JSON Parser
// ---------------------------------------------------------------------------

/**
 * AxlJsonCtx:
 *
 * Parsed JSON context. The token array is heap-allocated by
 * axl_json_parse and freed by axl_json_free. References the
 * original JSON buffer (do not free it while using the context).
 */
typedef struct {
    const char *json;
    size_t      json_len;
    int32_t    *tokens;
    int32_t     token_count;
    bool        owns_tokens;
} AxlJsonCtx;

/**
 * AxlJsonArrayIter:
 *
 * Iterator for JSON arrays.
 */
typedef struct {
    const AxlJsonCtx *ctx;
    int32_t           array_idx;
    int32_t           pos;
    int32_t           remaining;
} AxlJsonArrayIter;

/**
 * @brief Parse a JSON string.
 *
 * Allocates a token array sized to fit the document. Call
 * axl_json_free() when done to release the token memory.
 *
 * @return true on success, false on parse error or allocation failure.
 */
bool
axl_json_parse(
    const char *json,  ///< JSON string (not NUL-terminated required)
    size_t      len,   ///< length of @a json in bytes
    AxlJsonCtx *ctx    ///< context to fill
);

/**
 * @brief Free a parsed JSON context.
 *
 * Releases the heap-allocated token array. Safe to call on
 * contexts that don't own their tokens (e.g. array elements).
 * NULL-safe.
 */
void
axl_json_free(
    AxlJsonCtx *ctx  ///< context to free (NULL-safe)
);

/**
 * @brief Extract a string value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a string.
 */
bool
axl_json_get_string(
    const AxlJsonCtx *ctx,         ///< parsed context
    const char       *key,         ///< key to look up
    char             *value,       ///< buffer for string value
    size_t            value_size   ///< size of @a value buffer
);

/**
 * @brief Extract an integer value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a number.
 */
bool
axl_json_get_int(
    const AxlJsonCtx *ctx,    ///< parsed context
    const char       *key,    ///< key to look up
    int64_t          *value   ///< receives the integer
);

/**
 * @brief Extract an unsigned integer value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a number.
 */
bool
axl_json_get_uint(
    const AxlJsonCtx *ctx,    ///< parsed context
    const char       *key,    ///< key to look up
    uint64_t         *value   ///< receives the unsigned integer
);

/**
 * @brief Extract a boolean value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a boolean.
 */
bool
axl_json_get_bool(
    const AxlJsonCtx *ctx,    ///< parsed context
    const char       *key,    ///< key to look up
    bool             *value   ///< receives the boolean
);

/**
 * @brief One-shot: parse + extract a string in one call.
 *
 * Handles allocation and cleanup internally.
 *
 * @return true on success.
 */
bool
axl_json_extract_string(
    const char *json,        ///< JSON string
    size_t      len,         ///< length
    const char *key,         ///< key to extract
    char       *value,       ///< buffer for value
    size_t      value_size   ///< buffer size
);

/**
 * @brief Begin iterating a JSON array value by key name.
 *
 * @return true if array found, false if not found or not an array.
 */
bool
axl_json_array_begin(
    const AxlJsonCtx *ctx,    ///< parsed context
    const char       *key,    ///< key of the array field
    AxlJsonArrayIter *iter    ///< iterator to initialize
);

/**
 * @brief Begin iterating a root-level JSON array.
 *
 * Use when the JSON document is a bare array: `[{...}, {...}, ...]`
 * rather than an object with a named array field.
 *
 * @return true if root is an array, false otherwise.
 */
bool
axl_json_root_array_begin(
    const AxlJsonCtx *ctx,    ///< parsed context
    AxlJsonArrayIter *iter    ///< iterator to initialize
);

/**
 * @brief Advance to the next array element.
 *
 * The element context borrows the parent's token array — do not
 * call axl_json_free on it. It remains valid until the parent
 * context is freed.
 *
 * @return true if element returned, false if no more elements.
 */
bool
axl_json_array_next(
    AxlJsonArrayIter *iter,     ///< iterator
    AxlJsonCtx       *element   ///< context for the element
);

// ---------------------------------------------------------------------------
// JSON string escaping
// ---------------------------------------------------------------------------

/**
 * @brief Escape a string for safe embedding in JSON.
 *
 * Writes the escaped string WITH surrounding double quotes into @p out.
 * Escapes double-quote, backslash, the standard whitespace escapes
 * (newline / carriage-return / tab), and any remaining control
 * characters below 0x20.
 *
 * Useful for building JSON fragments via axl_snprintf: escape the
 * string first, then splice the quoted form directly into the
 * format template (%s). Example:
 *
 *     char esc[64];
 *     axl_json_escape_string(name, esc, sizeof(esc));
 *     axl_snprintf(buf, size, "{\"name\":%s}", esc);
 *
 * @return number of bytes written (excluding NUL), or -1 on error
 *     or truncation.
 */
int
axl_json_escape_string(
    const char *src,   ///< input string (UTF-8)
    char       *out,   ///< output buffer
    size_t      size   ///< output buffer size
);

// ---------------------------------------------------------------------------
// JSON Builder
// ---------------------------------------------------------------------------

/**
 * AxlJsonBuilder:
 *
 * JSON builder with fixed caller-provided buffer. No dynamic allocation.
 * Check overflow after building to detect truncation.
 */
typedef struct {
    char   *buffer;
    size_t  size;
    size_t  pos;
    bool    need_comma;
    bool    overflow;
} AxlJsonBuilder;

/// Initialize a JSON builder with a caller-provided buffer.
void
axl_json_init(
    AxlJsonBuilder *j,       ///< builder to initialize
    char           *buffer,  ///< caller-provided buffer
    size_t          size     ///< buffer size
);

/// Open a top-level JSON object (`{`).
void
axl_json_object_start(
    AxlJsonBuilder *j  ///< builder
);

/// Close the current JSON object (`}`).
void
axl_json_object_end(
    AxlJsonBuilder *j  ///< builder
);

/// Open a named nested object (`"key": {`).
void
axl_json_object_start_named(
    AxlJsonBuilder *j,    ///< builder
    const char     *key   ///< object key name
);

/// Open a named JSON array (`"key": [`).
void
axl_json_array_start(
    AxlJsonBuilder *j,    ///< builder
    const char     *key   ///< array key name
);

/// Close the current JSON array (`]`).
void
axl_json_array_end(
    AxlJsonBuilder *j  ///< builder
);

/// Open an object inside the current array.
void
axl_json_array_object_start(
    AxlJsonBuilder *j  ///< builder
);

/// Append a string value to the current array.
void
axl_json_array_add_string(
    AxlJsonBuilder *j,      ///< builder
    const char     *value   ///< string to add
);

/// Add a string key-value pair (`"key": "value"`).
void
axl_json_add_string(
    AxlJsonBuilder *j,      ///< builder
    const char     *key,    ///< key
    const char     *value   ///< string value
);

/// Add an unsigned integer key-value pair (`"key": 123`).
void
axl_json_add_uint(
    AxlJsonBuilder *j,      ///< builder
    const char     *key,    ///< key
    uint64_t        value   ///< unsigned integer value
);

/// Add a signed integer key-value pair (`"key": -42`).
void
axl_json_add_int(
    AxlJsonBuilder *j,      ///< builder
    const char     *key,    ///< key
    int64_t         value   ///< integer value
);

/// Add a boolean key-value pair (`"key": true`).
void
axl_json_add_bool(
    AxlJsonBuilder *j,      ///< builder
    const char     *key,    ///< key
    bool            value   ///< boolean value
);

/// Add a hex-formatted string key-value pair (`"key": "0x1A2B"`).
void
axl_json_add_hex(
    AxlJsonBuilder *j,      ///< builder
    const char     *key,    ///< key
    uint64_t        value   ///< value (written as hex string)
);

/// Add a null key-value pair (`"key": null`).
void
axl_json_add_null(
    AxlJsonBuilder *j,    ///< builder
    const char     *key   ///< key
);

/// Finalize the builder and NUL-terminate the output.
///
/// @return number of characters written (excluding NUL).
///     Check `j->overflow` to detect truncation.
size_t
axl_json_finish(
    AxlJsonBuilder *j  ///< builder
);

// ---------------------------------------------------------------------------
// JSON Pretty-Printer (UEFI console output with colors)
// ---------------------------------------------------------------------------

/**
 * @brief Pretty-print JSON to the console with colors and indentation.
 *
 * Colors: cyan keys, green strings, yellow numbers, magenta booleans.
 */
void
axl_json_pretty_print(
    const char *json,  ///< JSON string (ASCII)
    size_t      len    ///< length in bytes
);

/**
 * @brief Print JSON to the console without formatting.
 */
void
axl_json_print_raw(
    const char *json,  ///< JSON string (ASCII)
    size_t      len    ///< length in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_JSON_H */
