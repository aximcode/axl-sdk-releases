/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/tables.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_TABLES_H
#define AXL_UEFI_GEN_TABLES_H

#include "types.h"
#include "status.h"

#define EFI_SYSTEM_TABLE_SIGNATURE 0x5453595320494249

#define EFI_2_100_SYSTEM_TABLE_REVISION ((2<<16) | (100))

#define EFI_2_90_SYSTEM_TABLE_REVISION  ((2<<16) | (90))

#define EFI_2_80_SYSTEM_TABLE_REVISION  ((2<<16) | (80))

#define EFI_2_70_SYSTEM_TABLE_REVISION  ((2<<16) | (70))

#define EFI_2_60_SYSTEM_TABLE_REVISION  ((2<<16) | (60))

#define EFI_2_50_SYSTEM_TABLE_REVISION  ((2<<16) | (50))

#define EFI_2_40_SYSTEM_TABLE_REVISION  ((2<<16) | (40))

#define EFI_2_31_SYSTEM_TABLE_REVISION  ((2<<16) | (31))

#define EFI_2_30_SYSTEM_TABLE_REVISION  ((2<<16) | (30))

#define EFI_2_20_SYSTEM_TABLE_REVISION  ((2<<16) | (20))

#define EFI_2_10_SYSTEM_TABLE_REVISION  ((2<<16) | (10))

#define EFI_2_00_SYSTEM_TABLE_REVISION  ((2<<16) | (00))

#define EFI_1_10_SYSTEM_TABLE_REVISION  ((1<<16) | (10))

#define EFI_1_02_SYSTEM_TABLE_REVISION  ((1<<16) | (02))

#define EFI_SPECIFICATION_VERSION       EFI_SYSTEM_TABLE_REVISION

#define EFI_SYSTEM_TABLE_REVISION       EFI_2_100_SYSTEM_TABLE_REVISION

typedef
EFI_STATUS
(EFIAPI *EFI_IMAGE_ENTRY_POINT) (
  IN EFI_HANDLE                  ImageHandle,
  IN EFI_SYSTEM_TABLE            *SystemTable
  );

typedef
EFI_TPL
(EFIAPI   *EFI_RAISE_TPL) (
  IN EFI_TPL   NewTpl
  );

typedef
VOID
(EFIAPI *EFI_RESTORE_TPL) (
   IN EFI_TPL OldTpl
   );

typedef
EFI_STATUS
(EFIAPI *EFI_ALLOCATE_PAGES) (
   IN EFI_ALLOCATE_TYPE                   Type,
   IN EFI_MEMORY_TYPE                     MemoryType,
   IN UINTN                               Pages,
   IN OUT EFI_PHYSICAL_ADDRESS            *Memory
   );

typedef
EFI_STATUS
(EFIAPI *EFI_FREE_PAGES) (
IN EFI_PHYSICAL_ADDRESS    Memory,
IN UINTN                   Pages
);

typedef
EFI_STATUS
(EFIAPI *EFI_GET_MEMORY_MAP) (
   IN OUT UINTN                  *MemoryMapSize,
   OUT EFI_MEMORY_DESCRIPTOR     *MemoryMap,
   OUT UINTN                     *MapKey,
   OUT UINTN                     *DescriptorSize,
   OUT UINT32                    *DescriptorVersion
  );

typedef
EFI_STATUS
(EFIAPI  *EFI_ALLOCATE_POOL) (
   IN EFI_MEMORY_TYPE            PoolType,
   IN UINTN                      Size,
   OUT VOID                      **Buffer
   );

typedef
EFI_STATUS
(EFIAPI *EFI_FREE_POOL) (
   IN VOID           *Buffer
   );

typedef
EFI_STATUS
(EFIAPI *EFI_CREATE_EVENT) (
   IN UINT32                   Type,
   IN EFI_TPL                  NotifyTpl,
   IN EFI_EVENT_NOTIFY         NotifyFunction OPTIONAL,
   IN VOID                     *NotifyContext OPTIONAL,
   OUT EFI_EVENT               *Event
   );

typedef
EFI_STATUS
(EFIAPI *EFI_SET_TIMER) (
   IN EFI_EVENT               Event,
   IN EFI_TIMER_DELAY         Type,
   IN UINT64                  TriggerTime
   );

typedef
EFI_STATUS
(EFIAPI *EFI_WAIT_FOR_EVENT) (
   IN UINTN             NumberOfEvents,
   IN EFI_EVENT         *Event,
   OUT UINTN            *Index
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIGNAL_EVENT) (
  IN EFI_EVENT Event
    );

typedef
EFI_STATUS
(EFIAPI *EFI_CLOSE_EVENT) (
  IN EFI_EVENT      Event
);

typedef
EFI_STATUS
(EFIAPI *EFI_CHECK_EVENT) (
  IN EFI_EVENT                Event
);

typedef
EFI_STATUS
(EFIAPI *EFI_INSTALL_PROTOCOL_INTERFACE) (
   IN OUT EFI_HANDLE             *Handle,
   IN EFI_GUID                   *Protocol,
   IN EFI_INTERFACE_TYPE         InterfaceType,
   IN VOID                       *Interface
   );

typedef
EFI_STATUS
(EFIAPI *EFI_REINSTALL_PROTOCOL_INTERFACE) (
   IN EFI_HANDLE           Handle,
   IN EFI_GUID             *Protocol,
   IN VOID                 *OldInterface,
   IN VOID                 *NewInterface
   );

typedef
EFI_STATUS
(EFIAPI *EFI_UNINSTALL_PROTOCOL_INTERFACE) (
   IN EFI_HANDLE        Handle,
   IN EFI_GUID          *Protocol,
   IN VOID              *Interface
   );

typedef
EFI_STATUS
(EFIAPI *EFI_HANDLE_PROTOCOL) (
   IN EFI_HANDLE                    Handle,
   IN EFI_GUID                      *Protocol,
   OUT VOID                         **Interface
   );

typedef
EFI_STATUS
(EFIAPI *EFI_REGISTER_PROTOCOL_NOTIFY) (
   IN EFI_GUID                         *Protocol,
   IN EFI_EVENT                        Event,
   OUT VOID                            **Registration
   );

typedef
EFI_STATUS
(EFIAPI *EFI_LOCATE_HANDLE) (
   IN EFI_LOCATE_SEARCH_TYPE                 SearchType,
   IN EFI_GUID                               *Protocol OPTIONAL,
   IN VOID                                   *SearchKey OPTIONAL,
   IN OUT UINTN                              *BufferSize,
   OUT EFI_HANDLE                            *Buffer
   );

typedef
EFI_STATUS
(EFIAPI *EFI_LOCATE_DEVICE_PATH) (
   IN EFI_GUID                         *Protocol,
   IN OUT EFI_DEVICE_PATH_PROTOCOL     **DevicePath,
   OUT EFI_HANDLE                      *Device
   );

typedef
EFI_STATUS
(EFIAPI *EFI_INSTALL_CONFIGURATION_TABLE) (
   IN EFI_GUID                               *Guid,
   IN VOID                                   *Table
   );

typedef
EFI_STATUS
(EFIAPI *EFI_IMAGE_LOAD) (
   IN BOOLEAN                          BootPolicy,
   IN EFI_HANDLE                       ParentImageHandle,
   IN EFI_DEVICE_PATH_PROTOCOL         *DevicePath   OPTIONAL,
   IN VOID                             *SourceBuffer OPTIONAL,
   IN UINTN                            SourceSize,
   OUT EFI_HANDLE                      *ImageHandle
   );

typedef
EFI_STATUS
(EFIAPI *EFI_IMAGE_START) (
   IN EFI_HANDLE                             ImageHandle,
   OUT UINTN                                 *ExitDataSize,
   OUT CHAR16                                **ExitData OPTIONAL
   );

typedef
EFI_STATUS
(EFIAPI *EFI_IMAGE_UNLOAD) (
   IN EFI_HANDLE           ImageHandle
   );

typedef
EFI_STATUS
(EFIAPI *EFI_EXIT) (
   IN EFI_HANDLE                      ImageHandle,
   IN EFI_STATUS                      ExitStatus,
   IN UINTN                           ExitDataSize,
   IN CHAR16                          *ExitData OPTIONAL
   );

typedef
EFI_STATUS
(EFIAPI *EFI_EXIT_BOOT_SERVICES) (
  IN EFI_HANDLE                       ImageHandle,
  IN UINTN                            MapKey
  );

typedef
EFI_STATUS
(EFIAPI *EFI_GET_NEXT_MONOTONIC_COUNT) (
   OUT UINT64                       *Count
   );

typedef
EFI_STATUS
(EFIAPI *EFI_STALL) (
   IN UINTN                Microseconds
   );

typedef
EFI_STATUS
(EFIAPI *EFI_SET_WATCHDOG_TIMER) (
   IN UINTN                          Timeout,
   IN UINT64                         WatchdogCode,
   IN UINTN                          DataSize,
   IN CHAR16                         *WatchdogData OPTIONAL
   );

typedef
EFI_STATUS
(EFIAPI *EFI_CONNECT_CONTROLLER) (
   IN EFI_HANDLE                       ControllerHandle,
   IN EFI_HANDLE                       *DriverImageHandle OPTIONAL,
   IN EFI_DEVICE_PATH_PROTOCOL         *RemainingDevicePath OPTIONAL,
   IN BOOLEAN Recursive
   );

typedef
EFI_STATUS
(EFIAPI *EFI_DISCONNECT_CONTROLLER) (
   IN EFI_HANDLE                       ControllerHandle,
   IN EFI_HANDLE                       DriverImageHandle OPTIONAL,
   IN EFI_HANDLE                       ChildHandle OPTIONAL
   );

typedef
EFI_STATUS
(EFIAPI *EFI_OPEN_PROTOCOL) (
   IN EFI_HANDLE                    Handle,
   IN EFI_GUID                      *Protocol,
   OUT VOID                         **Interface OPTIONAL,
   IN EFI_HANDLE                    AgentHandle,
   IN EFI_HANDLE                    ControllerHandle,
   IN UINT32                        Attributes
   );

typedef
EFI_STATUS
(EFIAPI *EFI_CLOSE_PROTOCOL) (
   IN EFI_HANDLE                 Handle,
   IN EFI_GUID                   *Protocol,
   IN EFI_HANDLE                 AgentHandle,
   IN EFI_HANDLE                 ControllerHandle
   );

typedef
EFI_STATUS
(EFIAPI *EFI_OPEN_PROTOCOL_INFORMATION) (
   IN EFI_HANDLE                             Handle,
   IN EFI_GUID                               *Protocol,
   OUT EFI_OPEN_PROTOCOL_INFORMATION_ENTRY   **EntryBuffer,
   OUT UINTN                                 *EntryCount
   );

typedef
EFI_STATUS
(EFIAPI *EFI_PROTOCOLS_PER_HANDLE) (
   IN EFI_HANDLE                             Handle,
   OUT EFI_GUID                              ***ProtocolBuffer,
   OUT UINTN                                 *ProtocolBufferCount
   );

typedef
EFI_STATUS
(EFIAPI *EFI_LOCATE_HANDLE_BUFFER) (
   IN EFI_LOCATE_SEARCH_TYPE                    SearchType,
   IN EFI_GUID                                  *Protocol OPTIONAL,
   IN VOID                                      *SearchKey OPTIONAL,
   OUT UINTN                                    *NoHandles,
   OUT EFI_HANDLE                               **Buffer
   );

typedef
EFI_STATUS
(EFIAPI *EFI_LOCATE_PROTOCOL) (
  IN EFI_GUID                            *Protocol,
  IN VOID                                *Registration OPTIONAL,
  OUT VOID                               **Interface
 );

typedef
EFI_STATUS
(EFIAPI *EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES) (
   IN OUT EFI_HANDLE                               *Handle,
   ...
   );

typedef
EFI_STATUS
(EFIAPI *EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES) (
   IN EFI_HANDLE Handle,
   ...
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CALCULATE_CRC32) (
   IN VOID                          *Data,
   IN UINTN                         DataSize,
   OUT UINT32                       *Crc32
   );

typedef
VOID
(EFIAPI *EFI_COPY_MEM) (
   IN VOID                       *Destination,
   IN VOID                       *Source,
   IN UINTN                      Length
   );

typedef
VOID
(EFIAPI *EFI_SET_MEM) (
   IN VOID                             *Buffer,
   IN UINTN                            Size,
   IN UINT8                            Value
   );

typedef
EFI_STATUS
(EFIAPI *EFI_CREATE_EVENT_EX) (
   IN UINT32                  Type,
   IN EFI_TPL                 NotifyTpl,
   IN EFI_EVENT_NOTIFY        NotifyFunction OPTIONAL,
   IN CONST VOID              *NotifyContext OPTIONAL,
   IN CONST EFI_GUID          *EventGroup OPTIONAL,
   OUT EFI_EVENT              *Event
   );

#define EFI_BOOT_SERVICES_SIGNATURE 0x56524553544f4f42

#define EFI_BOOT_SERVICES_REVISION EFI_SPECIFICATION_VERSION

typedef struct _EFI_BOOT_SERVICES {
  EFI_TABLE_HEADER     Hdr;

  //
  // Task Priority Services
  //
  EFI_RAISE_TPL        RaiseTPL;       // EFI 1.0+
  EFI_RESTORE_TPL      RestoreTPL;     // EFI 1.0+

    //
    // Memory Services
    //
    EFI_ALLOCATE_PAGES   AllocatePages;  // EFI 1.0+
    EFI_FREE_PAGES       FreePages;      // EFI 1.0+
    EFI_GET_MEMORY_MAP   GetMemoryMap;   // EFI 1.0+
    EFI_ALLOCATE_POOL    AllocatePool;   // EFI 1.0+
    EFI_FREE_POOL        FreePool;       // EFI 1.0+

    //
    // Event & Timer Services
    //
    EFI_CREATE_EVENT     CreateEvent;    // EFI 1.0+
    EFI_SET_TIMER        SetTimer;       // EFI 1.0+
    EFI_WAIT_FOR_EVENT   WaitForEvent;   // EFI 1.0+
    EFI_SIGNAL_EVENT     SignalEvent;    // EFI 1.0+
    EFI_CLOSE_EVENT      CloseEvent;     // EFI 1.0+
    EFI_CHECK_EVENT      CheckEvent;     // EFI 1.0+

    //
    // Protocol Handler Services
    //
    EFI_INSTALL_PROTOCOL_INTERFACE     InstallProtocolInterface;            // EFI 1.0+
    EFI_REINSTALL_PROTOCOL_INTERFACE   ReinstallProtocolInterface;          // EFI 1.0+
    EFI_UNINSTALL_PROTOCOL_INTERFACE   UninstallProtocolInterface;          // EFI 1.0+
    EFI_HANDLE_PROTOCOL                HandleProtocol;                      // EFI 1.0+
 VOID*   Reserved;    // EFI 1.0+
    EFI_REGISTER_PROTOCOL_NOTIFY       RegisterProtocolNotify;              // EFI  1.0+
    EFI_LOCATE_HANDLE                  LocateHandle;                        // EFI 1.0+
    EFI_LOCATE_DEVICE_PATH             LocateDevicePath;                    // EFI 1.0+
 EFI_INSTALL_CONFIGURATION_TABLE       InstallConfigurationTable;           // EFI 1.0+

    //
    // Image Services
    //
    EFI_IMAGE_LOAD               LoadImage;        // EFI 1.0+
    EFI_IMAGE_START                StartImage;       // EFI 1.0+
    EFI_EXIT                       Exit;             // EFI 1.0+
    EFI_IMAGE_UNLOAD               UnloadImage;      // EFI 1.0+
    EFI_EXIT_BOOT_SERVICES         ExitBootServices; // EFI 1.0+

    //
    // Miscellaneous Services
    //
    EFI_GET_NEXT_MONOTONIC_COUNT   GetNextMonotonicCount; // EFI 1.0+
    EFI_STALL                      Stall;                 // EFI 1.0+
    EFI_SET_WATCHDOG_TIMER         SetWatchdogTimer;      // EFI 1.0+

    //
    // DriverSupport Services
    //
    EFI_CONNECT_CONTROLLER         ConnectController;     // EFI 1.1
    EFI_DISCONNECT_CONTROLLER      DisconnectController;  // EFI 1.1+

    //
    // Open and Close Protocol Services
    //
    EFI_OPEN_PROTOCOL              OpenProtocol;           // EFI 1.1+
    EFI_CLOSE_PROTOCOL             CloseProtocol;          // EFI 1.1+
 EFI_OPEN_PROTOCOL_INFORMATION     OpenProtocolInformation;// EFI 1.1+

    //
    // Library Services
    //
    EFI_PROTOCOLS_PER_HANDLE       ProtocolsPerHandle;     // EFI 1.1+
    EFI_LOCATE_HANDLE_BUFFER       LocateHandleBuffer;     // EFI 1.1+
    EFI_LOCATE_PROTOCOL            LocateProtocol;         // EFI 1.1+
  EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES  InstallMultipleProtocolInterfaces;    // EFI 1.1+
  EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES UninstallMultipleProtocolInterfaces;   // EFI 1.1+*

    //
    // 32-bit CRC Services
    //
    EFI_CALCULATE_CRC32    CalculateCrc32;     // EFI 1.1+

    //
    // Miscellaneous Services
    //
    EFI_COPY_MEM           CopyMem;        // EFI 1.1+
    EFI_SET_MEM            SetMem;         // EFI 1.1+
    EFI_CREATE_EVENT_EX    CreateEventEx;  // UEFI 2.0+
  } EFI_BOOT_SERVICES;

typedef
EFI_STATUS
(EFIAPI *GetTime) (
   OUT EFI_TIME                  *Time,
   OUT EFI_TIME_CAPABILITIES     *Capabilities OPTIONAL
  );
typedef GetTime EFI_GET_TIME;

typedef
EFI_STATUS
(EFIAPI *SetTime) (
  IN EFI_TIME       *Time
 );
typedef SetTime EFI_SET_TIME;

typedef
EFI_STATUS
(EFIAPI *GetWakeupTime) (
   OUT BOOLEAN            *Enabled,
   OUT BOOLEAN            *Pending,
   OUT EFI_TIME           *Time
  );
typedef GetWakeupTime EFI_GET_WAKEUP_TIME;

typedef
EFI_STATUS
(EFIAPI *SetWakeupTime) (
   IN BOOLEAN         Enable,
   IN EFI_TIME        *Time OPTIONAL
  );
typedef SetWakeupTime EFI_SET_WAKEUP_TIME;

typedef
EFI_STATUS
(EFIAPI *SetVirtualAddressMap) (
   IN UINTN                 MemoryMapSize,
   IN UINTN                 DescriptorSize,
   IN UINT32                DescriptorVersion,
   IN EFI_MEMORY_DESCRIPTOR *VirtualMap
  );
typedef SetVirtualAddressMap EFI_SET_VIRTUAL_ADDRESS_MAP;

typedef
EFI_STATUS
(EFIAPI *ConvertPointer) (
   IN UINTN             DebugDisposition,
   IN VOID              **Address
  );
typedef ConvertPointer EFI_CONVERT_POINTER;

#define EFI_VARIABLE_NON_VOLATILE                           0x00000001

#define EFI_VARIABLE_BOOTSERVICE_ACCESS                     0x00000002

#define EFI_VARIABLE_RUNTIME_ACCESS                         0x00000004

#define EFI_VARIABLE_HARDWARE_ERROR_RECORD                  0x00000008

#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS  0x00000020

#define EFI_VARIABLE_APPEND_WRITE                           0x00000040

#define EFI_VARIABLE_ENHANCED_AUTHENTICATED_ACCESS          0x00000080

typedef
EFI_STATUS
(EFIAPI *GetVariable) (
  IN CHAR16           *VariableName,
  IN EFI_GUID         *VendorGuid,
  OUT UINT32          *Attributes OPTIONAL,
  IN OUT UINTN        *DataSize,
  OUT VOID            *Data OPTIONAL
 );
typedef GetVariable EFI_GET_VARIABLE;

typedef
EFI_STATUS
(EFIAPI *GetNextVariableName) (
  IN OUT UINTN           *VariableNameSize,
  IN OUT CHAR16          *VariableName,
  IN OUT EFI_GUID        *VendorGuid
 );
typedef GetNextVariableName EFI_GET_NEXT_VARIABLE_NAME;

typedef
EFI_STATUS
(EFIAPI *SetVariable) (
   IN CHAR16            *VariableName,
   IN EFI_GUID          *VendorGuid,
   IN UINT32            Attributes,
   IN UINTN             DataSize,
   IN VOID              *Data
);
typedef SetVariable EFI_SET_VARIABLE;

typedef
EFI_STATUS
(EFIAPI *GetNextHighMonotonicCount) (
  OUT UINT32               *HighCount
 );
typedef GetNextHighMonotonicCount EFI_GET_NEXT_HIGH_MONO_COUNT;

typedef
VOID
(EFIAPI *EFI_RESET_SYSTEM) (
   IN EFI_RESET_TYPE          ResetType,
   IN EFI_STATUS              ResetStatus,
   IN UINTN                   DataSize,
   IN VOID                    *ResetData OPTIONAL
 );

typedef
EFI_STATUS
(EFIAPI *UpdateCapsule) (
   IN EFI_CAPSULE_HEADER      **CapsuleHeaderArray,
   IN UINTN                   CapsuleCount,
   IN EFI_PHYSICAL_ADDRESS    ScatterGatherList OPTIONAL
  );
typedef UpdateCapsule EFI_UPDATE_CAPSULE;

typedef
EFI_STATUS
(EFIAPI *QueryCapsuleCapabilities) (
   IN EFI_CAPSULE_HEADER         **CapsuleHeaderArray,
   IN UINTN                      CapsuleCount,
   OUT UINT64                    *MaximumCapsuleSize,
   OUT EFI_RESET_TYPE            *ResetType
  );
typedef QueryCapsuleCapabilities EFI_QUERY_CAPSULE_CAPABILITIES;

typedef
EFI_STATUS
(EFIAPI *QueryVariableInfo) (
   IN UINT32             Attributes,
   OUT UINT64            *MaximumVariableStorageSize,
   OUT UINT64            *RemainingVariableStorageSize,
   OUT UINT64            *MaximumVariableSize
  );
typedef QueryVariableInfo EFI_QUERY_VARIABLE_INFO;

#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544e5552

#define EFI_RUNTIME_SERVICES_REVISION EFI_SPECIFICATION_VERSION

typedef struct _EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER                 Hdr;

    //
    // Time Services
    //
    EFI_GET_TIME                     GetTime;
    EFI_SET_TIME                     SetTime;
    EFI_GET_WAKEUP_TIME              GetWakeupTime;
    EFI_SET_WAKEUP_TIME              SetWakeupTime;

    //
    // Virtual Memory Services
    //
    EFI_SET_VIRTUAL_ADDRESS_MAP      SetVirtualAddressMap;
  EFI_CONVERT_POINTER                ConvertPointer;

    //
    // Variable Services
    //
    EFI_GET_VARIABLE                 GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME       GetNextVariableName;
    EFI_SET_VARIABLE                 SetVariable;


    //
    // Miscellaneous Services
    //
    EFI_GET_NEXT_HIGH_MONO_COUNT     GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM                 ResetSystem;

   //
   // UEFI 2.0 Capsule Services
   //
   EFI_UPDATE_CAPSULE               UpdateCapsule;
   EFI_QUERY_CAPSULE_CAPABILITIES   QueryCapsuleCapabilities;


 //
 // Miscellaneous UEFI 2.0 Service
 //
  EFI_QUERY_VARIABLE_INFO          QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct _EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER                 Hdr;
  CHAR16                           *FirmwareVendor;
  UINT32                           FirmwareRevision;
  EFI_HANDLE                       ConsoleInHandle;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL   *ConIn;
  EFI_HANDLE                       ConsoleOutHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
  EFI_HANDLE                       StandardErrorHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *StdErr;
  EFI_RUNTIME_SERVICES             *RuntimeServices;
  EFI_BOOT_SERVICES                *BootServices;
  UINTN                            NumberOfTableEntries;
  EFI_CONFIGURATION_TABLE          *ConfigurationTable;
} EFI_SYSTEM_TABLE;


#endif /* AXL_UEFI_GEN_TABLES_H */
