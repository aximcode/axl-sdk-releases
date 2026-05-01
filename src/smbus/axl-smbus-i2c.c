/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbus-i2c.c
    EFI_I2C_MASTER_PROTOCOL transport for AxlSmbus.

    The I2C Master protocol exposes raw I2C operations; this module
    builds the SMBus block-transfer framing on top:

      Write:  [CmdCode][ByteCount][Data...]   total bytes = 2 + ByteCount
      Read:   write [CmdCode], then read      response = [ByteCount][Data...]

    (SMBus 2.0 §5.5.7/5.5.8). EFI_SMBUS_HC_PROTOCOL would insert the
    count for us; EFI_I2C_MASTER_PROTOCOL does not, so we build the
    outbound wire format manually and strip the inbound count byte
    from the response. Regression coverage for B1
    (code-review finding in 78859fa): the byte-count prefix MUST
    appear at buf[1] on writes and MUST be stripped on reads. Missing
    either bite only surfaces on real hardware (Dell iDRAC,
    Nvidia Grace Arm64) — matches uefi-ipmitool's I2C helpers
    (IpmiSsif.c:237-306).
**/

#include "axl-smbus-internal.h"

#include <axl/axl-log.h>

AXL_LOG_DOMAIN("smbus-i2c");

// ---------------------------------------------------------------------------
// Vtable methods
// ---------------------------------------------------------------------------

static int
i2c_read_block(void *vctx,
               uint8_t slave, uint8_t command,
               uint8_t *buf, size_t *len)
{
    EFI_I2C_MASTER_PROTOCOL *i2c = (EFI_I2C_MASTER_PROTOCOL *)vctx;

    //
    // Two-operation transaction:
    //   op[0] writes the 1-byte SMBus command code.
    //   op[1] reads into a local buffer whose first byte is the
    //         response byte count; remaining bytes are the payload.
    // Strip the count byte and copy the clamped payload into the
    // caller's buffer.
    //
    uint8_t cmd_byte = command;
    uint8_t rx[AXL_SMBUS_BLOCK_MAX + 1];
    UINT32  rx_cap = (UINT32)sizeof(rx);

    struct {
        UINTN              OperationCount;
        EFI_I2C_OPERATION  Operation[2];
    } pkt = {
        .OperationCount = 2,
        .Operation = {
            { .Flags = 0,             .LengthInBytes = 1,      .Buffer = &cmd_byte },
            { .Flags = I2C_FLAG_READ, .LengthInBytes = rx_cap, .Buffer = rx },
        },
    };

    EFI_STATUS s = axl_efi_call(i2c->StartRequest, 5,
                                i2c, (UINTN)slave,
                                (EFI_I2C_REQUEST_PACKET *)&pkt,
                                NULL, NULL);
    if (EFI_ERROR(s)) {
        return -1;
    }

    size_t count = rx[0];
    if (count > sizeof(rx) - 1) {
        count = sizeof(rx) - 1;
    }
    if (count > *len) {
        count = *len;
    }
    for (size_t i = 0; i < count; i++) {
        buf[i] = rx[i + 1];
    }
    *len = count;
    return 0;
}

static int
i2c_write_block(void *vctx,
                uint8_t slave, uint8_t command,
                const uint8_t *buf, size_t len)
{
    EFI_I2C_MASTER_PROTOCOL *i2c = (EFI_I2C_MASTER_PROTOCOL *)vctx;

    //
    // SMBus block-write wire format: [CmdCode][ByteCount][Data...].
    // Stack-allocated — max 34 bytes, no heap needed.
    //
    uint8_t tx[AXL_SMBUS_BLOCK_MAX + 2];
    tx[0] = command;
    tx[1] = (uint8_t)len;
    for (size_t i = 0; i < len; i++) {
        tx[i + 2] = buf[i];
    }

    struct {
        UINTN              OperationCount;
        EFI_I2C_OPERATION  Operation[1];
    } pkt = {
        .OperationCount = 1,
        .Operation = {
            { .Flags = 0, .LengthInBytes = (UINT32)(len + 2), .Buffer = tx },
        },
    };

    EFI_STATUS s = axl_efi_call(i2c->StartRequest, 5,
                                i2c, (UINTN)slave,
                                (EFI_I2C_REQUEST_PACKET *)&pkt,
                                NULL, NULL);
    return EFI_ERROR(s) ? -1 : 0;
}

static int
i2c_read_byte(void *vctx,
              uint8_t slave, uint8_t command,
              uint8_t *out)
{
    EFI_I2C_MASTER_PROTOCOL *i2c = (EFI_I2C_MASTER_PROTOCOL *)vctx;

    //
    // SMBus "Read Byte" (§5.5.5) on raw I2C: write the command byte,
    // then read a single data byte (no count prefix — matches 24Cxx
    // EEPROM auto-increment behavior used by JEDEC SPDs).
    //
    uint8_t cmd_byte = command;
    uint8_t rx       = 0;

    struct {
        UINTN              OperationCount;
        EFI_I2C_OPERATION  Operation[2];
    } pkt = {
        .OperationCount = 2,
        .Operation = {
            { .Flags = 0,             .LengthInBytes = 1, .Buffer = &cmd_byte },
            { .Flags = I2C_FLAG_READ, .LengthInBytes = 1, .Buffer = &rx       },
        },
    };

    EFI_STATUS s = axl_efi_call(i2c->StartRequest, 5,
                                i2c, (UINTN)slave,
                                (EFI_I2C_REQUEST_PACKET *)&pkt,
                                NULL, NULL);
    if (EFI_ERROR(s)) {
        return -1;
    }
    *out = rx;
    return 0;
}

static int
i2c_write_byte(void *vctx,
               uint8_t slave, uint8_t command,
               uint8_t value)
{
    EFI_I2C_MASTER_PROTOCOL *i2c = (EFI_I2C_MASTER_PROTOCOL *)vctx;

    //
    // SMBus "Write Byte" (§5.5.4) on raw I2C: [CmdCode][DataByte] in
    // a single write transaction.
    //
    uint8_t tx[2] = { command, value };

    struct {
        UINTN              OperationCount;
        EFI_I2C_OPERATION  Operation[1];
    } pkt = {
        .OperationCount = 1,
        .Operation = {
            { .Flags = 0, .LengthInBytes = 2, .Buffer = tx },
        },
    };

    EFI_STATUS s = axl_efi_call(i2c->StartRequest, 5,
                                i2c, (UINTN)slave,
                                (EFI_I2C_REQUEST_PACKET *)&pkt,
                                NULL, NULL);
    return EFI_ERROR(s) ? -1 : 0;
}

static void
i2c_close(void *vctx)
{
    //
    // Firmware owns the EFI_I2C_MASTER_PROTOCOL instance — nothing to free.
    //
    (void)vctx;
}

// ---------------------------------------------------------------------------
// Public opener
// ---------------------------------------------------------------------------

int
axl_smbus_i2c_open(AxlSmbusTransportOps *ops)
{
    if (ops == NULL) {
        return -1;
    }

    EFI_I2C_MASTER_PROTOCOL *i2c  = NULL;
    EFI_GUID                 guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    EFI_STATUS s = gBS->LocateProtocol(&guid, NULL, (VOID **)&i2c);
    if (EFI_ERROR(s) || i2c == NULL) {
        return -1;
    }
    if (i2c->StartRequest == NULL) {
        axl_warning("EFI_I2C_MASTER_PROTOCOL found but StartRequest is NULL");
        return -1;
    }

    ops->kind        = AXL_SMBUS_TRANSPORT_I2C;
    ops->read_block  = i2c_read_block;
    ops->write_block = i2c_write_block;
    ops->read_byte   = i2c_read_byte;
    ops->write_byte  = i2c_write_byte;
    ops->close       = i2c_close;
    ops->ctx         = i2c;
    return 0;
}
