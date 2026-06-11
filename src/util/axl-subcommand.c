/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-subcommand.c
    Subcommand-style CLI dispatch for multi-command UEFI apps.

    DEPRECATED: see <axl/axl-subcommand.h> — the implementation is
    retained as a thin shim while existing consumers migrate to
    AxlArgs (`<axl/axl-args.h>`). The deprecation attribute on the
    public declarations would otherwise fire warnings against the
    out-of-line definitions in this file; silence them here only —
    callers in other translation units still get the warning.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-stream.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>

/* Silence the deprecation across the whole TU — the internal cross-
   calls between axl_subcommand_dispatch / _print_help /
   _print_command_help would otherwise fire warnings on every
   build. Callers in OTHER translation units still get the warning. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <axl/axl-subcommand.h>

AXL_LOG_DOMAIN("subcommand");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Strip directory components from a path so error messages and the help
/// banner show e.g. "tool" instead of "fs0:\\tool.efi". Caller-friendly
/// when the app is invoked via the UEFI shell with a fully-qualified path.
static const char *
basename_of(
    const char *path
    )
{
    if (path == NULL) { return ""; }
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') { base = p + 1; }
    }
    return base;
}

/// Print straight to the UEFI console. We deliberately don't go through
/// axl-log here — help output is user-facing CLI text, not a log event.
static void
print_str(
    const char *s
    )
{
    if (s == NULL) { return; }
    axl_printf("%s", s);
}

/// Distance(a, b) capped at @a max — a cheap stop-early Levenshtein. We
/// only want enough resolution to spot a typo (1-2 edits); deep diffs
/// are not interesting for "did you mean".
static size_t
edit_distance(
    const char *a,
    const char *b,
    size_t      max
    )
{
    size_t la = axl_strlen(a);
    size_t lb = axl_strlen(b);

    /* Trivial cases. */
    if (la == 0) { return (lb < max) ? lb : max; }
    if (lb == 0) { return (la < max) ? la : max; }

    /* Length-difference lower bound — saves a full pass when the
       strings are obviously too far apart. */
    size_t diff = (la > lb) ? la - lb : lb - la;
    if (diff >= max) { return max; }

    /* Two-row DP. la and lb are short (subcommand names, < 32 chars in
       practice) so a stack-allocated buffer is fine. */
    size_t prev[64];
    size_t curr[64];
    if (lb >= sizeof(prev) / sizeof(prev[0])) {
        return max;   /* over-long: bail rather than truncate */
    }

    for (size_t j = 0; j <= lb; j++) { prev[j] = j; }

    for (size_t i = 1; i <= la; i++) {
        curr[0] = i;
        size_t row_min = curr[0];
        for (size_t j = 1; j <= lb; j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            size_t del  = prev[j] + 1;
            size_t ins  = curr[j - 1] + 1;
            size_t sub  = prev[j - 1] + cost;
            size_t m    = del;
            if (ins < m) { m = ins; }
            if (sub < m) { m = sub; }
            curr[j] = m;
            if (m < row_min) { row_min = m; }
        }
        /* Early exit: this row already exceeds max. */
        if (row_min >= max) { return max; }
        for (size_t j = 0; j <= lb; j++) { prev[j] = curr[j]; }
    }
    return prev[lb];
}

/// Find the longest entry name in the table — used to align the help
/// columns. Capped at 24 chars; anything longer triggers a fallback to
/// inline (newline-separated) layout.
static size_t
longest_name(
    const AxlSubcommand *table,
    size_t               count
    )
{
    size_t max_len = 0;
    for (size_t i = 0; i < count; i++) {
        if (table[i].name == NULL) { continue; }
        size_t n = axl_strlen(table[i].name);
        if (n > max_len) { max_len = n; }
    }
    if (max_len > 24) { max_len = 24; }
    return max_len;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_subcommand_print_help(
    const AxlSubcommand  *table,
    size_t                count,
    const char           *prog_name
    )
{
    const char *prog = (prog_name != NULL) ? prog_name : "<app>";
    axl_printf("Usage: %s <command> [args...]\n\n", prog);
    if (count == 0 || table == NULL) {
        axl_printf("(no subcommands registered)\n");
        return;
    }
    axl_printf("Commands:\n");
    size_t pad = longest_name(table, count);
    for (size_t i = 0; i < count; i++) {
        const AxlSubcommand *e = &table[i];
        if (e->name == NULL) { continue; }
        const char *summary = (e->summary != NULL) ? e->summary : "";
        size_t name_len = axl_strlen(e->name);
        axl_printf("  %s", e->name);
        /* Pad to align the summary column. */
        for (size_t s = name_len; s < pad; s++) {
            axl_printf(" ");
        }
        axl_printf("  %s\n", summary);
    }
    axl_printf("\nRun '%s help <command>' for detailed usage.\n", prog);
}

void
axl_subcommand_print_command_help(
    const AxlSubcommand  *entry,
    const char           *prog_name
    )
{
    const char *prog = (prog_name != NULL) ? prog_name : "<app>";
    if (entry == NULL || entry->name == NULL) {
        axl_printf("Usage: %s <command> [args...]\n", prog);
        return;
    }
    axl_printf("%s %s", prog, entry->name);
    if (entry->summary != NULL && entry->summary[0] != '\0') {
        axl_printf(" - %s", entry->summary);
    }
    axl_printf("\n\n");
    if (entry->usage != NULL && entry->usage[0] != '\0') {
        print_str(entry->usage);
        size_t n = axl_strlen(entry->usage);
        if (n > 0 && entry->usage[n - 1] != '\n') { axl_printf("\n"); }
    } else if (entry->summary != NULL) {
        axl_printf("(no detailed usage; see '%s help' for the full command list)\n", prog);
    }
}

/// Find an entry by exact name. NULL on miss.
static const AxlSubcommand *
find_exact(
    const AxlSubcommand *table,
    size_t               count,
    const char          *name
    )
{
    if (table == NULL || name == NULL) { return NULL; }
    for (size_t i = 0; i < count; i++) {
        if (table[i].name == NULL) { continue; }
        if (axl_strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/// Find the closest entry by edit distance, with a max threshold of 2.
/// Returns NULL if nothing close enough.
static const AxlSubcommand *
find_close(
    const AxlSubcommand *table,
    size_t               count,
    const char          *name
    )
{
    if (table == NULL || name == NULL || name[0] == '\0') { return NULL; }
    const AxlSubcommand *best = NULL;
    size_t best_dist = 3;   /* threshold: must be < 3 to win */
    for (size_t i = 0; i < count; i++) {
        if (table[i].name == NULL) { continue; }
        size_t d = edit_distance(name, table[i].name, best_dist);
        if (d < best_dist) {
            best_dist = d;
            best = &table[i];
        }
    }
    return best;
}

int
axl_subcommand_dispatch(
    const AxlSubcommand  *table,
    size_t                count,
    int                   argc,
    char                **argv,
    const char           *prog_name
    )
{
    /* Resolve the display name for help / errors. */
    const char *prog = prog_name;
    if (prog == NULL) {
        prog = (argc > 0 && argv != NULL && argv[0] != NULL)
               ? basename_of(argv[0]) : "<app>";
    }

    /* Empty / help / -h / --help → print help table. The argv ==
     * NULL guard handles the (technically valid, occasionally
     * exercised by harnesses) case where main is invoked with
     * argv = NULL — argc < 2 is true on a NULL argv too, but
     * being explicit short-circuits clang-tidy's null-deref
     * analyzer cleanly. */
    if (argc < 2 || argv == NULL || argv[1] == NULL) {
        axl_subcommand_print_help(table, count, prog);
        return 0;
    }
    const char *cmd = argv[1];
    bool is_help =    axl_strcmp(cmd, "help") == 0
                   || axl_strcmp(cmd, "-h")   == 0
                   || axl_strcmp(cmd, "--help") == 0;
    if (is_help) {
        if (argc >= 3 && argv[2] != NULL) {
            const AxlSubcommand *e = find_exact(table, count, argv[2]);
            if (e == NULL) {
                axl_fprintf(axl_stderr, "%s: unknown command '%s'\n", prog, argv[2]);
                return -1;
            }
            axl_subcommand_print_command_help(e, prog);
        } else {
            axl_subcommand_print_help(table, count, prog);
        }
        return 0;
    }

    /* Lookup. */
    const AxlSubcommand *e = find_exact(table, count, cmd);
    if (e == NULL) {
        const AxlSubcommand *near = find_close(table, count, cmd);
        if (near != NULL) {
            axl_fprintf(axl_stderr, "%s: unknown command '%s'  (did you mean '%s'?)\n",
                        prog, cmd, near->name);
        } else {
            axl_fprintf(axl_stderr, "%s: unknown command '%s'\n", prog, cmd);
            axl_fprintf(axl_stderr, "Run '%s help' for the command list.\n", prog);
        }
        return -1;
    }

    /* Invoke with shifted argv so the subcommand sees its own name as
       argv[0]. argc-1 is at least 1 (the cmd itself). */
    if (e->fn == NULL) {
        axl_fprintf(axl_stderr, "%s: command '%s' has no implementation (NULL fn)\n",
                    prog, cmd);
        return -1;
    }
    return e->fn(argc - 1, argv + 1);
}

#pragma GCC diagnostic pop
