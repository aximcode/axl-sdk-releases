/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-inet-address.c
    AxlInetAddress and AxlSocketAddress implementation.
**/

#include <axl/axl-inet-address.h>
#include <axl/axl-net.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("net");

// ---------------------------------------------------------------------------
// Internal structures
// ---------------------------------------------------------------------------

struct AxlInetAddress {
    uint8_t addr[4];
    char    str[16];     /* lazily cached "ddd.ddd.ddd.ddd\0" */
    bool    str_valid;
};

struct AxlSocketAddress {
    AxlInetAddress *address;  /* owned */
    uint16_t        port;
};

// ---------------------------------------------------------------------------
// AxlInetAddress
// ---------------------------------------------------------------------------

AxlInetAddress *
axl_inet_address_new_from_string(const char *str)
{
    AxlInetAddress *addr;

    if (str == NULL) {
        return NULL;
    }

    addr = axl_calloc(1, sizeof(*addr));
    if (addr == NULL) {
        axl_debug(
          "axl_inet_address_new_from_string: OOM allocating AxlInetAddress (%zu bytes)",
          sizeof(*addr)
          );
        return NULL;
    }

    if (axl_ipv4_parse(str, addr->addr) != AXL_OK) {
        axl_debug("invalid IPv4 address: %s", str);
        axl_free(addr);
        return NULL;
    }

    /* Cache the string since we already have it */
    axl_ipv4_format(addr->addr, addr->str, sizeof(addr->str));
    addr->str_valid = true;

    return addr;
}

AxlInetAddress *
axl_inet_address_new_from_bytes(const uint8_t *bytes)
{
    AxlInetAddress *addr;

    if (bytes == NULL) {
        return NULL;
    }

    addr = axl_calloc(1, sizeof(*addr));
    if (addr == NULL) {
        axl_debug(
          "axl_inet_address_new_from_bytes: OOM allocating AxlInetAddress (%zu bytes)",
          sizeof(*addr)
          );
        return NULL;
    }

    axl_memcpy(addr->addr, bytes, 4);
    addr->str_valid = false;

    return addr;
}

AxlInetAddress *
axl_inet_address_new_any(void)
{
    AxlInetAddress *addr;

    addr = axl_calloc(1, sizeof(*addr));
    if (addr == NULL) {
        axl_debug(
          "axl_inet_address_new_any: OOM allocating AxlInetAddress (%zu bytes)",
          sizeof(*addr)
          );
        return NULL;
    }
    /* addr->addr is already zeroed by calloc */
    return addr;
}

AxlInetAddress *
axl_inet_address_new_loopback(void)
{
    static const uint8_t lo[] = { 127, 0, 0, 1 };

    return axl_inet_address_new_from_bytes(lo);
}

void
axl_inet_address_free(AxlInetAddress *addr)
{
    axl_free(addr);
}

const char *
axl_inet_address_to_string(AxlInetAddress *addr)
{
    if (addr == NULL) {
        return NULL;
    }

    if (!addr->str_valid) {
        axl_ipv4_format(addr->addr, addr->str, sizeof(addr->str));
        addr->str_valid = true;
    }

    return addr->str;
}

const uint8_t *
axl_inet_address_to_bytes(const AxlInetAddress *addr)
{
    if (addr == NULL) {
        return NULL;
    }

    return addr->addr;
}

bool
axl_inet_address_equal(const AxlInetAddress *a, const AxlInetAddress *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return a->addr[0] == b->addr[0]
        && a->addr[1] == b->addr[1]
        && a->addr[2] == b->addr[2]
        && a->addr[3] == b->addr[3];
}

bool
axl_inet_address_is_any(const AxlInetAddress *addr)
{
    if (addr == NULL) {
        return false;
    }

    return addr->addr[0] == 0
        && addr->addr[1] == 0
        && addr->addr[2] == 0
        && addr->addr[3] == 0;
}

bool
axl_inet_address_is_loopback(const AxlInetAddress *addr)
{
    if (addr == NULL) {
        return false;
    }

    return addr->addr[0] == 127
        && addr->addr[1] == 0
        && addr->addr[2] == 0
        && addr->addr[3] == 1;
}

// ---------------------------------------------------------------------------
// AxlSocketAddress
// ---------------------------------------------------------------------------

AxlSocketAddress *
axl_socket_address_new(AxlInetAddress *addr, uint16_t port)
{
    AxlSocketAddress *sa;

    if (addr == NULL) {
        return NULL;
    }

    sa = axl_calloc(1, sizeof(*sa));
    if (sa == NULL) {
        axl_inet_address_free(addr);
        return NULL;
    }

    sa->address = addr;
    sa->port = port;

    return sa;
}

AxlSocketAddress *
axl_socket_address_new_from_string(const char *str, uint16_t default_port)
{
    AxlInetAddress *addr;
    char host_buf[64];
    uint16_t port;
    const char *colon;
    size_t host_len;

    if (str == NULL) {
        return NULL;
    }

    /* Find the last colon to split host:port */
    colon = NULL;
    for (const char *p = str; *p != '\0'; p++) {
        if (*p == ':') {
            colon = p;
        }
    }

    if (colon != NULL) {
        /* Strict u16 parse rejects empty (trailing colon), non-digit
         * characters, and > 65535. Same correctness as the previous
         * 22-line hand-rolled loop. */
        if (axl_str_to_u16(colon + 1, 10, &port, NULL) != 0) {
            return NULL;
        }
        host_len = (size_t)(colon - str);
    } else {
        port = default_port;
        host_len = axl_strlen(str);
    }

    if (host_len == 0 || host_len >= sizeof(host_buf)) {
        return NULL;
    }

    axl_memcpy(host_buf, str, host_len);
    host_buf[host_len] = '\0';

    addr = axl_inet_address_new_from_string(host_buf);
    if (addr == NULL) {
        return NULL;
    }

    return axl_socket_address_new(addr, port);
}

void
axl_socket_address_free(AxlSocketAddress *sa)
{
    if (sa == NULL) {
        return;
    }

    axl_inet_address_free(sa->address);
    axl_free(sa);
}

AxlInetAddress *
axl_socket_address_get_address(const AxlSocketAddress *sa)
{
    if (sa == NULL) {
        return NULL;
    }

    return sa->address;
}

uint16_t
axl_socket_address_get_port(const AxlSocketAddress *sa)
{
    if (sa == NULL) {
        return 0;
    }

    return sa->port;
}

void
axl_socket_address_to_ipv4(const AxlSocketAddress *sa,
                           AxlIPv4Address *out_addr, uint16_t *out_port)
{
    if (sa == NULL) {
        return;
    }

    if (out_addr != NULL) {
        const uint8_t *bytes = axl_inet_address_to_bytes(sa->address);
        if (bytes != NULL) {
            axl_memcpy(out_addr->addr, bytes, 4);
        }
    }

    if (out_port != NULL) {
        *out_port = sa->port;
    }
}
