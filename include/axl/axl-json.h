/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-json.h:
 *
 * JSON reader (JSMN-based) and writer (AxlString-backed). The reader
 * parses a JSON string into a token array; the writer builds JSON into
 * an auto-growing AxlString with orthogonal container/key/atom calls
 * and optional 2-space-indent pretty-print mode. A separate colored
 * UEFI-console pretty-printer is provided for debug output.
 *
 * Three independent APIs:
 *   - AxlJsonReader        — parse + query
 *   - AxlJsonWriter        — build into an AxlString
 *   - axl_json_console_print — colored console output for already-built JSON
 */

#ifndef AXL_JSON_H
#define AXL_JSON_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// JSON Reader
// ---------------------------------------------------------------------------

/**
 * AxlJsonReader:
 *
 * Parsed JSON reader. The token array is heap-allocated by
 * axl_json_parse and freed by axl_json_free. References the
 * original JSON buffer (do not free it while using the reader).
 *
 * Fields are considered private; treat as opaque.
 */
typedef struct {
    const char *json;
    size_t      json_len;
    int32_t    *tokens;
    int32_t     token_count;
    bool        owns_tokens;
} AxlJsonReader;

/**
 * AxlJsonArrayIter:
 *
 * Iterator for JSON arrays. Fields are private.
 */
typedef struct {
    const AxlJsonReader *reader;
    int32_t              array_idx;
    int32_t              pos;
    int32_t              remaining;
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
    const char    *json,  ///< JSON string (NUL terminator not required)
    size_t         len,   ///< length of @a json in bytes
    AxlJsonReader *r      ///< reader to fill
);

/**
 * @brief Free a parsed JSON reader.
 *
 * Releases the heap-allocated token array. Safe to call on
 * readers that don't own their tokens (e.g. array elements).
 * NULL-safe.
 */
void
axl_json_free(
    AxlJsonReader *r  ///< reader to free (NULL-safe)
);

/**
 * @brief Extract a string value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a string.
 */
bool
axl_json_get_string(
    const AxlJsonReader *r,           ///< reader
    const char          *key,         ///< key to look up
    char                *value,       ///< buffer for string value
    size_t               value_size   ///< size of @a value buffer
);

/**
 * @brief Extract an integer value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a number.
 */
bool
axl_json_get_int(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    int64_t             *value    ///< receives the integer
);

/**
 * @brief Extract an unsigned integer value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a number.
 */
bool
axl_json_get_uint(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    uint64_t            *value    ///< receives the unsigned integer
);

/**
 * @brief Extract a boolean value from a parsed JSON object.
 *
 * @return true if found, false if not found or not a boolean.
 */
bool
axl_json_get_bool(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    bool                *value    ///< receives the boolean
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
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key of the array field
    AxlJsonArrayIter    *iter     ///< iterator to initialize
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
    const AxlJsonReader *r,      ///< reader
    AxlJsonArrayIter    *iter    ///< iterator to initialize
);

/**
 * @brief Advance to the next array element.
 *
 * The element reader borrows the parent's token array — do not
 * call axl_json_free on it. It remains valid until the parent
 * reader is freed.
 *
 * @return true if element returned, false if no more elements.
 */
bool
axl_json_array_next(
    AxlJsonArrayIter *iter,    ///< iterator
    AxlJsonReader    *element  ///< reader for the element
);

// ---------------------------------------------------------------------------
// JSON String Escaping (utility, used by both reader and writer)
// ---------------------------------------------------------------------------

/**
 * @brief Escape a string for safe embedding in JSON.
 *
 * Writes the escaped string WITH surrounding double quotes into @p out.
 * Escapes double-quote, backslash, the standard whitespace escapes
 * (newline / carriage-return / tab), and any remaining control
 * characters below 0x20.
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
// JSON Writer
// ---------------------------------------------------------------------------

/**
 * AxlJsonWriterFlags:
 *
 * Flags passed to axl_json_writer_init.
 */
typedef enum {
    AXL_JSON_WRITER_DEFAULT = 0,        ///< compact output, no whitespace
    AXL_JSON_WRITER_PRETTY  = 1u << 0,  ///< 2-space indent + newlines
} AxlJsonWriterFlags;

/// Maximum nesting depth the writer's state machine tracks.
#define AXL_JSON_WRITER_MAX_DEPTH 32

/**
 * AxlJsonWriter:
 *
 * Streaming JSON writer that builds into a caller-owned AxlString.
 * Containers, keys, and atoms are independent calls; the state
 * machine handles comma placement, indentation, and the
 * object-vs-array distinction. Fields are private — use accessors.
 */
typedef struct {
    AxlString *out;                                    ///< backing store (caller-owned)
    uint32_t   flags;                                  ///< AxlJsonWriterFlags
    uint32_t   depth;                                  ///< current nesting depth
    uint32_t   in_array_bits;                          ///< bit n: depth-n container is array
    bool       needs_comma;                            ///< previous emit needs a trailing comma
    bool       expecting_value;                        ///< object: true after a key
    bool       error;                                  ///< sticky error flag
} AxlJsonWriter;

/**
 * @brief Initialize a writer.
 *
 * The writer appends to the AxlString — it does not clear it. To
 * reuse a string between writes, the caller calls axl_string_clear()
 * before init.
 */
void
axl_json_writer_init(
    AxlJsonWriter *w,      ///< writer to initialize
    AxlString     *out,    ///< destination string (caller-owned)
    uint32_t       flags   ///< AxlJsonWriterFlags
);

/**
 * @brief Finalize the writer.
 *
 * Validates that all opened containers were closed; sets the sticky
 * error flag if not.
 *
 * @return total number of bytes written into @p out by this writer.
 */
size_t
axl_json_writer_finish(
    AxlJsonWriter *w  ///< writer
);

/**
 * @brief Query the sticky error flag.
 *
 * Set on AxlString OOM or structural misuse (see README). Once set,
 * all subsequent writer calls become no-ops.
 *
 * @return true if any error occurred since init.
 */
bool
axl_json_writer_error(
    const AxlJsonWriter *w  ///< writer
);

// --- Containers ---

/// Open an object (`{`).
void
axl_json_obj_begin(
    AxlJsonWriter *w  ///< writer
);

/// Close the current object (`}`).
void
axl_json_obj_end(
    AxlJsonWriter *w  ///< writer
);

/// Open an array (`[`).
void
axl_json_arr_begin(
    AxlJsonWriter *w  ///< writer
);

/// Close the current array (`]`).
void
axl_json_arr_end(
    AxlJsonWriter *w  ///< writer
);

// --- Keys (object context only) ---

/// Emit a key (`"key":`). Must be inside an object context.
void
axl_json_key(
    AxlJsonWriter *w,    ///< writer
    const char    *key   ///< object key (escaped, NUL-terminated)
);

/// Emit a key from a non-NUL-terminated buffer (`"key":`).
void
axl_json_keyn(
    AxlJsonWriter *w,    ///< writer
    const char    *key,  ///< object key bytes (escaped)
    size_t         n     ///< number of bytes
);

// --- Atoms (after a key, or in array context) ---

/// Emit a string atom (escaped).
void
axl_json_str(
    AxlJsonWriter *w,   ///< writer
    const char    *s    ///< string value (NUL-terminated)
);

/// Emit a string atom from a non-NUL-terminated buffer (escaped).
void
axl_json_strn(
    AxlJsonWriter *w,   ///< writer
    const char    *s,   ///< string bytes
    size_t         n    ///< number of bytes
);

/// Emit a signed integer atom.
void
axl_json_int(
    AxlJsonWriter *w,   ///< writer
    int64_t        v    ///< value
);

/// Emit an unsigned integer atom.
void
axl_json_uint(
    AxlJsonWriter *w,   ///< writer
    uint64_t       v    ///< value
);

/// Emit a boolean atom.
void
axl_json_bool(
    AxlJsonWriter *w,   ///< writer
    bool           v    ///< value
);

/// Emit a null atom.
void
axl_json_null(
    AxlJsonWriter *w  ///< writer
);

/// Emit a hex-formatted string atom (`"0x1A2B"`).
void
axl_json_hex(
    AxlJsonWriter *w,   ///< writer
    uint64_t       v    ///< value
);

/**
 * @brief Splice a raw JSON fragment into the output (no escaping).
 *
 * The caller asserts that @p fragment is well-formed JSON. The writer
 * splices it as-is, just like an atom (handles comma placement and
 * indentation around it).
 */
void
axl_json_raw(
    AxlJsonWriter *w,         ///< writer
    const char    *fragment   ///< pre-formed JSON
);

// --- Convenience: key + atomic value pairs (object context) ---

/// `"key":"value"` — string.
void
axl_json_kv_str(
    AxlJsonWriter *w,
    const char    *key,
    const char    *value
);

/// `"key":"value"` — string from a non-NUL-terminated buffer.
void
axl_json_kv_strn(
    AxlJsonWriter *w,
    const char    *key,
    const char    *value,
    size_t         value_n
);

/// `"key":<int>` — signed integer.
void
axl_json_kv_int(
    AxlJsonWriter *w,
    const char    *key,
    int64_t        value
);

/// `"key":<uint>` — unsigned integer.
void
axl_json_kv_uint(
    AxlJsonWriter *w,
    const char    *key,
    uint64_t       value
);

/// `"key":true|false` — boolean.
void
axl_json_kv_bool(
    AxlJsonWriter *w,
    const char    *key,
    bool           value
);

/// `"key":null` — null.
void
axl_json_kv_null(
    AxlJsonWriter *w,
    const char    *key
);

/// `"key":"0x1A2B"` — hex-formatted string.
void
axl_json_kv_hex(
    AxlJsonWriter *w,
    const char    *key,
    uint64_t       value
);

// --- Bridge: parse → write round-trip ---

/**
 * @brief Splice an already-parsed token into the writer's output.
 *
 * Walks the token tree at @p tok_idx (objects, arrays, atoms),
 * re-escaping strings as necessary, and emits a faithful copy into
 * the writer. Use to round-trip parts of a parsed document into a
 * larger output without manual re-formatting.
 *
 * Pass tok_idx = 0 to splice the root document.
 */
void
axl_json_write_token(
    AxlJsonWriter       *w,          ///< writer
    const AxlJsonReader *r,          ///< source reader
    int                  tok_idx     ///< token index in @p r (0 = root)
);

// ---------------------------------------------------------------------------
// JSON Console Pretty-Printer (UEFI console output with colors)
// ---------------------------------------------------------------------------

/**
 * @brief Pretty-print JSON to the console with colors and indentation.
 *
 * Colors: cyan keys, green strings, yellow numbers, magenta booleans.
 * Distinct from the writer's AXL_JSON_WRITER_PRETTY flag — that
 * produces buffer output without color; this writes directly to the
 * UEFI console using the platform's attribute-based color API.
 */
void
axl_json_console_print(
    const char *json,  ///< JSON string (ASCII)
    size_t      len    ///< length in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_JSON_H */
