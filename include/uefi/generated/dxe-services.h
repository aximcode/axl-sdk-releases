/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/dxe-services.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_DXE_SERVICES_H
#define AXL_UEFI_GEN_DXE_SERVICES_H

#include "types.h"
#include "status.h"

typedef enum {
  EfiGcdMemoryTypeNonExistent,
  EfiGcdMemoryTypeReserved,
  EfiGcdMemoryTypeSystemMemory,
  EfiGcdMemoryTypeMemoryMappedIo,
  EfiGcdMemoryTypePersistent,
  EfiGcdMemoryTypeMoreReliable,
  EfiGcdMemoryTypeUnaccepted,
  EfiGcdMemoryTypeMaximum
} EFI_GCD_MEMORY_TYPE;

typedef struct _EFI_GCD_MEMORY_SPACE_DESCRIPTOR {
  EFI_PHYSICAL_ADDRESS    BaseAddress;
  UINT64                  Length;
  UINT64                  Capabilities;
  UINT64                  Attributes;
  EFI_GCD_MEMORY_TYPE     GcdMemoryType;
  EFI_HANDLE              ImageHandle;
  EFI_HANDLE              DeviceHandle;
} EFI_GCD_MEMORY_SPACE_DESCRIPTOR;

typedef
EFI_STATUS
(EFIAPI *EFI_GET_MEMORY_SPACE_MAP) (
  OUT UINTN                            *NumberOfDescriptors,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR  **MemorySpaceMap
  );

typedef enum {
  EfiGcdIoTypeNonExistent,
  EfiGcdIoTypeReserved,
  EfiGcdIoTypeIo,
  EfiGcdIoTypeMaximum
} EFI_GCD_IO_TYPE;

typedef struct _EFI_GCD_IO_SPACE_DESCRIPTOR {
  EFI_PHYSICAL_ADDRESS  BaseAddress;
  UINT64                Length;
  EFI_GCD_IO_TYPE       GcdIoType;
  EFI_HANDLE            ImageHandle;
  EFI_HANDLE            DeviceHandle;
} EFI_GCD_IO_SPACE_DESCRIPTOR;

typedef
EFI_STATUS
(EFIAPI *EFI_GET_IO_SPACE_MAP) (
  OUT UINTN                        *NumberOfDescriptors,
  OUT EFI_GCD_IO_SPACE_DESCRIPTOR  **IoSpaceMap
  );

typedef struct _DXE_SERVICES {
  EFI_TABLE_HEADER                  Hdr;

  //
  // Global Coherency               Domain Services
  //
  void  *AddMemorySpace;
  void  *AllocateMemorySpace;
  void  *FreeMemorySpace;
  void  *RemoveMemorySpace;
  void  *GetMemorySpaceDescriptor;
  void  *SetMemorySpaceAttributes;
  EFI_GET_MEMORY_SPACE_MAP          GetMemorySpaceMap;
  void  *AddIoSpace;
  void  *AllocateIoSpace;
  void  *FreeIoSpace;
  void  *RemoveIoSpace;
  void  *GetIoSpaceDescriptor;
  EFI_GET_IO_SPACE_MAP              GetIoSpaceMap;

  //
  // Dispatcher Services
  //
  void  *Dispatch;
  void  *Schedule;
  void  *Trust;


  //
  // Service to process a single firmware volume found in
  // a capsule
  //
  void  *ProcessFirmwareVolume;
  //
  // Extensions to Global Coherency Domain Services
  //
  void  *SetMemorySpaceCapabilities;
} DXE_SERVICES;


#endif /* AXL_UEFI_GEN_DXE_SERVICES_H */
