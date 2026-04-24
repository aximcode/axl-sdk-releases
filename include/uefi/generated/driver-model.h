/** @file generated/driver-model.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit — regenerate with scripts/generate-uefi-headers.py
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


#endif /* AXL_UEFI_GEN_DRIVER_MODEL_H */
