/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbus-internal.h
    Private types for the AxlSmbus module.

    Not shipped to SDK consumers. The module files in src/smbus/
    share these definitions; everything else goes through the
    public header <axl/axl-smbus.h>.
**/

#ifndef AXL_SMBUS_INTERNAL_H
#define AXL_SMBUS_INTERNAL_H

#include <axl/axl-smbus.h>
#include <axl/axl-mem.h>

#include "../backend/axl-backend.h"

// ---------------------------------------------------------------------------
// Transport vtable
// ---------------------------------------------------------------------------

/**
 * Per-transport block read. Same in/out semantics as axl_smbus_read_block.
 */
typedef int (*AxlSmbusReadBlockFn)(
    void     *ctx,
    uint8_t   slave,
    uint8_t   command,
    uint8_t  *buf,
    size_t   *len);

/**
 * Per-transport block write. Same in/out semantics as axl_smbus_write_block.
 */
typedef int (*AxlSmbusWriteBlockFn)(
    void           *ctx,
    uint8_t         slave,
    uint8_t         command,
    const uint8_t  *buf,
    size_t          len);

/**
 * Per-transport byte read. Same in/out semantics as axl_smbus_read_byte.
 */
typedef int (*AxlSmbusReadByteFn)(
    void     *ctx,
    uint8_t   slave,
    uint8_t   command,
    uint8_t  *out);

/**
 * Per-transport byte write. Same in/out semantics as axl_smbus_write_byte.
 */
typedef int (*AxlSmbusWriteByteFn)(
    void     *ctx,
    uint8_t   slave,
    uint8_t   command,
    uint8_t   value);

/**
 * Per-transport SMBus QUICK op — no command, no data, just an
 * address + R/W bit ACK probe. Same in/out semantics as
 * axl_smbus_quick. NULL is allowed in the ops table; the dispatcher
 * returns AXL_ERR if the backend doesn't implement it.
 */
typedef int (*AxlSmbusQuickFn)(
    void     *ctx,
    uint8_t   slave,
    bool      is_read);

/**
 * Per-transport SMBus Receive Byte — read 1 byte, no command.
 */
typedef int (*AxlSmbusReceiveByteFn)(
    void     *ctx,
    uint8_t   slave,
    uint8_t  *out);

/**
 * Per-transport cleanup; frees whatever ctx held. Called during
 * axl_smbus_free().
 */
typedef void (*AxlSmbusCloseFn)(void *ctx);

/**
 * Transport operations bundle. A session holds exactly one.
 *
 * @c desc holds a per-instance human-readable identity, filled by
 * the backend at open time (e.g., "AMD FCH PIIX4 port 1 at 0xB20"
 * for PIIX4). 48 bytes covers every backend's worst case with
 * margin; truncation is unlikely in practice.
 */
typedef struct {
    AxlSmbusTransport     kind;
    AxlSmbusReadBlockFn   read_block;
    AxlSmbusWriteBlockFn  write_block;
    AxlSmbusReadByteFn    read_byte;
    AxlSmbusWriteByteFn   write_byte;
    AxlSmbusQuickFn       quick;        /* may be NULL */
    AxlSmbusReceiveByteFn receive_byte; /* may be NULL */
    AxlSmbusCloseFn       close;
    void                 *ctx;
    char                  desc[48];
} AxlSmbusTransportOps;

struct AxlSmbus {
    AxlSmbusTransportOps  ops;
};

// ---------------------------------------------------------------------------
// Per-transport constructors (called by axl-smbus.c's auto-detect)
//
// Each returns 0 and fills @a ops on success, or -1 if the transport
// isn't available (LocateProtocol failed).
// ---------------------------------------------------------------------------

int axl_smbus_hc_open(AxlSmbusTransportOps *ops);

int axl_smbus_i2c_open(AxlSmbusTransportOps *ops);

/**
 * Open a specific handle as an SMBus HC transport. Used by the
 * multi-handle walker in axl_smbus_new_with_probe to try every
 * candidate when LocateProtocol's first-instance pick isn't the
 * right one (multi-segment server platform topologies).
 *
 * Returns 0 on success and fills @a ops; -1 if the handle doesn't
 * publish EFI_SMBUS_HC_PROTOCOL or its Execute is NULL.
 */
int axl_smbus_hc_open_handle(AxlSmbusTransportOps *ops, void *handle);

/**
 * Same as axl_smbus_hc_open_handle but for EFI_I2C_MASTER_PROTOCOL.
 */
int axl_smbus_i2c_open_handle(AxlSmbusTransportOps *ops, void *handle);

/**
 * Open the AMD FCH (PIIX4-compatible) SMBus controller's @p port_index
 * port via direct I/O port access. Used by the smbus walker as a
 * fallback when EFI_SMBUS_HC_PROTOCOL / EFI_I2C_MASTER don't expose
 * the bus carrying DIMM SPDs (some AMD server platforms).
 *
 * @return 0 on success, -1 if AMD FCH SMBus PCI device not present
 *     or the I/O range looks unmapped.
 */
int axl_smbus_piix4_open_port(AxlSmbusTransportOps *ops, size_t port_index);

/**
 * Number of PIIX4 ports the walker should advertise (0 if AMD FCH
 * SMBus controller isn't present in this system).
 */
size_t axl_smbus_piix4_port_count(void);

#endif /* AXL_SMBUS_INTERNAL_H */
