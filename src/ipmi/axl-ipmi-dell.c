/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-dell.c
    Dell EFI_IPMI_TRANSPORT dispatcher.

    Dell platforms expose a proprietary protocol at
    gDellIpmiProtocolGuid (7409d614-5abf-4869-b8f0-b9c380393ed8).
    Shape matches uefi-ipmitool's IpmiDellProtocolSendCommand
    (IpmiTransportLib.c:151-222).

    Quirk: the Dell firmware returns response data WITHOUT the
    IPMI completion-code byte. Every other AXL IPMI transport
    places the completion code at resp[0]; to keep that invariant,
    we write the vendor's bytes into resp[1..] and synthesize a
    CC=0x00 at resp[0]. Callers see the same shape whether they're
    talking to a Dell BMC or anything else.

    Structure + GUID: include/uefi/axl-uefi-extra.h.
**/

#include "axl-ipmi-internal.h"
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("ipmi-dell");

// ---------------------------------------------------------------------------
// Send-raw entry point (vtable method)
// ---------------------------------------------------------------------------

static int
dell_send_raw(void *vctx,
              uint8_t netfn, uint8_t cmd,
              const uint8_t *req, size_t req_len,
              uint8_t *resp, size_t *resp_len)
{
    DELL_IPMI_TRANSPORT *dell = (DELL_IPMI_TRANSPORT *)vctx;

    if (dell == NULL || dell->IpmiSubmitCommand == NULL) {
        return -1;
    }
    //
    // Dell firmware returns response data without a leading completion
    // code, so the dispatcher synthesizes CC=0x00 at resp[0] and gives
    // the firmware a shifted buffer starting at resp[1]. Require at
    // least 2 bytes so the synthesis has room to hold both CC and at
    // least one vendor byte; a 1-byte buffer is degenerate (firmware
    // would get zero room and we'd still write resp[0]=0x00 over it).
    //
    if (*resp_len < 2 || req_len > 0xFF) {
        return -1;
    }

    //
    // Dell's protocol uses UINT8 response sizes — capped at 255.
    // Leave room for the synthesized CC byte at resp[0].
    //
    size_t  avail = (*resp_len - 1);
    if (avail > 0xFF) {
        avail = 0xFF;
    }
    UINT8   rsp_size = (UINT8)avail;

    EFI_STATUS s = dell->IpmiSubmitCommand(
        dell,
        netfn,
        cmd,
        (UINT8 *)req,
        (UINT8)req_len,
        /*ResponseData=*/ &resp[1],
        /*ResponseDataSize=*/ &rsp_size);

    if (EFI_ERROR(s)) {
        axl_debug("Dell IpmiSubmitCommand returned 0x%lx",
                  (unsigned long)s);
        return -1;
    }

    //
    // Synthesize the IPMI completion code byte. This matches upstream
    // uefi-ipmitool behavior and lets callers treat Dell responses
    // identically to KCS/SSIF/EDKII ones.
    //
    resp[0] = 0x00;
    *resp_len = (size_t)rsp_size + 1;
    return 0;
}

static void
dell_close(void *vctx)
{
    //
    // Firmware owns the protocol instance.
    //
    (void)vctx;
}

// ---------------------------------------------------------------------------
// Public opener
// ---------------------------------------------------------------------------

int
axl_ipmi_dell_open(AxlIpmiTransportOps *ops)
{
    if (ops == NULL) {
        return -1;
    }

    DELL_IPMI_TRANSPORT *dell = NULL;
    EFI_GUID             guid = gDellIpmiProtocolGuid;
    EFI_STATUS           s = gBS->LocateProtocol(&guid, NULL, (VOID **)&dell);
    if (EFI_ERROR(s) || dell == NULL) {
        return -1;
    }
    if (dell->IpmiSubmitCommand == NULL) {
        axl_warning("Dell EFI_IPMI_TRANSPORT found but IpmiSubmitCommand is NULL");
        return -1;
    }

    ops->kind     = AXL_IPMI_TRANSPORT_DELL;
    ops->send_raw = dell_send_raw;
    ops->close    = dell_close;
    ops->ctx      = dell;

    axl_info("IPMI Dell transport ready (revision=%llu)",
             (unsigned long long)dell->Revision);
    return 0;
}
