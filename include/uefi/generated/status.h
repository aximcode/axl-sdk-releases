/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/status.h
    Auto-generated from UEFI Specification 2.11.
    EFI status codes from Appendix D.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_STATUS_H
#define AXL_UEFI_GEN_STATUS_H

#include "types.h"

#define EFI_SUCCESS  0ULL

#define EFI_ERROR_BIT  0x8000000000000000ULL
#define EFI_ERROR(x)   ((INTN)(x) < 0)

// Error codes (high bit set)
#define EFI_LOAD_ERROR                           (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER                    (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED                          (EFI_ERROR_BIT | 3)
#define EFI_BAD_BUFFER_SIZE                      (EFI_ERROR_BIT | 4)
#define EFI_BUFFER_TOO_SMALL                     (EFI_ERROR_BIT | 5)
#define EFI_NOT_READY                            (EFI_ERROR_BIT | 6)
#define EFI_DEVICE_ERROR                         (EFI_ERROR_BIT | 7)
#define EFI_WRITE_PROTECTED                      (EFI_ERROR_BIT | 8)
#define EFI_OUT_OF_RESOURCES                     (EFI_ERROR_BIT | 9)
#define EFI_VOLUME_CORRUPTED                     (EFI_ERROR_BIT | 10)
#define EFI_VOLUME_FULL                          (EFI_ERROR_BIT | 11)
#define EFI_NO_MEDIA                             (EFI_ERROR_BIT | 12)
#define EFI_MEDIA_CHANGED                        (EFI_ERROR_BIT | 13)
#define EFI_NOT_FOUND                            (EFI_ERROR_BIT | 14)
#define EFI_ACCESS_DENIED                        (EFI_ERROR_BIT | 15)
#define EFI_NO_RESPONSE                          (EFI_ERROR_BIT | 16)
#define EFI_NO_MAPPING                           (EFI_ERROR_BIT | 17)
#define EFI_TIMEOUT                              (EFI_ERROR_BIT | 18)
#define EFI_NOT_STARTED                          (EFI_ERROR_BIT | 19)
#define EFI_ALREADY_STARTED                      (EFI_ERROR_BIT | 20)
#define EFI_ABORTED                              (EFI_ERROR_BIT | 21)
#define EFI_ICMP_ERROR                           (EFI_ERROR_BIT | 22)
#define EFI_TFTP_ERROR                           (EFI_ERROR_BIT | 23)
#define EFI_PROTOCOL_ERROR                       (EFI_ERROR_BIT | 24)
#define EFI_INCOMPATIBLE_VERSION                 (EFI_ERROR_BIT | 25)
#define EFI_SECURITY_VIOLATION                   (EFI_ERROR_BIT | 26)
#define EFI_CRC_ERROR                            (EFI_ERROR_BIT | 27)
#define EFI_END_OF_MEDIA                         (EFI_ERROR_BIT | 28)
#define EFI_END_OF_FILE                          (EFI_ERROR_BIT | 31)
#define EFI_INVALID_LANGUAGE                     (EFI_ERROR_BIT | 32)
#define EFI_COMPROMISED_DATA                     (EFI_ERROR_BIT | 33)
#define EFI_IP_ADDRESS_CONFLICT                  (EFI_ERROR_BIT | 34)
#define EFI_HTTP_ERROR                           (EFI_ERROR_BIT | 35)

// Warning codes
#define EFI_WARN_UNKNOWN_GLYPH                   1
#define EFI_WARN_DELETE_FAILURE                  2
#define EFI_WARN_WRITE_FAILURE                   3
#define EFI_WARN_BUFFER_TOO_SMALL                4
#define EFI_WARN_STALE_DATA                      5
#define EFI_WARN_FILE_SYSTEM                     6
#define EFI_WARN_RESET_REQUIRED                  7


#endif /* AXL_UEFI_GEN_STATUS_H */
