/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-str-scan.c
    AxlStrReader (cursor-based string parser) and the
    `axl_sscanf` / `axl_vsscanf` family built on top of it.

    Split out of axl-str.c per docs/Style-Cleanup-Plan.md Pass C.
    AxlStrReader is its own self-contained primitive; the scanf
    layer reuses its take/consume helpers. Both want their own
    file rather than living among the basic string ops.
**/

#include <stdarg.h>
#include <stdint.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

// AxlStrReader — cursor-based string parser
// ===========================================================================

void
axl_str_reader_init(
    AxlStrReader  *r,
    const char    *s
    )
{
    if (r == NULL) { return; }
    if (s == NULL) {
        r->p = r->end = NULL;
    } else {
        r->p = s;
        r->end = s + axl_strlen(s);
    }
    r->ok = true;
}

void
axl_str_reader_init_n(
    AxlStrReader  *r,
    const char    *s,
    size_t         n
    )
{
    if (r == NULL) { return; }
    if (s == NULL) {
        r->p = r->end = NULL;
        /* NULL pointer with non-zero length is a programming error.
         * Mark not-ok so subsequent ops short-circuit cleanly. */
        r->ok = (n == 0);
        return;
    }
    r->p = s;
    r->end = s + n;
    r->ok = true;
}

bool
axl_str_reader_eof(
    const AxlStrReader *r
    )
{
    if (r == NULL) { return true; }
    return r->p >= r->end;
}

size_t
axl_str_reader_remaining(
    const AxlStrReader *r
    )
{
    if (r == NULL || r->p >= r->end) { return 0; }
    return (size_t)(r->end - r->p);
}

char
axl_str_reader_peek(
    const AxlStrReader *r
    )
{
    if (r == NULL || !r->ok || r->p >= r->end) { return '\0'; }
    return *r->p;
}

bool
axl_str_reader_skip_ws(
    AxlStrReader *r
    )
{
    if (r == NULL || !r->ok) { return false; }
    while (r->p < r->end) {
        char c = *r->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n'
            || c == '\f' || c == '\v')
        {
            r->p++;
        } else {
            break;
        }
    }
    return true;
}

bool
axl_str_reader_consume_char(
    AxlStrReader  *r,
    char           c
    )
{
    if (r == NULL || !r->ok) { return false; }
    if (r->p >= r->end || *r->p != c) {
        r->ok = false;
        return false;
    }
    r->p++;
    return true;
}

bool
axl_str_reader_consume_str(
    AxlStrReader  *r,
    const char    *literal
    )
{
    if (r == NULL || !r->ok) { return false; }
    if (literal == NULL || *literal == '\0') { return true; }
    size_t lit_len = axl_strlen(literal);
    if ((size_t)(r->end - r->p) < lit_len) {
        r->ok = false;
        return false;
    }
    for (size_t i = 0; i < lit_len; i++) {
        if (r->p[i] != literal[i]) {
            r->ok = false;
            return false;
        }
    }
    r->p += lit_len;
    return true;
}

bool
axl_str_reader_take_until(
    AxlStrReader   *r,
    char            delim,
    const char    **out,
    size_t         *out_len
    )
{
    if (r == NULL || !r->ok) { return false; }
    const char *start = r->p;
    while (r->p < r->end && *r->p != delim) {
        r->p++;
    }
    if (r->p >= r->end) {
        /* Delim not found before EOF — leave cursor at start, mark not ok. */
        r->p = start;
        r->ok = false;
        return false;
    }
    if (out != NULL)     { *out     = start; }
    if (out_len != NULL) { *out_len = (size_t)(r->p - start); }
    r->p++;   /* consume the delimiter */
    return true;
}

bool
axl_str_reader_take_while(
    AxlStrReader   *r,
    bool          (*pred)(char),
    const char    **out,
    size_t         *out_len
    )
{
    if (r == NULL || !r->ok || pred == NULL) {
        if (r != NULL && pred == NULL) { r->ok = false; }
        return false;
    }
    const char *start = r->p;
    while (r->p < r->end && pred(*r->p)) {
        r->p++;
    }
    if (out != NULL)     { *out     = start; }
    if (out_len != NULL) { *out_len = (size_t)(r->p - start); }
    return true;
}

bool
axl_str_reader_take_u64(
    AxlStrReader  *r,
    int            base,
    uint64_t      *out
    )
{
    if (r == NULL || !r->ok || out == NULL) {
        if (r != NULL) { r->ok = false; }
        return false;
    }
    if (r->p >= r->end) {
        r->ok = false;
        return false;
    }
    /* axl_str_to_u64 expects a NUL-terminated input. The reader's input
     * may not be (slice into a larger buffer), but `endptr` lets us walk
     * up to but not past `end`. We rely on `endptr` to tell us where
     * parsing stopped, then advance the cursor accordingly. To handle
     * the non-NUL-terminated case safely, we copy a digit run into a
     * stack buffer first.
     *
     * Buffer size 80: comfortably above the longest u64 literal in
     * any supported base. 64 binary digits + "0b" prefix would be 66
     * but we don't accept binary prefixes; the practical worst case
     * is a base-2 explicit-base parse of 64 raw digits. Decimal
     * overflows at 20, octal at 22, hex at 16 (+2 for the 0x prefix).
     *
     * Zero-init via designated initializer so the static analyzer can
     * prove every byte is defined — axl_str_to_u64 will stop at the
     * first NUL anyway, but the analyzer doesn't know that. */
    char     tmp[80] = {0};
    size_t   max = sizeof(tmp) - 1;
    size_t   avail = (size_t)(r->end - r->p);
    if (avail > max) { avail = max; }

    /* Copy candidate digit run, including optional 0x prefix and any
     * leading sign axl_str_to_u64 tolerates. The function itself
     * stops at the first non-digit, so over-copying is harmless. */
    for (size_t i = 0; i < avail; i++) {
        tmp[i] = r->p[i];
    }
    tmp[avail] = '\0';

    uint64_t     v;
    const char  *endptr = NULL;
    if (axl_str_to_u64(tmp, base, &v, &endptr) != AXL_OK || endptr == tmp) {
        r->ok = false;
        return false;
    }
    /* endptr - tmp = bytes consumed; advance the reader by the same. */
    size_t consumed = (size_t)(endptr - tmp);
    r->p += consumed;
    *out = v;
    return true;
}

static bool
is_ident_start(char c)
{
    return axl_isalpha((unsigned char)c) || c == '_';
}

static bool
is_ident_cont(char c)
{
    return is_ident_start(c) || axl_isdigit((unsigned char)c);
}

bool
axl_str_reader_take_ident(
    AxlStrReader   *r,
    const char    **out,
    size_t         *out_len
    )
{
    if (r == NULL || !r->ok) { return false; }
    if (r->p >= r->end || !is_ident_start(*r->p)) {
        r->ok = false;
        return false;
    }
    const char *start = r->p;
    r->p++;
    while (r->p < r->end && is_ident_cont(*r->p)) {
        r->p++;
    }
    if (out != NULL)     { *out     = start; }
    if (out_len != NULL) { *out_len = (size_t)(r->p - start); }
    return true;
}

// ===========================================================================
// axl_sscanf / axl_vsscanf
//
// Dogfoods the cursor parser above. Supports a useful subset of C99
// sscanf — see the header for the conversion list.
// ===========================================================================

/* Largest explicit field width a float conversion will honour. A width
 * bounds the field, so honouring one means staging that many bytes; the
 * bound has to live somewhere, and the format string is the only place
 * it can be checked before any input is touched. 256 is deliberate:
 * axl_vsscanf already carries a `bool table[256]` frame for %[set], so
 * a 256-byte char buffer is proportionate to what this function already
 * spends, and 256 digits is an order of magnitude past the 17 a double
 * round-trips in — no plausible fixed-width float field comes near it.
 * A wider width returns -1 rather than being quietly clamped. */
#define SCAN_FLOAT_WIDTH_MAX  256u

/* Width modifiers parsed from a conversion specifier. */
typedef enum {
    SCAN_LEN_DEFAULT,    /* int / unsigned int / etc. */
    SCAN_LEN_CHAR,       /* %hh */
    SCAN_LEN_SHORT,      /* %h  */
    SCAN_LEN_LONG,       /* %l  */
    SCAN_LEN_LONG_LONG,  /* %ll */
    SCAN_LEN_SIZE_T,     /* %z  */
    SCAN_LEN_INTMAX,     /* %j  */
} ScanLen;

/* Store helpers are macros (rather than functions taking `va_list *`)
 * because `va_list` is an array type on x86_64 — passing &ap doesn't
 * decay the way the caller expects, and clang flags the pointer
 * mismatch. Macros keep `va_arg(ap, ...)` in the caller's scope. */
#define SCAN_STORE_UNSIGNED(v, len, ap) do {                                  \
    uint64_t _val = (uint64_t)(v);                                            \
    switch (len) {                                                            \
        case SCAN_LEN_CHAR:      *va_arg(ap, unsigned char *)      = (unsigned char)_val;  break;  \
        case SCAN_LEN_SHORT:     *va_arg(ap, unsigned short *)     = (unsigned short)_val; break; \
        case SCAN_LEN_LONG:      *va_arg(ap, unsigned long *)      = (unsigned long)_val;  break; \
        case SCAN_LEN_LONG_LONG: *va_arg(ap, unsigned long long *) = (unsigned long long)_val; break; \
        case SCAN_LEN_SIZE_T:    *va_arg(ap, size_t *)             = (size_t)_val;         break; \
        case SCAN_LEN_INTMAX:    *va_arg(ap, uint64_t *)           = _val;                 break; \
        case SCAN_LEN_DEFAULT:   *va_arg(ap, unsigned int *)       = (unsigned int)_val;   break; \
    }                                                                         \
} while (0)

#define SCAN_STORE_SIGNED(v, len, ap) do {                                    \
    int64_t _val = (int64_t)(v);                                              \
    switch (len) {                                                            \
        case SCAN_LEN_CHAR:      *va_arg(ap, signed char *)      = (signed char)_val;      break; \
        case SCAN_LEN_SHORT:     *va_arg(ap, short *)            = (short)_val;            break; \
        case SCAN_LEN_LONG:      *va_arg(ap, long *)             = (long)_val;             break; \
        case SCAN_LEN_LONG_LONG: *va_arg(ap, long long *)        = (long long)_val;        break; \
        case SCAN_LEN_SIZE_T:    *va_arg(ap, size_t *)           = (size_t)_val;           break; \
        case SCAN_LEN_INTMAX:    *va_arg(ap, int64_t *)          = _val;                   break; \
        case SCAN_LEN_DEFAULT:   *va_arg(ap, int *)              = (int)_val;              break; \
    }                                                                         \
} while (0)

/* Match a single character class spec '[...]'. @a fmt points at the
 * char AFTER '['. On success advances @a *fmt to the char AFTER ']'.
 * On malformed spec, returns NULL fmt unchanged (caller treats as
 * format error). The returned 256-byte table[c] is true if c is in
 * the set.
 *
 * Supports: leading '^' for negation, ']' as first char to include
 * literal ']', and ranges 'a-z'. */
static int
scan_parse_charset(const char **fmt, bool table[256])
{
    const char *p = *fmt;
    bool negate = false;
    if (*p == '^') {
        negate = true;
        p++;
    }
    for (size_t i = 0; i < 256; i++) { table[i] = false; }

    /* ']' as first char (after '^' if any) is literal. */
    bool first = true;
    while (*p != '\0' && (first || *p != ']')) {
        first = false;
        unsigned char c = (unsigned char)*p;
        /* Range: x-y where y > '-' and the char after isn't ']'. */
        if (p[1] == '-' && p[2] != ']' && p[2] != '\0' && (unsigned char)p[2] >= c) {
            unsigned char to = (unsigned char)p[2];
            for (unsigned int x = c; x <= to; x++) { table[x] = true; }
            p += 3;
        } else {
            table[c] = true;
            p++;
        }
    }
    if (*p != ']') { return -1; }   /* unterminated set */
    p++;
    if (negate) {
        for (size_t i = 0; i < 256; i++) { table[i] = !table[i]; }
    }
    *fmt = p;
    return 0;
}

int
axl_vsscanf(
    const char  *str,
    const char  *fmt,
    va_list      ap
    )
{
    if (str == NULL || fmt == NULL) { return -1; }
    AxlStrReader r;
    axl_str_reader_init(&r, str);
    int conversions = 0;
    const char *f = fmt;

    while (*f != '\0') {
        char fc = *f;

        /* Whitespace in fmt matches any run of input whitespace. */
        if (fc == ' ' || fc == '\t' || fc == '\n' || fc == '\r') {
            axl_str_reader_skip_ws(&r);
            f++;
            continue;
        }

        /* Literal char in fmt must match input exactly. */
        if (fc != '%') {
            if (r.p >= r.end || *r.p != fc) { return conversions; }
            r.p++;
            f++;
            continue;
        }

        /* '%' — parse a conversion spec. */
        f++;

        /* Optional '*' suppresses assignment. */
        bool suppress = false;
        if (*f == '*') { suppress = true; f++; }

        /* Optional max-width.
         *
         * The accumulator has to be checked BEFORE it wraps. A width that
         * does not fit in size_t cannot describe any buffer the caller
         * could have passed, so it is a malformed FORMAT STRING, and the
         * wrap is silently downward: 2^64+1 becomes 1, 2^64 becomes 0.
         * Every consumer below then clamps that plausible-looking width
         * against the input, finds the clamp harmless, and reports a
         * successful conversion of the wrong field -- memory-safe and
         * completely wrong, which is the failure mode nothing else in the
         * build would catch. -1 is the answer %s without a width already
         * gives, and like every other malformed-format -1 in this function
         * it discards the count of conversions completed earlier in the
         * same call; the format is nonsense, so there is no partial
         * result worth reporting.
         *
         * Leading zeros never trip this: max_width stays 0 through them. */
        size_t max_width = 0;
        bool   have_width = false;
        while (*f >= '0' && *f <= '9') {
            size_t digit = (size_t)(*f - '0');
            if (max_width > (SIZE_MAX - digit) / 10) { return -1; }
            max_width = max_width * 10 + digit;
            have_width = true;
            f++;
        }

        /* Length modifier. */
        ScanLen len = SCAN_LEN_DEFAULT;
        if (*f == 'h' && f[1] == 'h')      { len = SCAN_LEN_CHAR;      f += 2; }
        else if (*f == 'h')                { len = SCAN_LEN_SHORT;     f += 1; }
        else if (*f == 'l' && f[1] == 'l') { len = SCAN_LEN_LONG_LONG; f += 2; }
        else if (*f == 'l')                { len = SCAN_LEN_LONG;      f += 1; }
        else if (*f == 'z')                { len = SCAN_LEN_SIZE_T;    f += 1; }
        else if (*f == 'j')                { len = SCAN_LEN_INTMAX;    f += 1; }

        char conv = *f;
        if (conv == '\0') { return -1; }   /* malformed: trailing '%'... */
        f++;

        switch (conv) {
            case '%': {
                /* Literal '%' — must match input. */
                if (r.p >= r.end || *r.p != '%') { return conversions; }
                r.p++;
                break;
            }

            case 'c': {
                /* %c: read N chars (default 1). No leading-ws skip. */
                size_t n = have_width ? max_width : 1;
                if ((size_t)(r.end - r.p) < n) { return conversions; }
                if (!suppress) {
                    char *dst = va_arg(ap, char *);
                    for (size_t i = 0; i < n; i++) { dst[i] = r.p[i]; }
                }
                r.p += n;
                if (!suppress) { conversions++; }
                break;
            }

            case 's': {
                /* %s: skip leading ws, then a run of non-ws. Width is
                 * required for unsuppressed %s so the destination buffer
                 * is bounded — without it, a format-string typo can write
                 * past the caller's buffer. Suppressed (%*s) doesn't write
                 * anywhere, so it's safe without a width. */
                if (!have_width && !suppress) { return -1; }
                axl_str_reader_skip_ws(&r);
                if (r.p >= r.end) { return conversions; }
                const char *start = r.p;
                while (r.p < r.end
                       && (!have_width || (size_t)(r.p - start) < max_width)
                       && *r.p != ' '  && *r.p != '\t' && *r.p != '\n'
                       && *r.p != '\r' && *r.p != '\f' && *r.p != '\v')
                {
                    r.p++;
                }
                size_t n = (size_t)(r.p - start);
                if (n == 0) { return conversions; }
                if (!suppress) {
                    char *dst = va_arg(ap, char *);
                    for (size_t i = 0; i < n; i++) { dst[i] = start[i]; }
                    dst[n] = '\0';
                    conversions++;
                }
                break;
            }

            case '[': {
                /* %[set] / %[^set] — width required. */
                if (!have_width) { return -1; }
                bool table[256];
                if (scan_parse_charset(&f, table) != 0) { return -1; }
                const char *start = r.p;
                while (r.p < r.end
                       && (size_t)(r.p - start) < max_width
                       && table[(unsigned char)*r.p])
                {
                    r.p++;
                }
                size_t n = (size_t)(r.p - start);
                if (n == 0) { return conversions; }
                if (!suppress) {
                    char *dst = va_arg(ap, char *);
                    for (size_t i = 0; i < n; i++) { dst[i] = start[i]; }
                    dst[n] = '\0';
                    conversions++;
                }
                break;
            }

            case 'd': case 'i': case 'u':
            case 'o': case 'x': case 'X': {
                /* Numeric: leading-ws skip, optional sign for d/i,
                 * optional 0x for i/x. Use the cursor-take_u64 helper
                 * (which uses axl_str_to_u64) for the heavy lifting. */
                axl_str_reader_skip_ws(&r);
                if (r.p >= r.end) { return conversions; }
                bool negative = false;
                bool is_signed = (conv == 'd' || conv == 'i');
                if (is_signed && (*r.p == '+' || *r.p == '-')) {
                    negative = (*r.p == '-');
                    r.p++;
                }
                int base;
                switch (conv) {
                    case 'd': base = 10; break;
                    case 'i': base = 0;  break;   /* auto-detect */
                    case 'u': base = 10; break;
                    case 'o': base = 8;  break;
                    case 'x': case 'X': base = 16; break;
                    default:  base = 10; break;   /* unreachable */
                }
                /* Apply max_width by clamping the reader's view. */
                AxlStrReader sub = r;
                if (have_width
                    && (size_t)(sub.end - sub.p) > max_width)
                {
                    sub.end = sub.p + max_width;
                }
                uint64_t v;
                if (!axl_str_reader_take_u64(&sub, base, &v)) {
                    return conversions;
                }
                /* sub started as a copy of r and only moved forward
                 * within the (possibly width-clamped) view. */
                r.p = sub.p;
                if (suppress) { break; }
                if (is_signed) {
                    int64_t sv;
                    if (negative) {
                        /* INT64_MIN is -INT64_MAX-1, which corresponds
                         * to the unsigned magnitude (uint64_t)INT64_MAX+1
                         * = 0x8000000000000000. Computing -(int64_t)v
                         * for that magnitude is signed-int overflow
                         * (UB per C99 §6.5/5) — even though gcc/clang
                         * emit `neg` and yield INT64_MIN today, a future
                         * optimizer is free to assume it can't happen
                         * and UBSan trips. Special-case the boundary
                         * and use unsigned arithmetic for the rest. */
                        if (v > (uint64_t)INT64_MAX + 1) { return conversions; }
                        sv = (v == (uint64_t)INT64_MAX + 1)
                             ? INT64_MIN
                             : -(int64_t)v;
                    } else {
                        if (v > (uint64_t)INT64_MAX) { return conversions; }
                        sv = (int64_t)v;
                    }
                    SCAN_STORE_SIGNED(sv, len, ap);
                } else {
                    SCAN_STORE_UNSIGNED(v, len, ap);
                }
                conversions++;
                break;
            }

            case 'f': case 'e': case 'g':
            case 'E': case 'G': {
                /* Float conversions. C99 has %f taking a `float *` and
                 * %lf a `double *` — the REVERSE of printf, where %f
                 * takes a double by default argument promotion. Route
                 * the two at axl_str_to_float / axl_str_to_double so
                 * the double->float narrowing keeps exactly one
                 * implementation in the tree.
                 *
                 * Only the default and 'l' modifiers mean anything
                 * here; %hf, %llf and friends are not C99 and are
                 * rejected loudly rather than silently taken as one of
                 * the two. %Lf never reaches this switch at all: 'L'
                 * matches no length modifier above, so conv becomes 'L'
                 * and hits default. AXL has no long double, and that
                 * rejection is deliberate. */
                if (len != SCAN_LEN_DEFAULT && len != SCAN_LEN_LONG) {
                    return -1;
                }
                bool is_double = (len == SCAN_LEN_LONG);
                /* An explicit field width caps the field at N bytes —
                 * the same semantics %Nc and %Ns already carry.
                 * Truncating to N when the caller wrote %Nf IS what %Nf
                 * means, and N comes from the FORMAT STRING, so
                 * bounding the field is an up-front, deterministic
                 * decision rather than a truncation risk pushed onto
                 * untrusted input. An absurd N is a format-string
                 * property too, so it is rejected loudly here instead
                 * of being honoured with a heap copy (which would put
                 * an allocation-failure path into sscanf) or silently
                 * ignored. */
                if (have_width && max_width > SCAN_FLOAT_WIDTH_MAX) {
                    return -1;
                }

                /* LOAD-BEARING for the width: leading whitespace is not
                 * part of the field, so it has to be stepped over
                 * before N is counted — otherwise "   3.14159" under
                 * %3lf stages three spaces and fails to parse. Without
                 * a width this call would be redundant, since
                 * axl_str_to_double skips its own leading whitespace. */
                axl_str_reader_skip_ws(&r);

                /* Without a width there is no staging copy and none is
                 * needed: this reader is always built by
                 * axl_str_reader_init above, which sets
                 * end = str + axl_strlen(str), so *r.end is the input's
                 * own NUL. The parse physically cannot run past it and
                 * `endptr` therefore lands at or before r.end. That same
                 * NUL is why an already-exhausted reader needs no
                 * separate early-out: r.p == r.end parses an empty
                 * string, which is the syntax error handled below. A
                 * mantissa of any length parses in full on this path.
                 *
                 * With a width the field is capped at N, which the check
                 * above has already bounded, so a buffer sized to that
                 * bound truncates nothing the caller did not ask for. */
                char        tmp[SCAN_FLOAT_WIDTH_MAX + 1];
                const char *src = r.p;
                if (have_width) {
                    size_t avail = (size_t)(r.end - r.p);
                    size_t n = (avail < max_width) ? avail : max_width;
                    for (size_t i = 0; i < n; i++) { tmp[i] = r.p[i]; }
                    tmp[n] = '\0';
                    src = tmp;
                }
                const char  *fend = src;
                double       dv = 0.0;
                float        fv = 0.0f;
                if (is_double) {
                    axl_str_to_double(src, &dv, &fend);
                } else {
                    axl_str_to_float(src, &fv, &fend);
                }
                /* A RANGE error ("1e400") consumed the field and wrote
                 * the saturated IEEE value, so it counts as a completed
                 * conversion — C99 stores +/-HUGE_VAL and sets ERANGE,
                 * and this family's rule is that a range error still
                 * writes the value. Only a SYNTAX error, which consumes
                 * nothing and leaves endptr at the start, ends the
                 * scan. */
                if (fend == src) { return conversions; }
                /* Advance by what was actually consumed, which can be
                 * fewer bytes than the width allowed. */
                r.p += (size_t)(fend - src);
                if (suppress) { break; }
                if (is_double) {
                    *va_arg(ap, double *) = dv;
                } else {
                    *va_arg(ap, float *) = fv;
                }
                conversions++;
                break;
            }

            case 'n': {
                /* %n — assigns # bytes consumed so far. Doesn't count
                 * as a conversion. */
                if (!suppress) {
                    int *dst = va_arg(ap, int *);
                    *dst = (int)(r.p - str);
                }
                break;
            }

            default:
                /* Unsupported / malformed conversion. */
                return -1;
        }
    }
    return conversions;
}

int
axl_sscanf(
    const char  *str,
    const char  *fmt,
    ...
    )
{
    va_list ap;
    va_start(ap, fmt);
    int rc = axl_vsscanf(str, fmt, ap);
    va_end(ap);
    return rc;
}
