/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-dell.c
    Dell EFI_IPMI_TRANSPORT dispatcher.

    Dell platforms expose a proprietary protocol at
    gDellIpmiProtocolGuid (7409d614-5abf-4869-b8f0-b9c380393ed8).
    Shape matches uefi-ipmitool's IpmiDellProtocolSendCommand
    (IpmiTransportLib.c:151-222).

    Two quirks must be honored exactly or the firmware silently
    returns garbage on real Dell hardware:

      1. The vendor's SendIpmiCommand has 8 args including a Lun
         byte at slot 3 (always 0 in our usage). The signature is
         encoded in the typedef in axl-uefi-extra.h.

      2. The Dell firmware returns response data WITHOUT the IPMI
         completion-code byte AND will only populate the buffer if
         it's handed at offset 0 — passing &resp[1] yields zeros
         (uefi-ipmitool's IpmiTransportLib.c:184-188 documents the
         same empirical finding). The dispatcher hands the firmware
         resp[0..N-1], shifts the bytes right by one after the
         call, and synthesizes CC=0x00 at resp[0] so callers see
         the same shape as KCS/SSIF/EDKII.

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
    // Need at least 2 bytes: one for the synthesized CC and one for a
    // vendor byte. Dell's protocol uses UINT8 sizes so request length
    // is capped at 255.
    //
    if (*resp_len < 2 || req_len > 0xFF) {
        return -1;
    }

    //
    // Quirk #2 (see file docstring): the firmware only populates the
    // buffer it was handed at offset 0. We hand it resp[0..avail-1],
    // limited so the post-call shift can still fit the CC byte.
    //
    size_t  avail = *resp_len;
    if (avail > 0xFF) {
        avail = 0xFF;       /* leaves shift room: 0xFE body + CC = 0xFF */
    }
    UINT8   rsp_size = (UINT8)(avail - 1);

    EFI_STATUS s = dell->IpmiSubmitCommand(
        dell,
        netfn,
        /*Lun=*/ 0,
        cmd,
        (UINT8 *)req,
        (UINT8)req_len,
        /*ResponseData=*/ resp,
        /*ResponseDataSize=*/ &rsp_size);

    if (EFI_ERROR(s)) {
        axl_debug("Dell IpmiSubmitCommand returned 0x%lx",
                  (unsigned long)s);
        return -1;
    }

    //
    // Shift the firmware's data right by one byte and synthesize CC=0x00
    // at resp[0]. After this, resp[0] = CC and resp[1..rsp_size] holds
    // the vendor body — same shape as KCS/SSIF/EDKII.
    //
    for (size_t i = (size_t)rsp_size; i > 0; i--) {
        resp[i] = resp[i - 1];
    }
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
        return AXL_ERR;
    }

    DELL_IPMI_TRANSPORT *dell = NULL;
    EFI_GUID             guid = gDellIpmiProtocolGuid;
    EFI_STATUS           s = gBS->LocateProtocol(&guid, NULL, (VOID **)&dell);
    if (EFI_ERROR(s) || dell == NULL) {
        return AXL_ERR;
    }
    if (dell->IpmiSubmitCommand == NULL) {
        axl_debug("Dell EFI_IPMI_TRANSPORT found but IpmiSubmitCommand is NULL");
        return AXL_ERR;
    }

    ops->kind     = AXL_IPMI_TRANSPORT_DELL;
    ops->send_raw = dell_send_raw;
    ops->close    = dell_close;
    ops->ctx      = dell;

    axl_debug("IPMI Dell transport ready (revision=%llu)",
              (unsigned long long)dell->Revision);
    return AXL_OK;
}
