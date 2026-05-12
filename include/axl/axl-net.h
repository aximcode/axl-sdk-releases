/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-net.h:
 *
 * Networking umbrella header. Includes socket layer, TCP, UDP, URL,
 * HTTP server, HTTP client, and network utilities.
 *
 * Individual headers can be included separately:
 *   - `#include <axl/axl-inet-address.h>`  — IP address + socket address
 *   - `#include <axl/axl-socket.h>`        — Unified socket
 *   - `#include <axl/axl-socket-client.h>` — DNS + connect helper
 *   - `#include <axl/axl-tcp.h>`           — TCP sockets (low-level)
 *   - `#include <axl/axl-udp.h>`           — UDP sockets (low-level)
 *   - `#include <axl/axl-url.h>`           — URL parsing only
 *   - `#include <axl/axl-http-core.h>`     — HTTP raw-buffer parsers
 *   - `#include <axl/axl-http-server.h>`   — HTTP server
 *   - `#include <axl/axl-http-client.h>`   — HTTP client
 */

#ifndef AXL_NET_H
#define AXL_NET_H

#include <stdint.h>
#include <axl/axl-macros.h>

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
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_net_get_ip_address(
    AxlIPv4Address *addr  ///< receives the IPv4 address
);

/**
 * @brief Send an ICMP echo request and measure round-trip time.
 *
 * @return AXL_OK on success, AXL_ERR on failure or timeout.
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
 * @return AXL_OK on success, AXL_ERR on failure.
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
 * 1. Calls axl_net_ensure_drivers() to locate and load NIC drivers
 *    from the standard driver search path.
 * 2. Connects all SNP handles to trigger protocol stack creation.
 * 3. Selects a NIC (by @p nic_index, or first available if SIZE_MAX).
 * 4. Waits up to @p dhcp_timeout_sec for an IPv4 address via DHCP.
 *
 * @return AXL_OK on success (IP address acquired), AXL_ERR on failure.
 */
int
axl_net_auto_init(
    size_t nic_index,        ///< NIC index (SIZE_MAX = auto-select first)
    size_t dhcp_timeout_sec  ///< DHCP timeout in seconds (0 = 10s default)
);

// ---------------------------------------------------------------------------
// Driver auto-load
// ---------------------------------------------------------------------------

/// axl_net_ensure_drivers() return codes.
#define AXL_NET_DRIVERS_OK         0   ///< SNP is registered (already, or after load)
#define AXL_NET_DRIVERS_NOT_FOUND (-1) ///< no NIC drivers found on any mounted volume
#define AXL_NET_DRIVERS_NO_LINK   (-2) ///< drivers loaded, but no SNP came up

/**
 * @brief Ensure network drivers are loaded and SNP is up.
 *
 * Locates and loads `NetworkCommon.efi` plus a known list of NIC
 * drivers (Realtek, Intel/iPXE, Broadcom/iPXE, USB-CDC ECM/NCM,
 * USB-RNDIS, ASIX-USB) from the standard driver search path used by
 * axl_driver_ensure() — drivers/&lt;arch&gt;/&lt;name&gt; on the booted volume,
 * the image's own directory, drivers/&lt;name&gt; at the volume root, then
 * drivers/&lt;arch&gt;/&lt;name&gt; on every other mounted FAT volume. After
 * loading, ConnectController is run globally to wire the SNP/MNP/
 * IP4/TCP4 stack.
 *
 * Drivers absent from the volume are skipped silently — the cost of a
 * missing entry is one file existence check. Drivers whose hardware
 * isn't present register their binding but never bind to a controller,
 * which is also fine.
 *
 * Short-circuits if an SNP handle already exists. Idempotent — safe to
 * call multiple times.
 *
 * Same trust caveat as axl_driver_ensure: this loads .efi files off
 * any mounted FAT volume with full firmware privileges.
 *
 * Typical use, before touching any networking:
 * @code
 * if (axl_net_ensure_drivers() != AXL_NET_DRIVERS_OK) {
 *     axl_printf("MyTool: networking unavailable\n");
 *     return 1;
 * }
 * @endcode
 *
 * @return AXL_NET_DRIVERS_OK on success;
 *     AXL_NET_DRIVERS_NOT_FOUND if no NIC drivers were found on any
 *     mounted volume; AXL_NET_DRIVERS_NO_LINK if drivers were loaded
 *     but no SNP came up (likely no NIC plugged in).
 */
int
axl_net_ensure_drivers(void);

/**
 * @brief Load drivers, connect SNP, wait for link.
 *
 * Decoupled from address assignment — does NOT run DHCP and does NOT
 * touch IP4Config2. Composes axl_net_ensure_drivers + per-handle
 * SNP reconnect + a 5 s link-up poll, which is the front half of
 * axl_net_auto_init.
 *
 * Used directly by axl_net_bring_up's static-IP path (where the
 * DHCP wait that auto_init would otherwise burn is dead time) and
 * internally by axl_net_auto_init. Consumers that want IP
 * assignment should call axl_net_bring_up or axl_net_auto_init
 * — those layer DHCP / static configuration on top of this primitive.
 *
 * @return AXL_OK on success (at least one NIC link came up); AXL_ERR if
 *     no NIC link was detected within the 5 s wait. Drivers and SNP
 *     reconnect are best-effort — failures there are not surfaced.
 */
int
axl_net_drivers_up(void);

/**
 * @brief Bring up networking with a single call — drivers + DHCP or
 *     static IP + address read-back.
 *
 * Composes the typical "what every networked tool does at startup"
 * sequence into one call so consumers don't reinvent it. Behavior is
 * controlled by @p static_ipv4:
 *
 *   - @p static_ipv4 == NULL → DHCP. Calls axl_net_auto_init
 *     (which itself runs axl_net_drivers_up and waits up to
 *     @p timeout_sec for a lease).
 *
 *   - @p static_ipv4 != NULL → static. Calls axl_net_drivers_up
 *     (load drivers + link wait, no DHCP timeout), then
 *     axl_net_set_static_ip with @p netmask (defaulting to
 *     `255.255.255.0` if NULL) and @p gateway (NULL = no gateway).
 *     Sleeps 500 ms after to let IP4Config2 apply the change — the
 *     firmware applies the policy + address asynchronously and a
 *     subsequent @c GetData can still report the prior state without
 *     the settle.
 *
 * In either case, on success @p addr_out is populated via
 * axl_net_get_ip_address (skipped if @p addr_out is NULL).
 *
 * Used by HTTP services (axl-webfs and similar), REST tools, and
 * one-shot fetch-style utilities — they all open with the same
 * "load drivers, get an IP, here's my address" preamble. AxlService
 * is NOT on the call path; this is plain network bring-up, callable
 * from any AXL-consuming code.
 *
 * @return AXL_OK on success (network up, IP acquired, @p addr_out
 *     populated if non-NULL); AXL_ERR if drivers couldn't be loaded,
 *     no NIC came up, DHCP timed out, or static-IP configuration
 *     failed.
 */
int
axl_net_bring_up(
    size_t            nic_index,    ///< NIC index (SIZE_MAX = auto-select)
    const uint8_t    *static_ipv4,  ///< NULL = DHCP; non-NULL = 4-byte static IPv4
    const uint8_t    *netmask,      ///< 4-byte netmask (NULL = 255.255.255.0); ignored on DHCP path
    const uint8_t    *gateway,      ///< 4-byte gateway (NULL = none); ignored on DHCP path
    size_t            timeout_sec,  ///< DHCP wait (0 = 10 s default; ignored on static path)
    AxlIPv4Address   *addr_out      ///< [out] resolved IPv4 (NULL = caller doesn't care)
);

/**
 * @brief Configure a static IPv4 address on a NIC.
 *
 * Sets the IP4Config2 policy to static and assigns the given address,
 * subnet mask, and optional gateway. Pass NULL for @p gateway to
 * leave it unconfigured.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
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
 * @return AXL_OK on success, AXL_ERR on invalid input.
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
 * @return AXL_OK on success, AXL_ERR if buffer is too small or args are NULL.
 */
int
axl_ipv4_format(
    const uint8_t octets[4],  ///< four octets
    char         *buf,        ///< output buffer
    size_t        size        ///< buffer size (16 bytes sufficient)
);

/**
 * @brief Format 16 IPv6 octets to a colon-separated text representation.
 *
 * Emits the canonical lowercase form with `::` collapsing the longest
 * run of all-zero 16-bit groups, per RFC 5952. Single zero groups are
 * not collapsed; ties go to the leftmost run.
 *
 * Writes at most @p size bytes (including NUL). 40 bytes is always
 * sufficient (max form: "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" + NUL,
 * 39 chars + 1).
 *
 * @return AXL_OK on success, AXL_ERR if buffer is too small or args
 *         are NULL.
 */
int
axl_ipv6_format(
    const uint8_t octets[16],  ///< sixteen octets
    char         *buf,         ///< output buffer
    size_t        size         ///< buffer size (40 bytes sufficient)
);

/// True if @p a equals @p b byte-for-byte.
bool
axl_ipv4_equals(
    const uint8_t a[4],
    const uint8_t b[4]
);

/// True if @p dest is in the same subnet as @p station given @p mask.
/// A zero mask is treated as "no policy" and returns false rather
/// than the technically-true "every IP matches" — callers using this
/// for routing decisions don't want an unconfigured interface to
/// claim every destination.
bool
axl_ipv4_in_subnet(
    const uint8_t dest[4],
    const uint8_t station[4],
    const uint8_t mask[4]
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
 * @return AXL_OK on success, AXL_ERR on error.
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
