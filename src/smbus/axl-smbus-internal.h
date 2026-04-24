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
 * Per-transport cleanup; frees whatever ctx held. Called during
 * axl_smbus_free().
 */
typedef void (*AxlSmbusCloseFn)(void *ctx);

/**
 * Transport operations bundle. A session holds exactly one.
 */
typedef struct {
    AxlSmbusTransport     kind;
    AxlSmbusReadBlockFn   read_block;
    AxlSmbusWriteBlockFn  write_block;
    AxlSmbusCloseFn       close;
    void                 *ctx;
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

#endif /* AXL_SMBUS_INTERNAL_H */
