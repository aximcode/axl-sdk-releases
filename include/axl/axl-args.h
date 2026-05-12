/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-args.h:
 *
 * Declarative command-line parser for AXL tools. A tool declares a
 * static AxlArgsNode tree (program name + flags + positionals +
 * handler, OR a list of child verbs) and calls axl_args_run from
 * main. The framework parses argv, validates types and bounds,
 * generates a structured `--help`, and dispatches to the matching
 * leaf handler.
 *
 * The same `AxlArgsNode` type describes the root program AND every
 * verb at every level. A node is either a **leaf** (sets `handler`,
 * optionally `positionals`) or a **branch** (sets `verbs`, an array
 * of child nodes terminated by a zero entry). Mutually exclusive,
 * validated at parse time.
 *
 * **Single-leaf tool** (one shape, no verbs):
 *
 * @code
 * static int do_run(AxlArgs *a) {
 *     const char *path = axl_args_get_string(a, "path");
 *     return process(path);
 * }
 *
 * static const AxlArgDesc positionals[] = {
 *     { .name = "path", .type = AXL_ARG_STRING, .required = true,
 *       .help = "Input file" },
 *     {0}
 * };
 *
 * int main(int argc, char **argv) {
 *     return axl_args_run(argc, argv, &(AxlArgsNode){
 *         .name = "mytool", .help = "Process a file",
 *         .positionals = positionals, .handler = do_run,
 *     });
 * }
 * @endcode
 *
 * **Multi-verb tool** (one level of verbs):
 *
 * @code
 * static const AxlArgsNode verbs[] = {
 *     { .name = "show", .handler = do_show, .positionals = slot_arg,
 *       .help = "Decoded fields for one slot" },
 *     { .name = "list", .handler = do_list,
 *       .help = "List populated slots" },
 *     {0}
 * };
 *
 * int main(int argc, char **argv) {
 *     return axl_args_run(argc, argv, &(AxlArgsNode){
 *         .name = "memspd", .help = "Read JEDEC SPD content",
 *         .flags = flags, .verbs = verbs,
 *     });
 * }
 * @endcode
 *
 * **Nested verbs** (`do <category> <verb>` shape):
 *
 * @code
 * static const AxlArgsNode bios_verbs[] = {
 *     { .name = "test", .handler = bios_test, .help = "..." },
 *     { .name = "pci",  .handler = bios_pci,  .help = "..." },
 *     {0}
 * };
 *
 * static const AxlArgsNode top_verbs[] = {
 *     { .name = "bios", .verbs = bios_verbs,
 *       .help = "BIOS / SMBIOS subcommands" },
 *     { .name = "load", .handler = do_load, .positionals = load_args,
 *       .help = "Load and run a UEFI image" },
 *     {0}
 * };
 *
 * int main(int argc, char **argv) {
 *     return axl_args_run(argc, argv, &(AxlArgsNode){
 *         .name = "do", .help = "Hardware diagnostic CLI",
 *         .verbs = top_verbs,
 *     });
 * }
 * @endcode
 *
 * **Branch with a default handler** — the `do bios` → "print
 * summary" pattern. Same node sets BOTH `verbs` and `handler`;
 * the handler runs only when no sub-verb is supplied.
 *
 * @code
 * static const AxlArgsNode bios_verbs[] = {
 *     { .name = "info", .handler = bios_info, .help = "Type 0 summary" },
 *     { .name = "test", .handler = bios_test, .help = "Walk every record" },
 *     {0}
 * };
 *
 * static const AxlArgsNode bios_node = {
 *     .name    = "bios",
 *     .help    = "BIOS / SMBIOS subcommands",
 *     .verbs   = bios_verbs,
 *     .handler = bios_info,    // fires when 'do bios' has no sub-verb
 * };
 * @endcode
 *
 * `--help` and `-h` are always recognised and recurse naturally —
 * `do --help` shows the top tree, `do bios --help` shows just the
 * bios subtree. Unknown flags / verbs / out-of-range typed args
 * produce an error message qualified by the full breadcrumb
 * (`do bios: unknown verb 'flarble'`) and the auto-generated usage,
 * exit non-zero, no handler invocation.
 *
 * **Flag and `user_data` visibility across levels.** Flags declared
 * on a parent node are visible to descendant handlers via the same
 * accessors (`axl_args_get_*` walks up the parent chain on miss).
 * `user_data` works the same way — descendants inherit the nearest
 * non-NULL parent value. Per-leaf positionals are leaf-local.
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
    AXL_ARG_CHOICE,  ///< String value restricted to a caller-supplied set
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
 *    axl_str_to_u64 family. Default 0 = auto.
 *  - @c min, @c max: inclusive bounds. Each independently 0 = no
 *    bound (so a U8 with `.min = 0, .max = 0xFF` is the same as
 *    "no bounds at all").
 *  - For `AXL_ARG_S64` the bounds are cast as `int64_t`. To express
 *    a negative lower bound, set `.min = (uint64_t)(int64_t)-N` —
 *    two's complement round-trips through the cast.
 *
 * For positionals: @c short_name and @c default_value are ignored.
 * Position in the array determines argument order. The LAST entry
 * may have @c type == @c AXL_ARG_MULTI to mean "collect all
 * remaining positionals" (variadic tail).
 */
typedef struct {
    const char  *name;          ///< long name; positional arg name; lookup key
    char         short_name;    ///< short flag (single char), 0 if none
    AxlArgType   type;
    const char  *default_value; ///< default for flags when unset, NULL = unset
    const char  *help;          ///< one-line description (shown in --help)
    bool         required;      ///< (positionals only) error if missing
    int          base;          ///< (numeric types) 0 (auto) | 10 | 16
    uint64_t     min;           ///< (numeric types) inclusive min, 0 = none
    uint64_t     max;           ///< (numeric types) inclusive max, 0 = none
    /// (`AXL_ARG_CHOICE` only) NULL-terminated array of allowed string
    /// values; the framework rejects any input not present here with
    /// the same breadcrumb-prefixed error it uses for out-of-range
    /// numerics, and lists the choices in `--help`. Comparison is
    /// case-sensitive by default; set choices_case_insensitive
    /// to relax. NULL or empty array makes a CHOICE behave like an
    /// unconstrained AXL_ARG_STRING.
    const char *const *choices;
    /// (`AXL_ARG_CHOICE` only) when true, match against choices
    /// using ASCII case-folded comparison (`axl_strcasecmp`). Folds
    /// letters only — non-ASCII bytes compare as bytes, so localized
    /// or UTF-8 multi-byte choice strings still effectively match
    /// case-sensitively. The returned string reflects the user's
    /// original casing; only the validation step is case-insensitive.
    /// Default false preserves the byte-equal contract earlier
    /// consumers rely on.
    bool               choices_case_insensitive;
} AxlArgDesc;

// ---------------------------------------------------------------------------
// Node tree
// ---------------------------------------------------------------------------

typedef struct AxlArgs AxlArgs;
typedef struct AxlArgsNode AxlArgsNode;

/**
 * @brief Verb handler signature. Return value becomes the program's
 *     exit code.
 *
 * The @p args object exposes parsed flags and positional values via
 * the @c axl_args_get_* accessors; flags declared on parent nodes
 * are visible (the accessors walk up the parent chain on miss).
 *
 * **Lifetime contract** — important when the handler enters an event
 * loop (axl_loop_run) or otherwise blocks before returning:
 *  - The @c AxlArgs struct itself, all `axl_args_get_*` accessor
 *    return values, and the variadic-positional view all live until
 *    the handler returns to axl_args_run (every level's
 *    `AxlArgs` lives on its own stack frame, so a deep handler still
 *    sees its parent's flags through the recursion).
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
 * @brief Optional pre-handler hook. Called once at this node's level
 *     after argument parsing succeeds, before recursing into a child
 *     verb (branch) or invoking the leaf handler. Useful for setting
 *     up shared resources whose lifetime spans every descendant
 *     (config files, opening sessions). When stacked across nested
 *     levels, parent's @c pre_run runs before child's.
 */
typedef void (*AxlPreRunFunc)(AxlArgs *args);

/**
 * @brief A single node in the args tree. Same type at every level —
 *     the root program, an inner branch ("category"), and a leaf
 *     verb all use it.
 *
 * A node is one of three shapes:
 *   - **Leaf**: `handler` set, `verbs` NULL. Positionals and
 *     per-leaf flags consumed at this level; handler invoked once
 *     parsing completes.
 *   - **Branch**: `verbs` set (NULL-terminated array of child
 *     `AxlArgsNode`s), `handler` NULL. Positionals MUST be NULL on
 *     a branch — the first non-flag argument is the verb name. If
 *     no verb is supplied, the framework prints `--help`.
 *   - **Branch + default handler**: `verbs` AND `handler` both set.
 *     Same dispatch as Branch when the user supplies a verb;
 *     when no verb is supplied (just branch-level flags or no
 *     args at all), `handler` runs as the no-verb default. Useful
 *     for the `do bios` → "print summary" pattern (`bios` has
 *     subverbs but defaults to a summary action). Positionals
 *     still MUST be NULL — the handler runs with parsed flags
 *     only. An unknown verb still errors; the handler is not a
 *     catch-all.
 *
 * A node with neither `handler` nor `verbs` is a configuration
 * error and the parser exits non-zero before invoking anything.
 *
 * Designated initializers expected; zero defaults are sane.
 */
struct AxlArgsNode {
    const char           *name;          ///< program name at root, verb name at depth
    const char           *help;          ///< one-line description (shown in --help)
    const AxlArgDesc     *flags;         ///< per-node flags, NULL-terminated; may be NULL
    const AxlArgDesc     *positionals;   ///< leaf only (positional args, NULL-terminated)
    const AxlArgsNode    *verbs;         ///< branch only (child nodes, zero-entry-terminated)
    AxlVerbHandler        handler;       ///< leaf only
    AxlPreRunFunc         pre_run;       ///< optional, runs at this level top-down
    void                 *user_data;     ///< per-node; descendants inherit nearest non-NULL

    /// Optional free-form text printed in `--help` BEFORE the
    /// auto-generated `Usage:` line (and the per-section blocks
    /// that follow it). Use for a multi-paragraph description,
    /// usage examples, environment-variable list, or anything
    /// that doesn't fit on the single-line help summary.
    /// Per-node: each level's `--help` uses its own prolog;
    /// `do --help` shows the root node's, `do bios --help` shows
    /// the bios node's. NULL = nothing printed (default).
    ///
    /// Whitespace policy: printed verbatim, then the framework
    /// adds a single trailing blank line as a separator before
    /// `Usage:`. Don't include leading or trailing newlines
    /// yourself — the framework handles separation.
    const char           *help_prolog;

    /// Optional free-form text printed in `--help` AFTER all
    /// auto-generated sections (Usage, Verbs / Flags,
    /// Arguments). Standard place for "see also" pointers,
    /// "report bugs to ..." footers, links to external docs,
    /// version stamps, etc. Per-node like help_prolog;
    /// NULL = nothing printed.
    ///
    /// Whitespace policy: same as help_prolog — the
    /// framework prepends a single blank-line separator and
    /// appends a single trailing newline. Consumer should not
    /// include leading or trailing newlines.
    const char           *help_epilog;
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/**
 * @brief Parse @p argv against @p root and dispatch to the matching
 *     leaf handler.
 *
 * Behaviour:
 *  - `--help` / `-h` at any level prints the auto-generated help for
 *    that level to stdout and returns 0.
 *  - Unknown flags / unknown verbs / missing required positionals /
 *    out-of-range typed args print a breadcrumb-prefixed error to
 *    stderr followed by the usage line and return 1.
 *  - Successful dispatch returns whatever the leaf handler returned.
 *
 * Return value is **POSIX exit-code shaped**, not AxlStatus —
 * tools doing `return axl_args_run(...)` from main propagate it as
 * the process exit code, so 0/1/2 follow shell convention rather
 * than the negative-value AxlStatus scheme. Handlers should likewise
 * return process-exit-shaped ints (0 success, non-zero failure).
 *
 * @return handler return value, 0 on `--help`, 1 on parse error.
 */
int
axl_args_run(
    int                    argc,
    char                 **argv,
    const AxlArgsNode     *root
);

// ---------------------------------------------------------------------------
// Accessors (called from inside a handler)
// ---------------------------------------------------------------------------

/**
 * @brief Get a string-valued flag or positional by name.
 *
 * Returns the parsed value, the descriptor's @c default_value if the
 * arg was unset, or NULL if no such name exists at this level OR
 * any ancestor level (parent walk).
 */
const char *
axl_args_get_string(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get a boolean flag value by name. Defaults to false when
 *     unset and no @c default_value is configured. Walks parents.
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
 * by the framework), or 0 if unset / unknown name. Walks parents.
 */
uint64_t
axl_args_get_uint(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get a signed-integer flag or positional by name. (Only
 *     `AXL_ARG_S64` is supported.) Returns 0 if unset / unknown.
 *     Walks parents.
 */
int64_t
axl_args_get_int(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Read a string-typed arg and parse it as `base[+offset]`.
 *
 * Convenience over `axl_args_get_string` + axl_strtou64_with_offset. Suited for tools that accept
 * register/memory addresses in a single token like `0x1000+0x50`
 * across many verbs — the descriptor is declared as
 * `AXL_ARG_STRING` and this accessor handles the parse so each
 * handler doesn't repeat it.
 *
 * Walks parents like the other accessors. The descriptor's
 * @c default_value (if set) is used when the arg was unset.
 *
 * Note: unlike axl_args_get_uint (which returns the value
 * directly with 0 on miss), this accessor returns
 * @c AXL_OK / @c AXL_ERR — there is no in-band sentinel for a
 * `uint64_t` parse failure, and 0 is a legitimate result.
 *
 * @return AXL_OK on success (with @a out_value populated),
 *     AXL_ERR if the arg is missing, unset with no default, or
 *     fails to parse.
 */
int
axl_args_get_uint_offset(
    AxlArgs    *args,
    const char *name,
    uint64_t   *out_value   ///< [out] base + offset
);

/**
 * @brief Number of variadic positional arguments collected at this
 *     level (only meaningful when the leaf's positional descriptor
 *     used `AXL_ARG_MULTI` as its tail entry).
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
 *     Walks parents (so a leaf can inspect a `--include` repeatable
 *     declared on the root).
 */
int
axl_args_get_multi_count(
    AxlArgs    *args,
    const char *name
);

/**
 * @brief Get the n-th value of a repeatable (`AXL_ARG_MULTI`) flag,
 *     or NULL if @p index >= count or the flag was never specified.
 *     Walks parents.
 */
const char *
axl_args_get_multi(
    AxlArgs    *args,
    const char *name,
    int         index
);

/**
 * @brief Return the nearest non-NULL @c user_data found by walking
 *     from this node up to the root. NULL if no node set one.
 */
void *
axl_args_user_data(
    AxlArgs *args
);

/**
 * @brief Return the program name (the root node's @c name).
 */
const char *
axl_args_program_name(
    AxlArgs *args
);

/**
 * @brief Print the auto-generated help for this node to stdout.
 *     Useful when a handler wants to surface help on bad input it
 *     detects itself.
 */
void
axl_args_print_help(
    AxlArgs *args
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ARGS_H */
