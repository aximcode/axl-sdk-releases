/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-args.h:
 *
 * Declarative command-line parser for AXL tools. Replaces the
 * AxlConfig + axl_subcommand_dispatch + hand-rolled positional-arg
 * boilerplate that every tool used to repeat.
 *
 * A tool declares a static @ref AxlArgsApp tree (program name +
 * synopsis + global flags + verbs + per-verb flags + per-verb
 * positional args + handler) and calls @ref axl_args_run from main.
 * The framework parses argv, validates types and bounds, generates a
 * structured `--help`, and dispatches to the right verb handler.
 *
 * Example (memspd-shaped tool with three verbs):
 *
 * @code
 * static int do_show(AxlArgs *a) {
 *     uint8_t addr = (uint8_t)axl_args_get_uint(a, "slot");
 *     // slot is already validated against min/max by the framework
 *     return read_and_print(addr);
 * }
 *
 * static const AxlArgDesc kSlotArg[] = {
 *     { .name = "slot", .type = AXL_ARG_U8, .base = 0,
 *       .min = 0x50, .max = 0x57, .required = true,
 *       .help = "SMBus slot address (hex)" },
 *     {0}
 * };
 *
 * static const AxlVerb kVerbs[] = {
 *     { .name = "show", .handler = do_show, .positionals = kSlotArg,
 *       .help = "Decoded fields for one slot" },
 *     {0}
 * };
 *
 * static const AxlArgDesc kFlags[] = {
 *     { .name = "jedec-file", .short_name = 'j', .type = AXL_ARG_STRING,
 *       .help = "Path to JEDEC vendor JSON sidecar" },
 *     {0}
 * };
 *
 * int main(int argc, char **argv) {
 *     return axl_args_run(argc, argv, &(AxlArgsApp){
 *         .name = "memspd",
 *         .help = "Read JEDEC SPD content",
 *         .global_flags = kFlags,
 *         .verbs = kVerbs,
 *     });
 * }
 * @endcode
 *
 * `--help` and `-h` are always recognised. Unknown flags / missing
 * required positionals / out-of-range typed args produce an error
 * message + the auto-generated usage and exit non-zero, with no
 * handler invocation.
 */

#ifndef AXL_ARGS_H
#define AXL_ARGS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Argument descriptors
// ---------------------------------------------------------------------------

/**
 * @brief Argument value type. Drives parsing and validation.
 */
typedef enum {
    AXL_ARG_BOOL,    ///< Presence flag (no value); --flag / -f
    AXL_ARG_STRING,  ///< String value
    AXL_ARG_U8,      ///< uint8_t with optional [min,max] bounds
    AXL_ARG_U16,     ///< uint16_t
    AXL_ARG_U32,     ///< uint32_t
    AXL_ARG_U64,     ///< uint64_t
    AXL_ARG_S64,     ///< int64_t (only signed type supported)
    AXL_ARG_MULTI,   ///< Repeatable string (variadic positional, or repeatable flag)
} AxlArgType;

/**
 * @brief Single-argument descriptor — used for both flags and
 *     positional arguments.
 *
 * Designated initializers expected: only the fields you care about
 * need to be set (zero defaults are sane for all fields).
 *
 * For numeric types (`AXL_ARG_U8`..`AXL_ARG_S64`):
 *  - @c base: 0 (auto-detect 0x prefix), 10, or 16 — passed to the
 *    @ref axl_str_to_u64 family. Default 0 = auto.
 *  - @c min, @c max: inclusive bounds. Both 0 = no bound.
 *
 * For positionals: @c short_name and @c default_value are ignored.
 * Position in the array determines argument order. The LAST entry
 * may have @c type == @c AXL_ARG_MULTI to mean "collect all
 * remaining positionals" (variadic tail).
 */
typedef struct {
    const char *name;          ///< long name; positional arg name; lookup key
    char        short_name;    ///< short flag (single char), 0 if none
    AxlArgType  type;
    const char *default_value; ///< default for flags when unset, NULL = unset
    const char *help;          ///< one-line description (shown in --help)
    bool        required;      ///< (positionals only) error if missing
    int         base;          ///< (numeric types) 0 (auto) | 10 | 16
    uint64_t    min;           ///< (numeric types) inclusive min, 0 = none
    uint64_t    max;           ///< (numeric types) inclusive max, 0 = none
} AxlArgDesc;

// ---------------------------------------------------------------------------
// Verb tree + app
// ---------------------------------------------------------------------------

typedef struct AxlArgs AxlArgs;

/**
 * @brief Verb handler signature. Return value becomes the program's
 *     exit code.
 *
 * The @p args object exposes parsed flags and positional values via
 * the @c axl_args_get_* accessors. Do not call @c axl_args_run
 * recursively.
 *
 * **Lifetime contract** — important when the handler enters an event
 * loop (@ref axl_loop_run) or otherwise blocks before returning:
 *  - The @c AxlArgs struct itself, all `axl_args_get_*` accessor
 *    return values, and the variadic-positional view all live until
 *    the handler returns to @ref axl_args_run.
 *  - String values returned by @c axl_args_get_string and
 *    @c axl_args_get_multi point into the program's @c argv (which
 *    the runtime keeps alive for the program's lifetime). They
 *    remain valid even after the handler returns — safe to stash in
 *    a global and reference from a later AxlLoop callback.
 *  - Iteration views (@c axl_args_get_pos_count, @c get_multi_count,
 *    and the underlying arrays) are freed when the handler returns.
 *    Loop callbacks that need to inspect repeatable flags or
 *    variadic positionals must capture them inside the handler
 *    before entering the loop.
 *  - Numeric values are plain values — copy and use freely.
 *
 * In short: extract everything you need from @p args **before**
 * entering @c axl_loop_run; never call @c axl_args_get_* from a
 * loop callback.
 */
typedef int (*AxlVerbHandler)(AxlArgs *args);

/**
 * @brief Optional pre-handler hook. Called once after argument
 *     parsing succeeds, before the verb handler runs. Useful for
 *     setting up shared resources (config files, opening sessions).
 */
typedef void (*AxlPreRunFunc)(AxlArgs *args);

/**
 * @brief A single verb in a multi-verb tool.
 */
typedef struct {
    const char        *name;         ///< verb name (e.g. "show", "list")
    const char        *help;         ///< one-line description
    const AxlArgDesc  *flags;        ///< per-verb flags, NULL-terminated; may be NULL
    const AxlArgDesc  *positionals;  ///< positionals, NULL-terminated; may be NULL
    AxlVerbHandler     handler;      ///< must be non-NULL
} AxlVerb;

/**
 * @brief Top-level application descriptor.
 *
 * Two shapes:
 *   - Multi-verb: set @c verbs (NULL-terminated). The first
 *     positional is the verb name; remaining positionals are
 *     consumed by the verb's @c positionals. Use @c handler == NULL.
 *   - Single-verb: set @c handler and (optionally) @c positionals.
 *     Leave @c verbs == NULL.
 */
typedef struct {
    const char        *name;          ///< program name (used in usage line)
    const char        *help;          ///< one-line synopsis
    const char        *usage;         ///< optional usage suffix (after verb summary)
    const AxlArgDesc  *global_flags;  ///< flags accepted in both modes
    const AxlArgDesc  *positionals;   ///< single-verb mode only
    const AxlVerb     *verbs;         ///< multi-verb mode (NULL-terminated)
    AxlVerbHandler     handler;       ///< single-verb mode
    AxlPreRunFunc      pre_run;       ///< optional pre-handler hook
    void              *user_data;     ///< available via axl_args_user_data
} AxlArgsApp;

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/**
 * @brief Parse @p argv against @p app and dispatch the matching
 *     verb (or single handler).
 *
 * Behaviour:
 *  - `--help` / `-h` at any position prints auto-generated help to
 *    stdout and returns 0.
 *  - Unknown flags / unknown verbs / missing required positionals /
 *    out-of-range typed args print an error to stderr followed by
 *    the usage line and return 1.
 *  - Successful dispatch returns whatever the handler returned.
 *
 * @return handler return value, 0 on `--help`, 1 on parse error.
 */
int
axl_args_run(
    int                   argc,
    char                **argv,
    const AxlArgsApp     *app
);

// ---------------------------------------------------------------------------
// Accessors (called from inside a handler)
// ---------------------------------------------------------------------------

/**
 * @brief Get a string-valued flag or positional by name.
 *
 * Returns the parsed value, the descriptor's @c default_value if the
 * arg was unset, or NULL if no such name exists.
 */
const char *
axl_args_get_string(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get a boolean flag value by name. Defaults to false when
 *     unset and no @c default_value is configured.
 */
bool
axl_args_get_bool(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get an unsigned-integer flag or positional by name.
 *
 * Returns the parsed value (already validated against @c min / @c max
 * by the framework), or 0 if unset / unknown name. For
 * `AXL_ARG_U8`/`U16`/`U32` the value is range-checked but returned
 * via the wider type for caller convenience.
 */
uint64_t
axl_args_get_uint(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get a signed-integer flag or positional by name. (Only
 *     `AXL_ARG_S64` is supported.) Returns 0 if unset / unknown.
 */
int64_t
axl_args_get_int(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Number of variadic positional arguments collected (only
 *     meaningful when a positional descriptor used `AXL_ARG_MULTI`
 *     as its tail entry).
 */
int
axl_args_get_pos_count(
    AxlArgs *args
);

/**
 * @brief Get a variadic positional argument by index. NULL if out
 *     of range. Use after the named positionals have been consumed
 *     (the variadic tail starts at index 0 of this view).
 */
const char *
axl_args_get_pos(
    AxlArgs *args,
    int      index
);

/**
 * @brief Number of times a `AXL_ARG_MULTI` flag was specified.
 */
int
axl_args_get_multi_count(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get the n-th value of a repeatable (`AXL_ARG_MULTI`) flag,
 *     or NULL if @p index >= count or the flag was never specified.
 */
const char *
axl_args_get_multi(
    AxlArgs    *args,
    const char *name,
    int         index
);

/**
 * @brief Return the @c user_data pointer that was set on the
 *     @ref AxlArgsApp. Available to handlers without globals.
 */
void *
axl_args_user_data(
    AxlArgs *args
);

/**
 * @brief Return the program name (== @c app->name).
 */
const char *
axl_args_program_name(
    AxlArgs *args
);

/**
 * @brief Print the auto-generated help to stdout. Useful when a
 *     handler wants to surface help on bad input it detects itself.
 */
void
axl_args_print_help(
    AxlArgs *args
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ARGS_H */
