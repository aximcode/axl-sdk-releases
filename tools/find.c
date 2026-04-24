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

static const AxlConfigDesc descs[] = {
    { "name",    AXL_CFG_STRING, NULL,    0,   "Glob pattern (* and ? wildcards)", 0, 0 },
    { "type",    AXL_CFG_STRING, NULL,    0,   "Filter: f=files, d=directories",   0, 0 },
    { "verbose", AXL_CFG_BOOL,   "false", 'v', "Show size and date",               0, 0 },
    { "help",    AXL_CFG_BOOL,   "false", 'h', "Show this help",                   0, 0 },
    { 0 }
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

int
main(
    int    argc,
    char **argv
    )
{
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL || axl_config_parse_args(cfg, argc, argv) != 0) {
        axl_printf("Find: invalid option\n");
        axl_config_usage(cfg, "Find",
                         "[--name pattern] [--type f|d] [-v] [path]");
        return 1;
    }

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "Find",
                         "[--name pattern] [--type f|d] [-v] [path]");
        return 0;
    }

    verbose = axl_config_get_bool(cfg, "verbose");

    const char *name_pattern = axl_config_get(cfg, "name");

    char type_filter = '\0';
    const char *type_str = axl_config_get(cfg, "type");
    if (type_str != NULL) {
        type_filter = type_str[0];
        if (type_filter != 'f' && type_filter != 'd') {
            axl_printf("Find: --type must be 'f' or 'd'\n");
            return 1;
        }
    }

    const char *start_path = axl_config_pos(cfg, 0);
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
