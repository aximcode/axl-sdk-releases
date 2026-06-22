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
 * @brief Classification of an ICMP probe reply (see @c axl_net_ping_ex).
 */
typedef enum {
    AXL_PING_NO_REPLY      = 0,  ///< no response within the timeout
    AXL_PING_ECHO_REPLY    = 1,  ///< the target answered — destination reached
    AXL_PING_TIME_EXCEEDED = 2,  ///< TTL expired at a hop (a traceroute hop)
    AXL_PING_UNREACHABLE   = 3,  ///< destination/host/protocol/port unreachable
    AXL_PING_FRAG_NEEDED   = 4,  ///< "fragmentation needed but DF set" (path-MTU)
} AxlPingReply;

/**
 * @brief Result of an @c axl_net_ping_ex probe.
 */
typedef struct {
    AxlPingReply   reply;       ///< what came back (NO_REPLY on timeout)
    AxlIPv4Address responder;   ///< source IP of the reply (the hop, for traceroute)
    size_t         rtt_ms;      ///< round-trip time when a reply arrived (0 otherwise)
    uint16_t       next_mtu;    ///< FRAG_NEEDED: next-hop MTU from the ICMP message (0 if absent)
} AxlPingResult;

/**
 * @brief Send one ICMP echo probe with explicit TTL / Don't-Fragment control.
 *
 * The building block for the two classic ICMP diagnostics:
 * - **traceroute**: increment @p ttl from 1 until the reply is
 *   AXL_PING_ECHO_REPLY (target reached), reading @p out->responder at each
 *   hop's AXL_PING_TIME_EXCEEDED;
 * - **path-MTU discovery**: set @p dont_fragment with a large @p payload_len
 *   and read @p out->next_mtu from an AXL_PING_FRAG_NEEDED reply.
 *
 * @return AXL_OK if the probe completed — inspect @p out->reply; a timeout is
 *     AXL_OK with @p out->reply == AXL_PING_NO_REPLY. AXL_ERR on NULL args or
 *     if no IP4 stack is available.
 *
 * @note AXL_PING_TIME_EXCEEDED / AXL_PING_FRAG_NEEDED require a real
 *     multi-hop / MTU-bottleneck path. QEMU's SLIRP is a single-hop NAT, not a
 *     router, so those reply types only appear on real hardware; the
 *     AXL_PING_ECHO_REPLY path is exercised under QEMU.
 */
int
axl_net_ping_ex(
    AxlIPv4Address *target,        ///< target IPv4 address
    size_t          timeout_ms,    ///< timeout in milliseconds
    uint8_t         ttl,           ///< IP TTL (1 = first hop; 0 selects the default 64)
    bool            dont_fragment, ///< set the IP Don't-Fragment bit
    size_t          payload_len,   ///< ICMP payload bytes (0 selects 56; capped at 1472)
    AxlPingResult  *out            ///< [out] probe result
);

/**
 * @brief Result of an @c axl_sntp_query.
 */
typedef struct {
    int64_t unix_secs;   ///< server time as Unix seconds (UTC); 0 if unreachable
    int32_t offset_ms;   ///< (server time - local RTC) in ms; 0 if the local clock is unknown
    bool    reachable;   ///< true if the server answered
} AxlSntpResult;

/**
 * @brief Query an SNTP/NTP server for the current time (RFC 4330).
 *
 * Sends a client SNTP request over UDP and parses the server's transmit
 * timestamp into Unix seconds. The clock @p offset_ms (server minus the
 * local UEFI RTC) is best-effort — it's 0 when the firmware has no usable
 * real-time clock. @p server may be a hostname (resolved via DNS) or a
 * dotted-decimal IPv4 address.
 *
 * @return AXL_OK if the server answered (@p out->reachable == true with a
 *     valid @p out->unix_secs); AXL_ERR on NULL args, an unresolvable
 *     @p server, no network, or a timeout (@p out->reachable == false).
 */
int
axl_sntp_query(
    const char     *server,      ///< NTP server hostname or dotted-decimal IPv4
    uint16_t        port,        ///< UDP port (0 selects the standard 123)
    size_t          timeout_ms,  ///< response timeout in milliseconds
    AxlSntpResult  *out          ///< [out] query result
);

/**
 * @brief One neighbor/ARP cache entry (IPv4 <-> MAC).
 */
typedef struct {
    uint8_t ip[4];    ///< IPv4 software address
    uint8_t mac[6];   ///< Ethernet hardware address
} AxlArpEntry;

/**
 * @brief Read the ARP (IPv4 neighbor) cache for a network interface.
 *
 * Lists the resolved IPv4<->MAC entries the firmware's ARP layer holds for
 * the @p nic'th ARP-capable interface — the UEFI equivalent of `arp -a` /
 * the neighbor table. Only Ethernet (6-byte MAC) / IPv4 (4-byte) entries are
 * reported.
 *
 * @return AXL_OK on success — @p count is the total entry count (which may
 *     exceed @p cap, signalling truncation; @p out may be NULL to just
 *     count). AXL_ERR on NULL @p count, no ARP-capable interface at @p nic,
 *     or a firmware error. @note The cache only holds neighbors the firmware
 *     has actually resolved (e.g. the gateway after DHCP); a quiet link can
 *     legitimately return count == 0.
 */
int
axl_net_arp_list(
    size_t       nic,     ///< ARP-capable interface index (0 = first)
    AxlArpEntry *out,     ///< [out] caller array (NULL to just count)
    size_t       cap,     ///< capacity of @p out in entries
    size_t      *count    ///< [out] total entries found
);

/**
 * @brief Physical-link statistics for a NIC.
 */
typedef struct {
    uint64_t speed_bps;   ///< link speed in bits/sec, or 0 if the driver doesn't report it
    uint8_t  duplex;      ///< 0 unknown, 1 half, 2 full
    bool     autoneg;     ///< auto-negotiation enabled
    bool     link_up;     ///< media/link present
} AxlNetLinkStats;

/**
 * @brief Read physical-link stats for the @p nic'th SimpleNetwork interface.
 *
 * Reports @p link_up from the firmware's `EFI_SIMPLE_NETWORK_PROTOCOL` media
 * state — the reliably-available field.
 *
 * @note UEFI exposes **no portable link-speed/duplex/auto-neg** surface:
 *     SimpleNetwork has no speed field, and where a driver reports it at all
 *     it's via vendor `EFI_ADAPTER_INFORMATION_PROTOCOL` info types this
 *     reader does not decode. So @p speed_bps is typically 0, @p duplex 0
 *     (unknown), @p autoneg false — including under QEMU. Treat them as
 *     best-effort and @p link_up as authoritative.
 *
 * @return AXL_OK on success (@p out filled); AXL_ERR on NULL @p out or no
 *     SimpleNetwork interface at @p nic.
 */
int
axl_net_get_link_stats(
    size_t           nic,   ///< SimpleNetwork interface index (0 = first)
    AxlNetLinkStats *out    ///< [out] link stats
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
 * AxlNetResolveDoneFn:
 *
 * Completion callback for axl_net_resolve_async. @p addr is the resolved
 * address on success — borrowed, valid only for the duration of the call, so
 * copy it if needed — or NULL on failure. @p st is AXL_OK, an error, a
 * timeout, or AXL_CANCELLED.
 */
typedef void (*AxlNetResolveDoneFn)(
    const AxlIPv4Address *addr,  ///< resolved address (borrowed), or NULL on failure
    AxlStatus             st,    ///< AXL_OK, an error, timeout, or AXL_CANCELLED
    void                 *user   ///< opaque context
);

/**
 * @brief Resolve a hostname to an IPv4 address asynchronously on @p loop.
 *
 * The async peer of axl_net_resolve. The DNS4 query runs as an event source
 * on @p loop (no nested loop), so it is safe to call from inside a loop
 * callback or a resident driver-pump at raised TPL — where the sync
 * axl_net_resolve would nest an ephemeral loop and warn. @p cb fires exactly
 * once: on success, failure, AXL_CANCELLED, or the internal timeout (a fixed
 * 5 s, matching the sync resolver; there is no client to inherit one from). A
 * dotted-decimal IP literal still completes via @p cb on a later tick (the
 * callback is always deferred, never invoked re-entrantly from within this
 * call). Each call is self-contained (its own DNS4 child); the callback owns
 * nothing to free.
 *
 * @return AXL_OK if the query was initiated (@p cb WILL fire), or an error if
 *     it could not start (in which case @p cb does NOT fire).
 */
int
axl_net_resolve_async(
    const char       *hostname,  ///< hostname or IPv4 literal
    AxlLoop          *loop,      ///< loop to drive the query on
    AxlCancellable   *cancel,    ///< optional cancel token (NULL = uncancellable)
    AxlNetResolveDoneFn  cb,        ///< completion callback (required)
    void             *user       ///< opaque context for @p cb
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
 * @brief Take over the NIC with staged drivers — only if the firmware
 *     provides NO SimpleNetwork stack of its own.
 *
 * For OEM firmware that exposes its NIC only through proprietary drivers and
 * publishes ZERO SimpleNetwork handles. This disconnects the firmware's
 * controllers and connects AXL's staged drivers so a standard SNP/MNP/IP4/TCP4
 * stack comes up, then reconnects the stack.
 *
 * Condition-gated by design: if any SimpleNetwork handle is already present
 * — before, or after a plain @c axl_net_ensure_drivers attempt — this is a
 * NO-OP and returns AXL_OK. An over-eager takeover **destroys a working
 * firmware stack** (observed: forcing iPXE + disconnect on a box that already
 * had SNP killed networking), so the guard is the point: takeover runs only
 * when there is provably nothing to lose.
 *
 * @return AXL_OK if a SimpleNetwork handle is present after the call (whether
 *     it was already there — the no-op case — or the takeover brought one up);
 *     AXL_ERR if no SNP handle exists even after the takeover attempt.
 */
int
axl_net_takeover_if_no_snp(void);

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

/**
 * @brief Set the DNS resolver(s) on a NIC.
 *
 * Programs the IP4Config2 DNS-server list (Ip4Config2DataTypeDnsServer) —
 * the missing setter beside axl_net_resolve (which only *queries* whatever
 * resolver is configured). Pass a secondary in @p dns2, or NULL for a
 * single resolver. Works on both the static and DHCP paths (a DHCP box can
 * override the leased resolver). @p nic_index is a concrete index into the
 * interface list (it does NOT accept AXL_NET_NIC_AUTO).
 *
 * @return AXL_OK on success; AXL_ERR on NULL @p dns, no IP4Config2 on the
 *     NIC, or a SetData failure.
 */
int
axl_net_set_dns(
    size_t         nic_index,   ///< concrete NIC index (from axl_net_list_interfaces)
    const uint8_t  dns[4],      ///< primary DNS server IPv4
    const uint8_t *dns2         ///< secondary DNS server IPv4 (NULL = none)
);

/**
 * @brief Set the box's hostname (persisted).
 *
 * **What this does and does not do.** UEFI has no standard, firmware-
 * advertised hostname (IP4Config2 carries IP / gateway / DNS but not a
 * name, and there is no portable DHCP-option-12 hook through the
 * IP4Config2 DHCP policy). So this persists @p name to a dedicated AXL
 * non-volatile variable: a single source of truth an on-box UI sets +
 * displays, configuration tools read, and an AXL-aware DHCP/identity
 * consumer can advertise. It does NOT, by itself, cause the firmware's
 * DHCP client to send the name. Read it back with axl_net_get_hostname.
 *
 * @return AXL_OK on success; AXL_ERR on NULL/empty @p name, a too-long
 *     name, or a non-volatile-store write failure.
 */
int
axl_net_set_hostname(
    const char *name   ///< hostname (1..63 chars)
);

/**
 * @brief Read the persisted hostname set by axl_net_set_hostname.
 *
 * Writes the stored hostname (NUL-terminated, truncated to @p size) into
 * @p buf. "Unset" is a normal, expected state (a fresh box), not an error:
 * it returns AXL_OK with @p buf == "". Since axl_net_set_hostname rejects an
 * empty name, "" unambiguously means "no hostname configured". AXL_ERR is
 * reserved for genuine faults (NULL @p buf, zero @p size).
 *
 * @return AXL_OK (a hostname, or "" when none is set); AXL_ERR on NULL
 *     @p buf or zero @p size.
 */
int
axl_net_get_hostname(
    char   *buf,    ///< [out] hostname text ("" if none set)
    size_t  size    ///< capacity of @p buf in bytes (64 is sufficient)
);

/**
 * @brief Wait until an IP4Config2 address change has settled.
 *
 * IP4Config2 applies a policy / manual-address change asynchronously, so a
 * read-back (axl_net_get_ip_address) immediately after axl_net_set_static_ip
 * can still report the prior state. This polls the NIC's InterfaceInfo until
 * the address has taken or the timeout elapses — the diagnostic-correct
 * replacement for a blind sleep, so a UI knows exactly when its read-back is
 * valid.
 *
 * @p expect_ipv4 selects the settle signal:
 *   - non-NULL — wait until StationAddress **equals** these 4 octets. This is
 *     the strong check after a static set: a stale prior non-zero address does
 *     NOT satisfy it, closing the "read back the old IP" race. (Used by
 *     axl_net_init_static, which knows the address it just set.)
 *   - NULL — wait until StationAddress is merely non-zero (any address taken),
 *     e.g. after kicking DHCP when the specific lease isn't known up front.
 *
 * @p nic_index is a concrete index (it does NOT accept AXL_NET_NIC_AUTO). A
 * settle is sub-second, so the @p timeout_ms == 0 default is a deliberately
 * short 1 s (not the 10 s DHCP-wait default).
 *
 * @return AXL_OK once the address is observed; AXL_ERR on a bad NIC index /
 *     no IP4Config2, or if it has not settled within @p timeout_ms.
 */
int
axl_net_wait_ip_settled(
    size_t         nic_index,    ///< concrete NIC index (from axl_net_list_interfaces)
    const uint8_t *expect_ipv4,  ///< 4 octets to wait for, or NULL = any non-zero
    size_t         timeout_ms    ///< max wait in ms (0 = 1 s default)
);

// ---------------------------------------------------------------------------
// DHCP lease view
// ---------------------------------------------------------------------------

/**
 * @brief A NIC's active DHCP-leased configuration, as held by the firmware's
 *     persistent IP4Config2 layer.
 *
 * The leased *configuration* — address, mask, gateway, resolver(s) — that the
 * firmware applied and keeps live across application exits (IP4Config2 is a
 * resident DXE driver, so this survives the tool that read it). A zeroed
 * field means it is not configured (`router` all-zero = no default gateway).
 * `dns_count` is 0..2 (the first two resolvers).
 *
 * Intentionally NOT here: the DHCP *lease lifetimes* (lease/T1/T2 seconds),
 * the granting server's identity, and the domain-name option. IP4Config2
 * discards those once it applies the lease (it stops its internal DHCP client
 * and reconfigures the IP4 stack statically), so they cannot be read back
 * from a transient tool. Surfacing them requires a resident driver that owns
 * a bound DHCP client for the box's lifetime — out of scope for this view.
 */
typedef struct {
    uint8_t  address[4];   ///< leased client IPv4
    uint8_t  subnet[4];    ///< subnet mask
    uint8_t  router[4];    ///< default gateway (0.0.0.0 = none)
    uint8_t  dns[2][4];    ///< resolver(s); valid entries given by dns_count
    uint8_t  dns_count;    ///< number of valid entries in dns (0..2)
} AxlDhcpLease;

/**
 * @brief Read a NIC's active DHCP-leased configuration.
 *
 * Reports the live IP4Config2 configuration for a NIC whose policy is DHCP:
 * the leased address / mask / default gateway (from the interface's route
 * table) and the DHCP-provided resolver(s). A purely local, synchronous read
 * (no network round-trip). A NIC on a static policy, or one that has not yet
 * leased an address, is not a DHCP lease and returns AXL_ERR.
 *
 * @warning @p nic_index indexes the IP4Config2 handle buffer, which is NOT the
 *     same index space as @c axl_net_list_interfaces / @c
 *     axl_net_get_link_stats (those index the SimpleNetwork handle buffer, and
 *     IP4Config2 lives on a child handle on some OEM firmware). An out-of-range
 *     index is clamped to the first handle. On a multi-NIC box, passing a
 *     list-index here can therefore return a different NIC's lease. Use
 *     @c axl_net_get_dhcp_lease_by_mac to look a lease up unambiguously by
 *     the NIC's MAC (the stable key from @c AxlNetInterface.mac). It does NOT
 *     accept AXL_NET_NIC_AUTO.
 *
 * @return AXL_OK with @p out filled; AXL_ERR on NULL @p out, a bad NIC index,
 *     no IP4Config2 on the NIC, a non-DHCP policy, or no leased address.
 */
int
axl_net_get_dhcp_lease(
    size_t        nic_index,  ///< IP4Config2-handle index (see @warning; not the SNP/list index)
    AxlDhcpLease *out         ///< [out] leased configuration
);

/**
 * @brief Read a NIC's active DHCP-leased configuration, keyed by MAC.
 *
 * The robust counterpart to @c axl_net_get_dhcp_lease for multi-NIC hosts:
 * resolves the IP4Config2 instance by matching its SimpleNetwork MAC to @p mac
 * (the same MAC correlation @c axl_net_list_interfaces uses to fill its IPv4
 * columns), so the result is correct regardless of IP4Config2-vs-SNP handle
 * ordering. The reported fields and the DHCP-policy / leased-address
 * preconditions are identical to @c axl_net_get_dhcp_lease.
 *
 * @p mac is the stable key paired with @c AxlNetInterface.mac: iterate
 * @c axl_net_list_interfaces, then call this with a row's @c mac. Correlation
 * is by exact 6-byte match and assumes MACs are unique; if two NICs share a MAC
 * the first match in enumeration order wins. @p out is fully zeroed before any
 * field is set, so on AXL_ERR every field reads 0.
 *
 * @return AXL_OK with @p out filled; AXL_ERR on NULL @p mac or @p out, no
 *     IP4Config2 NIC carrying that MAC, a non-DHCP policy, or no leased address.
 */
int
axl_net_get_dhcp_lease_by_mac(
    const uint8_t  mac[6],  ///< NIC MAC (from AxlNetInterface.mac)
    AxlDhcpLease  *out      ///< [out] leased configuration
);

// ---------------------------------------------------------------------------
// Which mechanism brought the NIC up (IP4Config2-free bring-up ladder)
// ---------------------------------------------------------------------------

/**
 * @brief How @c axl_net_init / @c axl_net_auto_init last configured a NIC.
 *
 * The standard path is the firmware's `EFI_IP4_CONFIG2_PROTOCOL` policy layer.
 * Some OEM firmware (e.g. HP business laptops) ships a full network stack —
 * SimpleNetwork, MNP/ARP/IP4/TCP4/UDP4 and `Dhcp4ServiceBinding` — but **no**
 * IP4Config2. On such firmware the bring-up transparently falls back down a
 * ladder; this enum records which rung succeeded so a consumer can surface it
 * (e.g. "no IP4Config2 — configured via DHCP4-SB") instead of a mystery.
 */
typedef enum {
    AXL_NET_CONFIG_NONE = 0,    ///< not configured (no bring-up has succeeded)
    AXL_NET_CONFIG_IP4CONFIG2,  ///< via EFI_IP4_CONFIG2_PROTOCOL (the standard path)
    AXL_NET_CONFIG_DHCP4_SB,    ///< via EFI_DHCP4_SERVICE_BINDING (no IP4Config2)
    AXL_NET_CONFIG_PXE_BC,      ///< via EFI_PXE_BASE_CODE_PROTOCOL.Dhcp (last resort)
} AxlNetConfigMethod;

/**
 * @brief The mechanism that configured the NIC on the last bring-up.
 *
 * Reflects the most recent @c axl_net_init / @c axl_net_auto_init /
 * @c axl_net_bring_up call in this process. @c AXL_NET_CONFIG_NONE before
 * any successful bring-up (or after one that failed). Process-global, not
 * per-NIC — matches the single-NIC focus of the bring-up helpers.
 *
 * @return the active @c AxlNetConfigMethod.
 */
AxlNetConfigMethod
axl_net_last_config_method(void);

// ---------------------------------------------------------------------------
// Reverse DNS (PTR)
// ---------------------------------------------------------------------------

/**
 * @brief Reverse-DNS (PTR) lookup: an IPv4 address to a hostname.
 *
 * The reverse of axl_net_resolve — queries the in-addr.arpa PTR record for
 * @p ip via DNS4 and writes the NUL-terminated name (truncated to @p cap)
 * into @p out. "No PTR record for this address" is a normal negative result
 * (most addresses have none), reported as AXL_ERR — not a distinct status
 * from a transport fault, matching axl_net_resolve's forward direction.
 *
 * @return AXL_OK with @p out filled; AXL_ERR on NULL @p ip / @p out, zero
 *     @p cap, no DNS4 resolver configured, or no PTR record for @p ip.
 */
int
axl_net_resolve_ptr(
    const AxlIPv4Address *ip,    ///< address to look up
    char                 *out,   ///< [out] hostname buffer (NUL-terminated)
    size_t                cap    ///< capacity of @p out in bytes
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

// ===========================================================================
//
//  Per-NIC driver identity + bus location
//
// ===========================================================================

/**
 * @brief Bound-driver identity and bus location for one NIC.
 *
 * Resolved by axl_net_get_driver_info(). Kept separate from
 * AxlNetInterface deliberately: the resolution walks
 * OpenProtocolInformation + the driver image's device path + the
 * firmware DevicePathToText protocol, which is far heavier than the
 * plain SNP enumeration axl_net_list_interfaces() does — and that
 * listing is polled on a 100 ms tick during link bring-up. A UI fills
 * in the driver/bus columns by calling this once per row, off the hot
 * path.
 */
typedef struct {
    /// Bound driver's image name. Four states, distinguishable by value:
    ///   - the `.efi` filename for a disk-loaded driver
    ///     (e.g. "ipxe-intel.efi");
    ///   - "<firmware volume>" for a driver dispatched from a firmware
    ///     volume (e.g. OVMF's VirtioNetDxe — its image path is an FV
    ///     file GUID, not a printable name);
    ///   - "<unknown>" when a driver IS bound but its image name can't be
    ///     resolved (the loaded image has no usable file path);
    ///   - "" only when no driver is bound to the NIC (no BY_DRIVER agent
    ///     on any of NII3.1 / NII / SNP). In that case @c layer is "" too.
    char driver[64];
    /// Protocol layer the driver bound at — a closed set: "NII3.1" or
    /// "NII" when the binding installs EFI_NETWORK_INTERFACE_IDENTIFIER
    /// (UEFI driver-model NIC drivers — iPXE, vendor UNDI), "SNP" when the
    /// driver publishes Simple Network Protocol directly, or "" when no
    /// driver is bound (paired with @c driver == "").
    char layer[16];
    /// Stable hardware location, derived from the NIC's UEFI device path
    /// with the network-addressing tail (MAC/IPv4/IPv6/VLAN/…) trimmed:
    /// the PCI/USB topology that anchors the NIC to a physical slot/port
    /// — e.g. "PciRoot(0x0)/Pci(0x3,0x0)" (PCI) or
    /// "PciRoot(0x0)/Pci(0x1,0x0)/USB(0x1,0x0)" (USB). Stable across
    /// reboots (unlike the enumeration index) and distinguishes two
    /// otherwise-identical NICs. NUL-terminated and truncated (never
    /// overflowed) if a deeply-nested path exceeds the field. "" when the
    /// device path is unavailable or the firmware DevicePathToText
    /// protocol is absent.
    char bus_location[160];
} AxlNetDriverInfo;

/**
 * @brief Resolve which driver is bound to a NIC and where it sits on
 *        the bus.
 *
 * Correlates by MAC: enumerates SNP handles, finds the one whose
 * controller MAC equals @p mac (matching by MAC rather than handle
 * index because some OEM firmware binds the addressing protocols to a
 * child handle), then resolves the bound driver image name + binding
 * layer and the trimmed device-path bus location into @p out.
 *
 * The bound driver is found by walking past the SNP wrapper to the NII
 * installer (the layer that actually claims the NIC) when present, and
 * falling back to the SNP-installer agent otherwise — so the answer is
 * the driver that owns the hardware, not the generic SnpDxe shim on top.
 *
 * MAC is the stable key paired with AxlNetInterface.mac: iterate the
 * interfaces from axl_net_list_interfaces(), then call this per row to
 * fill the driver/bus columns. @p out is fully zeroed before any field
 * is set, so on AXL_ERR every field reads "" / 0.
 *
 * Correlation is by exact 6-byte match and assumes MACs are unique. If
 * two NICs share a MAC — including the all-zero MAC some adapters report
 * before link/SET_ADDRESS — this returns the first match in enumeration
 * order; a UI needing a guaranteed 1:1 mapping should anchor on
 * @c bus_location, not MAC alone.
 *
 * @return AXL_OK if a NIC with @p mac was found (fields are populated;
 *     @c driver / @c layer are "" if no driver is bound); AXL_ERR if
 *     @p mac or @p out is NULL, or no SNP handle carries that MAC.
 */
int
axl_net_get_driver_info(
    const uint8_t     mac[6],  ///< NIC MAC (from AxlNetInterface.mac)
    AxlNetDriverInfo *out      ///< [out] resolved driver identity + bus location
);

// ===========================================================================
//
//  Driver selection — list, try one, connect the stack
//
// ===========================================================================

/**
 * @brief A NIC-driver .efi discovered on the standard search path.
 */
typedef struct {
    /// Driver filename, e.g. "ipxe-intel.efi". NOT unique on its own —
    /// the same filename can be staged on more than one volume. Use
    /// @c path as the unambiguous key when calling axl_net_try_driver().
    char     name[64];
    /// Full UEFI path where the file was found (the dedup key — entries
    /// are unique by path). Pass this to axl_net_try_driver() to try
    /// exactly this file rather than first-match-by-name.
    char     path[256];
    /// File size in bytes — lets a UI tell a heavyweight iPXE build
    /// (~280 KB-1 MB) from a lightweight vendor UNDI shim (~20-200 KB).
    uint64_t size;
} AxlNetDriverFile;

/**
 * @brief List the NIC-driver files available to try on the driver
 *        search path.
 *
 * Scans `drivers/&lt;arch&gt;/` on every mounted FAT volume (the same
 * search territory axl_driver_locate / axl_net_ensure_drivers use) for
 * `.efi` / `.efidrv` files and reports each as an AxlNetDriverFile, so
 * a UI can offer "try driver X / Y / Z". Entries are de-duplicated by
 * full path. No driver is loaded — this only enumerates files.
 *
 * Unlike axl_net_ensure_drivers (which loads a curated known list),
 * this reports whatever is staged, including drivers AXL doesn't know
 * by name — a technician can drop a vendor driver into `drivers/<arch>/`
 * and have it appear here.
 *
 * Follows the query-count convention of axl_net_list_interfaces: call
 * with @p out = NULL to learn the count, then again with a sized buffer.
 *
 * @return AXL_OK on success (including zero drivers found); AXL_ERR if
 *     @p count is NULL.
 */
int
axl_net_list_available_drivers(
    AxlNetDriverFile *out,    ///< output array (NULL to query count)
    size_t           *count   ///< [in/out] capacity / entries filled
);

/// Max NIC MACs reported in one AxlNetTryResult.
#define AXL_NET_TRY_MAX_MACS  8

/**
 * @brief Outcome of an axl_net_try_driver() attempt.
 *
 * Fully zeroed before the attempt and always populated when @p out is
 * non-NULL — including on AXL_ERR, so the caller can tell "driver not
 * found" from "loaded but bound no NIC". On an early-out error every
 * field reads false / 0.
 */
typedef struct {
    bool     found;             ///< the driver file was located on the search path
    bool     loaded;            ///< LoadImage + StartImage succeeded
    /// The freshly-loaded image was unloaded again (failure rollback).
    /// false on success — the driver stays resident. Reflects "unload was
    /// attempted and reported success", which for the iPXE failure path
    /// is NOT a guarantee its LoadImage hook was reverted (see
    /// axl_net_try_driver).
    bool     unloaded;
    /// Count of NICs that newly produced an SNP handle as a result of
    /// this attempt — the honest attribution (the set of SNP handles
    /// present after connect that were not present before). This is the
    /// true count and may exceed AXL_NET_TRY_MAX_MACS; @c bound_nic_macs
    /// holds the first AXL_NET_TRY_MAX_MACS of them.
    uint32_t snp_handles_added;
    bool     link_up;           ///< at least one newly-bound NIC reports media present
    /// Number of MACs filled in @c bound_nic_macs — min(@c
    /// snp_handles_added, AXL_NET_TRY_MAX_MACS). Equals @c
    /// snp_handles_added unless more than AXL_NET_TRY_MAX_MACS NICs bound.
    size_t   bound_nic_count;
    /// MACs of the NICs that newly produced an SNP handle (first
    /// @c bound_nic_count entries valid).
    uint8_t  bound_nic_macs[AXL_NET_TRY_MAX_MACS][6];
} AxlNetTryResult;

/**
 * @brief Load one specific driver, connect it, and report whether it
 *        brought any NIC up — with a clean unload on failure.
 *
 * The selective-retry primitive a "my NIC needs a different driver"
 * tool is built on: load + start @p path_or_name, run
 * axl_net_connect_stack(), and measure the delta in SNP handles. The
 * MACs of the NICs that newly came up are reported in @p out so the UI
 * can attribute the result. If nothing bound (snp_handles_added == 0)
 * or load/start failed, the freshly-loaded image is unloaded again
 * (@c out->unloaded = true) so the next candidate can be tried from a
 * clean slate.
 *
 * @p path_or_name may be a full UEFI path or a bare filename; a bare
 * name is resolved through axl_driver_locate()'s search path (so a name
 * from axl_net_list_available_drivers() works directly).
 *
 * **iPXE must be tried LAST.** When @p path_or_name is *recognized* as
 * an iPXE driver — by filename heuristic, the same name list
 * axl_net_ensure_drivers uses — this disarms iPXE's 5-minute
 * boot-services watchdog for you. Detection is best-effort: an iPXE-
 * derived driver under an unrecognized filename (e.g. a relabeled vendor
 * build) gets neither the watchdog disarm nor any ordering protection.
 * Regardless, iPXE's LoadImage hook breaks subsequent `.efi` loads in
 * the same session — a hazard no unload can undo — so the *caller* must
 * order any iPXE attempt after every other candidate. On the failure
 * path the unload is still attempted, but its effect on an already-hooked
 * session is not guaranteed (hence @c out->unloaded means "unload
 * returned success", not "hook reverted").
 *
 * **MediaPresent is advisory.** Some firmware misreports link state, so
 * @c link_up == false is *not* a failure: an attempt that adds an SNP
 * handle succeeds (AXL_OK) regardless of reported link. Treat @c link_up
 * as a hint, not a gate.
 *
 * Same trust model as axl_driver_load: this executes a `.efi` off a
 * mounted FAT volume with full firmware privileges. Don't pass an
 * attacker-controlled @p path_or_name. Call at TPL_APPLICATION
 * (LoadImage/StartImage/ConnectController require it).
 *
 * @return AXL_OK if the driver loaded and added at least one SNP handle
 *     (@c out->snp_handles_added &gt; 0); AXL_ERR otherwise — driver not
 *     found, load/start failed, or it bound no NIC. @p out (if non-NULL)
 *     carries the detailed outcome in every case.
 */
int
axl_net_try_driver(
    const char      *path_or_name,  ///< driver path, or a bare name resolved on the search path
    AxlNetTryResult *out            ///< [out] attempt outcome (may be NULL)
);

/**
 * @brief Connect the driver stack onto every SNP handle.
 *
 * Runs a global ConnectController (mirroring shell `connect -r`) and
 * then a per-SNP-handle reconnect, wiring the MNP / IP4 / TCP4 / UDP4
 * stack on top of each NIC. This is the step buried inside
 * axl_net_ensure_drivers / axl_net_drivers_up, exposed on its own for
 * two cases:
 *
 *   - **ARM64 firmware that doesn't auto-connect.** Some platforms leave
 *     a bound NIC without its protocol stack until ConnectController
 *     runs; a "my NIC isn't showing up" action can call this without a
 *     full network re-init.
 *   - Internally by axl_net_try_driver(), after loading a candidate.
 *
 * Idempotent and side-effect-light — safe to call repeatedly. Call at
 * TPL_APPLICATION (ConnectController requires it).
 *
 * @return AXL_OK (the underlying connects are best-effort; a handle that
 *     declines to bind is not an error).
 */
int
axl_net_connect_stack(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NET_H */
