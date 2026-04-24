/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-config.h:
 *
 * Unified configuration framework. Declare options once in a
 * descriptor table, populate from command-line args or programmatic
 * set calls, retrieve with typed getters. Supports auto-apply via
 * offsetof, callbacks for custom logic, and optional parent
 * inheritance for cascading defaults.
 */

#ifndef AXL_CONFIG_H
#define AXL_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Option types
// ---------------------------------------------------------------------------

#define AXL_CFG_BOOL    1   ///< "true"/"false"/"1"/"0"
#define AXL_CFG_INT     2   ///< signed integer
#define AXL_CFG_UINT    3   ///< unsigned integer
#define AXL_CFG_STRING  4   ///< arbitrary string
#define AXL_CFG_MULTI   5   ///< repeatable string (array)

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------

/**
 * @brief Option descriptor. Define a static array terminated by {0}.
 */
typedef struct {
    const char *key;            ///< dotted name (e.g. "timeout.ms")
    int         type;           ///< AXL_CFG_BOOL, _INT, _UINT, _STRING, _MULTI
    const char *default_value;  ///< default as string (NULL = no default)
    char        short_flag;     ///< single-char CLI flag (0 = none)
    const char *description;    ///< help text
    size_t      offset;         ///< offsetof into target struct
    size_t      field_size;     ///< sizeof the target field (0 = no auto-apply)
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
 * @return 0 on success, -1 on unknown key, type mismatch, or
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
 * @return 0 on success, -1 on first error.
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
// Command-line argument parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parse command-line arguments into config.
 *
 * Maps short flags and long options (key names) to config values.
 * Flags without values are treated as boolean "true".
 * Supports `-k`, `--key`, `--key=value`, and `--` to stop parsing.
 * Unrecognized args are collected as positional arguments.
 *
 * @return 0 on success, -1 on error (unknown flag).
 */
int
axl_config_parse_args(
    AxlConfig *cfg,   ///< config
    int        argc,  ///< argument count
    char     **argv   ///< argument vector (argv[0] is program name)
);

/**
 * @brief Get positional argument by index.
 *
 * @return argument string, or NULL if index out of range.
 */
const char *
axl_config_pos(
    AxlConfig *cfg,   ///< config
    int        index  ///< 0-based index
);

/**
 * @brief Get count of positional arguments.
 */
int
axl_config_pos_count(
    AxlConfig *cfg  ///< config
);

// ---------------------------------------------------------------------------
// Help / usage
// ---------------------------------------------------------------------------

/**
 * @brief Print usage from descriptors.
 *
 * Outputs formatted help to stdout: "Usage: PROGRAM SYNOPSIS" followed
 * by option descriptions from the descriptor table.
 */
void
axl_config_usage(
    AxlConfig  *cfg,       ///< config (for descriptors)
    const char *program,   ///< program name
    const char *synopsis   ///< usage synopsis (e.g. "[options] FILE")
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
