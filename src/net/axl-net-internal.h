/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-internal.h
    Private header for axl-net internal functions shared between
    axl-http-server, axl-http-client, and axl-http-core.
**/

#ifndef AXL_NET_INTERNAL_H
#define AXL_NET_INTERNAL_H

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-net.h>

// ---------------------------------------------------------------------------
// Service-binding locator — shared between TCP, UDP, and any future
// per-NIC protocol that follows the select-then-CreateChild pattern.
// Walks all handles publishing @p sb_guid:
//   1. forced_source non-NULL: pick the handle whose IP4Config2
//      InterfaceInfo.StationAddress exactly matches (or fail).
//   2. Else skip handles with StationAddress == 0.0.0.0.
//   3. Else prefer a handle whose subnet contains @p dest.
//   4. Else fall back to the first valid handle.
// `dest == NULL` and `forced_source == NULL` is the default auto
// path (e.g. for a listener with no destination). Returns
// EFI_NOT_FOUND if no usable handle exists.
// ---------------------------------------------------------------------------
EFI_STATUS
axl_net_locate_sb(
    const EFI_GUID                 *sb_guid,
    const EFI_IPv4_ADDRESS         *dest,
    const EFI_IPv4_ADDRESS         *forced_source,
    EFI_SERVICE_BINDING_PROTOCOL  **sb,
    EFI_HANDLE                     *out_handle
);

// ---------------------------------------------------------------------------
// Driver-selection internals (axl-net-driver-select.c / axl-net-dhcp.c)
// ---------------------------------------------------------------------------

/// Reconnect the driver stack onto every SNP handle (per-handle
/// ConnectController). Shared by axl_net_drivers_up (axl-net-dhcp.c) and
/// axl_net_connect_stack (axl-net-driver-select.c).
void
_axl_net_connect_snp_handles(void);

/// Copy the lease cached by a non-IP4Config2 bring-up (Dhcp4-SB / PXE BC) into
/// @p out. Lets the IP4Config2-keyed readers (axl_net_get_ip_address /
/// axl_net_get_dhcp_lease) report a result on firmware that lacks IP4Config2.
/// @return true if a cached lease exists (and was copied); false otherwise.
bool
_axl_net_fallback_lease(
    AxlDhcpLease *out
);

/// Render a NIC's bus location (PCI/USB device-path topology, with the
/// MAC/IPv4/IPv6/VLAN network tail trimmed) into @p out. Always
/// NUL-terminates; "" when the path has no hardware prefix or the
/// firmware DevicePathToText protocol is unavailable. Exposed for unit
/// testing against synthetic device paths.
/// @return AXL_OK on success, AXL_ERR on NULL @p device_path / @p out or
///     zero @p out_size.
int
_axl_net_bus_location(
    const void *device_path,  ///< raw EFI device path
    char       *out,          ///< [out] bus-location string
    size_t      out_size      ///< capacity of @p out in bytes
);

// ---------------------------------------------------------------------------
// SNP MAC access — shared by axl-net-nic.c (registry build + dedup),
// axl-net-arp.c (ordinal -> MAC -> ARP service binding), and axl-net-dhcp.c
// (MAC -> IP4Config2 lookup). One rule, one place: the three call sites used
// to guard HwAddressSize independently and drifted apart (axl-net-dhcp.c's
// ip4cfg_for_mac clamped to a *prefix* match instead of rejecting a short
// address), so a NIC that axl-net-nic.c's registry (and therefore
// axl_net_list_interfaces) would skip could still resolve through
// axl_net_get_dhcp_lease_by_mac -- two public APIs disagreeing about which
// NICs exist.
// ---------------------------------------------------------------------------

/// SNP protocol on @p h, or NULL if the handle doesn't publish one.
EFI_SIMPLE_NETWORK_PROTOCOL *
_axl_net_snp_on(
    EFI_HANDLE h   ///< handle to query
);

/**
 * @brief Read an SNP handle's hardware address into @p out_mac[6].
 *
 * A handle reporting HwAddressSize < 6 cannot be keyed: a short/zero MAC
 * compares equal to too much, so it would both collapse distinct NICs into
 * one registry row (dedup) and match an arbitrary handle (MAC lookup).
 * Skip those handles uniformly -- every MAC-keyed reader in axl-net enforces
 * this same rule. HwAddressSize > 6 (e.g. InfiniBand) is fine; the leading 6
 * bytes key it.
 *
 * @return true and fills @p out_mac on success; false (leaving @p out_mac
 *     untouched) for a NULL @p snp, a NULL @p snp->Mode, or
 *     HwAddressSize < 6.
 */
bool
_axl_net_snp_mac(
    EFI_SIMPLE_NETWORK_PROTOCOL *snp,       ///< SNP protocol (from _axl_net_snp_on)
    uint8_t                      out_mac[6] ///< [out] hardware address
);

// ---------------------------------------------------------------------------
// NIC registry (axl-net-nic.c) — the canonical per-physical-NIC model.
//
// LocateHandleBuffer(SimpleNetwork) returns one handle per SNP *child*, so a
// single physical NIC commonly repeats 2-3x, and that enumeration diverges
// from the IP4Config2 one in BOTH order and count. Indexing one with the
// other's index lands config on the wrong NIC (real-HW symptom: a link-up NIC
// never leases because DHCP went to a link-down sibling). The registry is the
// single answer to "what NICs does this machine have": SNP handles deduped by
// MAC, each correlated to its IP4Config2 handle by MAC, in stable enumeration
// order — so a NIC index is a per-physical-NIC ordinal, consistent across
// every net API.
//
// Built fresh per public call and freed when that call returns: NICs appear as
// drivers load and connect, so a cached list would go stale exactly when it
// matters.
// ---------------------------------------------------------------------------

/// One physical NIC. Internal — never exposed through include/axl/.
typedef struct {
    uint8_t     mac[6];         ///< hardware address (the stable key)
    char        name[32];       ///< "eth<ordinal>"
    bool        link_up;        ///< !MediaPresentSupported || MediaPresent
    uint32_t    mtu;            ///< SNP Mode->MaxPacketSize
    bool        has_ipv4;       ///< true when ipv4/netmask are valid
    uint8_t     ipv4[4];        ///< station address (valid if has_ipv4)
    uint8_t     netmask[4];     ///< subnet mask (valid if has_ipv4)
    uint8_t     gateway[4];     ///< default gateway (valid if has_ipv4)
    EFI_HANDLE  snp_handle;     ///< first SNP child handle publishing this MAC
    EFI_HANDLE  ip4cfg_handle;  ///< IP4Config2 handle for this MAC, NULL if none
} AxlNic;

/**
 * @brief Build the canonical per-physical-NIC list.
 *
 * Stable firmware-enumeration order; the ordinal is the array position.
 * Zero NICs is success with @p *count == 0 and @p *out == NULL.
 *
 * @return AXL_OK on success (including zero NICs), AXL_ERR on NULL args or
 *     allocation failure.
 */
int
_axl_net_nics_build(
    AxlNic **out,    ///< [out] allocated array, release with _axl_net_nics_free
    size_t  *count   ///< [out] number of physical NICs
);

/// Release an array from _axl_net_nics_build. NULL is a no-op.
void
_axl_net_nics_free(
    AxlNic *nics
);

/**
 * @brief Resolve a NIC ordinal (explicit or AXL_NET_NIC_AUTO) to a concrete
 *     registry ordinal, without touching IP4Config2.
 *
 * Implements the same rule _axl_net_nic_resolve_ip4cfg applies before it
 * goes on to resolve a protocol pointer: AXL_NET_NIC_AUTO prefers the first
 * link-up NIC with an IP4Config2, else the first NIC with one, else index 0
 * (the placeholder _axl_net_nic_resolve_ip4cfg's positional fallback picks
 * up next). An explicit index is bounds-checked — out of range resolves to
 * @p count (never a clamp to NIC 0; that clamp was the wrong-NIC bug).
 *
 * Split out from _axl_net_nic_resolve_ip4cfg so a caller can resolve
 * AXL_NET_NIC_AUTO to a concrete ordinal ONCE and reuse that SAME ordinal
 * for both configuring a NIC and reading its state back afterward (e.g.
 * axl_net_bring_up's addr_out) — two independent AUTO resolutions can
 * disagree if link state shifts between them, while reusing one ordinal
 * cannot.
 *
 * @return the resolved ordinal in [0, @p count); @p count itself if
 *     unresolvable (@p nic_index out of range, @p count == 0, or @p nics
 *     is NULL).
 */
size_t
_axl_net_nic_resolve_index(
    const AxlNic *nics,       ///< registry from _axl_net_nics_build
    size_t        count,      ///< registry length
    size_t        nic_index   ///< ordinal, or AXL_NET_NIC_AUTO
);

/**
 * @brief Resolve a NIC ordinal to its IP4Config2 protocol.
 *
 * @p nic_index may be AXL_NET_NIC_AUTO (SIZE_MAX): resolved to an ordinal via
 * _axl_net_nic_resolve_index (see its doc comment for the AUTO ladder). An
 * explicit index is bounds-checked — out of range is an error, NOT a clamp to
 * NIC 0 (that clamp was the wrong-NIC bug).
 *
 * When the selected NIC has no correlated IP4Config2 — or, under AUTO, when no
 * NIC has one at all — falls back to the single positional handle, but only
 * when there is exactly one NIC and exactly one IP4Config2 handle: the only
 * case where the guess cannot be wrong. The guard, not the spelling of the
 * index, is what makes it safe, so AUTO and an explicit index reach it
 * identically. With 2+ NICs or 2+ IP4Config2 handles the fallback is refused
 * (warns, returns NULL).
 *
 * @return the protocol, or NULL if unresolvable.
 */
EFI_IP4_CONFIG2_PROTOCOL *
_axl_net_nic_resolve_ip4cfg(
    const AxlNic *nics,       ///< registry from _axl_net_nics_build
    size_t        count,      ///< registry length
    size_t        nic_index   ///< ordinal, or AXL_NET_NIC_AUTO
);

// ---------------------------------------------------------------------------
// HTTP Core — internal helpers (raw-buffer parsers are public, see
// <axl/axl-http-core.h>; this file declares only the internal builders.)
// ---------------------------------------------------------------------------

/**
 * @brief Build an HTTP request line into a buffer.
 *
 * @return number of bytes written (excluding NUL).
 */
size_t
http_build_request_line(
    char       *buf,       ///< output buffer
    size_t      buf_size,  ///< buffer size
    const char *method,    ///< HTTP method
    const char *path       ///< request path (including query if needed)
);

/**
 * @brief Build an HTTP status line into a buffer.
 *
 * @return number of bytes written (excluding NUL).
 */
size_t
http_build_status_line(
    char   *buf,          ///< output buffer
    size_t  buf_size,     ///< buffer size
    size_t  status_code   ///< HTTP status code
);

// ---------------------------------------------------------------------------
// IP address parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parse a dotted-decimal IPv4 address string into EFI_IPv4_ADDRESS.
 *
 * @return EFI_SUCCESS or EFI_INVALID_PARAMETER.
 */
EFI_STATUS
net_parse_ip_address(
    const char       *string,  ///< IPv4 address string (e.g. "192.168.1.1")
    EFI_IPv4_ADDRESS *addr     ///< receives the parsed address
);

// ---------------------------------------------------------------------------
// WebSocket Frame Protocol (axl-websocket.c)
// ---------------------------------------------------------------------------

#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

typedef struct {
    uint8_t  opcode;
    bool     fin;
    bool     masked;
    uint8_t  mask[4];
    size_t   payload_len;
    size_t   header_len;
} WsFrameHeader;

char  *ws_compute_accept_key(const char *client_key);
size_t ws_parse_header(const uint8_t *buf, size_t len, WsFrameHeader *out);
void   ws_unmask(uint8_t *data, size_t len, const uint8_t mask[4]);
size_t ws_build_frame(uint8_t opcode, const void *payload, size_t payload_len,
                      void *out, size_t out_size);

// ---------------------------------------------------------------------------
// HTTP Response Caching (shared between server and middleware)
// ---------------------------------------------------------------------------

typedef struct {
    void       *body;
    size_t      body_size;
    size_t      status_code;
    char        content_type[64];
    uint64_t    timestamp_ms;  /* for TTL expiry */
    uint64_t    insert_seq;    /* monotonic insertion counter for FIFO eviction
                                  ordering — UEFI GetTime is 1-second
                                  granularity, so timestamp_ms ties for many
                                  inserts within the same second */
    size_t      ttl_ms;
} CachedResponse;

// ---------------------------------------------------------------------------
// Tier 4 sync-wait helpers (axl-net-wait.c)
//
// Each wraps the shared _axl_event_wait_timeout_with_tick primitive and
// drives its protocol's Poll() as the periodic tick so the UEFI driver
// advances while the CPU idles between events. All four return @ref
// AxlStatus: AXL_OK on event, AXL_TIMEOUT on deadline, AXL_CANCELLED
// on Ctrl-C, AXL_ERR on internal failure.
// ---------------------------------------------------------------------------

/**
 * @brief Wait for a TCP4 completion event.
 *
 * @return AXL_OK on event, AXL_TIMEOUT on deadline, AXL_CANCELLED on
 *     Ctrl-C, AXL_ERR on internal failure.
 */
AxlStatus
_axl_tcp_wait(
    EFI_TCP4_PROTOCOL *tcp4,        ///< TCP4 protocol (polled each tick)
    EFI_EVENT          event,       ///< completion token's Event
    uint64_t           timeout_us   ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait for a DNS4 completion event.
 *
 * @return AXL_OK on event, AXL_TIMEOUT on deadline, AXL_CANCELLED on
 *     Ctrl-C, AXL_ERR on internal failure.
 */
AxlStatus
_axl_dns_wait(
    EFI_DNS4_PROTOCOL *dns4,        ///< DNS4 protocol (polled each tick)
    EFI_EVENT          event,       ///< completion token's Event
    uint64_t           timeout_us   ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait for an IP4 completion event.
 *
 * @return AXL_OK on event, AXL_TIMEOUT on deadline, AXL_CANCELLED on
 *     Ctrl-C, AXL_ERR on internal failure.
 */
AxlStatus
_axl_ip4_wait(
    EFI_IP4_PROTOCOL *ip4,          ///< IP4 protocol (polled each tick)
    EFI_EVENT         event,        ///< completion token's Event
    uint64_t          timeout_us    ///< timeout in microseconds (0 = forever)
);

#endif // AXL_NET_INTERNAL_H_
