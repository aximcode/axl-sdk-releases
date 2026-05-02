/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file Find.c
    File and directory finder (UEFI find(1) equivalent).

    Build with axl-cc:
      axl-cc Find.c -o Find.efi

    Usage:
      Find.efi [--name pattern] [--type f|d] [-v] [-h] [path]
**/

#include <axl.h>

#define MAX_WALK_DEPTH  32

static bool verbose = false;

static const AxlArgDesc flags[] = {
    { .name = "name",    .type = AXL_ARG_STRING,
      .help = "Glob pattern (* and ? wildcards)" },
    { .name = "type",    .type = AXL_ARG_STRING,
      .help = "Filter: f=files, d=directories" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Show size and date" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "path", .type = AXL_ARG_STRING,
      .help = "Starting path (default: '.')" },
    {0}
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void
print_entry(
    const char        *path,
    const AxlDirEntry *entry
    )
{
    if (verbose) {
        if (entry->is_dir) {
            axl_printf("     <DIR> ");
        } else {
            axl_printf("%10llu ", (unsigned long long)entry->size);
        }
    }

    axl_printf("%s\n", path);
}

// ---------------------------------------------------------------------------
// Recursive directory walker
// ---------------------------------------------------------------------------

typedef struct {
    const char *name_pattern;
    char        type_filter;
    size_t      count;
} FindCtx;

static int
find_walk_cb(const char *full_path, const AxlDirEntry *entry, void *user)
{
    FindCtx *c = (FindCtx *)user;

    bool type_ok = (c->type_filter == '\0')
                || (c->type_filter == 'f' && !entry->is_dir)
                || (c->type_filter == 'd' && entry->is_dir);

    bool name_ok = (c->name_pattern == NULL)
                || axl_fnmatch(c->name_pattern, entry->name);

    if (type_ok && name_ok) {
        print_entry(full_path, entry);
        c->count++;
    }
    return 0;
}

static size_t
find_walk(
    const char *base_path,
    const char *name_pattern,
    char        type_filter
    )
{
    /* Single-file argument: match against the supplied path directly,
       with no directory descent. */
    if (!axl_file_is_dir(base_path)) {
        if (type_filter != '\0' && type_filter != 'f') return 0;
        const char *name = base_path;
        for (const char *p = base_path; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\') name = p + 1;
        }
        if (name_pattern != NULL && !axl_fnmatch(name_pattern, name)) {
            return 0;
        }
        AxlDirEntry de = { .is_dir = false };
        axl_strlcpy(de.name, name, sizeof(de.name));
        AxlFileInfo fi;
        if (axl_file_info(base_path, &fi) == 0) de.size = fi.size;
        print_entry(base_path, &de);
        return 1;
    }

    FindCtx ctx = {
        .name_pattern = name_pattern,
        .type_filter  = type_filter,
        .count        = 0,
    };
    if (axl_dir_walk(base_path, find_walk_cb, &ctx, MAX_WALK_DEPTH) != 0
        && verbose) {
        axl_printf("Find: walk of '%s' did not complete cleanly\n", base_path);
    }
    return ctx.count;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_find(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");

    const char *name_pattern = axl_args_get_string(a, "name");

    char type_filter = '\0';
    const char *type_str = axl_args_get_string(a, "type");
    if (type_str != NULL) {
        type_filter = type_str[0];
        if (type_filter != 'f' && type_filter != 'd') {
            axl_printf("Find: --type must be 'f' or 'd'\n");
            return 1;
        }
    }

    const char *start_path = axl_args_get_string(a, "path");
    if (start_path == NULL) {
        start_path = ".";
    }

    size_t match_count = find_walk(start_path, name_pattern, type_filter);

    if (verbose) {
        axl_printf("\n%zu match(es) found.\n", match_count);
    }

    return (match_count > 0) ? 0 : 1;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "Find",
        .help         = "Find files and directories (UNIX find-style)",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_find,
    });
}
