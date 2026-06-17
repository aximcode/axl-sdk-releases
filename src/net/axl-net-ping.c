/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-ping.c
    axl_net_ping / axl_net_ping_ex — ICMP echo probes over EFI_IP4_PROTOCOL.

    The shared core (ping_core) sends one ICMP echo with caller-chosen TTL,
    Don't-Fragment, and payload size, then classifies the reply
    (echo-reply / time-exceeded / unreachable / frag-needed) and reports the
    responder's source IP — the substrate for traceroute and path-MTU
    discovery. axl_net_ping is the plain "did it answer + RTT" wrapper.
**/

#include "axl-net-internal.h"
#include "../backend/axl-backend.h"
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-time.h>

AXL_LOG_DOMAIN("net");

// ---------------------------------------------------------------------------
// ICMP structures
// ---------------------------------------------------------------------------

#define ICMP_ECHO_REQUEST      8
#define ICMP_ECHO_REPLY        0
#define ICMP_DEST_UNREACHABLE  3
#define ICMP_TIME_EXCEEDED     11
#define ICMP_CODE_FRAG_NEEDED  4    /* type 3, code 4 */

#define ICMP_HDR_LEN           8u   /* type/code/cksum/id/seq */
#define ICMP_DEFAULT_PAYLOAD   56u
#define ICMP_MAX_PAYLOAD       1472u  /* 1500 MTU - 20 IP - 8 ICMP */

// ---------------------------------------------------------------------------
// ICMP checksum
// ---------------------------------------------------------------------------

static uint16_t
icmp_checksum(
    void   *buf,
    size_t  length)
{
    uint32_t  sum;
    uint16_t *ptr;

    sum = 0;
    ptr = (uint16_t *)buf;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    if (length == 1) {
        sum += *(uint8_t *)ptr;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)(~sum);
}

// ---------------------------------------------------------------------------
// ping_core — one ICMP echo probe with explicit TTL / DF / payload, classifying
// the reply. Shared by axl_net_ping and axl_net_ping_ex.
//
// Returns AXL_OK once the probe COMPLETES (inspect out->reply — a timeout is
// AXL_OK with reply == AXL_PING_NO_REPLY); AXL_ERR only on bad args or a setup
// failure (no IP4 stack, child/configure/transmit error).
// ---------------------------------------------------------------------------

static int
ping_core(AxlIPv4Address *target, size_t timeout_ms, uint8_t ttl,
          bool dont_fragment, size_t payload_len, AxlPingResult *out)
{
    EFI_STATUS                    status;
    EFI_HANDLE                   *handles;
    size_t                        handle_count;
    EFI_SERVICE_BINDING_PROTOCOL *ip4_sb;
    EFI_HANDLE                    ip4_child;
    EFI_IP4_PROTOCOL             *ip4;
    EFI_IP4_CONFIG_DATA           ip4_config;
    EFI_EVENT                     tx_event;
    EFI_EVENT                     rx_event;
    EFI_IP4_TRANSMIT_DATA         tx_data;
    EFI_IP4_RECEIVE_DATA         *rx_data;
    EFI_IP4_COMPLETION_TOKEN      tx_token;
    EFI_IP4_COMPLETION_TOKEN      rx_token;
    EFI_IP4_OVERRIDE_DATA         override;
    uint8_t                       pkt[ICMP_HDR_LEN + ICMP_MAX_PAYLOAD];
    size_t                        pkt_len;
    size_t                        elapsed;
    size_t                        timeout_us;
    EFI_IP4_FRAGMENT_DATA         fragment;
    int                           rc = AXL_ERR;

    if (target == NULL || out == NULL) {
        return AXL_ERR;
    }

    axl_memset(out, 0, sizeof(*out));
    out->reply = AXL_PING_NO_REPLY;

    if (ttl == 0) {
        ttl = 64;
    }
    if (payload_len == 0) {
        payload_len = ICMP_DEFAULT_PAYLOAD;
    } else if (payload_len > ICMP_MAX_PAYLOAD) {
        payload_len = ICMP_MAX_PAYLOAD;
    }
    pkt_len = ICMP_HDR_LEN + payload_len;

    //
    // Locate IP4 service binding
    //
    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    &gEfiIp4ServiceBindingProtocolGuid,
                    NULL,
                    &handle_count,
                    &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        axl_error("no IP4 service binding");
        return AXL_ERR;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    handles[0],
                    &gEfiIp4ServiceBindingProtocolGuid,
                    (void **)&ip4_sb);
    axl_backend_free(handles);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    //
    // Create IP4 child
    //
    ip4_child = NULL;
    status = axl_efi_call(ip4_sb->CreateChild, 2, ip4_sb, &ip4_child);
    if (EFI_ERROR(status)) {
        axl_error("IP4 CreateChild: %llx", (unsigned long long)status);
        return AXL_ERR;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    ip4_child,
                    &gEfiIp4ProtocolGuid,
                    (void **)&ip4);
    if (EFI_ERROR(status)) {
        axl_efi_call(ip4_sb->DestroyChild, 2, ip4_sb, ip4_child);
        return AXL_ERR;
    }

    //
    // Configure for ICMP (protocol 1)
    //
    axl_memset(&ip4_config, 0, sizeof(ip4_config));
    ip4_config.DefaultProtocol    = 1;
    ip4_config.AcceptIcmpErrors   = true;
    ip4_config.AcceptPromiscuous  = false;
    ip4_config.AcceptBroadcast    = false;
    ip4_config.AcceptAnyProtocol  = false;
    ip4_config.RawData            = false;
    ip4_config.UseDefaultAddress  = true;
    ip4_config.TimeToLive         = ttl;

    status = axl_efi_call(ip4->Configure, 2, ip4, &ip4_config);
    if (EFI_ERROR(status)) {
        axl_error("IP4 Configure: %llx", (unsigned long long)status);
        axl_efi_call(ip4_sb->DestroyChild, 2, ip4_sb, ip4_child);
        return AXL_ERR;
    }

    //
    // Create events
    //
    tx_event = NULL;
    rx_event = NULL;
    axl_backend_event_create((AxlEventHandle *)&tx_event);
    axl_backend_event_create((AxlEventHandle *)&rx_event);
    if (tx_event == NULL || rx_event == NULL) {
        goto done;   /* rc is still AXL_ERR */
    }

    //
    // Build ICMP echo request: 8-byte header + payload, identifier "UN".
    // type/code/checksum/id/seq are at offsets 0/1/2-3/4-5/6-7.
    //
    axl_memset(pkt, 0, pkt_len);
    pkt[0] = ICMP_ECHO_REQUEST;      /* type */
    pkt[1] = 0;                       /* code */
    pkt[4] = 0x4E;                    /* identifier "UN" (0x554E), LE */
    pkt[5] = 0x55;
    pkt[6] = 1;                       /* sequence */
    pkt[7] = 0;
    uint16_t cksum = icmp_checksum(pkt, pkt_len);
    pkt[2] = (uint8_t)(cksum & 0xFF);
    pkt[3] = (uint8_t)(cksum >> 8);

    //
    // Prepare transmit
    //
    fragment.FragmentLength = (uint32_t)pkt_len;
    fragment.FragmentBuffer = pkt;

    axl_memset(&override, 0, sizeof(override));
    override.Protocol      = 1;
    override.TimeToLive    = ttl;
    override.DoNotFragment = dont_fragment;

    axl_memset(&tx_data, 0, sizeof(tx_data));
    axl_memcpy(&tx_data.DestinationAddress, target, sizeof(EFI_IPv4_ADDRESS));
    tx_data.OverrideData  = &override;
    tx_data.TotalDataLength = (uint32_t)pkt_len;
    tx_data.FragmentCount   = 1;
    tx_data.FragmentTable[0] = fragment;

    axl_memset(&tx_token, 0, sizeof(tx_token));
    tx_token.Event  = tx_event;
    tx_token.Status = EFI_ABORTED;
    tx_token.Packet.TxData = &tx_data;

    //
    // Submit receive before transmit
    //
    axl_memset(&rx_token, 0, sizeof(rx_token));
    rx_token.Event  = rx_event;
    rx_token.Status = EFI_ABORTED;

    status = axl_efi_call(ip4->Receive, 2, ip4, &rx_token);
    if (EFI_ERROR(status)) {
        axl_error("IP4 Receive: %llx", (unsigned long long)status);
        goto done;
    }

    //
    // Transmit
    //
    status = axl_efi_call(ip4->Transmit, 2, ip4, &tx_token);
    if (EFI_ERROR(status)) {
        axl_error("IP4 Transmit: %llx", (unsigned long long)status);
        axl_efi_call(ip4->Cancel, 2, ip4, &rx_token);
        goto done;
    }

    //
    // The probe is now in flight: whatever happens next (a reply, an ICMP
    // error, or a timeout) the probe COMPLETED, so the call succeeds and the
    // outcome is reported in out->reply.
    //
    rc = AXL_OK;

    //
    // Wait for reply — loop because a spurious packet may arrive and
    // require a re-Receive. Each iteration waits for rx_event with the
    // remaining budget, charging elapsed against wall-clock. The IP4
    // driver advances via the Tier 4 helper's 10ms tick.
    //
    timeout_us = timeout_ms * 1000;
    elapsed = 0;

    uint64_t start_ms = axl_time_get_ms();
    while (elapsed < timeout_us) {
        AxlStatus wrc = _axl_ip4_wait(ip4, rx_event, timeout_us - elapsed);
        elapsed = (axl_time_get_ms() - start_ms) * 1000;

        if (wrc != AXL_OK) {
            break;  /* timeout or Ctrl-C */
        }

        if (!EFI_ERROR(rx_token.Status) && rx_token.Packet.RxData != NULL) {
            rx_data = rx_token.Packet.RxData;

            //
            // Classify the ICMP message: echo-reply (reached), time-exceeded
            // (a hop), or destination-unreachable (incl. frag-needed for PMTU).
            //
            bool terminal = false;
            if (rx_data->Header->Protocol == 1 && rx_data->DataLength >= 8) {
                uint8_t *icmp = (uint8_t *)rx_data->FragmentTable[0].FragmentBuffer;
                switch (icmp[0]) {
                    case ICMP_ECHO_REPLY:
                        out->reply = AXL_PING_ECHO_REPLY;
                        terminal = true;
                        break;
                    case ICMP_TIME_EXCEEDED:
                        out->reply = AXL_PING_TIME_EXCEEDED;
                        terminal = true;
                        break;
                    case ICMP_DEST_UNREACHABLE:
                        if (icmp[1] == ICMP_CODE_FRAG_NEEDED) {
                            out->reply = AXL_PING_FRAG_NEEDED;
                            /* type 3/code 4: next-hop MTU in bytes 6-7 (BE). */
                            out->next_mtu =
                                (uint16_t)((icmp[6] << 8) | icmp[7]);
                        } else {
                            out->reply = AXL_PING_UNREACHABLE;
                        }
                        terminal = true;
                        break;
                    default:
                        break;
                }
            }

            if (terminal) {
                axl_memcpy(out->responder.addr,
                           &rx_data->Header->SourceAddress, 4);
                out->rtt_ms = elapsed / 1000;
                if (out->rtt_ms == 0) {
                    out->rtt_ms = 1;
                }
                axl_backend_event_signal((AxlEventHandle)rx_data->RecycleSignal);
                goto done;
            }

            axl_backend_event_signal((AxlEventHandle)rx_data->RecycleSignal);

            //
            // Not a terminal reply, resubmit receive
            //
            rx_token.Status = EFI_ABORTED;
            axl_efi_call(ip4->Receive, 2, ip4, &rx_token);
        }
    }

    //
    // Timed out — out->reply stays AXL_PING_NO_REPLY.
    //
    axl_efi_call(ip4->Cancel, 2, ip4, &rx_token);

done:
    if (tx_event != NULL) {
        axl_backend_event_close((AxlEventHandle)tx_event);
    }

    if (rx_event != NULL) {
        axl_backend_event_close((AxlEventHandle)rx_event);
    }

    axl_efi_call(ip4->Configure, 2, ip4, NULL);
    axl_efi_call(ip4_sb->DestroyChild, 2, ip4_sb, ip4_child);
    return rc;
}

// ---------------------------------------------------------------------------
// Public wrappers
// ---------------------------------------------------------------------------

int
axl_net_ping(AxlIPv4Address *target, size_t timeout_ms, size_t *out_rtt_ms)
{
    if (out_rtt_ms == NULL) {
        return AXL_ERR;
    }
    *out_rtt_ms = 0;

    AxlPingResult res;
    if (ping_core(target, timeout_ms, 64, false, 0, &res) != AXL_OK) {
        return AXL_ERR;
    }
    if (res.reply != AXL_PING_ECHO_REPLY) {
        return AXL_ERR;   /* timeout or an error reply — not "alive" */
    }
    *out_rtt_ms = res.rtt_ms;
    return AXL_OK;
}

int
axl_net_ping_ex(AxlIPv4Address *target, size_t timeout_ms, uint8_t ttl,
                bool dont_fragment, size_t payload_len, AxlPingResult *out)
{
    return ping_core(target, timeout_ms, ttl, dont_fragment, payload_len, out);
}
