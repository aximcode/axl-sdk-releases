/** @file generated/i2c.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit — regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_I2C_H
#define AXL_UEFI_GEN_I2C_H

#include "types.h"
#include "status.h"

#define I2C_FLAG_READ               0x00000001

#define I2C_ADDRESSING_10_BIT 0x80000000

typedef struct _EFI_I2C_OPERATION {
  UINT32             Flags;
  UINT32             LengthInBytes;
  UINT8              *Buffer;
} EFI_I2C_OPERATION;

typedef struct _EFI_I2C_REQUEST_PACKET {
  UINTN                   OperationCount;
  EFI_I2C_OPERATION       Operation[1];
} EFI_I2C_REQUEST_PACKET;

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_MASTER_PROTOCOL_SET_BUS_FREQUENCY) (
  IN CONST EFI_I2C_MASTER_PROTOCOL    *This,
  IN OUT UINTN                        *BusClockHertz
  );

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_MASTER_PROTOCOL_RESET) (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_MASTER_PROTOCOL_START_REQUEST) (
  IN CONST EFI_I2C_MASTER_PROTOCOL   *This,
  IN UINTN                           SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET          *RequestPacket,
  IN EFI_EVENT                       Event          OPTIONAL,
  OUT EFI_STATUS                     *I2cStatus     OPTIONAL
  );

typedef struct _EFI_I2C_MASTER_PROTOCOL {
  EFI_I2C_MASTER_PROTOCOL_SET_BUS_FREQUENCY  SetBusFrequency;
  EFI_I2C_MASTER_PROTOCOL_RESET              Reset;
  EFI_I2C_MASTER_PROTOCOL_START_REQUEST      StartRequest;
  void  *I2cControllerCapabilities;
} EFI_I2C_MASTER_PROTOCOL;


#endif /* AXL_UEFI_GEN_I2C_H */
