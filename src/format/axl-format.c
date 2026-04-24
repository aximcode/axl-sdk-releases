/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-format.c
    Lightweight printf engine using standard C va_arg.

    No dependency on EDK2 PrintLib — uses sysv_abi va_list so callers
    don't need EFIAPI annotation. Supports standard C format specifiers.
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
