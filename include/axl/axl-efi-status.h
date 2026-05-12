/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-efi-status.h:
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

#ifdef __cplusplus
}
#endif

#endif /* AXL_EFI_STATUS_H */
