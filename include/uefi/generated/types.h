/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/types.h
    Auto-generated from UEFI Specification 2.11.
    Base UEFI types, constants, and enums.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_TYPES_H
#define AXL_UEFI_GEN_TYPES_H

#include "calling.h"
#include <stdint.h>
#include <stddef.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "AXL UEFI types require x86_64 or AARCH64"
#endif

// Types not in UEFI spec (Shell Spec / project-specific)
typedef void     *SHELL_FILE_HANDLE;

#ifndef TRUE
#define TRUE   1
#endif
#ifndef FALSE
#define FALSE  0
#endif
#ifndef NULL
#define NULL   ((void *)0)
#endif

#define MAX_UINTN  ((UINTN)-1)


typedef uint8_t  BOOLEAN;

typedef int8_t  INT8;

typedef int16_t  INT16;

typedef int32_t  INT32;

typedef int64_t  INT64;

typedef uint8_t  UINT8;

typedef uint16_t  UINT16;

typedef uint32_t  UINT32;

typedef uint64_t  UINT64;

typedef char  CHAR8;

typedef uint16_t  CHAR16;

typedef uint64_t  UINTN;

typedef int64_t  INTN;

typedef void * EFI_HANDLE;

typedef void * EFI_EVENT;

typedef UINTN EFI_STATUS;

typedef UINTN EFI_TPL;

typedef UINT64 EFI_LBA;

typedef UINT64 EFI_PHYSICAL_ADDRESS;

typedef UINT64 EFI_VIRTUAL_ADDRESS;

typedef struct _EFI_GUID {
   UINT32      Data1;
   UINT16      Data2;
   UINT16      Data3;
   UINT8       Data4[8];
} EFI_GUID;

typedef struct _EFI_TIME {
   UINT16    Year;              // 1900 - 9999
   UINT8     Month;             // 1 - 12
   UINT8     Day;               // 1 - 31
   UINT8     Hour;              // 0 - 23
   UINT8     Minute;            // 0 - 59
   UINT8     Second;            // 0 - 59
   UINT8     Pad1;
   UINT32    Nanosecond;        // 0 - 999,999,999
   INT16     TimeZone;          // —1440 to 1440 or 2047
   UINT8     Daylight;
   UINT8     Pad2;
 }   EFI_TIME;

typedef struct _EFI_TIME_CAPABILITIES {
   UINT32                  Resolution;
   UINT32                  Accuracy;
   BOOLEAN                 SetsToZero;
}   EFI_TIME_CAPABILITIES;

typedef struct _EFI_INPUT_KEY {
 UINT16                             ScanCode;
 CHAR16                             UnicodeChar;
} EFI_INPUT_KEY;

typedef struct _EFI_IPv4_ADDRESS {
  UINT8             Addr[4];
} EFI_IPv4_ADDRESS;

typedef struct _EFI_MAC_ADDRESS {
  UINT8             Addr[32];
} EFI_MAC_ADDRESS;

typedef struct _EFI_IPv6_ADDRESS {
  UINT8             Addr[16];
}   EFI_IPv6_ADDRESS;

typedef enum {
   EfiReservedMemoryType,
   EfiLoaderCode,
   EfiLoaderData,
   EfiBootServicesCode,
   EfiBootServicesData,
   EfiRuntimeServicesCode,
   EfiRuntimeServicesData,
   EfiConventionalMemory,
   EfiUnusableMemory,
   EfiACPIReclaimMemory,
   EfiACPIMemoryNVS,
   EfiMemoryMappedIO,
   EfiMemoryMappedIOPortSpace,
   EfiPalCode,
   EfiPersistentMemory,
   EfiUnacceptedMemoryType,
   EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
   TimerCancel,
   TimerPeriodic,
   TimerRelative
} EFI_TIMER_DELAY;

typedef enum {
   AllHandles,
   ByRegisterNotify,
   ByProtocol
  } EFI_LOCATE_SEARCH_TYPE;

typedef enum {
   AllocateAnyPages,
   AllocateMaxAddress,
   AllocateAddress,
   MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
   EFI_NATIVE_INTERFACE
 } EFI_INTERFACE_TYPE;

typedef enum {
   EfiResetCold,
   EfiResetWarm,
   EfiResetShutdown,
   EfiResetPlatformSpecific
}   EFI_RESET_TYPE;

typedef struct _EFI_MEMORY_DESCRIPTOR {
   UINT32                     Type;
   EFI_PHYSICAL_ADDRESS       PhysicalStart;
   EFI_VIRTUAL_ADDRESS        VirtualStart;
   UINT64                     NumberOfPages;
   UINT64                     Attribute;
  } EFI_MEMORY_DESCRIPTOR;

typedef struct _EFI_OPEN_PROTOCOL_INFORMATION_ENTRY {
   EFI_HANDLE                          AgentHandle;
   EFI_HANDLE                          ControllerHandle;
   UINT32                              Attributes;
   UINT32                              OpenCount;
  } EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;

typedef struct _EFI_CAPSULE_HEADER {
   EFI_GUID             CapsuleGuid;
   UINT32               HeaderSize;
   UINT32               Flags;
   UINT32               CapsuleImageSize;
 } EFI_CAPSULE_HEADER;

typedef struct _EFI_SYSTEM_RESOURCE_ENTRY {
  EFI_GUID       FwClass;
  UINT32         FwType;
  UINT32         FwVersion;
  UINT32         LowestSupportedFwVersion;
  UINT32         CapsuleFlags;
  UINT32         LastAttemptVersion;
  UINT32         LastAttemptStatus;
}   EFI_SYSTEM_RESOURCE_ENTRY;

typedef struct _EFI_SYSTEM_RESOURCE_TABLE {
  UINT32                         FwResourceCount;
  UINT32                         FwResourceCountMax;
  UINT64                         FwResourceVersion;
  //EFI_SYSTEM_RESOURCE_ENTRY    Entries[1];
} EFI_SYSTEM_RESOURCE_TABLE;

typedef struct _EFI_DEVICE_PATH_PROTOCOL {
  UINT8           Type;
  UINT8           SubType;
  UINT8           Length[2];
 } EFI_DEVICE_PATH_PROTOCOL;

typedef struct _EFI_TABLE_HEADER {
  UINT64      Signature;
  UINT32      Revision;
  UINT32      HeaderSize;
  UINT32      CRC32;
  UINT32      Reserved;
 } EFI_TABLE_HEADER;

typedef struct _SIMPLE_TEXT_OUTPUT_MODE {
 INT32                              MaxMode;
 // current settings
 INT32                              Mode;
 INT32                              Attribute;
 INT32                              CursorColumn;
 INT32                              CursorRow;
 BOOLEAN                            CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_CONFIGURATION_TABLE {
  EFI_GUID           VendorGuid;
  VOID               *VendorTable;
}   EFI_CONFIGURATION_TABLE;

#define EFI_BLACK                              0x00

#define EFI_BLUE                               0x01

#define EFI_GREEN                              0x02

#define EFI_CYAN                               0x03

#define EFI_RED                                0x04

#define EFI_MAGENTA                            0x05

#define EFI_BROWN                              0x06

#define EFI_LIGHTGRAY                          0x07

#define EFI_BRIGHT                             0x08

#define EFI_DARKGRAY  0x08

#define EFI_LIGHTBLUE                          0x09

#define EFI_LIGHTGREEN                         0x0A

#define EFI_LIGHTCYAN                          0x0B

#define EFI_LIGHTRED                           0x0C

#define EFI_LIGHTMAGENTA                       0x0D

#define EFI_YELLOW                             0x0E

#define EFI_WHITE                              0x0F

#define EFI_BACKGROUND_BLACK                   0x00

#define EFI_BACKGROUND_BLUE                    0x10

#define EFI_BACKGROUND_GREEN                   0x20

#define EFI_BACKGROUND_CYAN                    0x30

#define EFI_BACKGROUND_RED                     0x40

#define EFI_BACKGROUND_MAGENTA                 0x50

#define EFI_BACKGROUND_BROWN                   0x60

#define EFI_BACKGROUND_LIGHTGRAY               0x70

typedef
VOID
(EFIAPI *EFI_EVENT_NOTIFY) (
  IN EFI_EVENT          Event,
  IN VOID              *Context
   );

#define EVT_TIMER                            0x80000000

#define EVT_RUNTIME                          0x40000000

#define EVT_NOTIFY_WAIT                      0x00000100

#define EVT_NOTIFY_SIGNAL                    0x00000200

#define EVT_SIGNAL_EXIT_BOOT_SERVICES        0x00000201

#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE    0x60000202

#define TPL_APPLICATION    4

#define TPL_CALLBACK       8

#define TPL_NOTIFY         16

#define TPL_HIGH_LEVEL     31

// EFI_TEXT_ATTR -- commented out in spec, hand-written here
#define EFI_TEXT_ATTR(fg, bg)  ((fg) | ((bg) << 4))

static inline int
axl_guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}


#endif /* AXL_UEFI_GEN_TYPES_H */
