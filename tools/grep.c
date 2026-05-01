/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file Grep.c
    Text pattern search in files (UEFI grep(1) equivalent).

    Build with axl-cc:
      axl-cc Grep.c -o Grep.efi

    Usage:
      Grep.efi [-i] [-n] [-c] [-r] [-v] [-h] pattern file [file...]
**/

#include <axl.h>

#define GREP_MAX_FILE_SIZE  (16 * 1024 * 1024)
#define MAX_WALK_DEPTH      32

static bool case_insensitive = false;
static bool show_line_numbers = false;
static bool count_only = false;
static bool verbose_mode = false;

static const AxlArgDesc kFlags[] = {
    { .name = "ignore-case", .short_name = 'i', .type = AXL_ARG_BOOL,
      .help = "Case-insensitive match" },
    { .name = "line-number", .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Show line numbers" },
    { .name = "count",       .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Count matches only" },
    { .name = "recursive",   .short_name = 'r', .type = AXL_ARG_BOOL,
      .help = "Recursive directory search" },
    { .name = "verbose",     .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output" },
    {0}
};

static const AxlArgDesc kPositional[] = {
    { .name = "pattern", .type = AXL_ARG_STRING, .required = true,
      .help = "Search pattern" },
    { .name = "files",   .type = AXL_ARG_MULTI,  .required = true,
      .help = "One or more files (or directories with -r)" },
    {0}
};

// ---------------------------------------------------------------------------
// Binary detection
// ---------------------------------------------------------------------------

static bool
is_binary_data(
    const uint8_t *data,
    size_t         size
    )
{
    size_t check = (size > 512) ? 512 : size;
    for (size_t i = 0; i < check; i++) {
        if (data[i] == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Search a single file
// ---------------------------------------------------------------------------

static size_t
grep_file(
    const char *pattern,
    const char *path,
    bool        show_filename
    )
{
    AXL_AUTO_FREE void *data = NULL;
    size_t size = 0;

    if (axl_file_get_contents(path, &data, &size) != 0) {
        if (verbose_mode) {
            axl_printf("Grep: cannot read '%s'\n", path);
        }
        return 0;
    }

    if (size == 0) {
        return 0;
    }

    if (size > GREP_MAX_FILE_SIZE) {
        if (verbose_mode) {
            axl_printf("Grep: skipping '%s' (too large: %zu bytes)\n",
                       path, size);
        }
        return 0;
    }

    if (is_binary_data((const uint8_t *)data, size)) {
        if (verbose_mode) {
            axl_printf("Grep: skipping binary file '%s'\n", path);
        }
        return 0;
    }

    /* Search line by line */
    size_t line_num = 1;
    size_t count = 0;
    size_t line_start = 0;
    const char *buf = (const char *)data;

    for (size_t i = 0; i <= size; i++) {
        bool is_eol = (i == size) || (buf[i] == '\n');
        if (!is_eol) {
            continue;
        }

        /* Extract line [line_start..i), strip trailing \r */
        size_t line_end = i;
        if (line_end > line_start && buf[line_end - 1] == '\r') {
            line_end--;
        }
        size_t line_len = line_end - line_start;

        /* NUL-terminate the line in a local buffer */
        char line_buf[1024];
        if (line_len >= sizeof(line_buf)) {
            line_len = sizeof(line_buf) - 1;
        }
        axl_memcpy(line_buf, buf + line_start, line_len);
        line_buf[line_len] = '\0';

        /* Match */
        const char *found;
        if (case_insensitive) {
            found = axl_strcasestr(line_buf, pattern);
        } else {
            found = axl_strstr_len(line_buf, -1, pattern);
        }

        if (found != NULL) {
            count++;
            if (!count_only) {
                if (show_filename) {
                    axl_printf("%s:", path);
                }
                if (show_line_numbers) {
                    axl_printf("%zu:", line_num);
                }
                axl_printf("%s\n", line_buf);
            }
        }

        line_num++;
        line_start = i + 1;
    }

    if (count_only) {
        if (show_filename) {
            axl_printf("%s:%zu\n", path, count);
        } else {
            axl_printf("%zu\n", count);
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// Recursive directory walker
// ---------------------------------------------------------------------------

static size_t
grep_directory(
    const char *pattern,
    const char *dir_path,
    size_t      depth
    )
{
    size_t total = 0;

    if (depth >= MAX_WALK_DEPTH) {
        if (verbose_mode) {
            axl_printf("Grep: max depth reached at '%s'\n", dir_path);
        }
        return 0;
    }

    AxlDir *dir = axl_dir_open(dir_path);
    if (dir == NULL) {
        if (verbose_mode) {
            axl_printf("Grep: cannot open directory '%s'\n", dir_path);
        }
        return 0;
    }

    AxlDirEntry entry;
    while (axl_dir_read(dir, &entry)) {
        if (axl_strcmp(entry.name, ".") == 0 ||
            axl_strcmp(entry.name, "..") == 0) {
            continue;
        }

        char full_path[512];
        size_t len = axl_strlen(dir_path);
        if (len > 0 && (dir_path[len - 1] == '/' ||
                        dir_path[len - 1] == '\\')) {
            axl_snprintf(full_path, sizeof(full_path), "%s%s",
                         dir_path, entry.name);
        } else {
            axl_snprintf(full_path, sizeof(full_path), "%s/%s",
                         dir_path, entry.name);
        }

        if (entry.is_dir) {
            total += grep_directory(pattern, full_path, depth + 1);
        } else {
            total += grep_file(pattern, full_path, true);
        }
    }

    axl_dir_close(dir);
    return total;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_grep(AxlArgs *a)
{
    verbose_mode      = axl_args_get_bool(a, "verbose");
    case_insensitive  = axl_args_get_bool(a, "ignore-case");
    show_line_numbers = axl_args_get_bool(a, "line-number");
    count_only        = axl_args_get_bool(a, "count");
    bool recursive    = axl_args_get_bool(a, "recursive");

    const char *pattern    = axl_args_get_string(a, "pattern");
    int         file_count = axl_args_get_pos_count(a);
    bool multi_file = (file_count > 1) || recursive || verbose_mode;

    size_t total_matches = 0;
    for (int i = 0; i < file_count; i++) {
        const char *path = axl_args_get_pos(a, i);
        if (recursive && axl_file_is_dir(path)) {
            total_matches += grep_directory(pattern, path, 0);
        } else {
            total_matches += grep_file(pattern, path, multi_file);
        }
    }

    return (total_matches > 0) ? 0 : 1;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsApp){
        .name         = "Grep",
        .help         = "Search file(s) for a pattern (UNIX grep-style)",
        .global_flags = kFlags,
        .positionals  = kPositional,
        .handler      = run_grep,
    });
}
