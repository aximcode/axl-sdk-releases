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
    unsigned int val;
    int          octet_idx;
    int          digit_count;

    if (str == NULL || octets == NULL) {
        return -1;
    }

    axl_memset(octets, 0, 4);
    val = 0;
    octet_idx = 0;
    digit_count = 0;

    for (int i = 0; ; i++) {
        char ch = str[i];

        if (ch >= '0' && ch <= '9') {
            val = val * 10 + (unsigned int)(ch - '0');
            if (val > 255) {
                return -1;
            }
            digit_count++;
        } else if (ch == '.' || ch == '\0') {
            if (digit_count == 0 || octet_idx >= 4) {
                return -1;
            }
            octets[octet_idx] = (uint8_t)val;
            octet_idx++;
            val = 0;
            digit_count = 0;
            if (ch == '\0') {
                break;
            }
        } else {
            return -1;
        }
    }

    if (octet_idx != 4) {
        return -1;
    }

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
