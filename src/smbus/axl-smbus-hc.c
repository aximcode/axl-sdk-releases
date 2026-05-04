/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbus-hc.c
    EFI_SMBUS_HC_PROTOCOL transport for AxlSmbus.

    The firmware already owns SMBus block framing — we just marshal
    arguments into the protocol's Execute method. No byte-count
    prefix to construct, no count byte to strip. If this transport is
    available it is strictly preferable to the I2C Master fallback.
**/

#include "axl-smbus-internal.h"

#include <axl/axl-log.h>

AXL_LOG_DOMAIN("smbus-hc");

// ---------------------------------------------------------------------------
// Vtable methods
// ---------------------------------------------------------------------------

static int
hc_read_block(void *vctx,
              uint8_t slave, uint8_t command,
              uint8_t *buf, size_t *len)
{
    EFI_SMBUS_HC_PROTOCOL   *hc = (EFI_SMBUS_HC_PROTOCOL *)vctx;
    EFI_SMBUS_DEVICE_ADDRESS addr = { .SmbusDeviceAddress = slave };
    EFI_SMBUS_DEVICE_COMMAND cmd  = command;
    UINTN                    length = *len;

    EFI_STATUS s = axl_efi_call(hc->Execute, 7,
                                hc, addr, cmd,
                                EfiSmbusReadBlock, FALSE,
                                &length, buf);
    if (EFI_ERROR(s)) {
        return -1;
    }
    *len = (size_t)length;
    return 0;
}

static int
hc_write_block(void *vctx,
               uint8_t slave, uint8_t command,
               const uint8_t *buf, size_t len)
{
    EFI_SMBUS_HC_PROTOCOL   *hc = (EFI_SMBUS_HC_PROTOCOL *)vctx;
    EFI_SMBUS_DEVICE_ADDRESS addr = { .SmbusDeviceAddress = slave };
    EFI_SMBUS_DEVICE_COMMAND cmd  = command;
    UINTN                    length = len;

    EFI_STATUS s = axl_efi_call(hc->Execute, 7,
                                hc, addr, cmd,
                                EfiSmbusWriteBlock, FALSE,
                                &length, (void *)buf);
    return EFI_ERROR(s) ? AXL_ERR : AXL_OK;
}

static int
hc_read_byte(void *vctx,
             uint8_t slave, uint8_t command,
             uint8_t *out)
{
    EFI_SMBUS_HC_PROTOCOL   *hc = (EFI_SMBUS_HC_PROTOCOL *)vctx;
    EFI_SMBUS_DEVICE_ADDRESS addr = { .SmbusDeviceAddress = slave };
    EFI_SMBUS_DEVICE_COMMAND cmd  = command;
    UINTN                    length = 1;
    uint8_t                  byte = 0;

    EFI_STATUS s = axl_efi_call(hc->Execute, 7,
                                hc, addr, cmd,
                                EfiSmbusReadByte, FALSE,
                                &length, &byte);
    if (EFI_ERROR(s)) {
        return -1;
    }
    *out = byte;
    return 0;
}

static int
hc_write_byte(void *vctx,
              uint8_t slave, uint8_t command,
              uint8_t value)
{
    EFI_SMBUS_HC_PROTOCOL   *hc = (EFI_SMBUS_HC_PROTOCOL *)vctx;
    EFI_SMBUS_DEVICE_ADDRESS addr = { .SmbusDeviceAddress = slave };
    EFI_SMBUS_DEVICE_COMMAND cmd  = command;
    UINTN                    length = 1;
    uint8_t                  byte = value;

    EFI_STATUS s = axl_efi_call(hc->Execute, 7,
                                hc, addr, cmd,
                                EfiSmbusWriteByte, FALSE,
                                &length, &byte);
    return EFI_ERROR(s) ? AXL_ERR : AXL_OK;
}

static void
hc_close(void *vctx)
{
    //
    // Firmware owns the EFI_SMBUS_HC_PROTOCOL instance — nothing to free.
    //
    (void)vctx;
}

// ---------------------------------------------------------------------------
// Public opener
// ---------------------------------------------------------------------------

int
axl_smbus_hc_open(AxlSmbusTransportOps *ops)
{
    if (ops == NULL) {
        return AXL_ERR;
    }

    EFI_SMBUS_HC_PROTOCOL *hc   = NULL;
    EFI_GUID               guid = gEfiSmbusHcProtocolGuid;
    EFI_STATUS s = gBS->LocateProtocol(&guid, NULL, (VOID **)&hc);
    if (EFI_ERROR(s) || hc == NULL) {
        return AXL_ERR;
    }
    if (hc->Execute == NULL) {
        axl_warning("EFI_SMBUS_HC_PROTOCOL found but Execute is NULL");
        return AXL_ERR;
    }

    ops->kind        = AXL_SMBUS_TRANSPORT_HC;
    ops->read_block  = hc_read_block;
    ops->write_block = hc_write_block;
    ops->read_byte   = hc_read_byte;
    ops->write_byte  = hc_write_byte;
    ops->close       = hc_close;
    ops->ctx         = hc;
    return AXL_OK;
}
