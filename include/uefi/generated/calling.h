/** @file generated/calling.h
    Auto-generated from UEFI Specification 2.11.
    UEFI calling convention macros.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_CALLING_H
#define AXL_UEFI_GEN_CALLING_H

// UEFI calling convention
#if defined(__x86_64__)
#define EFIAPI  __attribute__((ms_abi))
#elif defined(__aarch64__)
#define EFIAPI  /* AARCH64 UEFI uses standard AAPCS64 */
#else
#error "Unsupported architecture -- AXL requires x86_64 or AARCH64"
#endif

#define IN
#define OUT
#define OPTIONAL
#define CONST const

#ifndef VOID
#define VOID  void
#endif


#endif /* AXL_UEFI_GEN_CALLING_H */
