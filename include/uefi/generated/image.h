/** @file generated/image.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_IMAGE_H
#define AXL_UEFI_GEN_IMAGE_H

#include "types.h"
#include "status.h"

typedef struct _EFI_LOADED_IMAGE_PROTOCOL {
   UINT32                        Revision;
   EFI_HANDLE                    ParentHandle;
   EFI_SYSTEM_TABLE              *SystemTable;

   // Source location of the image
   EFI_HANDLE                    DeviceHandle;
   EFI_DEVICE_PATH_PROTOCOL      *FilePath;
   VOID                          *Reserved;

   // Image’s load options
   UINT32                        LoadOptionsSize;
   VOID                          *LoadOptions;

   // Location where image was loaded
   VOID                          *ImageBase;
   UINT64                        ImageSize;
   EFI_MEMORY_TYPE               ImageCodeType;
   EFI_MEMORY_TYPE               ImageDataType;
   EFI_IMAGE_UNLOAD              Unload;
} EFI_LOADED_IMAGE_PROTOCOL;


#endif /* AXL_UEFI_GEN_IMAGE_H */
