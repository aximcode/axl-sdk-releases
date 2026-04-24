/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-ping.c
    axl_net_ping — ICMP echo request over EFI_IP4_PROTOCOL.
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

#pragma pack(1)
typedef struct {
    uint8_t   type;
    uint8_t   code;
    uint16_t  checksum;
    uint16_t  identifier;
    uint16_t  sequence;
    uint8_t   data[56];
} IcmpEchoPacket;
#pragma pack()

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

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
// axl_net_ping
// ---------------------------------------------------------------------------

int
axl_net_ping(AxlIPv4Address *target, size_t timeout_ms,
             size_t *out_rtt_ms)
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
    IcmpEchoPacket                pkt;
    size_t                        elapsed;
    size_t                        timeout_us;
    EFI_IP4_FRAGMENT_DATA         fragment;

    if (target == NULL || out_rtt_ms == NULL) {
        return -1;
    }

    *out_rtt_ms = 0;

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
        return -1;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    handles[0],
                    &gEfiIp4ServiceBindingProtocolGuid,
                    (void **)&ip4_sb);
    axl_backend_free(handles);
    if (EFI_ERROR(status)) {
        return -1;
    }

    //
    // Create IP4 child
    //
    ip4_child = NULL;
    status = axl_efi_call(ip4_sb->CreateChild, 2, ip4_sb, &ip4_child);
    if (EFI_ERROR(status)) {
        axl_error("IP4 CreateChild: %llx", (unsigned long long)status);
        return -1;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    ip4_child,
                    &gEfiIp4ProtocolGuid,
                    (void **)&ip4);
    if (EFI_ERROR(status)) {
        axl_efi_call(ip4_sb->DestroyChild, 2, ip4_sb, ip4_child);
        return -1;
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
    ip4_config.TimeToLive         = 64;

    status = axl_efi_call(ip4->Configure, 2, ip4, &ip4_config);
    if (EFI_ERROR(status)) {
        axl_error("IP4 Configure: %llx", (unsigned long long)status);
        axl_efi_call(ip4_sb->DestroyChild, 2, ip4_sb, ip4_child);
        return -1;
    }

    //
    // Create events
    //
    tx_event = NULL;
    rx_event = NULL;
    axl_backend_event_create((AxlEventHandle *)&tx_event);
    axl_backend_event_create((AxlEventHandle *)&rx_event);
    if (tx_event == NULL || rx_event == NULL) {
        status = EFI_OUT_OF_RESOURCES;
        goto done;
    }

    //
    // Build ICMP echo request
    //
    axl_memset(&pkt, 0, sizeof(pkt));
    pkt.type       = ICMP_ECHO_REQUEST;
    pkt.code       = 0;
    pkt.identifier = 0x554E;  // "UN" for AxlNet
    pkt.sequence   = 1;
    pkt.checksum   = icmp_checksum(&pkt, sizeof(pkt));

    //
    // Prepare transmit
    //
    fragment.FragmentLength = sizeof(pkt);
    fragment.FragmentBuffer = &pkt;

    axl_memset(&override, 0, sizeof(override));
    override.Protocol   = 1;
    override.TimeToLive = 64;

    axl_memset(&tx_data, 0, sizeof(tx_data));
    axl_memcpy(&tx_data.DestinationAddress, target, sizeof(EFI_IPv4_ADDRESS));
    tx_data.OverrideData  = &override;
    tx_data.TotalDataLength = sizeof(pkt);
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
    // Wait for reply — loop because a spurious packet may arrive and
    // require a re-Receive. Each iteration waits for rx_event with the
    // remaining budget, charging elapsed against wall-clock. The IP4
    // driver advances via the Tier 4 helper's 10ms tick.
    //
    timeout_us = timeout_ms * 1000;
    elapsed = 0;

    uint64_t start_ms = axl_time_get_ms();
    while (elapsed < timeout_us) {
        int wrc = _axl_ip4_wait(ip4, rx_event, timeout_us - elapsed);
        elapsed = (axl_time_get_ms() - start_ms) * 1000;

        if (wrc != 0) {
            break;  /* timeout or Ctrl-C */
        }

        if (!EFI_ERROR(rx_token.Status) && rx_token.Packet.RxData != NULL) {
            rx_data = rx_token.Packet.RxData;

            //
            // Check for ICMP echo reply
            //
            if (rx_data->Header->Protocol == 1 && rx_data->DataLength >= 8) {
                uint8_t *icmp_hdr = (uint8_t *)rx_data->FragmentTable[0].FragmentBuffer;
                if (icmp_hdr[0] == ICMP_ECHO_REPLY) {
                    *out_rtt_ms = elapsed / 1000;
                    if (*out_rtt_ms == 0) {
                        *out_rtt_ms = 1;
                    }

                    axl_backend_event_signal((AxlEventHandle)rx_data->RecycleSignal);
                    status = EFI_SUCCESS;
                    goto done;
                }
            }

            axl_backend_event_signal((AxlEventHandle)rx_data->RecycleSignal);

            //
            // Not our reply, resubmit receive
            //
            rx_token.Status = EFI_ABORTED;
            axl_efi_call(ip4->Receive, 2, ip4, &rx_token);
        }
    }

    //
    // Timed out
    //
    axl_efi_call(ip4->Cancel, 2, ip4, &rx_token);
    status = EFI_TIMEOUT;

done:
    if (tx_event != NULL) {
        axl_backend_event_close((AxlEventHandle)tx_event);
    }

    if (rx_event != NULL) {
        axl_backend_event_close((AxlEventHandle)rx_event);
    }

    axl_efi_call(ip4->Configure, 2, ip4, NULL);
    axl_efi_call(ip4_sb->DestroyChild, 2, ip4_sb, ip4_child);
    return EFI_ERROR(status) ? -1 : 0;
}
