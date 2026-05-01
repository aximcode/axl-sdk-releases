/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-subcommand.h:
 *
 * @deprecated Use @ref axl_args_run from `<axl/axl-args.h>` instead.
 *     AxlArgs's multi-verb mode (`AxlArgsApp.verbs[]`) strictly subsumes
 *     this dispatcher and adds typed positional args, bounds checking,
 *     and auto-generated `--help`. AxlSubcommand is retained as a thin
 *     wrapper for source compatibility while existing consumers (notably
 *     delldiags `do.efi`) migrate. New tools should not use this header.
 *
 * Subcommand-style CLI dispatch for multi-command UEFI apps. Pairs with
 * axl-config — `mkrd` was the canonical "single-purpose tool" shape and
 * `do.efi` the "multi-command" shape; both have been migrated.
 *
 * Migrating to AxlArgs in one diff:
 *
 *     // before:
 *     static int do_bios(int argc, char **argv) { ... }
 *     static const AxlSubcommand kCommands[] = {
 *         { "bios", do_bios, "[test|pci|irq|slot|emb]",
 *           "do bios test  — ..." },
 *     };
 *     int main(int argc, char **argv) {
 *         return axl_subcommand_dispatch(kCommands, ARRAY_LEN(kCommands),
 *             argc, argv, "do");
 *     }
 *
 *     // after:
 *     static const AxlArgDesc kBiosArgs[] = {
 *         { .name = "args", .type = AXL_ARG_MULTI,
 *           .help = "subcommand arguments" }, {0}
 *     };
 *     static int do_bios(AxlArgs *a) {
 *         int n = axl_args_get_pos_count(a);
 *         const char *sub = (n > 0) ? axl_args_get_pos(a, 0) : NULL;
 *         ...
 *     }
 *     static const AxlVerb kVerbs[] = {
 *         { .name = "bios", .handler = do_bios, .positionals = kBiosArgs,
 *           .help = "BIOS / SMBIOS info (test|pci|irq|slot|emb)" }, {0}
 *     };
 *     int main(int argc, char **argv) {
 *         return axl_args_run(argc, argv, &(AxlArgsApp){
 *             .name = "do", .verbs = kVerbs,
 *         });
 *     }
 */

#ifndef AXL_SUBCOMMAND_H
#define AXL_SUBCOMMAND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Subcommand implementation function signature.
///
/// Receives (argc, argv) where argv[0] is the subcommand name (the
/// dispatcher rewrites it from the parent's argv so subcommand
/// implementations don't need to know about the parent's name). Use
/// AxlConfig or hand-written parsing on the remaining args.
typedef int (*AxlSubcommandFn)(int argc, char **argv);

/// One entry in a subcommand table. Define a static array of these
/// (or allocate dynamically — the dispatcher doesn't care) and pass it
/// to axl_subcommand_dispatch.
typedef struct {
    const char         *name;     ///< e.g. "bios", "sysid", "crb"
    AxlSubcommandFn     fn;       ///< Implementation function
    const char         *summary;  ///< One-line, shown in `<prog> help`
    const char         *usage;    ///< Multiline, shown in `<prog> help <cmd>` (NULL = use `summary`)
} AxlSubcommand;

/**
 * @brief Dispatch argv[1] to the matching subcommand, or print help.
 *
 * Behavior:
 *  - argc < 2, or argv[1] is "help" / "-h" / "--help" with no further
 *    args → prints the formatted help table and returns 0.
 *  - argv[1] is "help <cmd>" → prints @a cmd's `usage` field (or `summary`
 *    if `usage` is NULL) and returns 0.
 *  - argv[1] matches a table entry → invokes its `fn` with
 *    (argc - 1, argv + 1) so the subcommand sees its own name as
 *    argv[0]. Returns the function's return value.
 *  - argv[1] doesn't match anything → prints "<prog>: unknown command
 *    'foo'" plus a "did you mean 'bar'?" suggestion if a close match
 *    exists, and returns -1.
 *
 * The table is 100% caller-owned. No allocation; no internal state.
 *
 * @param table       array of AxlSubcommand
 * @param count       number of entries in @a table
 * @param argc        forwarded from main()
 * @param argv        forwarded from main()
 * @param prog_name   used in help / error output. NULL → use the basename
 *                    of argv[0].
 * @return whatever the subcommand returned, 0 for help/empty, or -1 if
 *         the command wasn't found.
 */
int
axl_subcommand_dispatch(
    const AxlSubcommand  *table,
    size_t                count,
    int                   argc,
    char                **argv,
    const char           *prog_name
) __attribute__((deprecated("use axl_args_run from <axl/axl-args.h>")));

/**
 * @brief Print only the formatted help table.
 *
 * Useful when the caller wants to show help in response to an invalid
 * argument outside the dispatch path. Output format:
 *
 *   Usage: <prog> <command> [args...]
 *
 *   Commands:
 *     bios   [test|pci|irq|slot|emb]
 *     sysid  [hexValue]
 *
 *   Run '<prog> help <command>' for detailed usage.
 */
void
axl_subcommand_print_help(
    const AxlSubcommand  *table,
    size_t                count,
    const char           *prog_name
) __attribute__((deprecated("use axl_args_run from <axl/axl-args.h>")));

/**
 * @brief Print a single subcommand's detailed usage.
 *
 * Used by `<prog> help <cmd>`. Prints @a entry->usage, falling back to
 * @a entry->summary if @a entry->usage is NULL. Pass NULL @a entry to
 * print the global help (same as axl_subcommand_print_help).
 */
void
axl_subcommand_print_command_help(
    const AxlSubcommand  *entry,
    const char           *prog_name
) __attribute__((deprecated("use axl_args_run from <axl/axl-args.h>")));

#ifdef __cplusplus
}
#endif

#endif /* AXL_SUBCOMMAND_H */
