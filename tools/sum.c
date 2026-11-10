/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file sum.c
    The body of md5sum and sha256sum: GNU coreutils md5sum(1) /
    sha256sum(1), one implementation behind two entry points (tools/md5sum.c,
    tools/sha256sum.c), the way BusyBox builds them from one source.

    Hash mode: each FILE (`-`, or none: standard input) is streamed through
    AxlDigest in fixed-size reads -- never loaded whole, so a multi-GB image
    on a read-only stick costs one buffer -- and printed as `<hex>  <name>`,
    `<hex> *<name>` with -b, `MD5 (<name>) = <hex>` with --tag, each line
    ended by NUL instead of LF with -z (written raw, byte for byte, so it
    survives a Shell `>`). -t is accepted (UEFI has no text mode).

    Standard input is hashed as the bytes it delivers: `< FILE` hands over
    the file's own bytes (the same hash as naming it), while a Shell `|`
    pipe carries UCS-2 text, so its hash is not the host's for that text.

    Check mode (-c): each LIST (`-`, or none: standard input) holds lines
    `<hex>  <name>`, `<hex> *<name>` or the --tag form `MD5 (<name>) = <hex>`
    (this tool's own algorithm name only), optionally GNU-escaped (a
    leading `\`, then `\\`, `\n`, `\r` in the name). Lines starting with
    `#` and empty lines are skipped; one CR before the line end is dropped.
    Each named file is hashed and reported `<name>: OK`, `<name>: FAILED`,
    or `<name>: FAILED open or read`; then GNU's WARNING lines on stderr.
    --quiet drops the OK lines, --status every line but a file's open
    error, -w names each improperly formatted line (the last of the three
    wins, as in GNU); --strict fails on improperly formatted lines;
    --ignore-missing skips files that do not exist. A list is read as
    text, so one the tool wrote through a Shell `>` (UCS-2) checks as
    written.

    Messages, line shapes and exit status (0, or 1 on any failure) are
    GNU's, with these differences, each forced by UEFI:
      - a name is escaped (leading `\`) only when it holds a line break; GNU
        also escapes `\`, which every UEFI path holds, so every line would
        carry it. A GNU-escaped `\\` is still read correctly in a list;
      - an error naming a file prints the name as given, without GNU's
        shell quoting of unusual names;
      - a list line longer than 8 KiB is improperly formatted;
      - standard input that is the console is refused rather than read:
        the UEFI console has no end of file, so the read would never end.
**/

#include <axl.h>

#include "sum.h"

#define SUM_READ_SIZE   (64u * 1024u)   /* one streamed read */
#define SUM_LINE_MAX    8192u           /* longest checksum-list line */
#define SUM_HEX_MAX     (2u * 64u + 1u) /* longest hex digest + NUL */

/** Which of GNU's three mutually exclusive verbosity options was given
    last (GNU: each one clears the other two). */
typedef enum {
    SUM_VERBOSE_DEFAULT = 0,
    SUM_VERBOSE_QUIET,
    SUM_VERBOSE_STATUS,
    SUM_VERBOSE_WARN,
} SumVerbose;

/** One run's settings and tallies (GNU's counters, in its order). */
typedef struct {
    const char     *prog;
    const char     *algo;           /* "MD5" / "SHA256": --tag and -w text */
    AxlChecksumType type;
    size_t          hex_len;
    SumVerbose      verbose;
    bool            strict;
    bool            ignore_missing;
    int             binary;         /* -1 unset, 0 -t, 1 -b or --tag */
    bool            tag;
    bool            zero;
    unsigned char  *buf;            /* SUM_READ_SIZE, shared by every read */
    size_t          n_misformatted;
    size_t          n_unreadable;
    size_t          n_mismatched;
} SumRun;

/** Result of hashing one file. */
typedef enum {
    SUM_FILE_OK = 0,
    SUM_FILE_MISSING,               /* does not exist */
    SUM_FILE_FAILED,                /* exists, could not be read */
} SumFileResult;

static int           sum_run(AxlArgs *a);
static void          sum_scan_order(int argc, char **argv, SumRun *r);
static bool          sum_stdin_refused(const SumRun *r);
static const char   *sum_open_error(const char *name, bool *missing);
static SumFileResult sum_file(SumRun *r, const char *name, bool report_missing,
                              char *hex, size_t hex_size);
static bool          sum_needs_escape(const char *name);
static void          sum_put_name(AxlStream *out, const char *name, bool escape);
static void          sum_print_hash(const SumRun *r, const char *name,
                                    const char *hex);
static int           sum_check_list(SumRun *r, const char *list);
static bool          sum_parse_line(const SumRun *r, char *line,
                                    const char **hex, const char **name);
static bool          sum_unescape(char *s);
static bool          sum_all_hex(const SumRun *r, const char *s);
static int           sum_usage_error(const SumRun *r, const char *msg);

static const AxlArgDesc sum_flags[] = {
    { .name = "binary", .short_name = 'b', .type = AXL_ARG_BOOL,
      .help = "Mark each name with `*` (binary mode; UEFI has no other)" },
    { .name = "check",  .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Read checksums from the FILEs and check them" },
    { .name = "tag",    .type = AXL_ARG_BOOL,
      .help = "Print BSD-style lines: ALGO (name) = hex" },
    { .name = "text",   .short_name = 't', .type = AXL_ARG_BOOL,
      .help = "Text mode (accepted; the same bytes as binary on UEFI)" },
    { .name = "zero",   .short_name = 'z', .type = AXL_ARG_BOOL,
      .help = "End each output line with NUL, not newline (raw bytes)" },
    { .name = "ignore-missing", .type = AXL_ARG_BOOL,
      .help = "With -c: don't fail or report for missing files" },
    { .name = "quiet",  .type = AXL_ARG_BOOL,
      .help = "With -c: don't print OK for each verified file" },
    { .name = "status", .type = AXL_ARG_BOOL,
      .help = "With -c: print nothing; the exit status says it all" },
    { .name = "strict", .type = AXL_ARG_BOOL,
      .help = "With -c: fail on improperly formatted checksum lines" },
    { .name = "warn",   .short_name = 'w', .type = AXL_ARG_BOOL,
      .help = "With -c: warn about each improperly formatted line" },
    {0}
};

static const AxlArgDesc sum_positional[] = {
    { .name = "file", .type = AXL_ARG_MULTI,
      .help = "Files to hash (with -c: checksum lists); `-` or none = stdin" },
    {0}
};

/* Two of GNU's option groups depend on ORDER, which axl_args does not
   report (it says only that a flag was set), so they are read from argv:
     - --quiet, --status and -w each switch the other two off: last wins;
     - -b sets binary, -t clears it, --tag sets it: last wins (so `-t --tag`
       is a tag line and `--tag -t` is refused, as in GNU).
   Every word before `--` that starts with `-` (and is not `-` alone) is a
   flag word to axl_args too -- these tools have no flag that takes a value
   -- so a short bundle like `-tb` is read letter by letter, and a name
   that looks like one (`-twig`) can only follow `--`, where the scan
   stops. A command axl_args refuses never reaches the handler. */
static void
sum_scan_order(
    int     argc,
    char  **argv,
    SumRun *r)
{
    r->verbose = SUM_VERBOSE_DEFAULT;
    r->binary  = -1;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (axl_strcmp(s, "--") == 0) {
            break;
        }
        if (s[0] != '-' || s[1] == '\0') {
            continue;
        }
        if (s[1] == '-') {
            if (axl_strcmp(s, "--quiet") == 0) {
                r->verbose = SUM_VERBOSE_QUIET;
            } else if (axl_strcmp(s, "--status") == 0) {
                r->verbose = SUM_VERBOSE_STATUS;
            } else if (axl_strcmp(s, "--warn") == 0) {
                r->verbose = SUM_VERBOSE_WARN;
            } else if (axl_strcmp(s, "--binary") == 0
                       || axl_strcmp(s, "--tag") == 0) {
                r->binary = 1;
            } else if (axl_strcmp(s, "--text") == 0) {
                r->binary = 0;
            }
            continue;
        }
        for (const char *c = s + 1; *c != '\0'; c++) {
            if (*c == 'w') {
                r->verbose = SUM_VERBOSE_WARN;
            } else if (*c == 'b') {
                r->binary = 1;
            } else if (*c == 't') {
                r->binary = 0;
            }
        }
    }
}

/* Standard input as a console has no end of file in UEFI: refuse to read
   it rather than wait forever. */
static bool
sum_stdin_refused(
    const SumRun *r)
{
    if (!axl_stdin_is_interactive()) {
        return false;
    }
    axl_printerr("%s: standard input is the console, which has no end of "
                 "file; name a FILE or redirect stdin\n", r->prog);
    return true;
}

/* GNU's reason for a NAME that could not be opened: it does not exist, or
   it does and still could not be read. */
static const char *
sum_open_error(
    const char *name,
    bool       *missing)
{
    AxlFsEntry info = {0};
    *missing = axl_file_info(name, &info) != AXL_OK;
    return *missing ? "No such file or directory" : "Input/output error";
}

/* Hash NAME (`-` = stdin) into HEX. On failure GNU's error line is printed,
   except for a missing file when REPORT_MISSING is false (--ignore-missing). */
static SumFileResult
sum_file(
    SumRun     *r,
    const char *name,
    bool        report_missing,
    char       *hex,
    size_t      hex_size)
{
    bool       is_stdin = axl_strcmp(name, "-") == 0;
    AxlStream *in       = NULL;

    if (is_stdin) {
        if (sum_stdin_refused(r)) {
            return SUM_FILE_FAILED;
        }
        in = axl_stdin;
    } else if (axl_file_is_dir(name)) {
        axl_printerr("%s: %s: Is a directory\n", r->prog, name);
        return SUM_FILE_FAILED;
    } else {
        in = axl_fopen(name, "r");
        if (in == NULL) {
            bool        missing = false;
            const char *why     = sum_open_error(name, &missing);
            if (!missing || report_missing) {
                axl_printerr("%s: %s: %s\n", r->prog, name, why);
            }
            return missing ? SUM_FILE_MISSING : SUM_FILE_FAILED;
        }
    }

    AxlChecksum *cs = axl_checksum_new(r->type);
    if (cs == NULL) {
        axl_printerr("%s: out of memory\n", r->prog);
        if (!is_stdin) {
            axl_fclose(in);
        }
        return SUM_FILE_FAILED;
    }
    size_t n;
    while ((n = axl_fread(r->buf, 1, SUM_READ_SIZE, in)) > 0) {
        axl_checksum_update(cs, r->buf, n);
    }
    bool read_error = axl_ferror(in) != 0;
    if (!is_stdin) {
        axl_fclose(in);
    }
    if (read_error) {
        axl_printerr("%s: %s: Input/output error\n", r->prog, name);
        axl_checksum_free(cs);
        return SUM_FILE_FAILED;
    }
    const char *s = axl_checksum_get_string(cs);
    axl_strlcpy(hex, (s != NULL) ? s : "", hex_size);
    axl_checksum_free(cs);
    return (s != NULL) ? SUM_FILE_OK : SUM_FILE_FAILED;
}

/* A name needs GNU's escaped form when it holds a line break (see the file
   comment for why a backslash alone does not). */
static bool
sum_needs_escape(
    const char *name)
{
    return axl_strchr(name, '\n') != NULL || axl_strchr(name, '\r') != NULL;
}

/* NAME, escaped GNU's way (`\\`, `\n`, `\r`) when ESCAPE is set. */
static void
sum_put_name(
    AxlStream  *out,
    const char *name,
    bool        escape)
{
    if (!escape) {
        axl_fprintf(out, "%s", name);
        return;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '\\') {
            axl_fprintf(out, "\\\\");
        } else if (*p == '\n') {
            axl_fprintf(out, "\\n");
        } else if (*p == '\r') {
            axl_fprintf(out, "\\r");
        } else {
            axl_fprintf(out, "%c", *p);
        }
    }
}

/* One hash-mode line: untagged or --tag, LF or (-z) NUL ended. With -z GNU
   escapes nothing, and the line goes out raw so the NUL survives. */
static void
sum_print_hash(
    const SumRun *r,
    const char   *name,
    const char   *hex)
{
    AxlStream *out    = r->zero ? axl_stdout_raw : axl_stdout;
    bool       escape = !r->zero && sum_needs_escape(name);

    if (escape) {
        axl_fprintf(out, "\\");
    }
    if (r->tag) {
        axl_fprintf(out, "%s (", r->algo);
        sum_put_name(out, name, escape);
        axl_fprintf(out, ") = %s", hex);
    } else {
        axl_fprintf(out, "%s %c", hex, (r->binary == 1) ? '*' : ' ');
        sum_put_name(out, name, escape);
    }
    if (r->zero) {
        axl_fwrite("", 1, 1, out);
    } else {
        axl_fprintf(out, "\n");
    }
}

/* GNU's `\` escapes in a name from an escaped line: `\\`, `\n` and `\r`.
   False on any other escape (the line is then improperly formatted). */
static bool
sum_unescape(
    char *s)
{
    char *out = s;
    for (const char *in = s; *in != '\0'; in++) {
        if (*in != '\\') {
            *out++ = *in;
            continue;
        }
        in++;
        if (*in == '\\') {
            *out++ = '\\';
        } else if (*in == 'n') {
            *out++ = '\n';
        } else if (*in == 'r') {
            *out++ = '\r';
        } else {
            return false;
        }
    }
    *out = '\0';
    return true;
}

/* Exactly hex_len hex digits, then the end of S. */
static bool
sum_all_hex(
    const SumRun *r,
    const char   *s)
{
    for (size_t i = 0; i < r->hex_len; i++) {
        if (!axl_isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return s[r->hex_len] == '\0';
}

/* Split one list line (its line end already removed) into HEX and NAME, in
   place, the way GNU's split_3 does: leading whitespace, an optional `\`
   (escaped name), then either the tag form `ALGO (name) = hex` -- ALGO this
   tool's own, one optional space before `(`, the name ending at the LAST
   `)`, blanks around `=` -- or `hex`, a blank, an optional second space or
   `*`, and a non-empty name. False when the line is neither. */
static bool
sum_parse_line(
    const SumRun *r,
    char         *line,
    const char  **hex,
    const char  **name)
{
    char *p = line;
    while (axl_isspace((unsigned char)*p)) {
        p++;
    }
    bool escaped = (*p == '\\');
    if (escaped) {
        p++;
    }

    size_t algo_len = axl_strlen(r->algo);
    if (axl_strncmp(p, r->algo, algo_len) == 0) {
        p += algo_len;
        if (*p == ' ') {
            p++;
        }
        if (*p != '(') {
            return false;
        }
        p++;
        char *close = NULL;
        for (char *c = p; *c != '\0'; c++) {
            if (*c == ')') {
                close = c;
            }
        }
        if (close == NULL) {
            return false;
        }
        *close = '\0';
        char *q = close + 1;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q != '=') {
            return false;
        }
        q++;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (!sum_all_hex(r, q) || (escaped && !sum_unescape(p))) {
            return false;
        }
        *hex  = q;
        *name = p;
        return true;
    }

    *hex = p;
    for (size_t i = 0; i < r->hex_len; i++, p++) {
        if (!axl_isxdigit((unsigned char)*p)) {
            return false;
        }
    }
    if (*p != ' ' && *p != '\t') {
        return false;
    }
    *p++ = '\0';
    if (*p == ' ' || *p == '*') {
        p++;
    }
    if (*p == '\0' || (escaped && !sum_unescape(p))) {
        return false;
    }
    *name = p;
    return true;
}

/* Check every line of LIST (`-` = stdin). 0 when the list held a checksum
   line, every listed file was read and matched (with --ignore-missing: at
   least one was verified), and (--strict) no line was improperly
   formatted; else 1. */
static int
sum_check_list(
    SumRun     *r,
    const char *list)
{
    bool        is_stdin = axl_strcmp(list, "-") == 0;
    const char *shown    = is_stdin ? "'standard input'" : list;
    AxlStream  *raw      = NULL;
    AxlStream  *text     = NULL;

    if (is_stdin) {
        if (sum_stdin_refused(r)) {
            return 1;
        }
        text = axl_stdin_text();
    } else if (axl_file_is_dir(list)) {
        /* GNU opens it and fails the first read. */
        axl_printerr("%s: %s: read error\n", r->prog, list);
        return 1;
    } else {
        raw = axl_fopen(list, "r");
        if (raw == NULL) {
            bool missing = false;
            axl_printerr("%s: %s: %s\n", r->prog, list,
                         sum_open_error(list, &missing));
            return 1;
        }
        text = axl_text_stream_wrap(raw);
    }
    if (text == NULL) {
        axl_printerr("%s: out of memory\n", r->prog);
        axl_fclose(raw);
        return 1;
    }

    bool   print     = r->verbose != SUM_VERBOSE_STATUS;
    size_t formatted = 0;
    size_t matched   = 0;
    size_t lineno    = 0;
    size_t misformat = 0;
    size_t unread    = 0;
    size_t mismatch  = 0;
    char  *line;
    char   got[SUM_HEX_MAX];

    /* One byte over the limit: a line longer than SUM_LINE_MAX comes back
       cut at SUM_LINE_MAX + 1 bytes, so any line that long is over it, and
       an unterminated last line of exactly SUM_LINE_MAX bytes is not. */
    while ((line = axl_readline_max(text, SUM_LINE_MAX + 2)) != NULL) {
        lineno++;
        size_t len = axl_strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        /* GNU: a `#` in column 0 is a comment (tested before the line end
           is stripped); one CR before the line end is dropped; an empty
           line is skipped. */
        if (line[0] == '#') {
            axl_free(line);
            continue;
        }
        if (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }
        if (len == 0) {
            axl_free(line);
            continue;
        }
        const char *hex  = NULL;
        const char *name = NULL;
        if (len > SUM_LINE_MAX || !sum_parse_line(r, line, &hex, &name)) {
            misformat++;
            if (r->verbose == SUM_VERBOSE_WARN) {
                axl_printerr("%s: %s: %zu: improperly formatted %s checksum "
                             "line\n", r->prog, shown, lineno, r->algo);
            }
            axl_free(line);
            continue;
        }
        formatted++;
        bool          esc = sum_needs_escape(name);
        SumFileResult fr  = sum_file(r, name, !r->ignore_missing, got,
                                     sizeof(got));
        const char   *verdict = NULL;
        if (fr == SUM_FILE_MISSING && r->ignore_missing) {
            verdict = NULL;
        } else if (fr != SUM_FILE_OK) {
            unread++;
            verdict = "FAILED open or read";
        } else if (axl_strncasecmp(got, hex, r->hex_len) != 0) {
            mismatch++;
            verdict = "FAILED";
        } else {
            matched++;
            if (r->verbose != SUM_VERBOSE_QUIET) {
                verdict = "OK";
            }
        }
        if (verdict != NULL && print) {
            if (esc) {
                axl_printf("\\");
            }
            sum_put_name(axl_stdout, name, esc);
            axl_printf(": %s\n", verdict);
        }
        axl_free(line);
    }
    axl_fclose(text);
    axl_fclose(raw);

    r->n_misformatted += misformat;
    r->n_unreadable   += unread;
    r->n_mismatched   += mismatch;

    if (formatted == 0) {
        axl_printerr("%s: %s: no properly formatted checksum lines found\n",
                     r->prog, shown);
        return 1;
    }
    if (print) {
        if (misformat != 0) {
            axl_printerr("%s: WARNING: %zu %s improperly formatted\n", r->prog,
                         misformat, (misformat == 1) ? "line is" : "lines are");
        }
        if (unread != 0) {
            axl_printerr("%s: WARNING: %zu listed %s could not be read\n",
                         r->prog, unread, (unread == 1) ? "file" : "files");
        }
        if (mismatch != 0) {
            axl_printerr("%s: WARNING: %zu computed %s did NOT match\n",
                         r->prog, mismatch,
                         (mismatch == 1) ? "checksum" : "checksums");
        }
    }
    if (print && r->ignore_missing && matched == 0) {
        axl_printerr("%s: %s: no file was verified\n", r->prog, shown);
    }
    bool ok = matched != 0 && mismatch == 0 && unread == 0
              && (!r->strict || misformat == 0);
    return ok ? 0 : 1;
}

/* GNU's two-line refusal of an option combination. */
static int
sum_usage_error(
    const SumRun *r,
    const char   *msg)
{
    axl_printerr("%s: %s\nTry '%s --help' for more information.\n",
                 r->prog, msg, r->prog);
    return 1;
}

static int
sum_run(
    AxlArgs *a)
{
    SumRun *r     = axl_args_user_data(a);
    bool    check = axl_args_get_bool(a, "check");
    r->tag            = axl_args_get_bool(a, "tag");
    r->zero           = axl_args_get_bool(a, "zero");
    r->strict         = axl_args_get_bool(a, "strict");
    r->ignore_missing = axl_args_get_bool(a, "ignore-missing");

    /* GNU's checks, in GNU's order. */
    if (check && r->zero) {
        return sum_usage_error(r, "the --zero option is not supported when "
                                  "verifying checksums");
    }
    if (check && r->tag) {
        return sum_usage_error(r, "the --tag option is meaningless when "
                                  "verifying checksums");
    }
    if (check && r->binary >= 0) {
        return sum_usage_error(r, "the --binary and --text options are "
                                  "meaningless when verifying checksums");
    }
    if (r->tag && r->binary == 0) {
        return sum_usage_error(r, "--tag does not support --text mode");
    }
    if (!check) {
        static const char *const only_check[] = {
            [SUM_VERBOSE_QUIET]  = "quiet",
            [SUM_VERBOSE_STATUS] = "status",
            [SUM_VERBOSE_WARN]   = "warn",
        };
        const char *bad = r->ignore_missing ? "ignore-missing"
                          : (r->verbose != SUM_VERBOSE_DEFAULT)
                              ? only_check[r->verbose]
                          : r->strict ? "strict" : NULL;
        if (bad != NULL) {
            char msg[96];
            axl_snprintf(msg, sizeof(msg), "the --%s option is meaningful "
                         "only when verifying checksums", bad);
            return sum_usage_error(r, msg);
        }
    }

    r->buf = axl_malloc(SUM_READ_SIZE);
    if (r->buf == NULL) {
        axl_printerr("%s: out of memory\n", r->prog);
        return 1;
    }

    int  count = axl_args_get_pos_count(a);
    int  rc    = 0;
    char hex[SUM_HEX_MAX];
    for (int i = 0; i < ((count > 0) ? count : 1); i++) {
        const char *name = (count > 0) ? axl_args_get_pos(a, i) : "-";
        if (check) {
            rc |= sum_check_list(r, name);
        } else if (sum_file(r, name, true, hex, sizeof(hex)) != SUM_FILE_OK) {
            rc = 1;
        } else {
            sum_print_hash(r, name, hex);
        }
    }
    axl_free(r->buf);
    r->buf = NULL;
    return rc;
}

int
sum_tool_run(
    int             argc,
    char          **argv,
    const char     *prog,
    AxlChecksumType type)
{
    SumRun run = {
        .prog    = prog,
        .algo    = (type == AXL_CHECKSUM_MD5) ? "MD5" : "SHA256",
        .type    = type,
        .hex_len = 2 * axl_checksum_type_get_length(type),
    };
    sum_scan_order(argc, argv, &run);
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = prog,
        .help        = (type == AXL_CHECKSUM_MD5)
                           ? "Print or check MD5 (128-bit) checksums"
                           : "Print or check SHA-256 (256-bit) checksums",
        .flags       = sum_flags,
        .positionals = sum_positional,
        .handler     = sum_run,
        .user_data   = &run,
    });
}
