/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-rng.c
    Cryptographic random bytes via EFI_RNG_PROTOCOL.

    The protocol struct isn't in the generated UEFI headers (no
    other module in axl-sdk consumes RNG yet), so its layout is
    declared locally. Spec: UEFI 2.11 §37.5.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-rng.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("rng");

/* The protocol struct itself isn't in the generated UEFI headers
   (no other module in axl-sdk consumes RNG yet); declare it
   locally per UEFI 2.11 §37.5. The protocol GUID is generated and
   reused via <uefi/axl-uefi.h>. */
#pragma pack(push, 1)
typedef struct EfiRngProtocol EfiRngProtocol;
struct EfiRngProtocol {
    EFI_STATUS (EFIAPI *GetInfo)(
        EfiRngProtocol  *self,
        UINTN           *algo_list_size,
        EFI_GUID        *algo_list);
    EFI_STATUS (EFIAPI *GetRNG)(
        EfiRngProtocol  *self,
        EFI_GUID        *algo,
        UINTN            value_size,
        uint8_t         *value);
};
#pragma pack(pop)

static EfiRngProtocol *
get_rng(
    void
    )
{
    static EfiRngProtocol *cached;
    if (cached != NULL) {
        return cached;
    }
    void *p = NULL;
    if (axl_bs()->LocateProtocol(
            &EFI_RNG_PROTOCOL_GUID, NULL, &p) == EFI_SUCCESS) {
        cached = (EfiRngProtocol *)p;
    }
    return cached;
}

int
axl_rng_bytes(
    void   *out,
    size_t  len
    )
{
    if (out == NULL || len == 0) {
        return AXL_ERR;
    }
    EfiRngProtocol *rng = get_rng();
    if (rng == NULL || rng->GetRNG == NULL) {
        return AXL_ERR;
    }
    EFI_STATUS status = rng->GetRNG(rng, NULL, (UINTN)len, (uint8_t *)out);
    if (EFI_ERROR(status)) {
        axl_debug("GetRNG(%zu) failed: 0x%llx",
                  len, (unsigned long long)status);
        return AXL_ERR;
    }
    return AXL_OK;
}
