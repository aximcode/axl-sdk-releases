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

static const AxlArgDesc kFlags[] = {
    { .name = "name",    .type = AXL_ARG_STRING,
      .help = "Glob pattern (* and ? wildcards)" },
    { .name = "type",    .type = AXL_ARG_STRING,
      .help = "Filter: f=files, d=directories" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Show size and date" },
    {0}
};

static const AxlArgDesc kPositional[] = {
    { .name = "path", .type = AXL_ARG_STRING,
      .help = "Starting path (default: '.')" },
    {0}
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void
print_entry(
    const char    *path,
    AxlDirEntry   *entry
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

static size_t
find_walk(
    const char *base_path,
    const char *name_pattern,
    char        type_filter,
    size_t      depth
    )
{
    size_t count = 0;

    if (depth >= MAX_WALK_DEPTH) {
        if (verbose) {
            axl_printf("Find: max directory depth reached at '%s'\n",
                       base_path);
        }
        return 0;
    }

    /* If base_path is a file (not a directory), check if it matches */
    if (!axl_file_is_dir(base_path)) {
        if (type_filter == '\0' || type_filter == 'f') {
            const char *name = base_path;
            for (const char *p = base_path; *p != '\0'; p++) {
                if (*p == '/' || *p == '\\') {
                    name = p + 1;
                }
            }
            if (name_pattern == NULL || axl_fnmatch(name_pattern, name)) {
                AxlDirEntry de;
                axl_memset(&de, 0, sizeof(de));
                de.is_dir = false;
                axl_strlcpy(de.name, name, sizeof(de.name));
                AxlFileInfo fi;
                if (axl_file_info(base_path, &fi) == 0) {
                    de.size = fi.size;
                }
                print_entry(base_path, &de);
                count++;
            }
        }
        return count;
    }

    /* Open directory */
    AxlDir *dir = axl_dir_open(base_path);
    if (dir == NULL) {
        axl_printf("Find: cannot open '%s'\n", base_path);
        return 0;
    }

    AxlDirEntry entry;
    while (axl_dir_read(dir, &entry)) {
        /* Skip . and .. */
        if (axl_strcmp(entry.name, ".") == 0 ||
            axl_strcmp(entry.name, "..") == 0) {
            continue;
        }

        /* Build full path */
        char full_path[512];
        size_t base_len = axl_strlen(base_path);
        if (base_len > 0 && (base_path[base_len - 1] == '/' ||
                             base_path[base_len - 1] == '\\')) {
            axl_snprintf(full_path, sizeof(full_path), "%s%s",
                         base_path, entry.name);
        } else {
            axl_snprintf(full_path, sizeof(full_path), "%s/%s",
                         base_path, entry.name);
        }

        /* Check type filter */
        bool type_ok = (type_filter == '\0') ||
                       (type_filter == 'f' && !entry.is_dir) ||
                       (type_filter == 'd' && entry.is_dir);

        /* Check name pattern */
        bool name_ok = (name_pattern == NULL) ||
                       axl_fnmatch(name_pattern, entry.name);

        if (type_ok && name_ok) {
            print_entry(full_path, &entry);
            count++;
        }

        /* Recurse into subdirectories */
        if (entry.is_dir) {
            count += find_walk(full_path, name_pattern, type_filter,
                               depth + 1);
        }
    }

    axl_dir_close(dir);
    return count;
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

    size_t match_count = find_walk(start_path, name_pattern,
                                   type_filter, 0);

    if (verbose) {
        axl_printf("\n%zu match(es) found.\n", match_count);
    }

    return (match_count > 0) ? 0 : 1;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsApp){
        .name         = "Find",
        .help         = "Find files and directories (UNIX find-style)",
        .global_flags = kFlags,
        .positionals  = kPositional,
        .handler      = run_find,
    });
}
