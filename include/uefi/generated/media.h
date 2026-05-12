/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/media.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_MEDIA_H
#define AXL_UEFI_GEN_MEDIA_H

#include "types.h"
#include "status.h"

typedef struct _EFI_FILE_INFO {
  UINT64                         Size;
  UINT64                         FileSize;
  UINT64                         PhysicalSize;
  EFI_TIME                       CreateTime;
  EFI_TIME                       LastAccessTime;
  EFI_TIME                       ModificationTime;
  UINT64                         Attribute;
  CHAR16                         FileName[1];
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ       0x0000000000000001

#define EFI_FILE_MODE_WRITE      0x0000000000000002

#define EFI_FILE_MODE_CREATE     0x8000000000000000

#define EFI_FILE_READ_ONLY       0x0000000000000001

#define EFI_FILE_HIDDEN          0x0000000000000002

#define EFI_FILE_SYSTEM          0x0000000000000004

#define EFI_FILE_DIRECTORY       0x0000000000000010

#define EFI_FILE_ARCHIVE         0x0000000000000020

#define EFI_FILE_VALID_ATTR      0x0000000000000037

typedef struct _EFI_FILE_SYSTEM_INFO {
  UINT64                               Size;
  BOOLEAN                              ReadOnly;
  UINT64                               VolumeSize;
  UINT64                               FreeSpace;
  UINT32                               BlockSize;
  CHAR16                               VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;

typedef struct _EFI_FILE_SYSTEM_VOLUME_LABEL {
 CHAR16                            VolumeLabel[1];
} EFI_FILE_SYSTEM_VOLUME_LABEL;

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_OPEN) (
  IN EFI_FILE_PROTOCOL                  *This,
  OUT EFI_FILE_PROTOCOL                 **NewHandle,
  IN CHAR16                             *FileName,
  IN UINT64                             OpenMode,
  IN UINT64                             Attributes
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_CLOSE) (
  IN EFI_FILE_PROTOCOL                     *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_DELETE) (
  IN EFI_FILE_PROTOCOL                     *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_READ) (
  IN EFI_FILE_PROTOCOL           *This,
  IN OUT UINTN                   *BufferSize,
  OUT VOID                       *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_WRITE) (
  IN EFI_FILE_PROTOCOL              *This,
  IN OUT UINTN                      *BufferSize,
  IN VOID                           *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_GET_POSITION) (
  IN EFI_FILE_PROTOCOL                *This,
  OUT UINT64                          *Position
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_SET_POSITION) (
   IN EFI_FILE_PROTOCOL      *This,
   IN UINT64                 Position
   );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_GET_INFO) (
  IN EFI_FILE_PROTOCOL             *This,
  IN EFI_GUID                      *InformationType,
  IN OUT UINTN                     *BufferSize,
  OUT VOID                         *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_SET_INFO) (
  IN EFI_FILE_PROTOCOL                *This,
  IN EFI_GUID                         *InformationType,
  IN UINTN                            BufferSize,
  IN VOID                             *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_FLUSH) (
  IN EFI_FILE_PROTOCOL             *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_OPEN_EX) (
IN EFI_FILE_PROTOCOL                  *This,
OUT EFI_FILE_PROTOCOL                 **NewHandle,
IN CHAR16                             *FileName,
IN UINT64                             OpenMode,
IN UINT64                             Attributes,
IN OUT VOID *Token
);

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_READ_EX) (
  IN EFI_FILE_PROTOCOL                      *This,
  IN OUT VOID *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_WRITE_EX) (
  IN EFI_FILE_PROTOCOL                *This,
  IN OUT VOID *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_FILE_FLUSH_EX) (
  IN EFI_FILE_PROTOCOL                       *This,
  IN OUT VOID *Token
  );

typedef struct _EFI_FILE_PROTOCOL {
  UINT64                          Revision;
  EFI_FILE_OPEN                   Open;
  EFI_FILE_CLOSE                  Close;
  EFI_FILE_DELETE                 Delete;
  EFI_FILE_READ                   Read;
  EFI_FILE_WRITE                  Write;
  EFI_FILE_GET_POSITION           GetPosition;
  EFI_FILE_SET_POSITION           SetPosition;
  EFI_FILE_GET_INFO               GetInfo;
  EFI_FILE_SET_INFO               SetInfo;
  EFI_FILE_FLUSH                  Flush;
  EFI_FILE_OPEN_EX                OpenEx; // Added for revision 2
  EFI_FILE_READ_EX                ReadEx; // Added for revision 2
  EFI_FILE_WRITE_EX               WriteEx; // Added for revision 2
  EFI_FILE_FLUSH_EX               FlushEx; // Added for revision 2
} EFI_FILE_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME) (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL                   *This,
  OUT EFI_FILE_PROTOCOL                                **Root
  );

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
 UINT64                                         Revision;
 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME    OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_RAM_DISK_REGISTER_RAMDISK) (
  IN UINT64                              RamDiskBase,
  IN UINT64                              RamDiskSize,
  IN EFI_GUID                            *RamDiskType,
  IN EFI_DEVICE_PATH_PROTOCOL                     *ParentDevicePath OPTIONAL,
  OUT EFI_DEVICE_PATH_PROTOCOL           **DevicePath
);

typedef
EFI_STATUS
(EFIAPI *EFI_RAM_DISK_UNREGISTER_RAMDISK) (
  IN EFI_DEVICE_PATH_PROTOCOL              *DevicePath
);

typedef struct  _EFI_RAM_DISK_PROTOCOL {
  EFI_RAM_DISK_REGISTER_RAMDISK              Register;
  EFI_RAM_DISK_UNREGISTER_RAMDISK            Unregister;
} EFI_RAM_DISK_PROTOCOL;


#endif /* AXL_UEFI_GEN_MEDIA_H */
