/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-uefi-extra.h
    Supplemental UEFI definitions not extractable from spec HTML.

    Contains:
      - EFI_SHELL_PROTOCOL (Shell Spec 2.2 — PDF only, no HTML)
      - EFI_SHELL_PARAMETERS_PROTOCOL (Shell Spec 2.2)
      - SMBIOS_HANDLE_PI_RESERVED (EDK2 convention, not in spec)
      - IPMI_PROTOCOL (EDK2 MdeModulePkg, not a UEFI/PI spec type)
      - DELL_IPMI_TRANSPORT (vendor-proprietary)
      - SMBIOS_TABLE_TYPE38 (DMTF SMBIOS spec, not UEFI/PI/ACPI)

    Include via <uefi/axl-uefi.h> after generated/all.h.
**/

#ifndef AXL_UEFI_EXTRA_H
#define AXL_UEFI_EXTRA_H

#include "generated/all.h"

// ===================================================================
// EFI_SHELL_PROTOCOL (UEFI Shell Spec 2.2, Section 2.2)
//
// 40 function pointer slots + 2 fields + 4 Shell 2.1 extensions.
// AXL uses: OpenFileByName, CloseFile, ReadFile, WriteFile,
//           DeleteFileByName, GetFilePosition, SetFilePosition,
//           GetFileSize, GetFileInfo, ExecutionBreak.
// ===================================================================

typedef struct _EFI_SHELL_PROTOCOL  EFI_SHELL_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SHELL_OPEN_FILE_BY_NAME)(
    IN  CHAR16             *FileName,
    OUT SHELL_FILE_HANDLE  *FileHandle,
    IN  UINT64              OpenMode
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_CLOSE_FILE)(
    IN SHELL_FILE_HANDLE  FileHandle
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_READ_FILE)(
    IN     SHELL_FILE_HANDLE  FileHandle,
    IN OUT UINTN             *BufferSize,
    OUT    VOID              *Buffer
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_WRITE_FILE)(
    IN     SHELL_FILE_HANDLE  FileHandle,
    IN OUT UINTN             *BufferSize,
    IN     VOID              *Buffer
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_DELETE_FILE_BY_NAME)(
    IN CHAR16  *FileName
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_GET_FILE_POSITION)(
    IN  SHELL_FILE_HANDLE  FileHandle,
    OUT UINT64            *Position
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_SET_FILE_POSITION)(
    IN SHELL_FILE_HANDLE  FileHandle,
    IN UINT64             Position
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_GET_FILE_SIZE)(
    IN  SHELL_FILE_HANDLE  FileHandle,
    OUT UINT64            *Size
    );

typedef EFI_FILE_INFO * (EFIAPI *EFI_SHELL_GET_FILE_INFO)(
    IN SHELL_FILE_HANDLE  FileHandle
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_SET_FILE_INFO)(
    IN SHELL_FILE_HANDLE  FileHandle,
    IN CONST EFI_FILE_INFO  *FileInfo
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_CREATE_FILE)(
    IN CONST CHAR16       *FileName,
    IN UINT64              FileAttribs,
    OUT SHELL_FILE_HANDLE *FileHandle
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_EXECUTE)(
    IN EFI_HANDLE   *ParentImageHandle,
    IN CHAR16       *CommandLine,
    IN CHAR16       **Environment,
    OUT EFI_STATUS  *StatusCode
    );

typedef CONST CHAR16 * (EFIAPI *EFI_SHELL_GET_ENV)(
    IN CONST CHAR16  *Name
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_SET_ENV)(
    IN CONST CHAR16  *Name,
    IN CONST CHAR16  *Value,
    IN BOOLEAN        Volatile
    );

typedef CONST CHAR16 * (EFIAPI *EFI_SHELL_GET_CUR_DIR)(
    IN CONST CHAR16  *FileSystemMapping
    );

typedef EFI_STATUS (EFIAPI *EFI_SHELL_SET_CUR_DIR)(
    IN CONST CHAR16  *FileSystemMapping,
    IN CONST CHAR16  *Dir
    );

// ===================================================================
// EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL (UEFI Spec 2.x, §12.2)
//
// AXL backend installs a key notify on this protocol's handle to
// catch raw 0x03 (Ctrl-C) bytes coming over serial — TerminalDxe
// delivers them with KeyShiftState=0 (no Ctrl modifier reported on
// serial), so the EDK2 Shell's own Ctrl-C notify (which requires
// Ctrl-pressed bits) doesn't fire. We register an additional notify
// that matches KeyShiftState=0 and signals shell->ExecutionBreak
// from there.
//
// Only RegisterKeyNotify / UnregisterKeyNotify are used; other
// fields are void* placeholders.
// ===================================================================

typedef struct {
    UINT32  KeyShiftState;
    UINT8   KeyToggleState;
} EFI_KEY_STATE;

typedef struct {
    EFI_INPUT_KEY  Key;
    EFI_KEY_STATE  KeyState;
} EFI_KEY_DATA;

typedef EFI_STATUS (EFIAPI *EFI_KEY_NOTIFY_FUNCTION)(
    IN EFI_KEY_DATA  *KeyData
    );

typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_EX_REGISTER_KEY_NOTIFY)(
    IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
    IN  EFI_KEY_DATA                       *KeyData,
    IN  EFI_KEY_NOTIFY_FUNCTION             KeyNotificationFunction,
    OUT VOID                              **NotifyHandle
    );

typedef EFI_STATUS (EFIAPI *EFI_INPUT_EX_UNREGISTER_KEY_NOTIFY)(
    IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
    IN VOID                               *NotificationHandle
    );

struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    void                                *Reset;
    void                                *ReadKeyStrokeEx;
    EFI_EVENT                            WaitForKeyEx;
    void                                *SetState;
    EFI_INPUT_EX_REGISTER_KEY_NOTIFY     RegisterKeyNotify;
    EFI_INPUT_EX_UNREGISTER_KEY_NOTIFY   UnregisterKeyNotify;
};

struct _EFI_SHELL_PROTOCOL {
    EFI_SHELL_EXECUTE              Execute;              // 2.2.2 (USED)
    EFI_SHELL_GET_ENV              GetEnv;               // 2.2.3 (USED)
    EFI_SHELL_SET_ENV              SetEnv;               // 2.2.4 (USED)
    void                          *GetAlias;             // 2.2.5
    void                          *SetAlias;             // 2.2.6
    void                          *GetHelpText;          // 2.2.7
    void                          *GetDevicePathFromMap; // 2.2.8
    void                          *GetMapFromDevicePath; // 2.2.9
    void                          *GetDevicePathFromFilePath; // 2.2.10
    void                          *GetFilePathFromDevicePath; // 2.2.11
    void                          *SetMap;               // 2.2.12
    EFI_SHELL_GET_CUR_DIR         GetCurDir;             // 2.2.13 (USED)
    EFI_SHELL_SET_CUR_DIR         SetCurDir;             // 2.2.14 (USED)
    void                          *OpenFileList;         // 2.2.15
    void                          *FreeFileList;         // 2.2.16
    void                          *RemoveDupInFileList;  // 2.2.17
    void                          *BatchIsActive;        // 2.2.18
    void                          *IsRootShell;          // 2.2.19
    void                          *EnablePageBreak;      // 2.2.20
    void                          *DisablePageBreak;     // 2.2.21
    void                          *GetPageBreak;         // 2.2.22
    void                          *GetDeviceName;        // 2.2.23
    EFI_SHELL_GET_FILE_INFO        GetFileInfo;          // 2.2.24 (USED)
    EFI_SHELL_SET_FILE_INFO        SetFileInfo;           // 2.2.25 (USED)
    EFI_SHELL_OPEN_FILE_BY_NAME    OpenFileByName;       // 2.2.26 (USED)
    EFI_SHELL_CLOSE_FILE           CloseFile;            // 2.2.27 (USED)
    EFI_SHELL_CREATE_FILE          CreateFile;            // 2.2.28 (USED)
    EFI_SHELL_READ_FILE            ReadFile;             // 2.2.29 (USED)
    EFI_SHELL_WRITE_FILE           WriteFile;            // 2.2.30 (USED)
    void                          *DeleteFile;           // 2.2.31
    EFI_SHELL_DELETE_FILE_BY_NAME  DeleteFileByName;     // 2.2.32 (USED)
    EFI_SHELL_GET_FILE_POSITION    GetFilePosition;      // 2.2.33 (USED)
    EFI_SHELL_SET_FILE_POSITION    SetFilePosition;      // 2.2.34 (USED)
    void                          *FlushFile;            // 2.2.35
    void                          *FindFiles;            // 2.2.36
    void                          *FindFilesInDir;       // 2.2.37
    EFI_SHELL_GET_FILE_SIZE        GetFileSize;          // 2.2.38 (USED)
    void                          *OpenRoot;             // 2.2.39
    void                          *OpenRootByHandle;     // 2.2.40
    EFI_EVENT                      ExecutionBreak;       // (USED)
    UINT32                         MajorVersion;
    UINT32                         MinorVersion;
    void                          *RegisterGuidName;     // Shell 2.1
    void                          *GetGuidName;          // Shell 2.1
    void                          *GetGuidFromName;      // Shell 2.1
    void                          *GetEnvEx;             // Shell 2.1
};

// ===================================================================
// EFI_SHELL_PARAMETERS_PROTOCOL (UEFI Shell Spec 2.2, Section 2.3)
// ===================================================================

typedef struct {
    CHAR16             **Argv;
    UINTN                Argc;
    SHELL_FILE_HANDLE    StdIn;
    SHELL_FILE_HANDLE    StdOut;
    SHELL_FILE_HANDLE    StdErr;
} EFI_SHELL_PARAMETERS_PROTOCOL;

// ===================================================================
// Types not in any spec <pre> block
// ===================================================================

// SMBIOS_HANDLE_PI_RESERVED — EDK2 convention, not in any spec.
#define SMBIOS_HANDLE_PI_RESERVED  0xFFFE

// SMBIOS entry point structs (DMTF SMBIOS Spec, not UEFI/PI)
#pragma pack(1)
typedef struct {
    UINT8   AnchorString[4];    // "_SM_"
    UINT8   Checksum;
    UINT8   Length;
    UINT8   MajorVersion;
    UINT8   MinorVersion;
    UINT16  MaxStructureSize;
    UINT8   Revision;
    UINT8   FormattedArea[5];
    UINT8   IntermediateAnchorString[5];
    UINT8   IntermediateChecksum;
    UINT16  TableLength;
    UINT32  TableAddress;
    UINT16  NumberOfStructures;
    UINT8   BcdRevision;
} SMBIOS_STRUCTURE_TABLE;

typedef struct {
    UINT8   AnchorString[5];    // "_SM3_"
    UINT8   Checksum;
    UINT8   Length;
    UINT8   MajorVersion;
    UINT8   MinorVersion;
    UINT8   Docrev;
    UINT8   Revision;
    UINT8   Reserved;
    UINT32  TableMaximumSize;
    UINT64  TableAddress;
} SMBIOS3_STRUCTURE_TABLE;
#pragma pack()

// ===================================================================
// Device path utility macros and types
// ===================================================================

#define HARDWARE_DEVICE_PATH              0x01
#define HW_VENDOR_DP                      0x04
#define END_DEVICE_PATH_TYPE              0x7F
#define END_ENTIRE_DEVICE_PATH_SUBTYPE    0xFF

#define EFI_DP_TYPE(dp)     ((dp)->Type)
#define EFI_DP_SUBTYPE(dp)  ((dp)->SubType)
#define EFI_DP_LENGTH(dp)   ((dp)->Length[0] | ((dp)->Length[1] << 8))
#define EFI_DP_NEXT(dp)     ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)(dp) + EFI_DP_LENGTH(dp)))
#define EFI_DP_IS_END(dp)   (EFI_DP_TYPE(dp) == 0x7F)

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL  Header;
    EFI_GUID                  Guid;
} VENDOR_DEVICE_PATH;

// ===================================================================
// Firmware table pointers (set by axl_driver_init or AXL_APP CRT0)
// ===================================================================

extern EFI_SYSTEM_TABLE     *gST;          ///< UEFI System Table
extern EFI_BOOT_SERVICES    *gBS;          ///< Boot Services
extern EFI_RUNTIME_SERVICES *gRT;          ///< Runtime Services
extern EFI_HANDLE            gImageHandle; ///< Current image handle

// ===================================================================
// Protocol revisions and size macros
// ===================================================================

#define EFI_FILE_PROTOCOL_REVISION                0x00010000
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION  0x00010000

#define SIZE_OF_EFI_FILE_INFO        __builtin_offsetof(EFI_FILE_INFO, FileName)
#define SIZE_OF_EFI_FILE_SYSTEM_INFO __builtin_offsetof(EFI_FILE_SYSTEM_INFO, VolumeLabel)

// ===================================================================
// Boot Services constants
// ===================================================================

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL       0x00000004

// ===================================================================
// Console scan codes
// ===================================================================

#define SCAN_ESC  0x0017

// ===================================================================
// GUIDs — hand-written (not protocol GUIDs, not auto-generated)
// ===================================================================

static __attribute__((unused)) EFI_GUID gEfiFileInfoGuid =
    { 0x09576e92, 0x6d3f, 0x11d2,
      {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };

static __attribute__((unused)) EFI_GUID gEfiFileSystemInfoGuid =
    { 0x09576e93, 0x6d3f, 0x11d2,
      {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };

static __attribute__((unused)) EFI_GUID gEfiFileSystemVolumeLabelInfoIdGuid =
    { 0xdb47d7d3, 0xfe81, 0x11d3,
      {0x9a, 0x35, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d} };

static __attribute__((unused)) EFI_GUID gEfiCpuArchProtocolGuid =
    { 0x26baccb1, 0x6f42, 0x11d4,
      {0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81} };

static __attribute__((unused)) EFI_GUID gEfiShellProtocolGuid =
    { 0x6302d008, 0x7f9b, 0x4f30,
      {0x87, 0xac, 0x60, 0xc9, 0xfe, 0xf5, 0xda, 0x4e} };

static __attribute__((unused)) EFI_GUID gEfiShellParametersProtocolGuid =
    { 0x752f3136, 0x4e16, 0x4fdc,
      {0xa2, 0x2a, 0xe5, 0xf4, 0x68, 0x12, 0xf4, 0xca} };

// ===================================================================
// IPMI_PROTOCOL (EDK2 MdeModulePkg/Include/Protocol/IpmiProtocol.h)
//
// EDK2-internal vendor protocol — not part of any UEFI/PI/ACPI spec
// and therefore not auto-generated. Declared here verbatim to match
// EDK2's definition so AxlIpmi can invoke firmware that exposes it.
// ===================================================================

typedef struct _IPMI_PROTOCOL IPMI_PROTOCOL;

typedef EFI_STATUS (EFIAPI *IPMI_SEND_COMMAND)(
    IN     IPMI_PROTOCOL  *This,
    IN     UINT8           NetFunction,
    IN     UINT8           Lun,
    IN     UINT8           Command,
    IN     UINT8          *CommandData,
    IN     UINT32          CommandDataSize,
    OUT    UINT8          *ResponseData,
    IN OUT UINT32         *ResponseDataSize
    );

typedef EFI_STATUS (EFIAPI *IPMI_GET_CHANNEL_ENUM)(
    IN  IPMI_PROTOCOL  *This,
    OUT UINT8          *ChannelEnum
    );

struct _IPMI_PROTOCOL {
    IPMI_SEND_COMMAND      IpmiSubmitCommand;
    IPMI_GET_CHANNEL_ENUM  GetBmcStatus;
};

static __attribute__((unused)) EFI_GUID gIpmiProtocolGuid =
    { 0xdbc6381f, 0x5554, 0x4d14,
      {0x8f, 0xfd, 0x76, 0xd7, 0x87, 0xb8, 0xac, 0xbf} };

// EFI_SMBUS_HC_PROTOCOL GUID — appears in the PI spec HTML but the
// generator doesn't lift it into guids.h (its macro formatting
// doesn't match the extractor's GUID regex), so mirror it here.
static __attribute__((unused)) EFI_GUID gEfiSmbusHcProtocolGuid =
    { 0xe49d33ed, 0x513d, 0x4634,
      {0xb6, 0x98, 0x6f, 0x55, 0xaa, 0x75, 0x1c, 0x1b} };

// ===================================================================
// DELL_IPMI_TRANSPORT — Dell vendor IPMI protocol
//
// Proprietary to Dell platforms. Shape extracted from uefi-ipmitool's
// IpmiTransportLib. Quirk: the firmware's response buffer does NOT
// include the completion code byte; callers must synthesize CC=0x00
// on top of the returned data (matches uefi-ipmitool behavior).
// ===================================================================

typedef struct _DELL_IPMI_TRANSPORT DELL_IPMI_TRANSPORT;

typedef EFI_STATUS (EFIAPI *DELL_IPMI_SEND_COMMAND)(
    IN  DELL_IPMI_TRANSPORT  *This,
    IN  UINT8                 NetFn,
    IN  UINT8                 Command,
    IN  UINT8                *CommandData,
    IN  UINT8                 CommandDataSize,
    OUT UINT8                *ResponseData,
    OUT UINT8                *ResponseDataSize
    );

struct _DELL_IPMI_TRANSPORT {
    UINT64                  Revision;
    DELL_IPMI_SEND_COMMAND   IpmiSubmitCommand;
};

static __attribute__((unused)) EFI_GUID gDellIpmiProtocolGuid =
    { 0x7409d614, 0x5abf, 0x4869,
      {0xb8, 0xf0, 0xb9, 0xc3, 0x80, 0x39, 0x3e, 0xd8} };

// ===================================================================
// SMBIOS_TABLE_TYPE38 — IPMI Device Information (DMTF SMBIOS spec)
//
// Not in the UEFI/PI/ACPI spec HTML we pull from. AxlIpmi reads this
// table via EFI_SMBIOS_PROTOCOL to auto-detect the transport.
//
//   InterfaceType: 0=Unknown 1=KCS 2=SMIC 3=BT 4=SSIF
//   BaseAddress: bit 0 selects address space (0=memory, 1=I/O).
//     For KCS/SMIC/BT the low bit must be cleared before use.
// ===================================================================

#pragma pack(1)
typedef struct {
    EFI_SMBIOS_TABLE_HEADER  Hdr;
    UINT8                    InterfaceType;
    UINT8                    IPMISpecificationRevision;
    UINT8                    I2CSlaveAddress;
    UINT8                    NVStorageDeviceAddress;
    UINT64                   BaseAddress;
    UINT8                    BaseAddressModifier;
    UINT8                    InterruptNumber;
} SMBIOS_TABLE_TYPE38;
#pragma pack()

#endif /* AXL_UEFI_EXTRA_H */
