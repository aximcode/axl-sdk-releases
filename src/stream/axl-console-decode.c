/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-decode.c
    Incremental decoder for raw bytes bound for the console (see
    axl-console-decode.h). */

#include "axl-console-decode.h"

void
axl_console_decoder_reset(
    AxlConsoleDecoder *d
    )
{
    d->mode  = AXL_CONSOLE_DECODE_UNKNOWN;
    d->npend = 0;
}

/* U+FFFD, the replacement character, in UTF-8. */
static size_t
put_replacement(
    char *out
    )
{
    out[0] = (char)0xEF;
    out[1] = (char)0xBF;
    out[2] = (char)0xBD;
    return 3;
}

/* @p cp (a scalar value) as UTF-8. */
static size_t
put_utf8(
    char     *out,
    uint32_t  cp
    )
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Decide the encoding from the first bytes in @p b (@p n of them),
   dropping a mark. Leaves the mode UNKNOWN when more bytes are needed. */
static void
decide_mode(
    AxlConsoleDecoder *d,
    const uint8_t     *b,
    size_t             n,
    size_t            *start
    )
{
    if (b[0] == 0xFFu) {
        if (n < 2) {
            return;
        }
        if (b[1] == 0xFEu) {
            d->mode = AXL_CONSOLE_DECODE_UTF16LE;
            *start  = 2;
            return;
        }
    } else if (b[0] == 0xEFu) {
        if (n < 2 || (b[1] == 0xBBu && n < 3)) {
            return;
        }
        if (b[1] == 0xBBu && b[2] == 0xBFu) {
            d->mode = AXL_CONSOLE_DECODE_UTF8;
            *start  = 3;
            return;
        }
    }
    d->mode = AXL_CONSOLE_DECODE_UTF8;
}

/* Consume what complete UTF-16LE units @p b[@p *i .. @p n) hold. */
static size_t
consume_utf16(
    const uint8_t *b,
    size_t         n,
    size_t        *i,
    char          *out
    )
{
    size_t o = 0;
    while (n - *i >= 2) {
        uint32_t u = (uint32_t)b[*i] | ((uint32_t)b[*i + 1] << 8);
        if (u >= 0xD800u && u <= 0xDBFFu) {
            if (n - *i < 4) {
                break;                       /* the low half is still to come */
            }
            uint32_t v = (uint32_t)b[*i + 2] | ((uint32_t)b[*i + 3] << 8);
            if (v >= 0xDC00u && v <= 0xDFFFu) {
                o  += put_utf8(out + o, 0x10000u + ((u - 0xD800u) << 10) + (v - 0xDC00u));
                *i += 4;
            } else {
                o  += put_replacement(out + o);
                *i += 2;
            }
        } else if (u >= 0xDC00u && u <= 0xDFFFu) {
            o  += put_replacement(out + o);
            *i += 2;
        } else {
            o  += put_utf8(out + o, u);
            *i += 2;
        }
    }
    return o;
}

/* The length of the well-formed UTF-8 sequence @p lead begins, and the
   range its SECOND byte must fall in (Unicode 3.9, Table 3-7): 0 for a
   byte that begins none -- a continuation byte, C0/C1 (overlong), F5..FF
   (past U+10FFFF). The narrowed second-byte ranges are what rule out the
   overlong forms (E0, F0), the surrogates (ED) and values past U+10FFFF
   (F4); every later byte is 80..BF. So a held prefix is always one that
   can still complete. */
static size_t
utf8_lead(
    uint8_t   lead,
    uint8_t  *lo,
    uint8_t  *hi
    )
{
    *lo = 0x80u;
    *hi = 0xBFu;
    if (lead < 0x80u) {
        return 1;
    }
    if (lead >= 0xC2u && lead <= 0xDFu) {
        return 2;
    }
    if (lead >= 0xE0u && lead <= 0xEFu) {
        if (lead == 0xE0u) {
            *lo = 0xA0u;
        } else if (lead == 0xEDu) {
            *hi = 0x9Fu;
        }
        return 3;
    }
    if (lead >= 0xF0u && lead <= 0xF4u) {
        if (lead == 0xF0u) {
            *lo = 0x90u;
        } else if (lead == 0xF4u) {
            *hi = 0x8Fu;
        }
        return 4;
    }
    return 0;
}

/* Consume what complete UTF-8 sequences @p b[@p *i .. @p n) hold. An
   ill-formed sequence is one U+FFFD per maximal subpart (Unicode 3.9,
   "U+FFFD Substitution of Maximal Subparts"): a valid prefix cut short is
   one, and a byte that begins no valid prefix -- a stray continuation
   byte, C0/C1, F5..FF, or a lead whose second byte is out of its range --
   is one on its own. */
static size_t
consume_utf8(
    const uint8_t *b,
    size_t         n,
    size_t        *i,
    char          *out
    )
{
    size_t o = 0;
    while (*i < n) {
        uint8_t lo;
        uint8_t hi;
        size_t  need = utf8_lead(b[*i], &lo, &hi);
        size_t  have = n - *i;
        size_t  ok   = 1;
        if (need == 0) {
            o  += put_replacement(out + o);
            *i += 1;
            continue;
        }
        if (need > 1 && have > 1) {
            ok = (b[*i + 1] >= lo && b[*i + 1] <= hi) ? 2 : 1;
        }
        while (ok >= 2 && ok < need && ok < have && (b[*i + ok] & 0xC0u) == 0x80u) {
            ok++;
        }
        if (ok == need) {
            for (size_t k = 0; k < need; k++) {
                out[o++] = (char)b[*i + k];
            }
            *i += need;
        } else if (ok == have) {
            break;                           /* the rest is still to come */
        } else {
            /* The maximal subpart -- the lead and the bytes that still fit --
               is ONE U+FFFD; decoding goes on at the byte that broke it. */
            o  += put_replacement(out + o);
            *i += ok;
        }
    }
    return o;
}

size_t
axl_console_decode(
    AxlConsoleDecoder *d,
    const void        *in,
    size_t             len,
    char              *out
    )
{
    const uint8_t *src = in;
    size_t         o   = 0;
    size_t         k   = 0;
    /* A second image after the first -- `cat --raw a.txt b.utf16` -- starts
       its own write with its own mark. In UTF-8 with nothing held, FF FE is
       never text (neither byte occurs in UTF-8), so it is that mark. */
    if (d->mode == AXL_CONSOLE_DECODE_UTF8 && d->npend == 0 && len >= 2
        && src[0] == 0xFFu && src[1] == 0xFEu) {
        d->mode = AXL_CONSOLE_DECODE_UTF16LE;
        k       = 2;
    }
    for (; k < len; k++) {
        /* The held-back bytes and this one: at most 4. */
        uint8_t b[4];
        size_t  n = 0;
        size_t  i = 0;
        for (size_t p = 0; p < d->npend; p++) {
            b[n++] = d->pend[p];
        }
        b[n++] = src[k];
        if (d->mode == AXL_CONSOLE_DECODE_UNKNOWN) {
            decide_mode(d, b, n, &i);
        }
        if (d->mode == AXL_CONSOLE_DECODE_UTF16LE) {
            o += consume_utf16(b, n, &i, out + o);
        } else if (d->mode == AXL_CONSOLE_DECODE_UTF8) {
            o += consume_utf8(b, n, &i, out + o);
        }
        d->npend = (uint8_t)(n - i);
        for (size_t p = 0; p < d->npend; p++) {
            d->pend[p] = b[i + p];
        }
    }
    return o;
}
