/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-format.c
    Lightweight printf engine using standard C va_arg.

    No dependency on EDK2 PrintLib — uses sysv_abi va_list so callers
    don't need EFIAPI annotation. Supports standard C format specifiers.

    Float conversions (%f %F %e %E %g %G) are no-libm: a single Grisu2
    shortest-round-trippable conversion (axl_dtoa) produces the digit
    string, which is then rounded to the requested precision (%f/%e) or
    shortened with trailing zeros trimmed (%g). There is no fixed
    precision cap and large magnitudes are emitted exactly (zero-padded,
    not saturated). Because rounding starts from axl_dtoa's SHORTEST
    digit string rather than the exact value, and is round-half-up (away
    from zero) rather than glibc's round-half-to-even, last digits at an
    exact tie may differ from a libc printf for any of the three styles
    (e.g. %.1e of 9.95 -> "1.0e+01" where glibc gives "9.9e+00"). This
    is the accepted tradeoff of the no-arbitrary-precision design.
    NaN prints "nan", +/-overflow prints "inf"; -0.0 prints without a
    sign.
**/

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <axl/axl-format.h>

// ---------------------------------------------------------------------------
// Number formatting
// ---------------------------------------------------------------------------

static void
write_chars(AxlWriteFunc write_fn, void *ctx, char c, size_t count)
{
    while (count > 0) {
        write_fn(&c, 1, ctx);
        count--;
    }
}

static size_t
format_uint(char *buf, size_t buf_size, uint64_t val, int base, int upper)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    char tmp[24];
    size_t len = 0;

    if (val == 0) {
        buf[0] = '0';
        return 1;
    }

    /* Format backwards into tmp */
    while (val > 0 && len < sizeof (tmp)) {
        tmp[len++] = digits[val % base];
        val /= base;
    }

    /* Reverse into output buffer */
    if (len > buf_size) {
        len = buf_size;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = tmp[len - 1 - i];
    }
    return len;
}

// ---------------------------------------------------------------------------
// Floating-point conversions (%f %e %g and upper variants)
//
// Built on axl_dtoa (Grisu2): one shortest-digits conversion feeds all
// three styles. axl_dtoa yields the shortest digit string d[0..n) and a
// decimal-point position decpt such that the magnitude is
//   <d as integer> x 10^(decpt - n).
// %f/%e then round that shortest string to the requested precision;
// %g picks the shorter of %e/%f and trims trailing zeros.
// ---------------------------------------------------------------------------

/* Round the digit string d[0..*len) to @a nsig significant digits,
 * half-up, propagating carry. A carry out of the front (e.g. "99"->"1")
 * bumps *decpt. nsig <= 0 rounds the value away: *len becomes 0. */
static void
round_to_sig(char *d, int *len, int *decpt, int nsig)
{
    if (nsig >= *len) {
        return;
    }
    if (nsig < 0) {
        *len = 0;
        return;
    }
    int round_up = (d[nsig] >= '5');
    *len = nsig;
    if (!round_up) {
        return;
    }
    int i = nsig - 1;
    while (i >= 0) {
        if (d[i] == '9') { d[i] = '0'; i--; }
        else { d[i]++; return; }
    }
    d[0] = '1';
    *len = 1;
    (*decpt)++;
}

/* Strip trailing '0' digits (used by %g); never below one digit. */
static void
strip_trailing_zeros(char *d, int *len)
{
    while (*len > 1 && d[*len - 1] == '0') {
        (*len)--;
    }
}

/* Emit the decimal exponent for %e: echar, sign, >= 2 digits. Returns
 * the number of characters written. */
static int
emit_exponent(AxlWriteFunc write_fn, void *ctx, char echar, int exp)
{
    write_fn(&echar, 1, ctx);
    char esign = exp < 0 ? '-' : '+';
    write_fn(&esign, 1, ctx);
    unsigned uexp = (unsigned)(exp < 0 ? -exp : exp);
    char eb[8];
    size_t en = format_uint(eb, sizeof eb, uexp, 10, 0);
    int pad = en < 2 ? (int)(2 - en) : 0;
    if (pad) {
        write_chars(write_fn, ctx, '0', (size_t)pad);
    }
    write_fn(eb, en, ctx);
    return 2 + pad + (int)en;            /* echar + sign + pad + digits */
}

/* Width of the exponent tail (echar + sign + >= 2 digits). */
static int
exponent_width(int exp)
{
    unsigned uexp = (unsigned)(exp < 0 ? -exp : exp);
    char eb[8];
    size_t en = format_uint(eb, sizeof eb, uexp, 10, 0);
    return 2 + (int)(en < 2 ? 2 : en);
}

/* Compute, for an %f render, the count of leading zeros after the point
 * (@a *lead), the count of significant digits shown after the point
 * (@a *real, into d at @a *real_start), and trailing zeros (@a *trail),
 * honoring %g trim. */
static void
fixed_fraction_layout(const char *d, int n, int decpt, int fprec, int trim,
                      int *lead, int *real_start, int *real, int *trail)
{
    *lead = decpt < 0 ? -decpt : 0;
    if (*lead > fprec) { *lead = fprec; }
    *real_start = decpt < 0 ? 0 : decpt;
    int real_end = decpt + fprec;
    if (real_end > n) { real_end = n; }
    *real = real_end - *real_start;
    if (*real < 0) { *real = 0; }
    *trail = fprec - *lead - *real;
    if (*trail < 0) { *trail = 0; }
    if (trim) {
        *trail = 0;
        while (*real > 0 && d[*real_start + *real - 1] == '0') { (*real)--; }
        if (*real == 0) { *lead = 0; }
    }
}

/* Width of the %f body (integer + optional '.' + fraction). */
static int
fixed_body_width(const char *d, int n, int decpt, int fprec, int trim)
{
    int w = (decpt <= 0) ? 1 : decpt;
    int lead, rs, real, trail;
    fixed_fraction_layout(d, n, decpt, fprec, trim, &lead, &rs, &real, &trail);
    if (fprec > 0 && (lead + real + trail) > 0) {
        w += 1 + lead + real + trail;
    }
    return w;
}

/* Emit the %f body. */
static void
emit_fixed_body(AxlWriteFunc write_fn, void *ctx,
                const char *d, int n, int decpt, int fprec, int trim)
{
    if (decpt <= 0) {
        write_fn("0", 1, ctx);
    } else {
        int intreal = n < decpt ? n : decpt;
        if (intreal > 0) {
            write_fn(d, (size_t)intreal, ctx);
        }
        if (decpt > intreal) {
            write_chars(write_fn, ctx, '0', (size_t)(decpt - intreal));
        }
    }
    int lead, rs, real, trail;
    fixed_fraction_layout(d, n, decpt, fprec, trim, &lead, &rs, &real, &trail);
    if (fprec > 0 && (lead + real + trail) > 0) {
        write_fn(".", 1, ctx);
        if (lead)  { write_chars(write_fn, ctx, '0', (size_t)lead); }
        if (real)  { write_fn(d + rs, (size_t)real, ctx); }
        if (trail) { write_chars(write_fn, ctx, '0', (size_t)trail); }
    }
}

/* Compute the fraction layout for an %e render: significant fraction
 * digits shown (@a *real) and trailing zeros (@a *trail). */
static void
sci_fraction_layout(const char *d, int n, int fprec, int trim,
                    int *real, int *trail)
{
    *real = (n > 1) ? n - 1 : 0;
    if (*real > fprec) { *real = fprec; }
    *trail = fprec - *real;
    if (trim) {
        *trail = 0;
        while (*real > 0 && d[1 + *real - 1] == '0') { (*real)--; }
    }
}

/* Width of the %e body (D + optional '.' + fraction + exponent). */
static int
sci_body_width(const char *d, int n, int decpt, int fprec, int trim)
{
    int w = 1;
    int real, trail;
    sci_fraction_layout(d, n, fprec, trim, &real, &trail);
    if ((trim ? real : fprec) > 0 && (real + trail) > 0) {
        w += 1 + real + trail;
    }
    w += exponent_width(decpt - 1);
    return w;
}

/* Emit the %e body. */
static void
emit_sci_body(AxlWriteFunc write_fn, void *ctx,
              const char *d, int n, int decpt, int fprec, int trim, char echar)
{
    char lead = (n > 0) ? d[0] : '0';
    write_fn(&lead, 1, ctx);
    int real, trail;
    sci_fraction_layout(d, n, fprec, trim, &real, &trail);
    if ((trim ? real : fprec) > 0 && (real + trail) > 0) {
        write_fn(".", 1, ctx);
        if (real)  { write_fn(d + 1, (size_t)real, ctx); }
        if (trail) { write_chars(write_fn, ctx, '0', (size_t)trail); }
    }
    emit_exponent(write_fn, ctx, echar, decpt - 1);
}

/* The %f/%e/%g entry point; @a conv is one of f F e E g G. */
static void
emit_float(AxlWriteFunc write_fn, void *ctx, double value, char conv,
           int has_prec, int prec, int width,
           int flag_zero, int flag_left, int flag_plus, int flag_space)
{
    int  is_upper = (conv >= 'A' && conv <= 'Z');
    char lc = is_upper ? (char)(conv + ('a' - 'A')) : conv;
    char echar = is_upper ? 'E' : 'e';

    /* NaN: no sign. */
    if (value != value) {
        const char *s = is_upper ? "NAN" : "nan";
        int pad = width > 3 ? width - 3 : 0;
        if (!flag_left) { write_chars(write_fn, ctx, ' ', (size_t)pad); }
        write_fn(s, 3, ctx);
        if (flag_left)  { write_chars(write_fn, ctx, ' ', (size_t)pad); }
        return;
    }

    char sign = 0;
    if (value < 0.0)     { sign = '-'; value = -value; }
    else if (flag_plus)  { sign = '+'; }
    else if (flag_space) { sign = ' '; }

    /* Infinity. */
    if (value > 1.7976931348623157e308) {
        const char *s = is_upper ? "INF" : "inf";
        int body = 3 + (sign ? 1 : 0);
        int pad  = width > body ? width - body : 0;
        if (!flag_left) { write_chars(write_fn, ctx, ' ', (size_t)pad); }
        if (sign) { write_fn(&sign, 1, ctx); }
        write_fn(s, 3, ctx);
        if (flag_left)  { write_chars(write_fn, ctx, ' ', (size_t)pad); }
        return;
    }

    /* Shortest digits from Grisu2. */
    char d[AXL_DTOA_BUF_MIN];
    int  decpt = 1;
    int  ndig = axl_dtoa(value, d, sizeof d, &decpt, NULL);
    if (ndig <= 0) { d[0] = '0'; d[1] = '\0'; ndig = 1; decpt = 1; }

    int style_e;            /* 1 -> scientific, 0 -> fixed */
    int fprec;
    int trim = 0;

    if (lc == 'g') {
        int P = has_prec ? (prec == 0 ? 1 : prec) : 6;
        round_to_sig(d, &ndig, &decpt, P);
        if (ndig == 0) { d[0] = '0'; ndig = 1; decpt = 1; }
        strip_trailing_zeros(d, &ndig);
        int exp = decpt - 1;
        trim = 1;
        if (exp < -4 || exp >= P) {
            style_e = 1;
            fprec = P - 1;
        } else {
            style_e = 0;
            fprec = P - 1 - exp;
            if (fprec < 0) { fprec = 0; }
        }
    } else if (lc == 'e') {
        fprec = has_prec ? prec : 6;
        round_to_sig(d, &ndig, &decpt, fprec + 1);
        if (ndig == 0) { d[0] = '0'; ndig = 1; decpt = 1; }
        style_e = 1;
    } else {                /* 'f' */
        fprec = has_prec ? prec : 6;
        round_to_sig(d, &ndig, &decpt, decpt + fprec);
        if (ndig == 0) { d[0] = '0'; ndig = 1; decpt = 1; }
        style_e = 0;
    }

    int body = style_e
        ? sci_body_width(d, ndig, decpt, fprec, trim)
        : fixed_body_width(d, ndig, decpt, fprec, trim);
    int total = body + (sign ? 1 : 0);
    int pad = width > total ? width - total : 0;
    char pad_char = (flag_zero && !flag_left) ? '0' : ' ';

    if (!flag_left && pad_char == ' ') { write_chars(write_fn, ctx, ' ', (size_t)pad); }
    if (sign) { write_fn(&sign, 1, ctx); }
    if (!flag_left && pad_char == '0') { write_chars(write_fn, ctx, '0', (size_t)pad); }
    if (style_e) {
        emit_sci_body(write_fn, ctx, d, ndig, decpt, fprec, trim, echar);
    } else {
        emit_fixed_body(write_fn, ctx, d, ndig, decpt, fprec, trim);
    }
    if (flag_left) { write_chars(write_fn, ctx, ' ', (size_t)pad); }
}

// ---------------------------------------------------------------------------
// Core engine
// ---------------------------------------------------------------------------

void
axl_vformat(AxlWriteFunc write_fn, void *ctx, const char *fmt, va_list args)
{
    const char *run_start;
    char num_buf[24]; /* enough for 2^64 in decimal + sign */
    size_t num_len;

    if (fmt == NULL) {
        return;
    }

    run_start = fmt;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            fmt++;
            continue;
        }

        /* Flush literal run before % */
        if (fmt > run_start) {
            write_fn(run_start, (size_t)(fmt - run_start), ctx);
        }
        fmt++; /* skip % */

        /* Parse flags */
        int flag_zero = 0;
        int flag_left = 0;
        int flag_plus = 0;
        int flag_space = 0;

        for (;;) {
            if (*fmt == '0') { flag_zero = 1; fmt++; }
            else if (*fmt == '-') { flag_left = 1; fmt++; }
            else if (*fmt == '+') { flag_plus = 1; fmt++; }
            else if (*fmt == ' ') { flag_space = 1; fmt++; }
            else { break; }
        }

        /* Parse width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                flag_left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Parse precision */
        int has_prec = 0;
        int prec = 0;
        if (*fmt == '.') {
            has_prec = 1;
            fmt++;
            if (*fmt == '*') {
                prec = va_arg(args, int);
                if (prec < 0) {
                    has_prec = 0;
                    prec = 0;
                }
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Parse length modifier */
        int len_mod = 0; /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                len_mod = 2;
                fmt++;
            } else {
                len_mod = 1;
            }
        } else if (*fmt == 'z') {
            len_mod = 3;
            fmt++;
        }

        /* Conversion */
        char conv = *fmt;
        if (conv == '\0') {
            break;
        }
        fmt++;

        switch (conv) {
        case '%':
            write_fn("%", 1, ctx);
            break;

        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t slen = 0;
            while (s[slen] != '\0') { slen++; }
            if (has_prec && (size_t)prec < slen) {
                slen = (size_t)prec;
            }
            int pad = (width > (int)slen) ? width - (int)slen : 0;
            if (!flag_left) { write_chars(write_fn, ctx, ' ', pad); }
            write_fn(s, slen, ctx);
            if (flag_left) { write_chars(write_fn, ctx, ' ', pad); }
            break;
        }

        case 'c': {
            char c = (char)va_arg(args, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!flag_left) { write_chars(write_fn, ctx, ' ', pad); }
            write_fn(&c, 1, ctx);
            if (flag_left) { write_chars(write_fn, ctx, ' ', pad); }
            break;
        }

        case 'd':
        case 'i': {
            int64_t val;
            /* The long / long long branches look identical on LP64
               targets where both are 64-bit, but va_arg's type argument
               must match what the caller actually passed (%ld vs %lld),
               so these branches must stay separate for portability to
               LLP64 (Windows) and ILP32. */
            // NOLINTBEGIN(bugprone-branch-clone)
            if (len_mod == 2) { val = va_arg(args, long long); }
            else if (len_mod == 1) { val = va_arg(args, long); }
            else if (len_mod == 3) { val = (int64_t)va_arg(args, size_t); }
            else { val = va_arg(args, int); }
            // NOLINTEND(bugprone-branch-clone)

            char sign = 0;
            uint64_t uval;
            if (val < 0) {
                sign = '-';
                uval = (uint64_t)(-val);
            } else {
                uval = (uint64_t)val;
                if (flag_plus) { sign = '+'; }
                else if (flag_space) { sign = ' '; }
            }

            num_len = format_uint(num_buf, sizeof (num_buf), uval, 10, 0);
            int total = (int)num_len + (sign ? 1 : 0);
            int pad = (width > total) ? width - total : 0;
            char pad_char = (flag_zero && !flag_left) ? '0' : ' ';

            if (!flag_left && pad_char == ' ') { write_chars(write_fn, ctx, ' ', pad); }
            if (sign) { write_fn(&sign, 1, ctx); }
            if (!flag_left && pad_char == '0') { write_chars(write_fn, ctx, '0', pad); }
            write_fn(num_buf, num_len, ctx);
            if (flag_left) { write_chars(write_fn, ctx, ' ', pad); }
            break;
        }

        case 'u':
        case 'x':
        case 'X': {
            uint64_t val;
            /* Same portability caveat as the %d/%i path above —
               keep the long / long long branches separate. */
            // NOLINTBEGIN(bugprone-branch-clone)
            if (len_mod == 2) { val = va_arg(args, unsigned long long); }
            else if (len_mod == 1) { val = va_arg(args, unsigned long); }
            else if (len_mod == 3) { val = (uint64_t)va_arg(args, size_t); }
            else { val = va_arg(args, unsigned int); }
            // NOLINTEND(bugprone-branch-clone)

            int base = (conv == 'u') ? 10 : 16;
            int upper = (conv == 'X') ? 1 : 0;
            num_len = format_uint(num_buf, sizeof (num_buf), val, base, upper);
            int pad = (width > (int)num_len) ? width - (int)num_len : 0;
            char pad_char = (flag_zero && !flag_left) ? '0' : ' ';

            if (!flag_left && pad_char == ' ') { write_chars(write_fn, ctx, ' ', pad); }
            if (!flag_left && pad_char == '0') { write_chars(write_fn, ctx, '0', pad); }
            write_fn(num_buf, num_len, ctx);
            if (flag_left) { write_chars(write_fn, ctx, ' ', pad); }
            break;
        }

        case 'p': {
            void *p = va_arg(args, void *);
            if (p == NULL) {
                const char *nil = "(nil)";
                write_fn(nil, 5, ctx);
            } else {
                write_fn("0x", 2, ctx);
                num_len = format_uint(num_buf, sizeof (num_buf), (uint64_t)(uintptr_t)p, 16, 0);
                write_fn(num_buf, num_len, ctx);
            }
            break;
        }

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double dval = va_arg(args, double);
            emit_float(write_fn, ctx, dval, conv,
                       has_prec, prec, width,
                       flag_zero, flag_left, flag_plus, flag_space);
            break;
        }

        default:
            /* Unknown conversion — output as-is */
            write_fn("%", 1, ctx);
            write_fn(&conv, 1, ctx);
            break;
        }

        run_start = fmt;
    }

    /* Flush trailing literal */
    if (fmt > run_start) {
        write_fn(run_start, (size_t)(fmt - run_start), ctx);
    }
}

void
axl_format(AxlWriteFunc write_fn, void *ctx, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    axl_vformat(write_fn, ctx, fmt, args);
    va_end(args);
}
