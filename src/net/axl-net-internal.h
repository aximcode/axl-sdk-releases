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
