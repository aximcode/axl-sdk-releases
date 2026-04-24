/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-net.h:
 *
 * Networking umbrella header. Includes socket layer, TCP, UDP, URL,
 * HTTP server, HTTP client, and network utilities.
 *
 * Individual headers can be included separately:
 *   #include <axl/axl-inet-address.h>  -- IP address + socket address
 *   #include <axl/axl-socket.h>        -- Unified socket
 *   #include <axl/axl-socket-client.h> -- DNS + connect helper
 *   #include <axl/axl-tcp.h>           -- TCP sockets (low-level)
 *   #include <axl/axl-udp.h>           -- UDP sockets (low-level)
 *   #include <axl/axl-url.h>           -- URL parsing only
 *   #include <axl/axl-http-core.h>     -- HTTP raw-buffer parsers
 *   #include <axl/axl-http-server.h>   -- HTTP server
 *   #include <axl/axl-http-client.h>   -- HTTP client
 */

#ifndef AXL_NET_H
#define AXL_NET_H

#include <stdint.h>

/* AxlIPv4Address (legacy IPv4 type) is declared in axl-inet-address.h
 * alongside AxlInetAddress / AxlSocketAddress. Pull that in first so
 * the network-utility function declarations below can use it. */
#include <axl/axl-inet-address.h>

#include <axl/axl-loop.h>
#include <axl/axl-tcp.h>
#include <axl/axl-udp.h>
#include <axl/axl-url.h>
#include <axl/axl-http-core.h>
#include <axl/axl-http-server.h>
#include <axl/axl-http-client.h>
#include <axl/axl-tls.h>
#include <axl/axl-socket.h>
#include <axl/axl-socket-client.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
//
//  Network Utilities
//
// ===========================================================================

/**
 * @brief Get the local IPv4 address of the first configured NIC.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_net_get_ip_address(
    AxlIPv4Address *addr  ///< receives the IPv4 address
);

/**
 * @brief Send an ICMP echo request and measure round-trip time.
 *
 * @return 0 on success, -1 on failure or timeout.
 */
int
axl_net_ping(
    AxlIPv4Address *target,       ///< target IPv4 address
    size_t          timeout_ms,   ///< timeout in milliseconds
    size_t         *out_rtt_ms    ///< receives round-trip time in milliseconds
);

/**
 * @brief Resolve a hostname to an IPv4 address via DNS4.
 * Falls back to parsing the hostname as a dotted-decimal IP.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_net_resolve(
    const char     *hostname,  ///< hostname or IP string
    AxlIPv4Address *addr       ///< receives the resolved address
);

/**
 * @brief Check whether any IPv4 network is available.
 *
 * @return true if at least one NIC has an IP address.
 */
bool
axl_net_is_available(void);

/**
 * @brief Bring up networking: load drivers, run DHCP, wait for IP.
 *
 * Performs a best-effort network initialization sequence:
 * 1. If no SNP handles exist, attempts to load NIC drivers from
 *    the "drivers" subdirectory next to the running application.
 * 2. Connects all SNP handles to trigger protocol stack creation.
 * 3. Selects a NIC (by @p nic_index, or first available if SIZE_MAX).
 * 4. Waits up to @p dhcp_timeout_sec for an IPv4 address via DHCP.
 *
 * @return 0 on success (IP address acquired), -1 on failure.
 */
int
axl_net_auto_init(
    size_t nic_index,        ///< NIC index (SIZE_MAX = auto-select first)
    size_t dhcp_timeout_sec  ///< DHCP timeout in seconds (0 = 10s default)
);

/**
 * @brief Configure a static IPv4 address on a NIC.
 *
 * Sets the IP4Config2 policy to static and assigns the given address,
 * subnet mask, and optional gateway. Pass NULL for @p gateway to
 * leave it unconfigured.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_net_set_static_ip(
    size_t         nic_index,   ///< NIC index (from axl_net_list_interfaces)
    const uint8_t  ip[4],       ///< IPv4 address
    const uint8_t  netmask[4],  ///< subnet mask (e.g. {255,255,255,0})
    const uint8_t *gateway      ///< gateway address (NULL = none)
);

// ---------------------------------------------------------------------------
// IPv4 address parsing / formatting
// ---------------------------------------------------------------------------

/**
 * @brief Parse a dotted-decimal IPv4 address string.
 *
 * Accepts strings like "192.168.1.1". Each octet must be 0-255.
 * No leading zeros validation — "01.02.03.04" is accepted.
 *
 * @return 0 on success, -1 on invalid input.
 */
int
axl_ipv4_parse(
    const char *str,        ///< IPv4 string (e.g. "192.168.1.1")
    uint8_t     octets[4]   ///< receives the four octets
);

/**
 * @brief Format an IPv4 address as a dotted-decimal string.
 *
 * Writes at most @p size bytes (including NUL). 16 bytes is always
 * sufficient ("255.255.255.255" + NUL).
 *
 * @return 0 on success, -1 if buffer is too small or args are NULL.
 */
int
axl_ipv4_format(
    const uint8_t octets[4],  ///< four octets
    char         *buf,        ///< output buffer
    size_t        size        ///< buffer size (16 bytes sufficient)
);

// ===========================================================================
//
//  Network Interface Enumeration
//
// ===========================================================================

/**
 * @brief Network interface descriptor.
 */
typedef struct {
    char     name[32];      ///< interface name ("eth0", "eth1", ...)
    uint8_t  mac[6];        ///< MAC address
    bool     link_up;       ///< true if link is up
    uint32_t mtu;           ///< maximum transmission unit
    bool     has_ipv4;      ///< true if IPv4 is configured
    uint8_t  ipv4[4];       ///< IPv4 address (valid if has_ipv4)
    uint8_t  netmask[4];    ///< subnet mask (valid if has_ipv4)
    uint8_t  gateway[4];    ///< default gateway (valid if has_ipv4)
} AxlNetInterface;

/**
 * @brief List available network interfaces.
 *
 * Fills @a out with up to @a *count interface descriptors.
 * On return, @a *count is set to the number of entries filled.
 * Call with @a out=NULL to query the number of interfaces.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_net_list_interfaces(
    AxlNetInterface *out,   ///< output array (NULL to query count)
    size_t          *count  ///< [in/out] capacity / entries filled
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NET_H */
