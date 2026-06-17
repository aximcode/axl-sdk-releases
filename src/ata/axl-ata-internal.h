/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ata-internal.h
    Private types for the AxlAta module.

    Not shipped to SDK consumers. The module files in src/ata/ share these
    definitions; everything else goes through the public header <axl/axl-ata.h>.
    The unit test (test/unit/axl-test-ata.c) also includes this to drive
    _axl_ata_exec against a fake EFI_ATA_PASS_THRU_PROTOCOL — the only way to
    pin the IoAlign-bounce contract without real ATA hardware.
**/

#ifndef AXL_ATA_INTERNAL_H
#define AXL_ATA_INTERNAL_H

#include <uefi/axl-uefi.h>   /* EFI_ATA_PASS_THRU_PROTOCOL (extra) */
#include <axl/axl-ata.h>

/* A device is (controller protocol, port, port-multiplier port). */
struct AxlAtaDev {
    EFI_ATA_PASS_THRU_PROTOCOL *p;
    uint16_t                    port;
    uint16_t                    pmport;
};

/* Issue one task-file command over EFI_ATA_PASS_THRU_PROTOCOL. @p efi_proto is
   the EFI pass-thru protocol (PIO_DATA_IN / PIO_DATA_OUT / NON_DATA); the data
   direction is derived from it. The caller's @p acb / @p asb / @p data may be
   arbitrarily aligned — _axl_ata_exec bounces all three through
   IoAlign-satisfying buffers before the call (AtaAtapiPassThru rejects any
   unaligned buffer with EFI_INVALID_PARAMETER) and copies @p asb / inbound
   @p data back afterward. Returns AXL_OK on transport success with the ATA ERR
   bit clear. */
int
_axl_ata_exec(
    const AxlAtaDev        *d,
    EFI_ATA_COMMAND_BLOCK  *acb,
    uint8_t                 efi_proto,
    void                   *data,
    size_t                  data_len,
    EFI_ATA_STATUS_BLOCK   *asb);

#endif /* AXL_ATA_INTERNAL_H */
