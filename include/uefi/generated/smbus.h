/** @file generated/smbus.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit — regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_SMBUS_H
#define AXL_UEFI_GEN_SMBUS_H

#include "types.h"
#include "status.h"

typedef struct _EFI_SMBUS_DEVICE_ADDRESS {
  UINTN SmbusDeviceAddress:7;
} EFI_SMBUS_DEVICE_ADDRESS;

typedef UINTN EFI_SMBUS_DEVICE_COMMAND;

typedef enum _EFI_SMBUS_OPERATION {
  EfiSmbusQuickRead,
  EfiSmbusQuickWrite,
  EfiSmbusReceiveByte,
  EfiSmbusSendByte,
  EfiSmbusReadByte,
  EfiSmbusWriteByte,
  EfiSmbusReadWord,
  EfiSmbusWriteWord,
  EfiSmbusReadBlock,
  EfiSmbusWriteBlock,
  EfiSmbusProcessCall,
  EfiSmbusBWBRProcessCall
} EFI_SMBUS_OPERATION;

typedef struct _EFI_SMBUS_UDID {
  UINT32 VendorSpecificId;
  UINT16 SubsystemDeviceId;
  UINT16 SubsystemVendorId;
  UINT16 Interface;
  UINT16 DeviceId;
  UINT16 VendorId;
  UINT8 VendorRevision;
  UINT8 DeviceCapabilities;
} EFI_SMBUS_UDID;

typedef struct _EFI_SMBUS_DEVICE_MAP {
  EFI_SMBUS_DEVICE_ADDRESS SmbusDeviceAddress;
  EFI_SMBUS_UDID SmbusDeviceUdid;
} EFI_SMBUS_DEVICE_MAP;

typedef
EFI_STATUS
(EFIAPI *EFI_SMBUS_NOTIFY_FUNCTION) (
IN EFI_SMBUS_DEVICE_ADDRESS SlaveAddress,
IN UINTN Data
);

typedef

EFI_STATUS

(EFIAPI *EFI_SMBUS_HC_EXECUTE_OPERATION) (

IN CONST EFI_SMBUS_HC_PROTOCOL *This,

IN EFI_SMBUS_DEVICE_ADDRESS SlaveAddress,

IN EFI_SMBUS_DEVICE_COMMAND Command,

IN EFI_SMBUS_OPERATION Operation,

IN BOOLEAN PecCheck,

IN OUT UINTN *Length,

IN OUT VOID *Buffer

);

typedef
EFI_STATUS
(EFIAPI *EFI_SMBUS_HC_PROTOCOL_ARP_DEVICE) (
IN CONST EFI_SMBUS_HC_PROTOCOL *This,
IN BOOLEAN ArpAll,
IN EFI_SMBUS_UDID *SmbusUdid OPTIONAL,
IN OUT EFI_SMBUS_DEVICE_ADDRESS *SlaveAddress OPTIONAL
);

typedef
EFI_STATUS
(EFIAPI *EFI_SMBUS_HC_PROTOCOL_GET_ARP_MAP) (
IN CONST EFI_SMBUS_HC_PROTOCOL *This,
IN OUT UINTN *Length,
IN OUT EFI_SMBUS_DEVICE_MAP **SmbusDeviceMap
);

typedef
EFI_STATUS
(EFIAPI *EFI_SMBUS_HC_PROTOCOL_NOTIFY) (
IN CONST EFI_SMBUS_HC_PROTOCOL *This,
IN EFI_SMBUS_DEVICE_ADDRESS SlaveAddress,
IN UINTN Data,
IN EFI_SMBUS_NOTIFY_FUNCTION NotifyFunction
);

typedef struct _EFI_SMBUS_HC_PROTOCOL {
EFI_SMBUS_HC_EXECUTE_OPERATION Execute;
EFI_SMBUS_HC_PROTOCOL_ARP_DEVICE ArpDevice;
EFI_SMBUS_HC_PROTOCOL_GET_ARP_MAP GetArpMap;
EFI_SMBUS_HC_PROTOCOL_NOTIFY Notify;
} EFI_SMBUS_HC_PROTOCOL;


#endif /* AXL_UEFI_GEN_SMBUS_H */
