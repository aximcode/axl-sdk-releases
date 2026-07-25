/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cut.c
    Remove sections from each line of files. A port of POSIX cut(1)
    to UEFI (the UEFI Shell has no cut of its own).

    Usage:
      cut -b LIST [-n] [FILE...]
      cut -c LIST [FILE...]
      cut -f LIST [-d DELIM] [-s] [FILE...]
      shared: [--complement] [--output-delimiter=STR] [-z] [FILE...]

    Exactly one of -b / -c / -f selects the mode:
      -b LIST   select by BYTE position (1-based)
      -c LIST   select by CHARACTER (UTF-8 codepoint) position (1-based)
      -f LIST   select fields split on DELIM (default TAB)

    LIST is a comma-separated list of 1-based ranges: `N`, `N-`, `N-M`,
    `-M`. Selected input is written in READ order and exactly once, so
    `cut -f3,1` still prints field 1 before field 3.

    With no FILE, or `-`, reads stdin (the right-hand side of a Shell pipe).
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum { MODE_NONE, MODE_BYTE, MODE_CHAR, MODE_FIELD } CutMode;

typedef struct {
    size_t lo;   ///< 1-based inclusive start
    size_t hi;   ///< 1-based inclusive end; SIZE_MAX == open-ended
} Range;

static CutMode  g_mode;
static Range   *g_ranges;          // sorted, merged
static size_t   g_nranges;
static bool     g_complement;      // --complement
static uint8_t  g_delim = '\t';    // -d (field mode), single byte
static bool     g_only_delim;      // -s
static const char *g_out_delim;    // --output-delimiter (NULL = default)
static size_t   g_out_delim_len;
static uint8_t  g_line_delim = '\n';  // -z switches to NUL

static const AxlArgDesc flags[] = {
    { .name = "bytes",      .short_name = 'b', .type = AXL_ARG_STRING,
      .help = "Select these byte positions (LIST)" },
    { .name = "characters", .short_name = 'c', .type = AXL_ARG_STRING,
      .help = "Select these character (UTF-8 codepoint) positions (LIST)" },
    { .name = "fields",     .short_name = 'f', .type = AXL_ARG_STRING,
      .help = "Select these fields (LIST), split on --delimiter" },
    { .name = "delimiter",  .short_name = 'd', .type = AXL_ARG_STRING,
      .help = "Field delimiter, a single byte (default: TAB)" },
    { .name = "only-delimited", .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Suppress lines with no delimiter (field mode)" },
    { .name = "complement",                    .type = AXL_ARG_BOOL,
      .help = "Invert the selection" },
    { .name = "output-delimiter",              .type = AXL_ARG_STRING,
      .help = "Output delimiter string (default: input delimiter)" },
    { .name = "zero-terminated", .short_name = 'z', .type = AXL_ARG_BOOL,
      .help = "Line delimiter is NUL, not newline" },
    { .name = "no-split-multibyte", .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Accepted for compatibility (no-op)" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "files", .type = AXL_ARG_MULTI,
      .help = "Zero or more files; `-` or none = read stdin" },
    {0}
};

// ---------------------------------------------------------------------------
// LIST parsing
// ---------------------------------------------------------------------------

/* Parse one unsigned decimal at *p, advancing it. Returns false if no digit
   is present. */
static bool
parse_uint(const char **p, size_t *out)
{
    const char *s = *p;
    if (*s < '0' || *s > '9') {
        return false;
    }
    size_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (size_t)(*s - '0');
        s++;
    }
    *p = s;
    *out = v;
    return true;
}

static int
cmp_range(const void *a, const void *b)
{
    const Range *ra = a;
    const Range *rb = b;
    if (ra->lo != rb->lo) {
        return (ra->lo < rb->lo) ? -1 : 1;
    }
    return 0;
}

/* Parse a cut LIST ("1,3-5,7-") into g_ranges (sorted + merged). Returns
   AXL_OK or AXL_ERR (with a message printed). */
static int
parse_list(const char *spec)
{
    size_t  cap = 8;
    size_t  n = 0;
    Range  *r = axl_malloc(cap * sizeof(*r));
    if (r == NULL) {
        return AXL_ERR;
    }

    const char *p = spec;
    while (*p != '\0') {
        size_t lo = 1;
        size_t hi = SIZE_MAX;
        bool   have_lo = parse_uint(&p, &lo);

        if (*p == '-') {
            p++;
            size_t m;
            if (parse_uint(&p, &m)) {
                hi = m;
            } else {
                hi = SIZE_MAX;   /* N- (open ended) */
            }
            if (!have_lo) {
                lo = 1;          /* -M == 1-M */
            }
        } else if (have_lo) {
            hi = lo;             /* single N */
        } else {
            axl_printerr("cut: invalid byte/character/field list\n");
            axl_free(r);
            return AXL_ERR;
        }

        if (lo == 0 || hi == 0) {
            axl_printerr("cut: fields and positions are numbered from 1\n");
            axl_free(r);
            return AXL_ERR;
        }
        if (lo > hi) {
            axl_printerr("cut: invalid decreasing range\n");
            axl_free(r);
            return AXL_ERR;
        }

        if (n == cap) {
            cap *= 2;
            Range *nr = axl_realloc(r, cap * sizeof(*r));
            if (nr == NULL) {
                axl_free(r);
                return AXL_ERR;
            }
            r = nr;
        }
        r[n].lo = lo;
        r[n].hi = hi;
        n++;

        if (*p == ',') {
            p++;
        } else if (*p != '\0') {
            axl_printerr("cut: invalid byte/character/field list\n");
            axl_free(r);
            return AXL_ERR;
        }
    }

    if (n == 0) {
        axl_printerr("cut: invalid byte/character/field list\n");
        axl_free(r);
        return AXL_ERR;
    }

    /* Sort by lo, then merge overlapping / adjacent ranges. */
    axl_qsort(r, n, sizeof(*r), cmp_range);
    size_t w = 0;
    for (size_t i = 1; i < n; i++) {
        /* Adjacent (hi+1 == next lo) or overlapping ranges merge. Guard the
           hi+1 against SIZE_MAX overflow (an open range absorbs everything
           after it anyway). */
        bool touch = (r[w].hi == SIZE_MAX) || (r[i].lo <= r[w].hi + 1);
        if (touch) {
            if (r[i].hi > r[w].hi) {
                r[w].hi = r[i].hi;
            }
        } else {
            w++;
            r[w] = r[i];
        }
    }
    g_ranges  = r;
    g_nranges = w + 1;
    return AXL_OK;
}

/* Is 1-based position @p pos selected (honoring --complement)? */
static bool
selected(size_t pos)
{
    bool in = false;
    for (size_t i = 0; i < g_nranges; i++) {
        if (pos >= g_ranges[i].lo && pos <= g_ranges[i].hi) {
            in = true;
            break;
        }
        if (g_ranges[i].lo > pos) {
            break;   /* sorted — no later range can contain pos */
        }
    }
    return g_complement ? !in : in;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static AxlStream *g_out;

static void
emit(const void *buf, size_t n)
{
    if (n > 0) {
        axl_write(g_out, buf, n);
    }
}

/* The separator printed BETWEEN selected pieces. For fields it defaults to the
   input delimiter; for bytes/chars there is no default separator (pieces are
   contiguous) unless --output-delimiter was given. */
static void
emit_out_delim(void)
{
    if (g_out_delim != NULL) {
        emit(g_out_delim, g_out_delim_len);
    } else if (g_mode == MODE_FIELD) {
        emit(&g_delim, 1);
    }
}

// ---------------------------------------------------------------------------
// Per-line processing
// ---------------------------------------------------------------------------

/* Bytes in the UTF-8 sequence starting at @p p, from the lead byte alone,
   clamped to @p remaining and never less than 1 — so it never reads past the
   line body even on a truncated or malformed sequence. */
static size_t
utf8_adv(const uint8_t *p, size_t remaining)
{
    uint8_t c = p[0];
    size_t  n = 1;   /* ASCII, a stray continuation, or an invalid lead byte */
    if ((c & 0xE0) == 0xC0) {
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
    }
    return (n > remaining) ? 1 : n;
}

/* Byte or character mode on one line body (no trailing line delimiter). For
   MODE_CHAR, positions count UTF-8 codepoints and whole codepoints are
   emitted; for MODE_BYTE, positions count bytes. A separator is emitted
   between non-contiguous selected positions only when --output-delimiter is
   set (matches GNU cut). */
static void
process_positions(const char *line, size_t len)
{
    size_t pos = 0;          /* 1-based logical position (byte or char) */
    bool   any = false;      /* have we emitted anything yet */
    bool   prev_sel = false; /* was the previous logical position selected */

    size_t i = 0;
    while (i < len) {
        size_t adv = 1;
        if (g_mode == MODE_CHAR) {
            adv = utf8_adv((const uint8_t *)line + i, len - i);
        }
        pos++;
        if (selected(pos)) {
            if (any && !prev_sel) {
                emit_out_delim();
            }
            emit(line + i, adv);
            any = true;
            prev_sel = true;
        } else {
            prev_sel = false;
        }
        i += adv;
    }
}

/* Field mode on one line body. Splits on g_delim; a line with no delimiter is
   passed through whole (unless -s), and selected fields are joined by the
   output delimiter in ascending field order. */
static void
process_fields(const char *line, size_t len)
{
    /* No delimiter present: GNU cut passes the whole line through, unless
       -s (only-delimited) suppresses it. */
    bool has_delim = false;
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)line[i] == g_delim) {
            has_delim = true;
            break;
        }
    }
    if (!has_delim) {
        if (!g_only_delim) {
            emit(line, len);
            emit(&g_line_delim, 1);
        }
        return;
    }

    size_t field = 0;
    size_t start = 0;
    bool   any = false;
    for (size_t i = 0; i <= len; i++) {
        bool at_end = (i == len);
        if (at_end || (uint8_t)line[i] == g_delim) {
            field++;
            if (selected(field)) {
                if (any) {
                    emit_out_delim();
                }
                emit(line + start, i - start);
                any = true;
            }
            start = i + 1;
        }
    }
    emit(&g_line_delim, 1);
}

/* Read a whole stream into a heap buffer (caller frees). Used for the
   NUL-delimited (-z) path, which axl_readline cannot serve. */
static int
slurp(AxlStream *in, void **out, size_t *out_len)
{
    size_t  cap = 4096;
    size_t  len = 0;
    char   *buf = axl_malloc(cap);
    if (buf == NULL) {
        return AXL_ERR;
    }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *nb = axl_realloc(buf, cap);
            if (nb == NULL) {
                axl_free(buf);
                return AXL_ERR;
            }
            buf = nb;
        }
        size_t got = axl_fread(buf + len, 1, cap - len, in);
        len += got;
        if (got == 0) {
            break;
        }
    }
    *out = buf;
    *out_len = len;
    return AXL_OK;
}

static void
process_stream(AxlStream *in);

/* Open @p path (NULL = stdin), wrap it in a text-decoding view so a UCS-2
   Shell pipe or a BOM'd file is transparently UTF-8 (axl_readline on a raw
   UCS-2 pipe would stop at the first NUL byte), process it, and clean up.

   @return AXL_OK, or AXL_ERR if a named file could not be opened. */
static int
process_source(const char *path)
{
    AxlStream *raw = NULL;
    AxlStream *txt;

    if (path == NULL) {
        txt = axl_stdin_text();   /* wraps axl_stdin; do NOT close axl_stdin */
    } else {
        raw = axl_fopen(path, "r");
        if (raw == NULL) {
            return AXL_ERR;
        }
        txt = axl_text_stream_wrap(raw);
    }
    if (txt == NULL) {
        if (raw != NULL) {
            axl_fclose(raw);
        }
        return AXL_ERR;
    }

    process_stream(txt);

    axl_fclose(txt);
    if (raw != NULL) {
        axl_fclose(raw);
    }
    return AXL_OK;
}

static void
process_stream(AxlStream *in)
{
    /* -z reads NUL-delimited "lines"; axl_readline is newline-based, so for
       -z we read the whole stream and split on NUL ourselves. */
    if (g_line_delim == '\n') {
        char *line;
        while ((line = axl_readline(in)) != NULL) {
            size_t len = axl_strlen(line);
            bool   nl  = (len > 0 && line[len - 1] == '\n');
            size_t body = nl ? len - 1 : len;
            if (g_mode == MODE_FIELD) {
                process_fields(line, body);
            } else {
                process_positions(line, body);
                emit(&g_line_delim, 1);
            }
            /* GNU cut always terminates output lines; a final unterminated
               input line still gets one. (process_fields emits its own.) */
            axl_free(line);
        }
        return;
    }

    /* NUL-delimited path: slurp and split. */
    void  *buf = NULL;
    size_t total = 0;
    if (slurp(in, &buf, &total) != AXL_OK || buf == NULL) {
        return;
    }
    const char *p = buf;
    size_t start = 0;
    for (size_t i = 0; i <= total; i++) {
        bool at_end = (i == total);
        if (at_end || (uint8_t)p[i] == 0) {
            if (at_end && i == start) {
                break;   /* no trailing empty record after a final NUL */
            }
            size_t body = i - start;
            if (g_mode == MODE_FIELD) {
                process_fields(p + start, body);
            } else {
                process_positions(p + start, body);
                emit(&g_line_delim, 1);
            }
            start = i + 1;
        }
    }
    axl_free(buf);
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

static int
run_cut(AxlArgs *a)
{
    const char *blist = axl_args_get_string(a, "bytes");
    const char *clist = axl_args_get_string(a, "characters");
    const char *flist = axl_args_get_string(a, "fields");

    int modes = (blist != NULL) + (clist != NULL) + (flist != NULL);
    if (modes == 0) {
        axl_printerr("cut: you must specify a list of bytes, characters, "
                     "or fields\n");
        return 1;
    }
    if (modes > 1) {
        axl_printerr("cut: only one type of list may be specified\n");
        return 1;
    }

    const char *list;
    if (blist != NULL) {
        g_mode = MODE_BYTE;
        list = blist;
    } else if (clist != NULL) {
        g_mode = MODE_CHAR;
        list = clist;
    } else {
        g_mode = MODE_FIELD;
        list = flist;
    }

    g_complement = axl_args_get_bool(a, "complement");
    g_only_delim = axl_args_get_bool(a, "only-delimited");
    if (axl_args_get_bool(a, "zero-terminated")) {
        g_line_delim = 0;
    }

    const char *d = axl_args_get_string(a, "delimiter");
    if (d != NULL) {
        if (g_mode != MODE_FIELD) {
            axl_printerr("cut: an input delimiter may be specified only "
                         "when operating on fields\n");
            return 1;
        }
        if (d[0] == '\0' || d[1] != '\0') {
            axl_printerr("cut: the delimiter must be a single character\n");
            return 1;
        }
        g_delim = (uint8_t)d[0];
    }
    if (g_only_delim && g_mode != MODE_FIELD) {
        axl_printerr("cut: suppressing non-delimited lines makes sense only "
                     "when operating on fields\n");
        return 1;
    }

    g_out_delim = axl_args_get_string(a, "output-delimiter");
    if (g_out_delim != NULL) {
        g_out_delim_len = axl_strlen(g_out_delim);
    }

    if (parse_list(list) != AXL_OK) {
        return 1;
    }

    g_out = axl_stdout;
    if (g_out == NULL) {
        axl_free(g_ranges);
        return 1;
    }

    int rc = 0;
    int nfiles = axl_args_get_pos_count(a);
    if (nfiles == 0) {
        process_source(NULL);   /* stdin */
    } else {
        for (int i = 0; i < nfiles; i++) {
            const char *path = axl_args_get_pos(a, i);
            if (axl_strcmp(path, "-") == 0) {
                process_source(NULL);
            } else if (process_source(path) != AXL_OK) {
                axl_printerr("cut: %s: No such file or directory\n", path);
                rc = 1;
            }
        }
    }

    axl_free(g_ranges);
    return rc;
}

AXL_TOOL_MAIN(cut)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "cut",
        .help        = "Remove sections from each line (UEFI cut(1) equivalent)",
        .flags       = flags,
        .positionals = positional,
        .handler     = run_cut,
    });
}
