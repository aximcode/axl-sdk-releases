/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-config.h:
 *
 * Live object property bag. Declare typed options once in a
 * descriptor table; mutate at runtime via axl_config_set;
 * retrieve via typed getters. Supports auto-apply into a target
 * struct via offsetof, dynamic-key callbacks (for namespaces like
 * "header.*"), and parent inheritance for cascading defaults.
 *
 * For command-line argument parsing, use axl_args_run from
 * <axl/axl-args.h>. AxlConfig used to do that too; the CLI surface
 * was retired once AxlArgs landed.
 */

#ifndef AXL_CONFIG_H
#define AXL_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Option types
// ---------------------------------------------------------------------------

/// Value type of a config option (AxlConfigDesc::type).
typedef enum {
    AXL_CFG_BOOL   = 1,  ///< "true"/"false"/"1"/"0"
    AXL_CFG_INT    = 2,  ///< signed integer
    AXL_CFG_UINT   = 3,  ///< unsigned integer
    AXL_CFG_STRING = 4,  ///< arbitrary string
    AXL_CFG_MULTI  = 5   ///< repeatable string (array)
} AxlConfigType;

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------

/**
 * @brief Option descriptor. Define a static array terminated by {0}.
 *
 * AxlConfig itself only consumes @c key, @c type, @c default_value,
 * @c offset, and @c field_size — the parser doesn't care whether an
 * option also has a CLI short flag or a closed set of allowed values.
 * The remaining fields exist so consumers can use a single descriptor
 * table to drive both AxlConfig auto-apply AND a synthesized
 * @c AxlArgDesc[] CLI surface (see @c axl_service_main, which walks
 * this table to build its argv parser). The CLI-only fields are
 * ignored by AxlConfig parsing.
 */
typedef struct {
    const char *key;            ///< dotted name (e.g. "timeout.ms")
    AxlConfigType type;         ///< AXL_CFG_BOOL, _INT, _UINT, _STRING, _MULTI
    const char *default_value;  ///< default as string (NULL = no default)
    const char *description;    ///< help text (logged in debug mode)
    size_t      offset;         ///< offsetof into target struct
    size_t      field_size;     ///< sizeof the target field (0 = no auto-apply)
    char        short_name;     ///< CLI short flag (single char), 0 = none.
                                ///< Used by axl_service_main when synthesizing
                                ///< its AxlArgDesc[]; AxlConfig parsing ignores.
    const char *const *choices; ///< NULL-terminated allowed-value list for
                                ///< STRING-typed options. When set, the
                                ///< axl_service_main synthesizer emits
                                ///< AXL_ARG_CHOICE instead of AXL_ARG_STRING
                                ///< so the CLI parser validates and the
                                ///< --help text lists the values. NULL = no
                                ///< restriction. Trailing position keeps the
                                ///< struct's existing zero-init layout
                                ///< compatible.
    int64_t     min;            ///< (numeric INT/UINT) inclusive lower bound,
                                ///< 0 = none (each of min/max independently).
                                ///< Like @c short_name / @c choices, AxlConfig
                                ///< parsing IGNORES this; it exists so the
                                ///< axl_service_main synthesizer can set the
                                ///< matching AxlArgDesc bound (CLI range
                                ///< validation + --help), and so a downstream
                                ///< settings-UI builder can size a spinner.
                                ///< Signed; cast to AxlArgDesc's uint64_t with
                                ///< the same convention (a UINT bound must
                                ///< still fit in int64_t — >= 2^63 is
                                ///< unrepresentable here). Trailing position
                                ///< keeps existing tables' zero-init valid.
    int64_t     max;            ///< (numeric INT/UINT) inclusive upper bound,
                                ///< 0 = none. See @c min.
} AxlConfigDesc;

// ---------------------------------------------------------------------------
// AxlConfig object
// ---------------------------------------------------------------------------

typedef struct AxlConfig AxlConfig;

/**
 * @brief Callback invoked when an option is set.
 *
 * Called BEFORE descriptor lookup — handles dynamic keys (e.g.
 * "header.*") that aren't in the descriptor table.
 *
 * Return values:
 *  -  0: accepted, proceed with descriptor lookup + auto-apply
 *  -  1: handled by callback (value stored, auto-apply skipped)
 *  - -1: rejected (set returns -1)
 */
typedef int (*AxlConfigApplyFunc)(
    void       *target,  ///< opaque pointer from axl_config_new
    const char *key,     ///< option key
    const char *value    ///< new value (string)
);

/**
 * @brief Create a config object from descriptors.
 *
 * Defaults from descriptors are applied immediately. If @p target
 * is non-NULL and descriptors have offset/field_size, defaults are
 * written into the target struct.
 *
 * @return new config, or NULL on allocation failure.
 */
AxlConfig *
axl_config_new(
    const AxlConfigDesc  *descs,     ///< descriptor table (borrowed, not copied)
    AxlConfigApplyFunc    apply_fn,  ///< change callback (NULL for auto-only)
    void                 *target     ///< opaque pointer for apply_fn + auto-apply
);

/**
 * @brief Free a config object. NULL-safe.
 */
void
axl_config_free(
    AxlConfig *cfg  ///< config to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlConfig, axl_config_free)
#endif

/**
 * @brief Set a config option.
 *
 * Validates type, calls apply_fn (if set), then auto-applies via
 * offsetof (if descriptor has offset). Stores the string value
 * internally for later retrieval.
 *
 * @return AXL_OK on success, AXL_ERR on unknown key, type mismatch, or
 *     callback rejection.
 */
int
axl_config_set(
    AxlConfig  *cfg,    ///< config
    const char *key,    ///< option key
    const char *value   ///< value as string
);

/**
 * @brief Set multiple options in one call.
 *
 * Accepts key/value string pairs terminated by NULL. Stops at the
 * first failure and returns -1.
 *
 * @code
 * axl_config_setv(cfg,
 *     "port", "8080",
 *     "verbose", "true",
 *     NULL);
 * @endcode
 *
 * @return AXL_OK on success, AXL_ERR on first error.
 */
int
axl_config_setv(
    AxlConfig  *cfg,  ///< config
    ...               ///< key, value pairs terminated by NULL
);

/**
 * @brief Get an option value as string.
 *
 * @return stored value, default, or NULL if unknown key.
 */
const char *
axl_config_get(
    AxlConfig  *cfg,  ///< config
    const char *key   ///< option key
);

/**
 * @brief Get a boolean option.
 *
 * Accepts "true"/"1"/"yes" as true, everything else as false.
 */
bool
axl_config_get_bool(
    AxlConfig  *cfg,  ///< config
    const char *key   ///< option key
);

/**
 * @brief Get a signed integer option.
 *
 * @return parsed value, or 0 if unset or not a number.
 */
int64_t
axl_config_get_int(
    AxlConfig  *cfg,  ///< config
    const char *key   ///< option key
);

/**
 * @brief Get an unsigned integer option.
 *
 * @return parsed value, or 0 if unset or not a number.
 */
uint64_t
axl_config_get_uint(
    AxlConfig  *cfg,  ///< config
    const char *key   ///< option key
);

/**
 * @brief Get the count of values for a MULTI option.
 */
size_t
axl_config_get_multi_count(
    AxlConfig  *cfg,  ///< config
    const char *key   ///< option key
);

/**
 * @brief Get a value from a MULTI option by index.
 *
 * @return value string, or NULL if index out of range.
 */
const char *
axl_config_get_multi(
    AxlConfig  *cfg,    ///< config
    const char *key,    ///< option key
    size_t      index   ///< 0-based index
);

// ---------------------------------------------------------------------------
// Cross-binary serialization
// ---------------------------------------------------------------------------
//
// Wire format: URL-encoded query string (RFC 3986) — `key=value&key=...`.
// Both keys and values percent-encode anything outside the unreserved
// set (so '&' and '=' inside values are escaped as `%26` / `%3D`).
// Repeated keys become AXL_CFG_MULTI values on the parse side. Used by
// AxlService to ship a foreground process's options through
// EFI_LOADED_IMAGE_PROTOCOL.LoadOptions to a same-source-tree driver
// image.
//
// Parsers / encoders dogfood axl_url_encode / axl_url_decode — same
// escaping rules as the rest of AXL's net stack, no bespoke format.

/**
 * @brief Serialize all set values to a URL query string.
 *
 * Walks every value currently set in the config (single and MULTI),
 * emits `key=value&...` with both key and value URL-percent-encoded.
 * Defaults that haven't been overridden are NOT emitted (the parsing
 * side will re-apply defaults from its own descriptor table).
 *
 * @return AXL_OK on success, AXL_ERR on @p out_size overflow or NULL
 *     args. On overflow @p out is left in an unspecified state.
 */
int
axl_config_to_string(
    AxlConfig *cfg,        ///< config to serialize
    char      *out,        ///< output buffer
    size_t     out_size    ///< capacity of @p out in bytes
);

/**
 * @brief Serialize a target struct directly via its descriptor table.
 *
 * Walks @p descs, reads each option's current value from @p target
 * via offsetof, formats it, URL-encodes the pair, appends to @p out.
 * Independent of any AxlConfig instance — useful for cross-binary
 * marshalling where the consumer populates @p target through CLI
 * parsing (axl_args_*) without ever creating an AxlConfig.
 *
 * Skips MULTI options (offset/field_size are 0 for those by
 * convention; serializing requires walking an opaque list the
 * descriptor doesn't reach). Skips entries with field_size == 0.
 *
 * @return AXL_OK on success, AXL_ERR on overflow or NULL args.
 */
int
axl_config_target_to_string(
    const AxlConfigDesc *descs,    ///< descriptor table (terminated by {0})
    const void          *target,   ///< struct to read fields from
    char                *out,      ///< output buffer
    size_t               out_size  ///< capacity of @p out in bytes
);

/**
 * @brief Parse a URL query string into the config.
 *
 * Splits on '&' into pairs, splits each pair on the first '=',
 * URL-decodes both halves, calls axl_config_set on each. A value
 * with no '=' is treated as the empty string. Repeated keys are
 * fed to axl_config_set in order (which appends to MULTI options
 * and overwrites scalar ones).
 *
 * Stops at the first axl_config_set failure and returns AXL_ERR —
 * callers that need partial-parse-on-error semantics should split
 * the string themselves and call axl_config_set in a loop.
 *
 * @return AXL_OK on success, AXL_ERR on NULL args, malformed encoding,
 *     or any axl_config_set failure.
 */
int
axl_config_from_string(
    AxlConfig  *cfg,   ///< config to populate
    const char *in     ///< URL query string (NUL-terminated)
);

// ---------------------------------------------------------------------------
// Descriptor-table composition helpers (group injection)
// ---------------------------------------------------------------------------
//
// Networked tools and services typically need the same NIC /
// static-IP / port / listen-IP options that a sibling library
// (e.g. AxlNetOpts) defines. Instead of every consumer copy-pasting
// the descriptor entries, the consumer embeds the standard sub-struct
// in its own options type and calls axl_config_descs_net(...) to
// emit the matching descriptors into a local accumulator. The
// consumer then appends its own descriptor fragment via
// axl_config_descs_append and terminates with a zeroed entry. The
// resulting table is fed to axl_config_new as usual.
//
// Why this shape (no varargs axl_config_compose): keeps the C type
// system fully engaged on the consumer's own fragment (no untyped
// va_args), preserves AxlConfigDesc's short_name / choices fields,
// and is ~30 LOC instead of ~120. Future axl_config_descs_log /
// axl_config_descs_tls slot in the same pattern when a second
// consumer asks for them.

/**
 * @brief Emit the standard NIC / static-IP / port / listen-IP
 *     descriptors into a consumer-owned accumulator.
 *
 * @p kinds is a bitmask of AxlNetOptKind values (see
 * <axl/axl-net-opts.h>) selecting which subset to emit; the
 * AXL_NET_OPT_CLIENT / _SERVER presets are the common cases.
 *
 * Each emitted descriptor's @c offset is added to @p base_offset —
 * the @c offsetof of the consumer's embedded @c AxlNetOpts
 * sub-struct in its own options type — so AxlConfig's auto-apply
 * lands the parsed value in the right place. @c field_size is set
 * from the corresponding @c AxlNetOpts member, including the
 * @c uint16_t @c port (so AXL_CFG_UINT auto-apply doesn't write
 * past the field).
 *
 * Writes consecutively into @p out starting at index 0. Does NOT
 * append a terminating zeroed entry — callers compose with
 * axl_config_descs_append and terminate the combined table
 * themselves.
 *
 * @return number of descriptors written. Returns 0 (no partial
 *     write) and logs a warning via log domain @c "net" if
 *     @p cap is too small or @p out is NULL.
 */
size_t
axl_config_descs_net(
    AxlConfigDesc *out,          ///< accumulator (caller-owned, written at [0..])
    size_t         cap,          ///< capacity of @p out in entries
    uint32_t       kinds,        ///< bitmask of AxlNetOptKind
    size_t         base_offset   ///< offsetof(consumer-Opts, AxlNetOpts-sub-struct)
);

/**
 * @brief Emit the static-IP / DNS / hostname (IP4Config2 policy)
 *     descriptors into a consumer-owned accumulator.
 *
 * The policy-group sibling of axl_config_descs_net: it injects the
 * descriptors an on-box `ifconfig` UI hand-authors today — `mode`
 * (a `"dhcp"`/`"static"` two-choice picker), `ip`, `netmask`,
 * `gateway`, `dns`, `dns2`, `hostname` — bound to a consumer-embedded
 * @c AxlNetStaticOpts (see <axl/axl-net-opts.h>). Each emitted
 * descriptor's @c offset is added to @p base_offset (the @c offsetof of
 * the embedded @c AxlNetStaticOpts), so AxlConfig auto-apply lands the
 * parsed value in the right field. Unlike axl_config_descs_net there is
 * no `kinds` selector — the policy form is taken as a whole.
 *
 * Writes 7 descriptors consecutively into @p out starting at index 0.
 * Does NOT append a terminating zeroed entry — compose with
 * axl_config_descs_append and terminate the combined table yourself.
 *
 * @return number of descriptors written (7). Returns 0 (no partial
 *     write) and logs a warning via log domain @c "net" if @p cap is
 *     too small or @p out is NULL.
 */
size_t
axl_config_descs_net_static(
    AxlConfigDesc *out,          ///< accumulator (caller-owned, written at [0..])
    size_t         cap,          ///< capacity of @p out in entries (>= 7)
    size_t         base_offset   ///< offsetof(consumer-Opts, AxlNetStaticOpts-sub-struct)
);

/**
 * @brief Copy a consumer-owned NULL-terminated descriptor fragment
 *     onto the end of an accumulator.
 *
 * Walks @p src until the first zeroed entry (key == NULL),
 * copying each preceding descriptor into @p out. The terminator
 * is NOT copied — the caller writes the final zeroed entry once,
 * after all fragments have been appended.
 *
 * @return number of descriptors copied. Returns 0 (no partial
 *     write) if @p out or @p src is NULL; an under-capacity
 *     request also returns 0 and logs a warning via log domain
 *     @c "config".
 */
size_t
axl_config_descs_append(
    AxlConfigDesc       *out,   ///< accumulator (write position)
    size_t               cap,   ///< remaining capacity in entries
    const AxlConfigDesc *src    ///< NULL-terminated fragment to copy
);

// ---------------------------------------------------------------------------
// Inheritance
// ---------------------------------------------------------------------------

/**
 * @brief Set a parent config for cascading defaults.
 *
 * When axl_config_get finds no value for a key, it falls through
 * to the parent. Useful for per-connection configs that inherit
 * server-level defaults.
 */
void
axl_config_set_parent(
    AxlConfig *cfg,     ///< child config
    AxlConfig *parent   ///< parent config (borrowed, not owned)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONFIG_H */
