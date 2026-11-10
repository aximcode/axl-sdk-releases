/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/timer-arch.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_TIMER_ARCH_H
#define AXL_UEFI_GEN_TIMER_ARCH_H

#include "types.h"
#include "status.h"

typedef
VOID
(EFIAPI *EFI_TIMER_NOTIFY) (
  IN UINT64  Time
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TIMER_REGISTER_HANDLER) (
  IN CONST EFI_TIMER_ARCH_PROTOCOL  *This,
  IN EFI_TIMER_NOTIFY               NotifyFunction
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TIMER_SET_TIMER_PERIOD) (
  IN CONST EFI_TIMER_ARCH_PROTOCOL   *This,
  IN UINT64                          TimerPeriod
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TIMER_GET_TIMER_PERIOD) (
  IN CONST EFI_TIMER_ARCH_PROTOCOL   *This,
  OUT UINT64                         *TimerPeriod
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TIMER_GENERATE_SOFT_INTERRUPT) (
  IN CONST EFI_TIMER_ARCH_PROTOCOL         *This
  );

typedef struct _EFI_TIMER_ARCH_PROTOCOL {
  EFI_TIMER_REGISTER_HANDLER          RegisterHandler;
  EFI_TIMER_SET_TIMER_PERIOD          SetTimerPeriod;
  EFI_TIMER_GET_TIMER_PERIOD          GetTimerPeriod;
  EFI_TIMER_GENERATE_SOFT_INTERRUPT   GenerateSoftInterrupt;
} EFI_TIMER_ARCH_PROTOCOL;


#endif /* AXL_UEFI_GEN_TIMER_ARCH_H */
