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
    if (str == NULL || octets == NULL) { return -1; }

    /* Dogfooded with axl_sscanf — far more readable than the
     * hand-rolled state machine and exercises the same conversion
     * paths the SDK ships for downstream consumers. The trailing
     * %n captures the bytes consumed, so we can reject trailing
     * garbage ("1.2.3.4junk") without an extra strlen. */
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int          consumed = 0;
    int n = axl_sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed);
    if (n != 4 || str[consumed] != '\0') { return -1; }
    if (a > 255 || b > 255 || c > 255 || d > 255) { return -1; }
    octets[0] = (uint8_t)a;
    octets[1] = (uint8_t)b;
    octets[2] = (uint8_t)c;
    octets[3] = (uint8_t)d;
    return 0;
}

int
axl_ipv4_format(const uint8_t octets[4], char *buf, size_t size)
{
    int n;

    if (octets == NULL || buf == NULL || size == 0) {
        return -1;
    }

    n = axl_snprintf(buf, size, "%d.%d.%d.%d",
                     octets[0], octets[1], octets[2], octets[3]);
    if (n < 0 || (size_t)n >= size) {
        return -1;
    }

    return 0;
}
