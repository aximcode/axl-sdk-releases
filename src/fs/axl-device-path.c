/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-device-path.c
    Device-path constructors. Today: vendor-path; future: PCI-path,
    file-path, fan-out helpers.
**/

#include <axl/axl-device-path.h>
#include <axl/axl-log.h>
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <uefi/axl-uefi.h>

AXL_LOG_DOMAIN("device-path");

/* The wire layout of the two-node chain we emit. Packed because
   EFI_DEVICE_PATH_PROTOCOL has no padding requirements and consumers
   walk it byte-by-byte via the Length[2] field. */
#pragma pack(1)
typedef struct {
    VENDOR_DEVICE_PATH       vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} VendorChain;
#pragma pack()

_Static_assert(sizeof(AxlGuid) == sizeof(EFI_GUID),
               "AxlGuid must be ABI-compatible with EFI_GUID");

int
axl_device_path_new_vendor(
    const AxlGuid  *vendor_guid,
    AxlDevicePath **out
    )
{
    if (vendor_guid == NULL || out == NULL) {
        return AXL_ERR;
    }

    VendorChain *chain = axl_malloc(sizeof(*chain));
    if (chain == NULL) {
        axl_warning("alloc failed");
        return AXL_ERR;
    }

    /* Vendor node: HARDWARE / HW_VENDOR / sizeof(VENDOR_DEVICE_PATH). */
    chain->vendor.Header.Type      = HARDWARE_DEVICE_PATH;
    chain->vendor.Header.SubType   = HW_VENDOR_DP;
    chain->vendor.Header.Length[0] = (uint8_t)(sizeof(VENDOR_DEVICE_PATH) & 0xFF);
    chain->vendor.Header.Length[1] = (uint8_t)((sizeof(VENDOR_DEVICE_PATH) >> 8) & 0xFF);
    axl_memcpy(&chain->vendor.Guid, vendor_guid, sizeof(EFI_GUID));

    /* END node: 0x7F / 0xFF / sizeof(EFI_DEVICE_PATH_PROTOCOL). */
    chain->end.Type      = END_DEVICE_PATH_TYPE;
    chain->end.SubType   = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    chain->end.Length[0] = (uint8_t)(sizeof(EFI_DEVICE_PATH_PROTOCOL) & 0xFF);
    chain->end.Length[1] = (uint8_t)((sizeof(EFI_DEVICE_PATH_PROTOCOL) >> 8) & 0xFF);

    *out = (AxlDevicePath *)chain;
    return AXL_OK;
}
