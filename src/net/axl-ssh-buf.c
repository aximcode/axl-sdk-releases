/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-buf.c
    SSH wire codec (RFC 4251 section 5).

    Readers advance *off ONLY on success, so a caller that ignores the status
    cannot silently consume a malformed field. See axl-ssh-internal.h for why
    the readers validate what the writers do not.
**/

#include "axl-ssh-internal.h"

int
axl_ssh_put_u32(AxlString *b, uint32_t v)
{
    const char d[4] = { (char)(uint8_t)(v >> 24), (char)(uint8_t)(v >> 16),
                        (char)(uint8_t)(v >> 8),  (char)(uint8_t)v };
    return axl_string_append_len(b, d, sizeof d);
}

int
axl_ssh_get_u32(const uint8_t *p, size_t len, size_t *off, uint32_t *out)
{
    if (p == NULL || off == NULL || out == NULL) {
        return AXL_ERR;
    }
    /* Order matters: *off > len must be tested BEFORE `len - *off`, because
       both are size_t and the subtraction underflows to a huge value for an
       out-of-range offset. Written the other way round the bounds check passes
       and the read runs off the end of the buffer. */
    if (*off > len || len - *off < 4) {
        return AXL_ERR;
    }
    const uint8_t *q = p + *off;
    *out = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
           ((uint32_t)q[2] << 8)  |  (uint32_t)q[3];
    *off += 4;
    return AXL_OK;
}

int
axl_ssh_put_string(AxlString *b, const void *s, size_t n)
{
    if (n > 0xFFFFFFFFu || axl_ssh_put_u32(b, (uint32_t)n) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_string_append_len(b, (const char *)s, n);
}

int
axl_ssh_parse_ident(const uint8_t *p, size_t len, size_t *end_off)
{
    if (p == NULL || end_off == NULL) {
        return AXL_ERR;
    }

    size_t start = 0;
    for (;;) {
        /* A peer may legally send preamble lines before identifying, but not
           unlimited ones -- that is a peer who keeps us reading forever. */
        if (start > AXL_SSH_PREAMBLE_MAX) {
            return AXL_ERR;
        }

        size_t i = start;
        while (i + 1 < len && !(p[i] == '\r' && p[i + 1] == '\n')) {
            i++;
        }

        if (i + 1 >= len) {
            /* No complete line yet. "Send more" is only a valid answer while
               this line could still BECOME legal: once the content alone
               cannot fit in AXL_SSH_IDENT_MAX with a CRLF appended, no future
               byte can rescue it, and waiting is waiting forever.

               A trailing CR is the terminator's first half, not content, so it
               must not be counted against the cap -- otherwise the legal
               253-content + CRLF line is rejected one byte early. */
            size_t content = len - start;
            if (content > 0 && p[len - 1] == '\r') {
                content--;
            }
            if (content + 2 > AXL_SSH_IDENT_MAX) {
                return AXL_ERR;
            }
            return AXL_INCOMPLETE;
        }

        size_t line_len = i - start;
        if (line_len + 2 > AXL_SSH_IDENT_MAX) {
            return AXL_ERR;
        }
        if (line_len >= 4 && axl_memcmp(p + start, "SSH-", 4) == 0) {
            /* Identification found. Anything that is not exactly SSH-2.0 is
               refused rather than negotiated: SSH-1.x is the downgrade this
               check exists to stop. */
            if (line_len < 8 || axl_memcmp(p + start + 4, "2.0-", 4) != 0) {
                return AXL_ERR;
            }
            *end_off = i + 2;
            return AXL_OK;
        }
        start = i + 2;   /* a preamble line; keep looking */
    }
}

int
axl_ssh_get_string(const uint8_t *p, size_t len, size_t *off,
                   const uint8_t **out, uint32_t *out_len)
{
    if (off == NULL || out == NULL || out_len == NULL) {
        return AXL_ERR;
    }
    /* Parse into a local offset so a failure anywhere below leaves *off
       untouched — the caller's stream position stays where it was. */
    size_t probe = *off;
    uint32_t n = 0;
    if (axl_ssh_get_u32(p, len, &probe, &n) != AXL_OK) {
        return AXL_ERR;
    }
    /* The length field is attacker-controlled: check it against what we
       actually hold before handing out a pointer. get_u32 succeeded, so
       probe <= len and this subtraction cannot underflow. */
    if (n > len - probe) {
        return AXL_ERR;
    }
    *out = p + probe;
    *out_len = n;
    *off = probe + n;
    return AXL_OK;
}
