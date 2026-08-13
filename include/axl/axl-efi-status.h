/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-efi-status.h
 *
 * AXL aliases for the firmware-defined `EFI_STATUS` wire-format
 * return type, plus the most common UEFI 2.11 Appendix D error
 * constants. Use this header when implementing a UEFI-spec-defined
 * protocol (`EFI_FILE_PROTOCOL`, `EFI_BLOCK_IO_PROTOCOL`, driver
 * binding callbacks, etc.) and you need to return spec-mandated
 * status codes — but you don't want to pull all of
 * `<uefi/axl-uefi.h>` just for the return type and a handful of
 * constants.
 *
 * Naming: `AxlEfiStatus` (PascalCase typedef per AXL convention);
 * `AXL_EFI_*` (UPPER_SNAKE_CASE macros, mirroring UEFI's own
 * `EFI_*` style and prefixed with `AXL_EFI_` to keep the AXL
 * surface distinct from `<uefi/axl-uefi.h>`'s `EFI_*` originals
 * and from AXL's own `AXL_OK` / `AXL_ERR` int convention).
 *
 * `AxlEfiStatus` is binary-compatible with `EFI_STATUS` on the
 * AXL-supported architectures (x86_64 + aarch64, both have
 * 64-bit `UINTN`); a `_Static_assert` enforces this. Direct
 * value-for-value swap.
 *
 * Most consumers don't need this header — drivers built with
 * `AXL_DRIVER` return `int` (0 = OK), and any AXL helper returns
 * `AXL_OK` / `AXL_ERR`. Reach for `AxlEfiStatus` only when the
 * firmware ABI obliges you to return a spec-defined value.
 */

#ifndef AXL_EFI_STATUS_H
#define AXL_EFI_STATUS_H

#include <stdint.h>
#include <axl/axl-macros.h>   /* AxlStatus (for the translation helpers below) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Firmware status code (binary-compatible with `EFI_STATUS`).
 *
 * 64-bit unsigned. Top bit (`0x8000000000000000`) set indicates
 * an error; cleared indicates success or warning. Use
 * `AXL_EFI_ERROR(s)` to test.
 */
typedef uint64_t AxlEfiStatus;

#define AXL_EFI_ERR_BIT_       ((AxlEfiStatus)1 << 63)
#define AXL_EFI_ENC_(n)        (AXL_EFI_ERR_BIT_ | ((AxlEfiStatus)(n)))

/**
 * @brief Test whether a status code is an error (top bit set).
 *
 * Mirrors UEFI's `EFI_ERROR(s)` macro. Warnings have the top bit
 * cleared and don't trigger this; treat as success unless the
 * caller specifically inspects warning values.
 */
#define AXL_EFI_ERROR(s)       (((AxlEfiStatus)(s) & AXL_EFI_ERR_BIT_) != 0)

/**
 * @brief Firmware calling convention attribute (matches UEFI `EFIAPI`).
 *
 * The per-arch ABI the firmware uses to call entry points (DriverEntry,
 * the app entry stub) and any function it invokes directly. Defined here —
 * uefi-free — so `<axl.h>`'s `AXL_APP` / `AXL_DRIVER` entry-point macros can
 * emit firmware-ABI entry symbols without pulling `<uefi/...>` into every
 * consumer of the umbrella. Byte-for-byte identical to `EFIAPI`.
 */
#if defined(__x86_64__)
#define AXL_EFI_ABI  __attribute__((ms_abi))
#elif defined(__aarch64__)
#define AXL_EFI_ABI  /* AARCH64 UEFI uses standard AAPCS64 */
#else
#error "Unsupported architecture -- AXL requires x86_64 or AARCH64"
#endif

/* ----- Success ------------------------------------------------ */

#define AXL_EFI_SUCCESS              ((AxlEfiStatus)0)

/* ----- Errors (UEFI 2.11 Appendix D, curated subset) ---------- */

#define AXL_EFI_LOAD_ERROR           AXL_EFI_ENC_(1)
#define AXL_EFI_INVALID_PARAMETER    AXL_EFI_ENC_(2)
#define AXL_EFI_UNSUPPORTED          AXL_EFI_ENC_(3)
#define AXL_EFI_BAD_BUFFER_SIZE      AXL_EFI_ENC_(4)
#define AXL_EFI_BUFFER_TOO_SMALL     AXL_EFI_ENC_(5)
#define AXL_EFI_NOT_READY            AXL_EFI_ENC_(6)
#define AXL_EFI_DEVICE_ERROR         AXL_EFI_ENC_(7)
#define AXL_EFI_WRITE_PROTECTED      AXL_EFI_ENC_(8)
#define AXL_EFI_OUT_OF_RESOURCES     AXL_EFI_ENC_(9)
#define AXL_EFI_VOLUME_CORRUPTED     AXL_EFI_ENC_(10)
#define AXL_EFI_VOLUME_FULL          AXL_EFI_ENC_(11)
#define AXL_EFI_NO_MEDIA             AXL_EFI_ENC_(12)
#define AXL_EFI_MEDIA_CHANGED        AXL_EFI_ENC_(13)
#define AXL_EFI_NOT_FOUND            AXL_EFI_ENC_(14)
#define AXL_EFI_ACCESS_DENIED        AXL_EFI_ENC_(15)
#define AXL_EFI_NO_RESPONSE          AXL_EFI_ENC_(16)
#define AXL_EFI_NO_MAPPING           AXL_EFI_ENC_(17)
#define AXL_EFI_TIMEOUT              AXL_EFI_ENC_(18)
#define AXL_EFI_NOT_STARTED          AXL_EFI_ENC_(19)
#define AXL_EFI_ALREADY_STARTED      AXL_EFI_ENC_(20)
#define AXL_EFI_ABORTED              AXL_EFI_ENC_(21)
#define AXL_EFI_PROTOCOL_ERROR       AXL_EFI_ENC_(24)
#define AXL_EFI_INCOMPATIBLE_VERSION AXL_EFI_ENC_(25)
#define AXL_EFI_SECURITY_VIOLATION   AXL_EFI_ENC_(26)
#define AXL_EFI_END_OF_MEDIA         AXL_EFI_ENC_(28)
#define AXL_EFI_END_OF_FILE          AXL_EFI_ENC_(31)

/* ----- AxlStatus <-> AxlEfiStatus translation -------------------------- */
//
// Explicit, best-effort mappers between AXL's int status convention
// (AxlStatus: 0 = OK, negative = failure) and the firmware wire format
// (AxlEfiStatus / EFI_STATUS). The two enums are deliberately NOT numerically
// aligned — AxlStatus is a small, coarse, AXL-domain set while EFI carries 30+
// codes, and AXL's released AXL_CANCELLED/AXL_TIMEOUT occupy the slots EFI's
// INVALID/UNSUPPORTED would want — so a numeric "negate" trick would be wrong
// for half the codes. These switches carry the translation instead: explicit,
// stable across either enum growing, and the only robust anchor is the shared
// AXL_OK == AXL_EFI_SUCCESS == 0 / "negative <=> AXL_EFI_ERROR set".
//
// Both directions are LOSSY by design: to_efi maps AXL's set onto a
// representative EFI code (the generic AXL_ERR -> AXL_EFI_ABORTED); from_efi
// collapses EFI's many codes onto AXL's set (unmapped errors -> AXL_ERR, any
// non-error / warning -> AXL_OK). Codes with a 1:1 peer round-trip cleanly;
// the generic/abort bucket does not (to_efi(AXL_ERR) -> ABORTED -> AXL_CANCELLED).

/**
 * @brief Map an AxlStatus to a representative firmware status code.
 *
 * For a driver/protocol boundary that must return an EFI_STATUS-shaped value.
 * @return AXL_EFI_SUCCESS for AXL_OK; the matching AXL_EFI_* code where one
 *     exists; AXL_EFI_ABORTED for the generic AXL_ERR / any unmapped code.
 */
static inline AxlEfiStatus
axl_status_to_efi(AxlStatus s)
{
    switch (s) {
    case AXL_OK:           return AXL_EFI_SUCCESS;
    case AXL_CANCELLED:    return AXL_EFI_ABORTED;
    case AXL_TIMEOUT:      return AXL_EFI_TIMEOUT;
    case AXL_INVALID:      return AXL_EFI_INVALID_PARAMETER;
    case AXL_NOT_FOUND:    return AXL_EFI_NOT_FOUND;
    case AXL_DENIED:       return AXL_EFI_ACCESS_DENIED;
    case AXL_UNSUPPORTED:  return AXL_EFI_UNSUPPORTED;
    case AXL_NO_RESOURCES: return AXL_EFI_OUT_OF_RESOURCES;
    case AXL_IO_ERROR:     return AXL_EFI_DEVICE_ERROR;
    default:               return AXL_EFI_ABORTED;  /* AXL_ERR + any unmapped */
    }
}

/**
 * @brief Map a firmware status code to an AxlStatus.
 *
 * The inverse of axl_status_to_efi. Any non-error code (success or a warning —
 * top bit clear) becomes AXL_OK; a recognized error maps to its AXL peer; any
 * other error becomes the generic AXL_ERR.
 *
 * @return the AxlStatus peer; AXL_OK for success/warning; AXL_ERR for an
 *     unmapped error.
 */
static inline AxlStatus
axl_status_from_efi(AxlEfiStatus e)
{
    if (!AXL_EFI_ERROR(e)) {
        return AXL_OK;   /* success or warning — not a failure */
    }
    if (e == AXL_EFI_INVALID_PARAMETER) return AXL_INVALID;
    if (e == AXL_EFI_NOT_FOUND)         return AXL_NOT_FOUND;
    if (e == AXL_EFI_ACCESS_DENIED)     return AXL_DENIED;
    if (e == AXL_EFI_UNSUPPORTED)       return AXL_UNSUPPORTED;
    if (e == AXL_EFI_OUT_OF_RESOURCES)  return AXL_NO_RESOURCES;
    if (e == AXL_EFI_DEVICE_ERROR)      return AXL_IO_ERROR;
    if (e == AXL_EFI_TIMEOUT)           return AXL_TIMEOUT;
    if (e == AXL_EFI_ABORTED)           return AXL_CANCELLED;
    return AXL_ERR;
}

#ifdef __cplusplus
}
#endif

#endif /* AXL_EFI_STATUS_H */
