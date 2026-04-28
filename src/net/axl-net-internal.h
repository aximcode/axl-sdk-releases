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
// advances while the CPU idles between events. All three follow the
// AxlWait return convention: 0 = event fired, -1 = timeout, -2 = Ctrl-C.
// ---------------------------------------------------------------------------

/**
 * @brief Wait for a UDP4 completion event.
 *
 * @return 0 on event, -1 on timeout, -2 on Ctrl-C.
 */
int
_axl_udp_wait(
    EFI_UDP4_PROTOCOL *udp4,        ///< UDP4 protocol (polled each tick)
    EFI_EVENT          event,       ///< completion token's Event
    uint64_t           timeout_us   ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait for a TCP4 completion event.
 *
 * @return 0 on event, -1 on timeout, -2 on Ctrl-C.
 */
int
_axl_tcp_wait(
    EFI_TCP4_PROTOCOL *tcp4,        ///< TCP4 protocol (polled each tick)
    EFI_EVENT          event,       ///< completion token's Event
    uint64_t           timeout_us   ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait for a DNS4 completion event.
 *
 * @return 0 on event, -1 on timeout, -2 on Ctrl-C.
 */
int
_axl_dns_wait(
    EFI_DNS4_PROTOCOL *dns4,        ///< DNS4 protocol (polled each tick)
    EFI_EVENT          event,       ///< completion token's Event
    uint64_t           timeout_us   ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait for an IP4 completion event.
 *
 * @return 0 on event, -1 on timeout, -2 on Ctrl-C.
 */
int
_axl_ip4_wait(
    EFI_IP4_PROTOCOL *ip4,          ///< IP4 protocol (polled each tick)
    EFI_EVENT         event,        ///< completion token's Event
    uint64_t          timeout_us    ///< timeout in microseconds (0 = forever)
);

#endif // AXL_NET_INTERNAL_H_
