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

/* Shell Spec 2.2 §2.2.9 — return the shell mapping name (e.g.
   L"fs0:") for a device path. The DevicePath argument is in/out:
   on input it's the full device path to look up; on success it's
   advanced past the matched volume portion (so the caller can read
   the file-portion afterward). Returns NULL if no mapping covers
   the device path. Used by axl_app_image_path to convert
   LoadedImage->DeviceHandle into a shell-resolvable prefix. */
typedef CONST CHAR16 *(EFIAPI *EFI_SHELL_GET_MAP_FROM_DEVICE_PATH)(
    IN OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
    );

// ===================================================================
// EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL (UEFI Spec 2.x, §12.2)
//
// AXL uses ReadKeyStrokeEx + WaitForKeyEx to detect raw serial Ctrl-C
// (UnicodeChar=0x03 with KeyShiftState=0 — what TerminalDxe emits,
// since the wire carries no shift bits). The loop adds WaitForKeyEx
// to its event-wait array and intercepts the 0x03 in dispatch.
//
// We deliberately do NOT call RegisterKeyNotify: doing so puts OVMF's
// ConSplitter into a TPL_NOTIFY-level key polling loop that preempts
// our TPL_CALLBACK loop and starves TCP4 (test-http.sh dropped from
// 40/19 to 6/53 with QEMU pinned at 100% CPU when the bridge used
// RegisterKeyNotify). The struct still defines those fields for
// completeness, but they aren't used here.
// ===================================================================

typedef struct {
    UINT32  KeyShiftState;
    UINT8   KeyToggleState;
} EFI_KEY_STATE;

// EFI_KEY_STATE bit definitions (UEFI 2.11 §12.2.5). Hand-written
// here alongside the EFI_KEY_STATE struct (SimpleTextInputEx is not
// emitted by the spec-HTML generator; the struct above is hand-written
// for the same reason). KeyShiftState is valid only when
// EFI_SHIFT_STATE_VALID is set; KeyToggleState only when
// EFI_TOGGLE_STATE_VALID is set.
#define EFI_SHIFT_STATE_VALID       0x80000000u
#define EFI_RIGHT_SHIFT_PRESSED     0x00000001u
#define EFI_LEFT_SHIFT_PRESSED      0x00000002u
#define EFI_RIGHT_CONTROL_PRESSED   0x00000004u
#define EFI_LEFT_CONTROL_PRESSED    0x00000008u
#define EFI_RIGHT_ALT_PRESSED       0x00000010u
#define EFI_LEFT_ALT_PRESSED        0x00000020u
#define EFI_RIGHT_LOGO_PRESSED      0x00000040u
#define EFI_LEFT_LOGO_PRESSED       0x00000080u

#define EFI_TOGGLE_STATE_VALID      0x80u
#define EFI_KEY_STATE_EXPOSED       0x40u
#define EFI_SCROLL_LOCK_ACTIVE      0x01u
#define EFI_NUM_LOCK_ACTIVE         0x02u
#define EFI_CAPS_LOCK_ACTIVE        0x04u

typedef struct {
    EFI_INPUT_KEY  Key;
    EFI_KEY_STATE  KeyState;
} EFI_KEY_DATA;

typedef EFI_STATUS (EFIAPI *EFI_KEY_NOTIFY_FUNCTION)(
    IN EFI_KEY_DATA  *KeyData
    );

typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_EX_READ_KEY_STROKE_EX)(
    IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
    OUT EFI_KEY_DATA                       *KeyData
    );

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
    EFI_INPUT_EX_READ_KEY_STROKE_EX      ReadKeyStrokeEx;
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
    EFI_SHELL_GET_MAP_FROM_DEVICE_PATH GetMapFromDevicePath; // 2.2.9 (USED)
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
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008
#define EFI_OPEN_PROTOCOL_BY_DRIVER           0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE           0x00000020

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

// DXE Services Table GUID (PI spec DXE_SERVICES_TABLE_GUID). The spec macro
// uses a flat 11-field initializer that the generator's GUID auto-extractor
// (which expects the nested EFI_GUID form) skips, so it is defined by hand
// here. AxlMemRegion looks this up in the EFI configuration table to reach
// gDS->GetMemorySpaceMap (the GCD memory-space map).
static __attribute__((unused)) EFI_GUID gEfiDxeServicesTableGuid =
    { 0x05ad34ba, 0x6f02, 0x4214,
      {0x95, 0x2e, 0x4d, 0xa0, 0x39, 0x8e, 0x2b, 0xb9} };

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

// EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL GUIDs — not lifted into
// guids.h. UEFI driver-model NIC drivers (iPXE, vendor UNDI) install
// one of these on the NIC controller handle; firmware-bundled SnpDxe
// then binds to NII and produces SNP. AxlNet's driver-identity resolver
// (axl_net_get_driver_info) walks past the SNP wrapper to the NII
// installer to find the driver that actually owns the hardware. The
// _31 GUID is the modern revision; the bare one is the legacy variant.
static __attribute__((unused)) EFI_GUID gEfiNetworkInterfaceIdentifierProtocolGuid_31 =
    { 0x1ACED566, 0x76ED, 0x4218,
      {0xBC, 0x81, 0x76, 0x7F, 0x1F, 0x97, 0x7A, 0x89} };
static __attribute__((unused)) EFI_GUID gEfiNetworkInterfaceIdentifierProtocolGuid =
    { 0xE18541CD, 0xF755, 0x4F73,
      {0x92, 0x8D, 0x64, 0x3C, 0x8A, 0x79, 0xB2, 0x29} };

// ===================================================================
// DELL_IPMI_TRANSPORT — Dell vendor IPMI protocol
//
// Proprietary to Dell platforms. Shape extracted from uefi-ipmitool's
// IpmiTransportLib. Two well-documented quirks:
//
//   1. The vendor's SendIpmiCommand has 8 positional args including a
//      Lun byte at slot 3. Omitting it slides every following arg into
//      the wrong register/stack slot — observable on the affected vendor BMC firmware as
//      Get Device ID returning uninitialized bytes.
//   2. The response buffer does NOT include the completion code byte;
//      callers must synthesize CC=0x00 on top of the returned data
//      (matches uefi-ipmitool behavior).
// ===================================================================

typedef struct _DELL_IPMI_TRANSPORT DELL_IPMI_TRANSPORT;

typedef EFI_STATUS (EFIAPI *DELL_IPMI_SEND_COMMAND)(
    IN  DELL_IPMI_TRANSPORT  *This,
    IN  UINT8                 NetFn,
    IN  UINT8                 Lun,
    IN  UINT8                 Command,
    IN  UINT8                *CommandData,
    IN  UINT8                 CommandDataSize,
    OUT UINT8                *ResponseData,
    IN OUT UINT8             *ResponseDataSize
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

// ===================================================================
// EFI_USB_IO_PROTOCOL (UEFI Spec §17.2)
//
// The spec HTML's `EFI_USB_IO_PROTOCOL` block has a missing semicolon
// and a typo (`EFI_USB_IO_SYNC_INTERRPUT_TRANSFER`) that breaks the
// generator's C extractor — hand-write the protocol here verbatim
// against EDK2's clean version. AxlUsb only invokes the descriptor
// readers (UsbGetDeviceDescriptor / UsbGetConfigDescriptor /
// UsbGetInterfaceDescriptor / UsbGetEndpointDescriptor /
// UsbGetStringDescriptor / UsbGetSupportedLanguages); the transfer
// slots are typed as `void *` so callers don't accidentally invoke
// them through a deliberately-narrow surface.
// ===================================================================

typedef struct _EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;

#pragma pack(1)
typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;
    UINT16  BcdUSB;
    UINT8   DeviceClass;
    UINT8   DeviceSubClass;
    UINT8   DeviceProtocol;
    UINT8   MaxPacketSize0;
    UINT16  IdVendor;
    UINT16  IdProduct;
    UINT16  BcdDevice;
    UINT8   StrManufacturer;
    UINT8   StrProduct;
    UINT8   StrSerialNumber;
    UINT8   NumConfigurations;
} EFI_USB_DEVICE_DESCRIPTOR;

typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;
    UINT16  TotalLength;
    UINT8   NumInterfaces;
    UINT8   ConfigurationValue;
    UINT8   Configuration;
    UINT8   Attributes;
    UINT8   MaxPower;
} EFI_USB_CONFIG_DESCRIPTOR;

typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;
    UINT8   InterfaceNumber;
    UINT8   AlternateSetting;
    UINT8   NumEndpoints;
    UINT8   InterfaceClass;
    UINT8   InterfaceSubClass;
    UINT8   InterfaceProtocol;
    UINT8   Interface;
} EFI_USB_INTERFACE_DESCRIPTOR;

typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;
    UINT8   EndpointAddress;
    UINT8   Attributes;
    UINT16  MaxPacketSize;
    UINT8   Interval;
} EFI_USB_ENDPOINT_DESCRIPTOR;
#pragma pack()

typedef enum {
    EfiUsbDataIn,
    EfiUsbDataOut,
    EfiUsbNoData
} EFI_USB_DATA_DIRECTION;

#pragma pack(1)
typedef struct {
    UINT8   RequestType;
    UINT8   Request;
    UINT16  Value;
    UINT16  Index;
    UINT16  Length;
} EFI_USB_DEVICE_REQUEST;
#pragma pack()

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_CONTROL_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL        *This,
    IN  EFI_USB_DEVICE_REQUEST     *Request,
    IN  EFI_USB_DATA_DIRECTION      Direction,
    IN  UINT32                      Timeout,
    IN OUT VOID                    *Data OPTIONAL,
    IN  UINTN                       DataLength,
    OUT UINT32                     *Status
    );

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_DEVICE_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL        *This,
    OUT EFI_USB_DEVICE_DESCRIPTOR  *DeviceDescriptor
    );

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_CONFIG_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL        *This,
    OUT EFI_USB_CONFIG_DESCRIPTOR  *ConfigurationDescriptor
    );

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_INTERFACE_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL           *This,
    OUT EFI_USB_INTERFACE_DESCRIPTOR  *InterfaceDescriptor
    );

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL          *This,
    IN  UINT8                         EndpointIndex,
    OUT EFI_USB_ENDPOINT_DESCRIPTOR  *EndpointDescriptor
    );

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_STRING_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL  *This,
    IN  UINT16                LangID,
    IN  UINT8                 StringID,
    OUT CHAR16              **String
    );

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_SUPPORTED_LANGUAGES)(
    IN  EFI_USB_IO_PROTOCOL  *This,
    OUT UINT16              **LangIDTable,
    OUT UINT16               *TableSize
    );

struct _EFI_USB_IO_PROTOCOL {
    EFI_USB_IO_CONTROL_TRANSFER          UsbControlTransfer;
    void                                *UsbBulkTransfer;
    void                                *UsbAsyncInterruptTransfer;
    void                                *UsbSyncInterruptTransfer;
    void                                *UsbIsochronousTransfer;
    void                                *UsbAsyncIsochronousTransfer;
    EFI_USB_IO_GET_DEVICE_DESCRIPTOR     UsbGetDeviceDescriptor;
    EFI_USB_IO_GET_CONFIG_DESCRIPTOR     UsbGetConfigDescriptor;
    EFI_USB_IO_GET_INTERFACE_DESCRIPTOR  UsbGetInterfaceDescriptor;
    EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR   UsbGetEndpointDescriptor;
    EFI_USB_IO_GET_STRING_DESCRIPTOR     UsbGetStringDescriptor;
    EFI_USB_IO_GET_SUPPORTED_LANGUAGES   UsbGetSupportedLanguages;
    void                                *UsbPortReset;
};

// USB device-path messaging-node subtype (UEFI Spec §10.3.5.7).
// Used by AxlUsb to derive (bus, addr) ordinals from the path of an
// EFI_USB_IO_PROTOCOL handle: the ParentPortNumber + InterfaceNumber
// pair lives at offset +4 of a USB messaging device-path node.
// Guarded so future regeneration that emits MESSAGING_DEVICE_PATH
// from the spec doesn't collide.
#ifndef MESSAGING_DEVICE_PATH
#define MESSAGING_DEVICE_PATH       0x03
#endif
#ifndef MSG_USB_DP
#define MSG_USB_DP                  0x05
#endif

#pragma pack(1)
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL  Header;
    UINT8                     ParentPortNumber;
    UINT8                     InterfaceNumber;
} USB_DEVICE_PATH;
#pragma pack()

// ===================================================================
// EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL (UEFI Spec 13.16)
//
// Hand-written because the spec HTML mixes the EFI_NVM_EXPRESS_COMMAND
// struct with a bitfield sub-struct and inline #defines, and the
// PassThru / command-packet blocks carry typos (`This` and
// `TransferBuffer` are shown without their `*`). The GUID is already in
// generated/guids.h. AXL uses Mode, PassThru, and GetNextNamespace for
// fixture capture (Identify Controller / Namespace).
// ===================================================================

typedef struct _EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL;

typedef struct {
    UINT32  Attributes;    // EFI_NVM_EXPRESS_PASS_THRU_ATTRIBUTES_*
    UINT32  IoAlign;       // required alignment of data buffers
    UINT32  NvmeVersion;   // controller's NVMe spec version (BCD)
} EFI_NVM_EXPRESS_PASS_THRU_MODE;

// Command Dword 0: opcode in the low 8 bits (6 = Identify).
typedef struct {
    UINT32  Opcode         : 8;
    UINT32  FusedOperation : 2;
    UINT32  Reserved       : 22;
} NVME_CDW0;

typedef struct {
    NVME_CDW0  Cdw0;
    UINT8      Flags;
    UINT32     Nsid;
    UINT32     Cdw2;
    UINT32     Cdw3;
    UINT32     Cdw10;   // Identify: CNS (1 = controller, 0 = namespace)
    UINT32     Cdw11;
    UINT32     Cdw12;
    UINT32     Cdw13;
    UINT32     Cdw14;
    UINT32     Cdw15;
} EFI_NVM_EXPRESS_COMMAND;

typedef struct {
    UINT32  DW0;
    UINT32  DW1;
    UINT32  DW2;
    UINT32  DW3;
} EFI_NVM_EXPRESS_COMPLETION;

typedef struct {
    UINT64                       CommandTimeout;   // 100 ns units; 0 = none
    VOID                        *TransferBuffer;
    UINT32                       TransferLength;
    VOID                        *MetadataBuffer;
    UINT32                       MetadataLength;
    UINT8                        QueueType;         // 0 = Admin, 1 = I/O
    EFI_NVM_EXPRESS_COMMAND     *NvmeCmd;
    EFI_NVM_EXPRESS_COMPLETION  *NvmeCompletion;
} EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET;

#define NVME_ADMIN_QUEUE          0x00
#define NVME_ADMIN_IDENTIFY_OPC   0x06
#define NVME_ADMIN_GET_LOG_PAGE   0x02   // Get Log Page (SMART/self-test)
#define NVME_ADMIN_DEVICE_SELF_TEST 0x14 // Device Self-test
// EFI_NVM_EXPRESS_COMMAND.Flags bits — gate which command Dwords the
// driver programs into the submission queue entry. Identify carries CNS
// in Cdw10, so CDW10_VALID must be set or Cdw10 is dropped (the NSID
// field is always programmed, no flag needed). Get Log Page programs the
// log id / length into Cdw10..11 likewise.
#define NVME_CDW2_VALID           0x01
#define NVME_CDW3_VALID           0x02
#define NVME_CDW10_VALID          0x04
#define NVME_CDW11_VALID          0x08
#define NVME_CDW12_VALID          0x10
#define NVME_CDW13_VALID          0x20
#define NVME_CDW14_VALID          0x40
#define NVME_CDW15_VALID          0x80

typedef EFI_STATUS (EFIAPI *EFI_NVM_EXPRESS_PASS_THRU_PASSTHRU)(
    IN     EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL        *This,
    IN     UINT32                                     NamespaceId,
    IN OUT EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET  *Packet,
    IN     EFI_EVENT                                  Event OPTIONAL
    );

typedef EFI_STATUS (EFIAPI *EFI_NVM_EXPRESS_PASS_THRU_GET_NEXT_NAMESPACE)(
    IN     EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL  *This,
    IN OUT UINT32                              *NamespaceId
    );

typedef EFI_STATUS (EFIAPI *EFI_NVM_EXPRESS_PASS_THRU_BUILD_DEVICE_PATH)(
    IN  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL  *This,
    IN  UINT32                               NamespaceId,
    OUT EFI_DEVICE_PATH_PROTOCOL           **DevicePath
    );

typedef EFI_STATUS (EFIAPI *EFI_NVM_EXPRESS_PASS_THRU_GET_NAMESPACE)(
    IN  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL  *This,
    IN  EFI_DEVICE_PATH_PROTOCOL            *DevicePath,
    OUT UINT32                              *NamespaceId
    );

struct _EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL {
    EFI_NVM_EXPRESS_PASS_THRU_MODE                *Mode;
    EFI_NVM_EXPRESS_PASS_THRU_PASSTHRU             PassThru;
    EFI_NVM_EXPRESS_PASS_THRU_GET_NEXT_NAMESPACE   GetNextNamespace;
    EFI_NVM_EXPRESS_PASS_THRU_BUILD_DEVICE_PATH    BuildDevicePath;
    EFI_NVM_EXPRESS_PASS_THRU_GET_NAMESPACE        GetNamespace;
};

// ===================================================================
// EFI_ATA_PASS_THRU_PROTOCOL (UEFI Spec 13.10)
//
// Hand-written like the NVMe pass-thru: the GUID is in generated/guids.h,
// but the manifest generator doesn't extract the protocol's funcptr web +
// command/status blocks cleanly. AXL uses Mode, PassThru, GetNextPort, and
// GetNextDevice for ATA/SATA identity + SMART (axl-ata).
// ===================================================================

typedef struct _EFI_ATA_PASS_THRU_PROTOCOL EFI_ATA_PASS_THRU_PROTOCOL;

typedef struct {
    UINT32  Attributes;   // EFI_ATA_PASS_THRU_ATTRIBUTES_*
    UINT32  IoAlign;      // required alignment of data buffers
} EFI_ATA_PASS_THRU_MODE;

#define EFI_ATA_PASS_THRU_ATTRIBUTES_PHYSICAL  0x0001
#define EFI_ATA_PASS_THRU_ATTRIBUTES_LOGICAL   0x0002

// The 12-register ATA command / status task files (offsets are spec-fixed).
typedef struct {
    UINT8  Reserved1[2];
    UINT8  AtaCommand;
    UINT8  AtaFeatures;
    UINT8  AtaSectorNumber;
    UINT8  AtaCylinderLow;
    UINT8  AtaCylinderHigh;
    UINT8  AtaDeviceHead;
    UINT8  AtaSectorNumberExp;
    UINT8  AtaCylinderLowExp;
    UINT8  AtaCylinderHighExp;
    UINT8  AtaFeaturesExp;
    UINT8  AtaSectorCount;
    UINT8  AtaSectorCountExp;
    UINT8  Reserved2[6];
} EFI_ATA_COMMAND_BLOCK;

typedef struct {
    UINT8  Reserved1[2];
    UINT8  AtaStatus;
    UINT8  AtaError;
    UINT8  AtaSectorNumber;
    UINT8  AtaCylinderLow;
    UINT8  AtaCylinderHigh;
    UINT8  AtaDeviceHead;
    UINT8  AtaSectorNumberExp;
    UINT8  AtaCylinderLowExp;
    UINT8  AtaCylinderHighExp;
    UINT8  Reserved2;
    UINT8  AtaSectorCount;
    UINT8  AtaSectorCountExp;
    UINT8  Reserved3[6];
} EFI_ATA_STATUS_BLOCK;

typedef UINT8 EFI_ATA_PASS_THRU_CMD_PROTOCOL;
#define EFI_ATA_PASS_THRU_PROTOCOL_ATA_NON_DATA  0x02
#define EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN   0x04
#define EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT  0x05
#define EFI_ATA_PASS_THRU_PROTOCOL_DMA           0x06

typedef UINT8 EFI_ATA_PASS_THRU_LENGTH;
#define EFI_ATA_PASS_THRU_LENGTH_BYTES            0x80  // count is in bytes
#define EFI_ATA_PASS_THRU_LENGTH_MASK             0x70
#define EFI_ATA_PASS_THRU_LENGTH_NO_DATA_TRANSFER 0x00
#define EFI_ATA_PASS_THRU_LENGTH_FEATURES         0x10
#define EFI_ATA_PASS_THRU_LENGTH_SECTOR_COUNT     0x20

typedef struct {
    EFI_ATA_STATUS_BLOCK            *Asb;
    EFI_ATA_COMMAND_BLOCK          *Acb;
    UINT64                          Timeout;        // 100 ns units; 0 = none
    VOID                           *InDataBuffer;
    VOID                           *OutDataBuffer;
    UINT32                          InTransferLength;
    UINT32                          OutTransferLength;
    EFI_ATA_PASS_THRU_CMD_PROTOCOL  Protocol;       // PIO_DATA_IN / NON_DATA ...
    EFI_ATA_PASS_THRU_LENGTH        Length;         // how the length is counted
} EFI_ATA_PASS_THRU_COMMAND_PACKET;

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_PASSTHRU)(
    IN     EFI_ATA_PASS_THRU_PROTOCOL        *This,
    IN     UINT16                             Port,
    IN     UINT16                             PortMultiplierPort,
    IN OUT EFI_ATA_PASS_THRU_COMMAND_PACKET  *Packet,
    IN     EFI_EVENT                          Event OPTIONAL
    );

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_GET_NEXT_PORT)(
    IN     EFI_ATA_PASS_THRU_PROTOCOL  *This,
    IN OUT UINT16                      *Port
    );

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_GET_NEXT_DEVICE)(
    IN     EFI_ATA_PASS_THRU_PROTOCOL  *This,
    IN     UINT16                       Port,
    IN OUT UINT16                      *PortMultiplierPort
    );

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_BUILD_DEVICE_PATH)(
    IN  EFI_ATA_PASS_THRU_PROTOCOL  *This,
    IN  UINT16                       Port,
    IN  UINT16                       PortMultiplierPort,
    OUT EFI_DEVICE_PATH_PROTOCOL   **DevicePath
    );

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_GET_DEVICE)(
    IN  EFI_ATA_PASS_THRU_PROTOCOL  *This,
    IN  EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
    OUT UINT16                      *Port,
    OUT UINT16                      *PortMultiplierPort
    );

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_RESET_PORT)(
    IN EFI_ATA_PASS_THRU_PROTOCOL  *This,
    IN UINT16                       Port
    );

typedef EFI_STATUS (EFIAPI *EFI_ATA_PASS_THRU_RESET_DEVICE)(
    IN EFI_ATA_PASS_THRU_PROTOCOL  *This,
    IN UINT16                       Port,
    IN UINT16                       PortMultiplierPort
    );

struct _EFI_ATA_PASS_THRU_PROTOCOL {
    EFI_ATA_PASS_THRU_MODE               *Mode;
    EFI_ATA_PASS_THRU_PASSTHRU            PassThru;
    EFI_ATA_PASS_THRU_GET_NEXT_PORT       GetNextPort;
    EFI_ATA_PASS_THRU_GET_NEXT_DEVICE     GetNextDevice;
    EFI_ATA_PASS_THRU_BUILD_DEVICE_PATH   BuildDevicePath;
    EFI_ATA_PASS_THRU_GET_DEVICE          GetDevice;
    EFI_ATA_PASS_THRU_RESET_PORT          ResetPort;
    EFI_ATA_PASS_THRU_RESET_DEVICE        ResetDevice;
};

// ===================================================================
// EFI_EXT_SCSI_PASS_THRU_PROTOCOL (UEFI Spec 14.7)
//
// Hand-written like the ATA/NVMe pass-thru: the GUID is in
// generated/guids.h, but the manifest generator doesn't extract the
// protocol's funcptr web + request packet cleanly. AXL uses Mode,
// PassThru, and GetNextTargetLun for SCSI identity + health (axl-scsi).
// ===================================================================

#ifndef TARGET_MAX_BYTES
#define TARGET_MAX_BYTES  0x10
#endif

typedef struct _EFI_EXT_SCSI_PASS_THRU_PROTOCOL EFI_EXT_SCSI_PASS_THRU_PROTOCOL;

typedef struct {
    UINT32  AdapterId;
    UINT32  Attributes;   // EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_*
    UINT32  IoAlign;      // required alignment of data buffers
} EFI_EXT_SCSI_PASS_THRU_MODE;

#define EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_PHYSICAL     0x0001
#define EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_LOGICAL      0x0002
#define EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_NONBLOCKIO   0x0004

// DataDirection
#define EFI_EXT_SCSI_DATA_DIRECTION_READ           0
#define EFI_EXT_SCSI_DATA_DIRECTION_WRITE          1
#define EFI_EXT_SCSI_DATA_DIRECTION_BIDIRECTIONAL  2

// HostAdapterStatus / TargetStatus (the two we test)
#define EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK        0x00
#define EFI_EXT_SCSI_STATUS_TARGET_GOOD            0x00
#define EFI_EXT_SCSI_STATUS_TARGET_CHECK_CONDITION 0x02

typedef struct {
    UINT64  Timeout;          // 100 ns units; 0 = none
    VOID   *InDataBuffer;
    VOID   *OutDataBuffer;
    VOID   *SenseData;
    VOID   *Cdb;
    UINT32  InTransferLength;  // in/out: requested -> actually transferred
    UINT32  OutTransferLength; // in/out: requested -> actually transferred
    UINT8   CdbLength;
    UINT8   DataDirection;     // EFI_EXT_SCSI_DATA_DIRECTION_*
    UINT8   HostAdapterStatus;
    UINT8   TargetStatus;
    UINT8   SenseDataLength;   // in/out: capacity -> length returned
} EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET;

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_PASSTHRU)(
    IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL              *This,
    IN     UINT8                                        *Target,
    IN     UINT64                                        Lun,
    IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET   *Packet,
    IN     EFI_EVENT                                     Event OPTIONAL
    );

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_GET_NEXT_TARGET_LUN)(
    IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
    IN OUT UINT8                           **Target,
    IN OUT UINT64                           *Lun
    );

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_BUILD_DEVICE_PATH)(
    IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
    IN     UINT8                            *Target,
    IN     UINT64                            Lun,
    IN OUT EFI_DEVICE_PATH_PROTOCOL        **DevicePath
    );

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_GET_TARGET_LUN)(
    IN  EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
    IN  EFI_DEVICE_PATH_PROTOCOL         *DevicePath,
    OUT UINT8                           **Target,
    OUT UINT64                           *Lun
    );

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_RESET_CHANNEL)(
    IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This
    );

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_RESET_TARGET_LUN)(
    IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
    IN UINT8                            *Target,
    IN UINT64                            Lun
    );

typedef EFI_STATUS (EFIAPI *EFI_EXT_SCSI_PASS_THRU_GET_NEXT_TARGET)(
    IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *This,
    IN OUT UINT8                           **Target
    );

struct _EFI_EXT_SCSI_PASS_THRU_PROTOCOL {
    EFI_EXT_SCSI_PASS_THRU_MODE                 *Mode;
    EFI_EXT_SCSI_PASS_THRU_PASSTHRU              PassThru;
    EFI_EXT_SCSI_PASS_THRU_GET_NEXT_TARGET_LUN  GetNextTargetLun;
    EFI_EXT_SCSI_PASS_THRU_BUILD_DEVICE_PATH    BuildDevicePath;
    EFI_EXT_SCSI_PASS_THRU_GET_TARGET_LUN       GetTargetLun;
    EFI_EXT_SCSI_PASS_THRU_RESET_CHANNEL        ResetChannel;
    EFI_EXT_SCSI_PASS_THRU_RESET_TARGET_LUN     ResetTargetLun;
    EFI_EXT_SCSI_PASS_THRU_GET_NEXT_TARGET      GetNextTarget;
};

// ===================================================================
// EFI_BLOCK_IO_PROTOCOL (UEFI Spec 13.9)
//
// Hand-written because the spec HTML's struct closer reads
// `} EFI _BLOCK_IO_PROTOCOL;` (a stray space), so the manifest-driven
// generator can't match it by name. The field-bearing media descriptor
// (EFI_BLOCK_IO_MEDIA) IS generated into media.h; only this thin
// protocol wrapper is hand-written. The four service entry points are
// unused by AXL (block enumeration reads Media only), so they are kept
// as opaque VOID * — correctly pointer-sized without four more funcptr
// typedefs. The GUID is already in generated/guids.h.
// ===================================================================

typedef struct _EFI_BLOCK_IO_PROTOCOL {
    UINT64              Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    VOID               *Reset;        // EFI_BLOCK_RESET (unused by AXL)
    VOID               *ReadBlocks;   // EFI_BLOCK_READ   (unused by AXL)
    VOID               *WriteBlocks;  // EFI_BLOCK_WRITE  (unused by AXL)
    VOID               *FlushBlocks;  // EFI_BLOCK_FLUSH  (unused by AXL)
} EFI_BLOCK_IO_PROTOCOL;

// ===================================================================
// EFI_FIRMWARE_VOLUME2_PROTOCOL (PI Spec 1.8, Vol 3)
//
// Hand-written because the spec HTML's typedef is mangled
// (`typedef struct_EFI_FIRMWARE_VOLUME_PROTOCOL {` with a glued tag and
// the wrong name on the opener vs the `EFI_FIRMWARE_VOLUME2_PROTOCOL`
// closer), so the manifest-driven generator can't match it. AXL uses
// only GetVolumeAttributes (FV attribute bits) and GetNextFile + KeySize
// (file-count loop); the other service entry points are kept opaque as
// VOID * — correctly pointer-sized without their funcptr typedefs. The
// GUID is not in generated/guids.h and is defined locally in axl-fv.c.
// ===================================================================

typedef UINT64 EFI_FV_ATTRIBUTES;
typedef UINT8  EFI_FV_FILETYPE;       // EFI_FV_FILETYPE_ALL == 0x00
typedef UINT32 EFI_FV_FILE_ATTRIBUTES;

typedef struct _EFI_FIRMWARE_VOLUME2_PROTOCOL  EFI_FIRMWARE_VOLUME2_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FV_GET_ATTRIBUTES)(
    IN  CONST EFI_FIRMWARE_VOLUME2_PROTOCOL  *This,
    OUT EFI_FV_ATTRIBUTES                    *FvAttributes
    );

typedef EFI_STATUS (EFIAPI *EFI_FV_GET_NEXT_FILE)(
    IN     CONST EFI_FIRMWARE_VOLUME2_PROTOCOL  *This,
    IN OUT VOID                                 *Key,
    IN OUT EFI_FV_FILETYPE                       *FileType,
    OUT    EFI_GUID                             *NameGuid,
    OUT    EFI_FV_FILE_ATTRIBUTES               *Attributes,
    OUT    UINTN                                *Size
    );

struct _EFI_FIRMWARE_VOLUME2_PROTOCOL {
    EFI_FV_GET_ATTRIBUTES   GetVolumeAttributes;
    VOID                   *SetVolumeAttributes;   // unused by AXL
    VOID                   *ReadFile;              // unused by AXL
    VOID                   *ReadSection;           // unused by AXL
    VOID                   *WriteFile;             // unused by AXL
    EFI_FV_GET_NEXT_FILE    GetNextFile;
    UINT32                  KeySize;
    EFI_HANDLE              ParentHandle;
    VOID                   *GetInfo;               // unused by AXL
    VOID                   *SetInfo;               // unused by AXL
};

// ===================================================================
// EFI_TCG2_PROTOCOL (TCG EFI Protocol Specification, Family 2.0)
//
// Hand-written because the TCG2 protocol is defined in the TCG spec
// (not a UEFI/PI spec we extract from). AXL uses only GetCapability;
// the other entry points are kept opaque as VOID *. The capability
// struct is NOT packed (matching the EDK2 definition) — it relies on
// natural alignment. The GUID is defined locally in axl-tpm.c.
// ===================================================================

typedef struct {
    UINT8  Major;
    UINT8  Minor;
} EFI_TCG2_VERSION;

typedef struct {
    UINT8             Size;                  // size of this structure
    EFI_TCG2_VERSION  StructureVersion;
    EFI_TCG2_VERSION  ProtocolVersion;
    UINT32            HashAlgorithmBitmap;
    UINT32            SupportedEventLogs;
    BOOLEAN           TPMPresentFlag;
    UINT16            MaxCommandSize;
    UINT16            MaxResponseSize;
    UINT32            ManufacturerID;
    UINT32            NumberOfPcrBanks;      // valid when StructureVersion >= 1.1
    UINT32            ActivePcrBanks;        // valid when StructureVersion >= 1.1
} EFI_TCG2_BOOT_SERVICE_CAPABILITY;

typedef struct _EFI_TCG2_PROTOCOL  EFI_TCG2_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_CAPABILITY)(
    IN     EFI_TCG2_PROTOCOL                 *This,
    IN OUT EFI_TCG2_BOOT_SERVICE_CAPABILITY  *ProtocolCapability
    );

// Pass a raw TPM2 command block through to the TPM and read the raw
// response block back (used by axl-tpm's EK reader).
typedef EFI_STATUS (EFIAPI *EFI_TCG2_SUBMIT_COMMAND)(
    IN EFI_TCG2_PROTOCOL  *This,
    IN UINT32              InputParameterBlockSize,
    IN UINT8              *InputParameterBlock,
    IN UINT32              OutputParameterBlockSize,
    IN UINT8              *OutputParameterBlock
    );

struct _EFI_TCG2_PROTOCOL {
    EFI_TCG2_GET_CAPABILITY   GetCapability;
    VOID                     *GetEventLog;                  // unused by AXL
    VOID                     *HashLogExtendEvent;           // unused by AXL
    EFI_TCG2_SUBMIT_COMMAND   SubmitCommand;
    VOID                     *GetActivePcrBanks;            // unused by AXL
    VOID                     *SetActivePcrBanks;            // unused by AXL
    VOID                     *GetResultOfSetActivePcrBanks; // unused by AXL
};

// ===================================================================
// EFI_PCI_IO_PROTOCOL — only GetLocation is bound (AxlDriverInfo maps a
// PCI address to its controller handle). The other members are width-
// correct placeholders so GetLocation lands at the right offset; the
// Mem/Io/Pci access members are 2-pointer sub-structs in the spec, hence
// the pair placeholders. Trailing members past GetLocation are omitted —
// AXL never reads them through this binding.
// ===================================================================

typedef struct _EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_PROTOCOL_GET_LOCATION)(
    IN  EFI_PCI_IO_PROTOCOL  *This,
    OUT UINTN                *SegmentNumber,
    OUT UINTN                *BusNumber,
    OUT UINTN                *DeviceNumber,
    OUT UINTN                *FunctionNumber
    );

struct _EFI_PCI_IO_PROTOCOL {
    VOID                              *PollMem;
    VOID                              *PollIo;
    VOID                              *Mem_Read;   VOID *Mem_Write;  // ACCESS Mem
    VOID                              *Io_Read;    VOID *Io_Write;   // ACCESS Io
    VOID                              *Pci_Read;   VOID *Pci_Write;  // CONFIG Pci
    VOID                              *CopyMem;
    VOID                              *Map;
    VOID                              *Unmap;
    VOID                              *AllocateBuffer;
    VOID                              *FreeBuffer;
    VOID                              *Flush;
    EFI_PCI_IO_PROTOCOL_GET_LOCATION   GetLocation;
};

// ===================================================================
// EFI_ARP_PROTOCOL (UEFI 2.11 §29.2) — only Configure + Find are bound
// (axl_net_arp_list reads the neighbor/ARP cache). GUIDs are in
// generated/guids.h (EFI_ARP_PROTOCOL_GUID, EFI_ARP_SERVICE_BINDING_*).
// Each Find entry is followed inline by HwAddressLength hardware-address
// bytes then SwAddressLength software-address bytes; entries are
// EntryLength apart.
// ===================================================================

typedef struct {
    UINT16  SwAddressType;     // 0x0800 for IPv4
    UINT8   SwAddressLength;
    VOID   *StationAddress;
    UINT32  EntryTimeOut;
    UINT32  RetryCount;
    UINT32  RetryTimeOut;
} EFI_ARP_CONFIG_DATA;

typedef struct {
    UINT32  Size;
    BOOLEAN DenyFlag;
    BOOLEAN StaticFlag;
    UINT16  HwAddressType;
    UINT16  SwAddressType;
    UINT8   HwAddressLength;
    UINT8   SwAddressLength;
} EFI_ARP_FIND_DATA;

typedef struct _EFI_ARP_PROTOCOL EFI_ARP_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_ARP_CONFIGURE)(
    IN EFI_ARP_PROTOCOL     *This,
    IN EFI_ARP_CONFIG_DATA  *ConfigData OPTIONAL
    );

typedef EFI_STATUS (EFIAPI *EFI_ARP_FIND)(
    IN  EFI_ARP_PROTOCOL    *This,
    IN  BOOLEAN              BySwAddress,
    IN  VOID                *AddressBuffer OPTIONAL,
    OUT UINT32              *EntryLength OPTIONAL,
    OUT UINT32              *EntryCount OPTIONAL,
    OUT EFI_ARP_FIND_DATA  **Entries OPTIONAL,
    IN  BOOLEAN              Refresh
    );

struct _EFI_ARP_PROTOCOL {
    EFI_ARP_CONFIGURE  Configure;
    VOID              *Add;        // unused by AXL
    EFI_ARP_FIND       Find;
    VOID              *Delete;     // unused
    VOID              *Flush;      // unused
    VOID              *Request;    // unused
    VOID              *Cancel;     // unused
};

// ===================================================================
// HII (Human Interface Infrastructure) — the SUBSET AxlHii (src/hii/)
// needs to enumerate setup form sets, walk IFR opcodes, and read/write
// question values.  Hand-written rather than manifest-generated because
// the IFR structs are cast directly onto the raw on-disk IFR byte
// stream, so they MUST be byte-packed (the auto-"unknown member -> void *"
// rewrite in the generator would silently shift field offsets and corrupt
// the parse), and because the UEFI 2.x spec HTML carries transcription
// quirks (EFI_IFR_ONE_OF shows `*Question` — a pointer typo; it is
// embedded — and EFI_IFR_VARSTORE_EFI drops a semicolon) that must be
// corrected on transcription.  Definitions transcribed (and corrected)
// from UEFI 2.11 §33 (deps/uefi-spec/33_Human_Interface_Infrastructure.html).
// The protocol GUIDs are already in generated/guids.h.
// ===================================================================

typedef VOID    *EFI_HII_HANDLE;
typedef CHAR16  *EFI_STRING;
typedef UINT16   EFI_STRING_ID;
typedef UINT16   EFI_QUESTION_ID;
typedef UINT16   EFI_VARSTORE_ID;

// --- HII package list / package headers (cast onto ExportPackageLists output) ---

typedef struct {
    EFI_GUID  PackageListGuid;
    UINT32    PackageLength;
} EFI_HII_PACKAGE_LIST_HEADER;

typedef struct {
    UINT32  Length : 24;   // size of the package (incl. this header)
    UINT32  Type   : 8;    // EFI_HII_PACKAGE_*
} EFI_HII_PACKAGE_HEADER;

#define EFI_HII_PACKAGE_TYPE_ALL     0x00
#define EFI_HII_PACKAGE_FORMS        0x02
#define EFI_HII_PACKAGE_DEVICE_PATH  0x08
#define EFI_HII_PACKAGE_END          0xDF

// --- IFR opcode + flag + value-type constants ---

#define EFI_IFR_FORM_SET_OP       0x0E
#define EFI_IFR_ONE_OF_OP         0x05
#define EFI_IFR_CHECKBOX_OP       0x06
#define EFI_IFR_NUMERIC_OP        0x07
#define EFI_IFR_ONE_OF_OPTION_OP  0x09
#define EFI_IFR_STRING_OP         0x1C
#define EFI_IFR_VARSTORE_OP       0x24
#define EFI_IFR_VARSTORE_EFI_OP   0x26
#define EFI_IFR_END_OP            0x29

#define EFI_IFR_FLAG_READ_ONLY        0x01
#define EFI_IFR_FLAG_CALLBACK         0x04
#define EFI_IFR_FLAG_RESET_REQUIRED   0x10

#define EFI_IFR_NUMERIC_SIZE    0x03
#define EFI_IFR_NUMERIC_SIZE_1  0x00
#define EFI_IFR_NUMERIC_SIZE_2  0x01
#define EFI_IFR_NUMERIC_SIZE_4  0x02
#define EFI_IFR_NUMERIC_SIZE_8  0x03

#define EFI_IFR_TYPE_NUM_SIZE_8   0x00
#define EFI_IFR_TYPE_NUM_SIZE_16  0x01
#define EFI_IFR_TYPE_NUM_SIZE_32  0x02
#define EFI_IFR_TYPE_NUM_SIZE_64  0x03
#define EFI_IFR_TYPE_BOOLEAN      0x04

// --- IFR structs (packed: cast directly onto the on-disk IFR stream) ---

#pragma pack(1)

typedef struct {
    UINT8  OpCode;
    UINT8  Length : 7;
    UINT8  Scope  : 1;
} EFI_IFR_OP_HEADER;

typedef struct {
    EFI_STRING_ID  Prompt;
    EFI_STRING_ID  Help;
} EFI_IFR_STATEMENT_HEADER;

typedef struct {
    EFI_IFR_STATEMENT_HEADER  Header;
    EFI_QUESTION_ID           QuestionId;
    EFI_VARSTORE_ID           VarStoreId;
    union {
        EFI_STRING_ID  VarName;
        UINT16         VarOffset;
    } VarStoreInfo;
    UINT8                     Flags;
} EFI_IFR_QUESTION_HEADER;

// Scalar arms of EFI_IFR_TYPE_VALUE — enough to read a ONE_OF option's
// value (the larger date/time/ref arms are unused by AxlHii).
typedef union {
    UINT8    u8;
    UINT16   u16;
    UINT32   u32;
    UINT64   u64;
    BOOLEAN  b;
} EFI_IFR_TYPE_VALUE;

typedef struct {
    EFI_IFR_OP_HEADER  Header;
    EFI_GUID           Guid;
    EFI_STRING_ID      FormSetTitle;
    EFI_STRING_ID      Help;
    UINT8              Flags;
    // EFI_GUID        ClassGuid[];  // trailing, variable count — unused
} EFI_IFR_FORM_SET;

typedef struct {
    EFI_IFR_OP_HEADER  Header;
    EFI_GUID           Guid;
    EFI_VARSTORE_ID    VarStoreId;
    UINT16             Size;
    // UINT8           Name[];       // trailing ASCII, variable length
} EFI_IFR_VARSTORE;

typedef struct {
    EFI_IFR_OP_HEADER  Header;
    EFI_VARSTORE_ID    VarStoreId;
    EFI_GUID           Guid;
    UINT32             Attributes;
    UINT16             Size;
    // UINT8           Name[];       // trailing ASCII, variable length
} EFI_IFR_VARSTORE_EFI;

typedef struct {
    EFI_IFR_OP_HEADER        Header;
    EFI_IFR_QUESTION_HEADER  Question;   // embedded (spec HTML's `*Question` is a typo)
    UINT8                    Flags;
    union {
        struct { UINT8  MinValue, MaxValue, Step; } u8;
        struct { UINT16 MinValue, MaxValue, Step; } u16;
        struct { UINT32 MinValue, MaxValue, Step; } u32;
        struct { UINT64 MinValue, MaxValue, Step; } u64;
    } data;
} EFI_IFR_ONE_OF;

typedef struct {
    EFI_IFR_OP_HEADER        Header;
    EFI_IFR_QUESTION_HEADER  Question;
    UINT8                    Flags;
} EFI_IFR_CHECKBOX;

typedef struct {
    EFI_IFR_OP_HEADER        Header;
    EFI_IFR_QUESTION_HEADER  Question;
    UINT8                    Flags;
    union {
        struct { UINT8  MinValue, MaxValue, Step; } u8;
        struct { UINT16 MinValue, MaxValue, Step; } u16;
        struct { UINT32 MinValue, MaxValue, Step; } u32;
        struct { UINT64 MinValue, MaxValue, Step; } u64;
    } data;
} EFI_IFR_NUMERIC;

typedef struct {
    EFI_IFR_OP_HEADER        Header;
    EFI_IFR_QUESTION_HEADER  Question;
    UINT8                    MinSize;
    UINT8                    MaxSize;
    UINT8                    Flags;
} EFI_IFR_STRING;

typedef struct {
    EFI_IFR_OP_HEADER   Header;
    EFI_STRING_ID       Option;
    UINT8               Flags;
    UINT8               Type;
    EFI_IFR_TYPE_VALUE  Value;
} EFI_IFR_ONE_OF_OPTION;

#pragma pack()

// --- HII protocols (only the methods AxlHii uses; rest kept VOID *) ---

typedef struct _EFI_HII_DATABASE_PROTOCOL  EFI_HII_DATABASE_PROTOCOL;
typedef struct _EFI_HII_STRING_PROTOCOL    EFI_HII_STRING_PROTOCOL;
typedef struct _EFI_HII_CONFIG_ROUTING_PROTOCOL EFI_HII_CONFIG_ROUTING_PROTOCOL;
typedef struct _EFI_HII_CONFIG_ACCESS_PROTOCOL  EFI_HII_CONFIG_ACCESS_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_HII_DATABASE_LIST_PACKS)(
    IN     CONST EFI_HII_DATABASE_PROTOCOL  *This,
    IN     UINT8                             PackageType,
    IN     CONST EFI_GUID                   *PackageGuid,
    IN OUT UINTN                            *HandleBufferLength,
    OUT    EFI_HII_HANDLE                    *Handle
    );

typedef EFI_STATUS (EFIAPI *EFI_HII_DATABASE_EXPORT_PACKS)(
    IN     CONST EFI_HII_DATABASE_PROTOCOL  *This,
    IN     EFI_HII_HANDLE                    Handle,
    IN OUT UINTN                            *BufferSize,
    OUT    EFI_HII_PACKAGE_LIST_HEADER      *Buffer
    );

typedef EFI_STATUS (EFIAPI *EFI_HII_DATABASE_GET_PACK_HANDLE)(
    IN  CONST EFI_HII_DATABASE_PROTOCOL  *This,
    IN  EFI_HII_HANDLE                    PackageListHandle,
    OUT EFI_HANDLE                       *DriverHandle
    );

struct _EFI_HII_DATABASE_PROTOCOL {
    VOID                              *NewPackageList;       // unused
    VOID                              *RemovePackageList;    // unused
    VOID                              *UpdatePackageList;    // unused
    EFI_HII_DATABASE_LIST_PACKS        ListPackageLists;
    EFI_HII_DATABASE_EXPORT_PACKS      ExportPackageLists;
    VOID                              *RegisterPackageNotify;   // unused
    VOID                              *UnregisterPackageNotify; // unused
    VOID                              *FindKeyboardLayouts;     // unused
    VOID                              *GetKeyboardLayout;       // unused
    VOID                              *SetKeyboardLayout;       // unused
    EFI_HII_DATABASE_GET_PACK_HANDLE   GetPackageListHandle;
};

typedef EFI_STATUS (EFIAPI *EFI_HII_GET_STRING)(
    IN  CONST EFI_HII_STRING_PROTOCOL  *This,
    IN  CONST CHAR8                    *Language,
    IN  EFI_HII_HANDLE                  PackageList,
    IN  EFI_STRING_ID                   StringId,
    OUT EFI_STRING                      String,
    IN OUT UINTN                       *StringSize,
    OUT VOID                           *StringFontInfo OPTIONAL
    );

struct _EFI_HII_STRING_PROTOCOL {
    VOID                *NewString;     // unused
    EFI_HII_GET_STRING   GetString;
    VOID                *SetString;     // unused
    VOID                *GetLanguages;  // unused
    VOID                *GetSecondaryLanguages;  // unused
};

typedef EFI_STATUS (EFIAPI *EFI_HII_EXTRACT_CONFIG)(
    IN  CONST EFI_HII_CONFIG_ROUTING_PROTOCOL  *This,
    IN  CONST EFI_STRING                        Request,
    OUT EFI_STRING                             *Progress,
    OUT EFI_STRING                             *Results
    );

typedef EFI_STATUS (EFIAPI *EFI_HII_BLOCK_TO_CONFIG)(
    IN  CONST EFI_HII_CONFIG_ROUTING_PROTOCOL  *This,
    IN  CONST EFI_STRING                        ConfigRequest,
    IN  CONST UINT8                            *Block,
    IN  CONST UINTN                             BlockSize,
    OUT EFI_STRING                             *Config,
    OUT EFI_STRING                             *Progress
    );

typedef EFI_STATUS (EFIAPI *EFI_HII_CONFIG_TO_BLOCK)(
    IN     CONST EFI_HII_CONFIG_ROUTING_PROTOCOL  *This,
    IN     CONST EFI_STRING                        ConfigResp,
    IN OUT UINT8                                  *Block,
    IN OUT UINTN                                  *BlockSize,
    OUT    EFI_STRING                             *Progress
    );

struct _EFI_HII_CONFIG_ROUTING_PROTOCOL {
    EFI_HII_EXTRACT_CONFIG    ExtractConfig;
    VOID                     *ExportConfig;   // unused
    VOID                     *RouteConfig;    // unused (routing-level)
    EFI_HII_BLOCK_TO_CONFIG   BlockToConfig;
    EFI_HII_CONFIG_TO_BLOCK   ConfigToBlock;
    VOID                     *GetAltCfg;      // unused
};

typedef EFI_STATUS (EFIAPI *EFI_HII_ACCESS_EXTRACT_CONFIG)(
    IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
    IN  CONST EFI_STRING                       Request,
    OUT EFI_STRING                            *Progress,
    OUT EFI_STRING                            *Results
    );

typedef EFI_STATUS (EFIAPI *EFI_HII_ACCESS_ROUTE_CONFIG)(
    IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
    IN  CONST EFI_STRING                       Configuration,
    OUT EFI_STRING                            *Progress
    );

struct _EFI_HII_CONFIG_ACCESS_PROTOCOL {
    EFI_HII_ACCESS_EXTRACT_CONFIG  ExtractConfig;
    EFI_HII_ACCESS_ROUTE_CONFIG    RouteConfig;
    VOID                          *Callback;   // unused
};

#endif /* AXL_UEFI_EXTRA_H */
