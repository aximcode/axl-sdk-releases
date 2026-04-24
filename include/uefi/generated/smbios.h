/** @file generated/smbios.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit — regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_SMBIOS_H
#define AXL_UEFI_GEN_SMBIOS_H

#include "types.h"
#include "status.h"

typedef UINT8 EFI_SMBIOS_TYPE;

typedef UINT16 EFI_SMBIOS_HANDLE;

typedef struct _EFI_SMBIOS_TABLE_HEADER {
  EFI_SMBIOS_TYPE Type ;
  UINT8 Length ;
  EFI_SMBIOS_HANDLE Handle ;
} EFI_SMBIOS_TABLE_HEADER;

typedef
EFI_STATUS
(EFIAPI *EFI_SMBIOS_ADD) (
  IN CONST EFI_SMBIOS_PROTOCOL *This ,
  IN EFI_HANDLE ProducerHandle  OPTIONAL,
  IN OUT EFI_SMBIOS_HANDLE *SmbiosHandle ,
  IN EFI_SMBIOS_TABLE_HEADER *Record
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SMBIOS_UPDATE_STRING) (
  IN CONST EFI_SMBIOS_PROTOCOL *This ,
  IN EFI_SMBIOS_HANDLE *SmbiosHandle ,
  IN UINTN *StringNumber ,
  IN CHAR8 *String
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SMBIOS_REMOVE) (
  IN CONST EFI_SMBIOS_PROTOCOL *This ,
  IN EFI_SMBIOS_HANDLE SmbiosHandle
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SMBIOS_GET_NEXT) (
  IN CONST EFI_SMBIOS_PROTOCOL *This ,
  IN OUT EFI_SMBIOS_HANDLE *SmbiosHandle ,
  IN EFI_SMBIOS_TYPE *Type  OPTIONAL,
  OUT EFI_SMBIOS_TABLE_HEADER **Record,
  OUT EFI_HANDLE *ProducerHandle OPTIONAL
  );

typedef struct _EFI_SMBIOS_PROTOCOL {
  EFI_SMBIOS_ADD           Add ;
  EFI_SMBIOS_UPDATE_STRING UpdateString ;
  EFI_SMBIOS_REMOVE        Remove ;
  EFI_SMBIOS_GET_NEXT      GetNext ;
  UINT8                    MajorVersion ;
  UINT8                    MinorVersion ;
} EFI_SMBIOS_PROTOCOL;


#endif /* AXL_UEFI_GEN_SMBIOS_H */
