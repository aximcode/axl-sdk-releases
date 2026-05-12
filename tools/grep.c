/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file grep.c
    Text pattern search in files (UEFI grep(1) equivalent).

    Build with axl-cc:
      axl-cc grep.c -o grep.efi

    Usage:
      grep [-i] [-n] [-c] [-r] [-v] [-h] pattern [file ...]

    With no file arguments, reads from stdin — works as the right-hand
    side of a UEFI Shell pipe (`some-tool | grep pattern`) on shells
    that publish EFI_SHELL_PARAMETERS_PROTOCOL.

    Streams its input line-by-line via axl_readline, so file and pipe
    size is unbounded and individual lines may be arbitrarily long
    (axl_readline grows its buffer as needed).
**/

#include <axl.h>

#define MAX_WALK_DEPTH      32
#define BINARY_PEEK_BYTES   512u   /* first-N bytes scanned for NUL */
/* Per-stream working buffer. Doubles as the maximum line length:
   lines longer than this get truncated and reported in verbose
   mode. 64 KiB is generous for any real text file (no real source
   file or log line approaches it) and matches GNU grep's chunk
   size. The cap exists to bound stack on degenerate inputs (a
   no-newline file, a minified blob misclassified as text, etc.). */
#define LINE_BUF_BYTES      (64u * 1024u)

static bool case_insensitive = false;
static bool show_line_numbers = false;
static bool count_only = false;
static bool invert_match = false;
static bool show_progress = false;

static const AxlArgDesc flags[] = {
    { .name = "ignore-case",   .short_name = 'i', .type = AXL_ARG_BOOL,
      .help = "Case-insensitive match" },
    { .name = "line-number",   .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Show line numbers" },
    { .name = "count",         .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Count matches only" },
    { .name = "recursive",     .short_name = 'r', .type = AXL_ARG_BOOL,
      .help = "Recursive directory search" },
    { .name = "invert-match",  .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Invert match — print lines that do NOT contain the pattern "
              "(matches Linux grep -v semantics)" },
    { .name = "show-progress",                    .type = AXL_ARG_BOOL,
      .help = "Print diagnostic notes (skipped binaries, "
              "truncated lines, unreadable files)" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "pattern", .type = AXL_ARG_STRING, .required = true,
      .help = "Search pattern" },
    { .name = "files",   .type = AXL_ARG_MULTI,
      .help = "Zero or more files (omit to read stdin; with -r, "
              "one or more directories)" },
    {0}
};

typedef struct {
    const char *pattern;
    size_t      total;
} GrepWalkCtx;

// ---------------------------------------------------------------------------
// Binary detection: scan up to BINARY_PEEK_BYTES for an embedded NUL.
// Lines (axl_readline) terminate at NUL, so a binary file with NUL in
// the first chunk also disrupts streaming — this check is the same
// signal Linux grep uses ("does the prefix look like text?").
// ---------------------------------------------------------------------------

static bool
is_binary_data(
    const uint8_t *data,
    size_t         size
    )
{
    size_t check = (size > BINARY_PEEK_BYTES) ? BINARY_PEEK_BYTES : size;
    for (size_t i = 0; i < check; i++) {
        if (data[i] == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Unified streaming search — used for both files and stdin.
//
// For files we peek the first chunk to detect binary content, then
// rewind. For stdin (not seekable) we skip the binary check and trust
// the caller — Linux grep does the same when stdin is a non-regular
// file.
//
// `path == NULL` selects stdin; `path` non-NULL opens that file.
// `show_filename` controls whether matched lines are prefixed with
// the filename (or "(stdin)" when reading the pipe).
// ---------------------------------------------------------------------------

static size_t
grep_stream(
    const char *pattern,
    const char *path,            /* NULL = stdin */
    bool        show_filename
    )
{
    AxlStream *src       = NULL;
    bool       owns_src  = false;

    if (path != NULL) {
        src = axl_fopen(path, "r");
        if (src == NULL) {
            if (show_progress) {
                axl_printf("grep: cannot read '%s'\n", path);
            }
            return 0;
        }
        owns_src = true;

        /* Peek for binary content. axl_fseek rewinds for the actual
           read so the wrapper's BOM probe sees the file from byte 0. */
        uint8_t      peek[BINARY_PEEK_BYTES];
        axl_ssize_t  n = axl_read(src, peek, sizeof(peek));
        if (n > 0 && is_binary_data(peek, (size_t)n)) {
            if (show_progress) {
                axl_printf("grep: skipping binary file '%s'\n", path);
            }
            axl_fclose(src);
            return 0;
        }
        if (axl_fseek(src, 0, AXL_SEEK_SET) != AXL_OK) {
            /* Files in this SDK are seekable; if the rewind ever
               fails, we'd be off-by-N bytes. Bail loudly. */
            if (show_progress) {
                axl_printf("grep: rewind failed on '%s'\n", path);
            }
            axl_fclose(src);
            return 0;
        }
    } else {
        src = axl_stdin;
    }

    /* BOM-probe + transparent UTF-8 over UCS-2 LE/BE if the source
       is a UEFI shell pipe or a Windows-emitted text file. */
    AxlStream *txt = axl_text_stream_wrap(src);
    if (txt == NULL) {
        if (owns_src) axl_fclose(src);
        return 0;
    }

    const char *label = (path != NULL) ? path : "(stdin)";

    /* Static — keeps the 64 KiB chunk off the EFI stack (typically
       128 KiB but recursive directory walking already consumes
       some). Not thread-safe, but UEFI tools are single-threaded
       per invocation. */
    static char    line_buf[LINE_BUF_BYTES];
    AxlLineReader  r;
    axl_line_reader_init(&r, txt, line_buf, sizeof(line_buf));

    const char  *line;
    size_t       len;
    bool         truncated;
    size_t       line_num = 1;
    size_t       count    = 0;

    while (axl_line_reader_next(&r, &line, &len, &truncated)) {
        if (truncated && show_progress) {
            axl_printf("grep: %s line %zu truncated at %zu bytes\n",
                       label, line_num, len);
        }

        /* Strip trailing '\r' from a CRLF pair — the '\n' itself is
           already excluded by the reader. */
        if (len > 0 && line[len - 1] == '\r') {
            len--;
        }

        /* The line slice points into line_buf and is NOT NUL-
           terminated; both matchers take an explicit length. */
        bool match = case_insensitive
                   ? (axl_strcasestr_len(line, (long long)len, pattern) != NULL)
                   : (axl_strstr_len(line, (long long)len, pattern) != NULL);
        /* `--invert-match` flips the predicate — print lines that
         * do NOT contain the pattern. Linux `grep -v` semantics. */
        bool emit = invert_match ? !match : match;
        if (emit && !count_only) {
            if (show_filename) axl_printf("%s:", label);
            if (show_line_numbers) axl_printf("%zu:", line_num);
            axl_printf("%.*s\n", (int)len, line);
        }
        if (emit) count++;
        line_num++;
    }

    axl_fclose(txt);
    if (owns_src) {
        axl_fclose(src);
    }

    if (count_only) {
        if (show_filename) {
            axl_printf("%s:%zu\n", label, count);
        } else {
            axl_printf("%zu\n", count);
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// Recursive directory walker
// ---------------------------------------------------------------------------

static int
grep_walk_cb(const char *full_path, const AxlDirEntry *entry, void *user)
{
    if (!entry->is_dir) {
        GrepWalkCtx *c = (GrepWalkCtx *)user;
        c->total += grep_stream(c->pattern, full_path, true);
    }
    return 0;
}

static size_t
grep_directory(
    const char *pattern,
    const char *dir_path
    )
{
    GrepWalkCtx ctx = { .pattern = pattern, .total = 0 };
    if (axl_dir_walk(dir_path, grep_walk_cb, &ctx, MAX_WALK_DEPTH) != 0
        && show_progress) {
        axl_printf("grep: walk of '%s' did not complete cleanly\n", dir_path);
    }
    return ctx.total;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_grep(AxlArgs *a)
{
    show_progress     = axl_args_get_bool(a, "show-progress");
    case_insensitive  = axl_args_get_bool(a, "ignore-case");
    show_line_numbers = axl_args_get_bool(a, "line-number");
    count_only        = axl_args_get_bool(a, "count");
    invert_match      = axl_args_get_bool(a, "invert-match");
    bool recursive    = axl_args_get_bool(a, "recursive");

    const char *pattern    = axl_args_get_string(a, "pattern");
    int         file_count = axl_args_get_pos_count(a);
    bool multi_file = (file_count > 1) || recursive || show_progress;

    size_t total_matches = 0;
    if (file_count == 0) {
        /* No files supplied — read from stdin. Useful as the right-
           hand side of a UEFI Shell pipe. */
        total_matches = grep_stream(pattern, NULL, false);
    } else {
        for (int i = 0; i < file_count; i++) {
            const char *path = axl_args_get_pos(a, i);
            if (recursive && axl_file_is_dir(path)) {
                total_matches += grep_directory(pattern, path);
            } else {
                total_matches += grep_stream(pattern, path, multi_file);
            }
        }
    }

    return (total_matches > 0) ? 0 : 1;
}

AXL_TOOL_MAIN(grep)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "grep",
        .help         = "Search file(s) for a pattern (UNIX grep-style)",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_grep,
    });
}
