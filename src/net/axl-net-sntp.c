/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-sntp.c
    axl_net_sntp_query — SNTP/NTP client (RFC 4330) over UDP.

    Sends a minimal client request and parses the server's transmit
    timestamp into Unix seconds; reports the local-clock offset
    best-effort against the UEFI RTC.
**/

#include "axl-net-internal.h"
#include <axl/axl-net.h>
#include <axl/axl-udp.h>
#include <axl/axl-time.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("net");

#define SNTP_PKT_LEN      48u
#define SNTP_DEFAULT_PORT 123u
/* NTP epoch is 1900-01-01; Unix is 1970-01-01. */
#define NTP_UNIX_DELTA    2208988800LL

static uint32_t
be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int
axl_net_sntp_query(const char *server, uint16_t port, size_t timeout_ms,
               AxlNetSntpResult *out)
{
    if (server == NULL || out == NULL) {
        return AXL_ERR;
    }
    out->unix_secs = 0;
    out->offset_ms = 0;
    out->reachable = false;

    if (port == 0) {
        port = SNTP_DEFAULT_PORT;
    }

    AxlIPv4Address dest;
    if (axl_net_resolve(server, &dest) != AXL_OK) {
        axl_debug("sntp: cannot resolve '%s'", server);
        return AXL_ERR;
    }

    AxlUdp *sock = NULL;
    if (axl_udp_open(&sock, 0) != AXL_OK || sock == NULL) {
        return AXL_ERR;
    }

    /* Client request: LI=0, VN=4, Mode=3 (client) -> 0x23; rest zero. */
    uint8_t req[SNTP_PKT_LEN] = { 0x23 };
    uint8_t resp[SNTP_PKT_LEN];
    size_t  rx_len = 0;

    int rc = axl_udp_sendrecv(sock, &dest, port, req, sizeof(req),
                              timeout_ms, resp, sizeof(resp), &rx_len);
    axl_udp_close(sock);

    if (rc != AXL_OK || rx_len < SNTP_PKT_LEN) {
        return AXL_ERR;   /* timeout or short/garbled reply */
    }

    /* Transmit Timestamp: 4 bytes seconds + 4 bytes fraction at offset 40. */
    uint32_t ntp_sec  = be32(&resp[40]);
    uint32_t ntp_frac = be32(&resp[44]);
    if (ntp_sec == 0) {
        return AXL_ERR;   /* not a valid time (kiss-o'-death / unsynced) */
    }

    int64_t  unix_secs = (int64_t)ntp_sec - NTP_UNIX_DELTA;
    uint32_t frac_ms   = (uint32_t)(((uint64_t)ntp_frac * 1000) >> 32);

    out->unix_secs = unix_secs;
    out->reachable = true;

    /* Best-effort offset against the local RTC (second-resolution on most
       firmware). Skip when the RTC reads as unset (tv_sec <= 0). */
    AxlTimespec local;
    if (axl_clock_gettime(AXL_CLOCK_REALTIME, &local) == 0 && local.tv_sec > 0) {
        int64_t server_ms = unix_secs * 1000 + (int64_t)frac_ms;
        int64_t local_ms  = local.tv_sec * 1000 + local.tv_nsec / 1000000;
        int64_t off       = server_ms - local_ms;
        if (off > INT32_MAX) {
            off = INT32_MAX;
        } else if (off < INT32_MIN) {
            off = INT32_MIN;
        }
        out->offset_ms = (int32_t)off;
    }

    return AXL_OK;
}
