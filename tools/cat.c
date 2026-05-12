/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cat.c
    Concatenate and display files (UEFI cat(1) equivalent).

    Build with axl-cc:
      axl-cc cat.c -o cat.efi

    Usage:
      cat [-n] [-s] [-A | -E -T] [--raw] [-e ENC] [FILE...]

    With no FILE arguments, or with `-`, reads from stdin — works as
    the right-hand side of a UEFI Shell pipe (`some-tool | cat`) on
    shells that publish EFI_SHELL_PARAMETERS_PROTOCOL.

    Input encoding is auto-detected by BOM probe (UTF-8 / UCS-2 LE /
    UCS-2 BE) unless `-e ENC` forces a specific wire encoding. The
    caller always sees UTF-8 in the output.

    `--raw` routes output through axl_stdout_raw, bypassing the
    UTF-8→UCS-2 console_write conversion — useful for piping bytes
    through to another tool intact (`cat --raw blob.bin | other`).
**/

#include <axl.h>

static bool number_lines     = false;   // -n
static bool squeeze_blanks   = false;   // -s
static bool show_ends        = false;   // -E
static bool show_tabs        = false;   // -T
static bool show_nonprinting = false;   // implied by -A
static bool raw_output       = false;   // --raw

/* Output sink — set once in run_cat to either axl_stdout or
   axl_stdout_raw, then used by every emit helper. */
static AxlStream *out_sink;

static const AxlArgDesc flags[] = {
    { .name = "number",        .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Number all output lines" },
    { .name = "squeeze-blank", .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Collapse runs of blank lines into one" },
    { .name = "show-all",      .short_name = 'A', .type = AXL_ARG_BOOL,
      .help = "Show all: equivalent to -ET plus caret/M-notation "
              "for control and high bytes" },
    { .name = "show-ends",     .short_name = 'E', .type = AXL_ARG_BOOL,
      .help = "Append `$` at the end of each line" },
    { .name = "show-tabs",     .short_name = 'T', .type = AXL_ARG_BOOL,
      .help = "Display TAB characters as `^I`" },
    { .name = "show-nonprinting", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Display non-printing characters via caret/M-notation "
              "(matches Linux `cat -v`)" },
    { .name = "raw",                              .type = AXL_ARG_BOOL,
      .help = "Write through axl_stdout_raw (binary-clean pipes; "
              "no UTF-8→UCS-2 console conversion)" },
    { .name = "encoding",      .short_name = 'e', .type = AXL_ARG_STRING,
      .help = "Force input encoding: utf8 | ucs2le | ucs2be | ascii "
              "(default: BOM-probe each input)" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "files", .type = AXL_ARG_MULTI,
      .help = "Zero or more files; `-` or none = read stdin" },
    {0}
};

// ---------------------------------------------------------------------------
// Encoding name → AxlEncoding (for -e ENC)
// ---------------------------------------------------------------------------

static int
parse_encoding(const char *s, AxlEncoding *out)
{
    if (axl_strcasecmp(s, "utf8")     == 0
     || axl_strcasecmp(s, "utf-8")    == 0) {
        *out = AXL_ENC_UTF8;
        return 0;
    }
    if (axl_strcasecmp(s, "ucs2le")   == 0
     || axl_strcasecmp(s, "ucs-2-le") == 0
     || axl_strcasecmp(s, "utf16le")  == 0
     || axl_strcasecmp(s, "utf-16-le") == 0) {
        *out = AXL_ENC_UCS2_LE;
        return 0;
    }
    if (axl_strcasecmp(s, "ucs2be")   == 0
     || axl_strcasecmp(s, "ucs-2-be") == 0
     || axl_strcasecmp(s, "utf16be")  == 0
     || axl_strcasecmp(s, "utf-16-be") == 0) {
        *out = AXL_ENC_UCS2_BE;
        return 0;
    }
    if (axl_strcasecmp(s, "ascii") == 0) {
        *out = AXL_ENC_ASCII;
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Output helpers — route all writes through one sink (axl_stdout or
// axl_stdout_raw) so --raw is transparent to the formatting code.
// ---------------------------------------------------------------------------

static void
out_write(const void *buf, size_t n)
{
    if (n == 0) return;
    (void)axl_write(out_sink, buf, n);
}

static void
out_byte(uint8_t b)
{
    out_write(&b, 1);
}

static void
out_str(const char *s)
{
    out_write(s, axl_strlen(s));
}

/* Caret-notation for a low control byte (0x00..0x1F).
   ^@ = 0x00, ^A = 0x01, ..., ^_ = 0x1F. */
static void
emit_caret_low(uint8_t b)
{
    out_byte('^');
    out_byte((uint8_t)('@' + b));
}

/* Render one byte with show-nonprinting semantics applied. */
static void
emit_byte_visible(uint8_t b)
{
    if (b == '\t') {
        if (show_tabs) {
            out_str("^I");
        } else {
            out_byte('\t');
        }
        return;
    }
    if (b == '\n') {
        /* Newlines are handled by the line writer; emit_byte_visible
           is called only on intra-line bytes. Pass through. */
        out_byte('\n');
        return;
    }
    if (b < 0x20) {
        if (show_nonprinting) {
            emit_caret_low(b);
        } else {
            out_byte(b);
        }
        return;
    }
    if (b == 0x7F) {
        if (show_nonprinting) {
            out_str("^?");
        } else {
            out_byte(b);
        }
        return;
    }
    if (b >= 0x80) {
        if (show_nonprinting) {
            /* M-notation: `M-` + (b - 0x80) rendered the same way
               (control chars get caret form, DEL → ^?). */
            uint8_t low = b & 0x7F;
            out_str("M-");
            if (low < 0x20) {
                emit_caret_low(low);
            } else if (low == 0x7F) {
                out_str("^?");
            } else {
                out_byte(low);
            }
        } else {
            out_byte(b);
        }
        return;
    }
    out_byte(b);
}

// ---------------------------------------------------------------------------
// Line-oriented processing — needed for -n and -s. Streams the input
// in O(line length) memory regardless of file size.
//
// `line_num` and `prev_blank` are owned by the caller and threaded
// across `process_stream` calls so numbering and blank-squeeze
// state continue across files within a single invocation (matching
// Linux `cat -n -s`), without leaking via function-local statics
// into a hypothetical second `axl_args_run` in the same image.
// ---------------------------------------------------------------------------

static void
process_stream(AxlStream *in, int *line_num, bool *prev_blank)
{
    char *line;
    while ((line = axl_readline(in)) != NULL) {
        size_t len          = axl_strlen(line);
        bool   has_newline  = (len > 0 && line[len - 1] == '\n');
        size_t content_len  = has_newline ? len - 1 : len;
        /* A bare trailing `\r` (file ends mid-CRLF) is real content,
           not a blank line — only treat `\r` as blank when it's the
           CR of a CRLF pair. */
        bool   is_blank     = (content_len == 0
                            || (content_len == 1 && line[0] == '\r'
                                && has_newline));

        if (squeeze_blanks && is_blank && *prev_blank) {
            axl_free(line);
            continue;
        }
        *prev_blank = is_blank;

        if (number_lines) {
            char prefix[16];
            axl_snprintf(prefix, sizeof(prefix), "%6d\t", (*line_num)++);
            out_str(prefix);
        }

        /* Body bytes — apply visibility transformations per byte. */
        for (size_t i = 0; i < content_len; i++) {
            emit_byte_visible((uint8_t)line[i]);
        }

        if (show_ends && has_newline) {
            out_byte('$');
        }
        if (has_newline) {
            out_byte('\n');
        }

        axl_free(line);
    }
}

// ---------------------------------------------------------------------------
// Per-input dispatch
// ---------------------------------------------------------------------------

static int
cat_one(
    const char  *path,
    bool         have_encoding,
    AxlEncoding  enc,
    int         *line_num,
    bool        *prev_blank
    )
{
    AxlStream  *in;
    bool        owns_outer  = false;   /* must axl_fclose the wrapper */
    bool        owns_inner  = false;   /* must axl_fclose the file too */
    AxlStream  *inner       = NULL;
    /* For the stdin+have_encoding path we mutate axl_stdin's encoding
       in place; snapshot so we can restore on exit. */
    bool        restore_stdin_enc = false;
    AxlEncoding saved_stdin_enc   = AXL_ENC_UTF8;

    bool is_stdin = (path == NULL || axl_strcmp(path, "-") == 0);

    if (is_stdin) {
        if (have_encoding) {
            saved_stdin_enc    = axl_stream_get_encoding(axl_stdin);
            restore_stdin_enc  = true;
            (void)axl_stream_set_encoding(axl_stdin, enc);
            in = axl_stdin;
        } else {
            in = axl_text_stream_wrap(axl_stdin);
            if (in == NULL) {
                axl_printerr("cat: cannot wrap stdin for BOM probe\n");
                return -1;
            }
            owns_outer = true;
        }
    } else {
        inner = axl_fopen(path, "r");
        if (inner == NULL) {
            axl_printerr("cat: cannot open '%s'\n", path);
            return -1;
        }
        owns_inner = true;
        if (have_encoding) {
            (void)axl_stream_set_encoding(inner, enc);
            in = inner;
        } else {
            in = axl_text_stream_wrap(inner);
            if (in == NULL) {
                axl_printerr("cat: cannot wrap '%s' for BOM probe\n", path);
                axl_fclose(inner);
                return -1;
            }
            owns_outer = true;
        }
    }

    process_stream(in, line_num, prev_blank);

    if (owns_outer) {
        axl_fclose(in);
    }
    if (owns_inner) {
        axl_fclose(inner);
    }
    if (restore_stdin_enc) {
        (void)axl_stream_set_encoding(axl_stdin, saved_stdin_enc);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_cat(AxlArgs *a)
{
    number_lines   = axl_args_get_bool(a, "number");
    squeeze_blanks = axl_args_get_bool(a, "squeeze-blank");
    show_ends        = axl_args_get_bool(a, "show-ends");
    show_tabs        = axl_args_get_bool(a, "show-tabs");
    show_nonprinting = axl_args_get_bool(a, "show-nonprinting");
    raw_output       = axl_args_get_bool(a, "raw");

    /* -A is a meta-flag that turns on -E, -T, and -v simultaneously
       (matching Linux `cat -A`). -v alone is the just-non-printing
       transformer. */
    if (axl_args_get_bool(a, "show-all")) {
        show_ends        = true;
        show_tabs        = true;
        show_nonprinting = true;
    }

    bool        have_encoding = false;
    AxlEncoding enc           = AXL_ENC_UTF8;
    const char *enc_str       = axl_args_get_string(a, "encoding");
    if (enc_str != NULL) {
        if (parse_encoding(enc_str, &enc) != 0) {
            axl_printerr(
                "cat: unknown encoding '%s' "
                "(want utf8 | ucs2le | ucs2be | ascii)\n",
                enc_str
                );
            return 1;
        }
        have_encoding = true;
    }

    /* --raw is for pipe fidelity (binary-clean bytes). Combining it
       with formatting flags would produce caret-noted bytes on a
       binary-bytes path — which the firmware console will mis-
       render as half-UCS-2 code units when the destination is the
       console rather than a redirected pipe. Refuse the combo
       up-front rather than ship garbled output. */
    if (raw_output
        && (number_lines || squeeze_blanks || show_ends || show_tabs
            || show_nonprinting)) {
        axl_printerr(
            "cat: --raw is incompatible with formatting flags "
            "(-n, -s, -A, -E, -T)\n"
            );
        return 1;
    }

    out_sink = raw_output ? axl_stdout_raw : axl_stdout;
    if (out_sink == NULL) {
        axl_printerr("cat: output sink not initialized\n");
        return 1;
    }

    int  file_count = axl_args_get_pos_count(a);
    int  errors     = 0;
    int  line_num   = 1;        /* numbering continues across files */
    bool prev_blank = false;    /* squeeze runs across file boundaries */

    if (file_count == 0) {
        if (cat_one(NULL, have_encoding, enc, &line_num, &prev_blank) != 0) {
            errors++;
        }
    } else {
        for (int i = 0; i < file_count; i++) {
            const char *path = axl_args_get_pos(a, i);
            if (cat_one(path, have_encoding, enc,
                        &line_num, &prev_blank) != 0) {
                errors++;
            }
        }
    }

    return (errors > 0) ? 1 : 0;
}

AXL_TOOL_MAIN(cat)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "cat",
        .help        = "Concatenate files to standard output (UNIX cat-style)",
        .flags       = flags,
        .positionals = positional,
        .handler     = run_cat,
    });
}
