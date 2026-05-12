/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sidecar.h
    Common scaffolding for JSON5 sidecar data files.

    A *sidecar* in the axl-sdk idiom is a curated JSON5 companion file
    that ships next to a `.efi` binary (or in a known location relative
    to it) and provides a small lookup table the binary consults at
    startup. Examples that ship in the source tree:

      - `share/pci-ids.json5` — PCI vendor / device / subsystem names
      - `share/pci-class.json5` — PCI class-name overlay
      - `share/jedec.json5` — JEDEC JEP-106 manufacturer codes

    Three module-side concerns recur for every sidecar consumer:

      1. **Open the file** with a useful error-code split so the
         consumer can log "deployment problem" (file missing) and
         "authoring problem" (file exists but malformed) differently.
      2. **Validate the schema field** — every sidecar in axl-sdk
         requires an explicit `schema: N` declaration so an old
         binary can refuse to misparse a future-version file.
      3. **Walk the typed entries** into the consumer's own table.

    AxlSidecar owns concerns 1 and 2; concern 3 (schema-specific
    typed walk) stays with each consumer because the table shapes
    differ — PCI's hierarchical vendors→devices→subsystems tree
    has nothing to share with SPD's flat `(code, name)` list.

    Internal axl-sdk modules also get a typed singleton wrapper and
    a hash-table foreach trampoline through @c axl-sidecar-internal.h
    (not part of the public API).

    @code
    AxlJsonReader  r;
    void          *raw;
    AxlSidecarStatus rc = axl_sidecar_open_file("foo.json5", &r, &raw);
    if (rc == AXL_SIDECAR_FILE_MISSING) { // log deployment hint
    } else if (rc == AXL_SIDECAR_PARSE_ERROR) { // log authoring hint
    } else {
        const uint64_t accepted[] = { 1u };
        uint64_t schema = 0;
        if (axl_sidecar_check_schema(&r, "foo", accepted,
                                     1, &schema) == AXL_SIDECAR_OK) {
            // schema 1 confirmed; walk fields and fill table
        }
        axl_json_free(&r);
        axl_free(raw);
    }
    @endcode
**/

#ifndef AXL_SIDECAR_H
#define AXL_SIDECAR_H

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-json.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------

/**
 * @brief Outcome of a sidecar load / parse / schema-check call.
 *
 * Numeric values are stable so legacy callers using `if (rc != 0)` /
 * `if (rc == -1)` keep working at the ABI level — but new code (and
 * every in-tree caller) compares against the named constants. Future
 * code that writes `-1` against a sidecar load API is wrong by
 * construction.
 *
 * The split between @c FILE_MISSING and @c PARSE_ERROR matters for
 * diagnostics: a missing file is a deployment problem (probably
 * fine to fall back to numeric IDs and continue silently), while a
 * present-but-malformed file is an authoring problem worth being
 * loud about.
 */
typedef enum {
    AXL_SIDECAR_OK            =  0,   ///< success
    AXL_SIDECAR_FILE_MISSING  = -1,   ///< file does not exist
    AXL_SIDECAR_PARSE_ERROR   = -2,   ///< file exists but JSON5 / schema rejection
} AxlSidecarStatus;

// ---------------------------------------------------------------------------
// File / buffer load
// ---------------------------------------------------------------------------

/**
 * @brief Open a JSON5 sidecar file with the standard error-code split.
 *
 * Performs the same sequence axl-sdk modules previously hand-rolled:
 * existence-check via axl_file_info (so we can distinguish
 * "missing" from "parse failed"), then axl_json_load_file_flags
 * with the @c AXL_JSON_PARSER_JSON5 grammar.
 *
 * On @c AXL_SIDECAR_OK, the caller owns @p r and @p *out_raw and must
 * release them with axl_json_free followed by @c axl_free
 * respectively. On any error return both are left untouched (the
 * caller does NOT free anything).
 *
 * @return @c AXL_SIDECAR_OK on a successful parse,
 *     @c AXL_SIDECAR_FILE_MISSING if @p path does not exist or is
 *     unreadable, @c AXL_SIDECAR_PARSE_ERROR on JSON5 syntax failure.
 */
AxlSidecarStatus
axl_sidecar_open_file(
    const char     *path,    ///< path to JSON5 file
    AxlJsonReader  *r,       ///< [out] reader to populate
    void          **out_raw  ///< [out] raw buffer the reader references; free with axl_free
);

/**
 * @brief Parse a JSON5 sidecar from an in-memory buffer.
 *
 * No @c AXL_SIDECAR_FILE_MISSING return — the buffer is the input,
 * so "not found" doesn't apply. Useful for embedded fixtures and
 * unit tests.
 *
 * On @c AXL_SIDECAR_OK, the caller owns @p r and must release it
 * with axl_json_free (the buffer itself stays caller-owned —
 * the reader references it but does not copy).
 *
 * @return @c AXL_SIDECAR_OK on a successful parse,
 *     @c AXL_SIDECAR_PARSE_ERROR on bad arguments or JSON5 failure.
 */
AxlSidecarStatus
axl_sidecar_open_buffer(
    const char     *json5,   ///< JSON5 source (no NUL required)
    size_t          len,     ///< buffer length in bytes
    AxlJsonReader  *r        ///< [out] reader to populate
);

// ---------------------------------------------------------------------------
// Schema validation
// ---------------------------------------------------------------------------

/**
 * @brief Read and validate the REQUIRED top-level `schema` field.
 *
 * Every axl-sdk sidecar declares its layout version up front:
 *
 * @code{.js}
 *   { schema: 2, vendors: [...] }
 * @endcode
 *
 * Without this declaration, a v1 file could be silently misparsed by
 * a v2-aware loader (or vice versa), producing entries that look
 * present but are missing nested data. AxlSidecar requires the
 * field; missing or unrecognized values trigger a uniform warning
 * and an @c AXL_SIDECAR_PARSE_ERROR return. The warning text names
 * @p module_name (e.g. "pci-ids", "jedec") so the operator knows
 * which sidecar to fix.
 *
 * @p accepted is a sorted list of the schema versions this loader
 * knows how to parse; the consumer dispatches on @p *out_schema after
 * the call returns @c AXL_SIDECAR_OK.
 *
 * @return @c AXL_SIDECAR_OK and @p *out_schema set on a recognized
 *     value, @c AXL_SIDECAR_PARSE_ERROR if the field is missing or
 *     not in @p accepted (with @p *out_schema unmodified).
 */
AxlSidecarStatus
axl_sidecar_check_schema(
    AxlJsonReader   *r,            ///< parsed reader at the document root
    const char      *module_name,  ///< short tag for diagnostics (e.g. "pci-ids")
    const uint64_t  *accepted,     ///< array of schema versions we accept
    size_t           n_accepted,   ///< length of @p accepted
    uint64_t        *out_schema    ///< [out] selected schema version on OK
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SIDECAR_H */
