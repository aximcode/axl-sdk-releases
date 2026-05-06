/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-edkii.c
    EDKII IPMI_PROTOCOL dispatcher.

    Some firmwares (particularly those built on MdeModulePkg's Ipmi
    driver) expose an IPMI_PROTOCOL handle that abstracts the
    underlying physical interface (KCS, SSIF, BT, or vendor-specific).
    Using the firmware-provided handle is strictly preferable: it
    works on platforms where we can't directly reach the BMC (e.g.,
    no port I/O access, or the SMBus slave lives on a bus we don't
    enumerate), and it inherits whatever quirks the platform vendor
    has already encoded.

    Structure + GUID are in include/uefi/axl-uefi-extra.h — they
    aren't part of any UEFI/PI/ACPI spec and aren't auto-generated.
**/

#include "axl-ipmi-internal.h"
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("ipmi-edkii");

// ---------------------------------------------------------------------------
// Send-raw entry point (vtable method)
// ---------------------------------------------------------------------------

static int
edkii_send_raw(void *vctx,
               uint8_t netfn, uint8_t cmd,
               const uint8_t *req, size_t req_len,
               uint8_t *resp, size_t *resp_len)
{
    IPMI_PROTOCOL *ipmi = (IPMI_PROTOCOL *)vctx;
    UINT32         rsp_size;

    if (ipmi == NULL || ipmi->IpmiSubmitCommand == NULL) {
        return -1;
    }
    if (*resp_len > 0xFFFFFFFFu) {
        return -1;
    }
    rsp_size = (UINT32)*resp_len;

    EFI_STATUS s = ipmi->IpmiSubmitCommand(
        ipmi,
        netfn,
        /*lun=*/ 0,
        cmd,
        /*CommandData=*/ (UINT8 *)req,
        /*CommandDataSize=*/ (UINT32)req_len,
        resp,
        &rsp_size);

    if (EFI_ERROR(s)) {
        axl_debug("EDKII IpmiSubmitCommand returned 0x%lx",
                  (unsigned long)s);
        return -1;
    }

    *resp_len = (size_t)rsp_size;
    return 0;
}

static void
edkii_close(void *vctx)
{
    //
    // Firmware owns the IPMI_PROTOCOL instance — nothing to free.
    //
    (void)vctx;
}

// ---------------------------------------------------------------------------
// Public opener
// ---------------------------------------------------------------------------

int
axl_ipmi_edkii_open(AxlIpmiTransportOps *ops)
{
    if (ops == NULL) {
        return AXL_ERR;
    }

    IPMI_PROTOCOL *ipmi = NULL;
    EFI_GUID       guid = gIpmiProtocolGuid;
    EFI_STATUS     s = gBS->LocateProtocol(&guid, NULL, (VOID **)&ipmi);
    if (EFI_ERROR(s) || ipmi == NULL) {
        return AXL_ERR;
    }
    if (ipmi->IpmiSubmitCommand == NULL) {
        axl_warning("EDKII IPMI_PROTOCOL found but IpmiSubmitCommand is NULL");
        return AXL_ERR;
    }

    //
    // Probe the protocol with Get Device ID (App 0x01) before
    // committing. A firmware that publishes the handle but has a
    // stub/broken IpmiSubmitCommand is otherwise indistinguishable
    // from a working one; without this probe, auto-detect would
    // pick EDKII and then every subsequent command would fail, with
    // no fallback to vendor/SMBIOS/default-KCS.
    //
    // Probe cost: one IPMI round-trip on the happy path (~10-100ms).
    // Paid once per axl_ipmi_session_new(), which callers generally
    // invoke once at startup.
    //
    uint8_t  probe_resp[16];
    UINT32   probe_size = (UINT32)sizeof(probe_resp);
    EFI_STATUS probe_st = ipmi->IpmiSubmitCommand(
        ipmi, /*netfn=*/0x06, /*lun=*/0, /*cmd=*/0x01,
        /*req=*/NULL, /*req_size=*/0,
        probe_resp, &probe_size);
    if (EFI_ERROR(probe_st) || probe_size < 1) {
        axl_debug("EDKII probe rejected (Get Device ID failed 0x%lx)",
                  (unsigned long)probe_st);
        return AXL_ERR;
    }
    if (probe_resp[0] != 0x00) {
        axl_debug("EDKII probe rejected (CC=0x%02x)",
                  (unsigned)probe_resp[0]);
        return AXL_ERR;
    }

    ops->kind     = AXL_IPMI_TRANSPORT_EDKII;
    ops->send_raw = edkii_send_raw;
    ops->close    = edkii_close;
    ops->ctx      = ipmi;

    axl_info("IPMI EDKII transport ready (IPMI_PROTOCOL @ %p)",
             (void *)ipmi);
    return AXL_OK;
}
