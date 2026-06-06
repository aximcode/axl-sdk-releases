/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/driver-model.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_DRIVER_MODEL_H
#define AXL_UEFI_GEN_DRIVER_MODEL_H

#include "types.h"
#include "status.h"

typedef
EFI_STATUS
(EFIAPI *EFI_DRIVER_BINDING_PROTOCOL_SUPPORTED) (
 IN EFI_DRIVER_BINDING_PROTOCOL              *This,
 IN EFI_HANDLE                               ControllerHandle,
 IN EFI_DEVICE_PATH_PROTOCOL                 *RemainingDevicePath OPTIONAL
 );

typedef
EFI_STATUS
(EFIAPI *EFI_DRIVER_BINDING_PROTOCOL_START) (
 IN EFI_DRIVER_BINDING_PROTOCOL         *This,
 IN EFI_HANDLE                          ControllerHandle,
 IN EFI_DEVICE_PATH_PROTOCOL            *RemainingDevicePath OPTIONAL
 );

typedef
EFI_STATUS
(EFIAPI *EFI_DRIVER_BINDING_PROTOCOL_STOP) (
 IN EFI_DRIVER_BINDING_PROTOCOL           *This,
 IN EFI_HANDLE                            ControllerHandle,
 IN UINTN                                 NumberOfChildren,
 IN EFI_HANDLE                            *ChildHandleBuffer OPTIONAL
 );

typedef struct _EFI_DRIVER_BINDING_PROTOCOL {
 EFI_DRIVER_BINDING_PROTOCOL_SUPPORTED        Supported;
 EFI_DRIVER_BINDING_PROTOCOL_START            Start;
 EFI_DRIVER_BINDING_PROTOCOL_STOP             Stop;
 UINT32                                       Version;
 EFI_HANDLE                                  ImageHandle;
 EFI_HANDLE                                   DriverBindingHandle;
} EFI_DRIVER_BINDING_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_COMPONENT_NAME_GET_DRIVER_NAME) (
  IN EFI_COMPONENT_NAME2_PROTOCOL            *This,
  IN CHAR8                                   *Language,
  OUT CHAR16                                 **DriverName
  );

typedef
EFI_STATUS
(EFIAPI *EFI_COMPONENT_NAME_GET_CONTROLLER_NAME) (
  IN EFI_COMPONENT_NAME2_PROTOCOL               *This,
  IN EFI_HANDLE                                 ControllerHandle,
  IN EFI_HANDLE                                 ChildHandle OPTIONAL,
  IN CHAR8                                      *Language,
  OUT CHAR16                                    **ControllerName
  );

typedef struct _EFI_COMPONENT_NAME2_PROTOCOL {
 EFI_COMPONENT_NAME_GET_DRIVER_NAME                GetDriverName;
 EFI_COMPONENT_NAME_GET_CONTROLLER_NAME            GetControllerName;
 CHAR8                                             *SupportedLanguages;
} EFI_COMPONENT_NAME2_PROTOCOL;


#endif /* AXL_UEFI_GEN_DRIVER_MODEL_H */
