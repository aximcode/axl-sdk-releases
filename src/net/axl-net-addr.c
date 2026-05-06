/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-addr.c
    IPv4 address parsing and formatting helpers.
**/

#include <axl/axl-str.h>
#include <axl/axl-net.h>

int
axl_ipv4_parse(const char *str, uint8_t octets[4])
{
    if (str == NULL || octets == NULL) { return AXL_ERR; }

    /* Dogfooded with axl_sscanf — far more readable than the
     * hand-rolled state machine and exercises the same conversion
     * paths the SDK ships for downstream consumers. The trailing
     * %n captures the bytes consumed, so we can reject trailing
     * garbage ("1.2.3.4junk") without an extra strlen. */
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int          consumed = 0;
    int n = axl_sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed);
    if (n != 4 || str[consumed] != '\0') { return AXL_ERR; }
    if (a > 255 || b > 255 || c > 255 || d > 255) { return AXL_ERR; }
    octets[0] = (uint8_t)a;
    octets[1] = (uint8_t)b;
    octets[2] = (uint8_t)c;
    octets[3] = (uint8_t)d;
    return AXL_OK;
}

int
axl_ipv4_format(const uint8_t octets[4], char *buf, size_t size)
{
    int n;

    if (octets == NULL || buf == NULL || size == 0) {
        return AXL_ERR;
    }

    n = axl_snprintf(buf, size, "%d.%d.%d.%d",
                     octets[0], octets[1], octets[2], octets[3]);
    if (n < 0 || (size_t)n >= size) {
        return AXL_ERR;
    }

    return AXL_OK;
}

bool
axl_ipv4_equals(const uint8_t a[4], const uint8_t b[4])
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

bool
axl_ipv4_in_subnet(const uint8_t dest[4], const uint8_t station[4],
                   const uint8_t mask[4])
{
    if (dest == NULL || station == NULL || mask == NULL) {
        return false;
    }
    /* Zero mask = no policy yet; refuse to match. */
    if ((mask[0] | mask[1] | mask[2] | mask[3]) == 0) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if ((dest[i] & mask[i]) != (station[i] & mask[i])) {
            return false;
        }
    }
    return true;
}

int
axl_ipv6_format(const uint8_t octets[16], char *buf, size_t size)
{
    if (octets == NULL || buf == NULL || size == 0) {
        return AXL_ERR;
    }

    /* Decompose into 8 16-bit groups (network byte order: hi byte first). */
    uint16_t g[8];
    for (size_t i = 0; i < 8; i++) {
        g[i] = (uint16_t)((uint16_t)octets[2 * i] << 8
                          | (uint16_t)octets[2 * i + 1]);
    }

    /* Find the longest run of consecutive zero groups for `::` collapsing.
       RFC 5952 §4.2: only collapse runs of length >= 2; ties go to the
       leftmost run. */
    int best_start = -1, best_len = 0;
    int cur_start  = -1, cur_len  = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (cur_start < 0) {
                cur_start = i;
                cur_len   = 1;
            } else {
                cur_len++;
            }
            if (cur_len > best_len) {
                best_start = cur_start;
                best_len   = cur_len;
            }
        } else {
            cur_start = -1;
            cur_len   = 0;
        }
    }
    if (best_len < 2) {
        best_start = -1;
    }

    size_t pos = 0;
    for (int i = 0; i < 8; i++) {
        if (i == best_start) {
            /* Emit "::" and skip the collapsed run. The leading colon
               handles the i==0 case (":...") and subsequent groups
               provide their own leading colon. */
            int n = axl_snprintf(buf + pos, size - pos, "::");
            if (n < 0 || (size_t)n >= size - pos) {
                return AXL_ERR;
            }
            pos += (size_t)n;
            i += best_len - 1;
            continue;
        }
        const char *fmt = (i == 0 || i == best_start + best_len) ? "%x" : ":%x";
        int n = axl_snprintf(buf + pos, size - pos, fmt, (unsigned)g[i]);
        if (n < 0 || (size_t)n >= size - pos) {
            return AXL_ERR;
        }
        pos += (size_t)n;
    }

    return AXL_OK;
}
