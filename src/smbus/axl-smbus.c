/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbus.c
    AxlSmbus — session lifecycle + transport auto-detection.

    Prefers EFI_SMBUS_HC_PROTOCOL (the firmware owns all SMBus framing).
    Falls back to EFI_I2C_MASTER_PROTOCOL (this module builds the
    SMBus block-transfer wire format on top of raw I2C operations).
**/

#include "axl-smbus-internal.h"

#include <axl/axl-log.h>

AXL_LOG_DOMAIN("smbus");

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlSmbus *
axl_smbus_new(void)
{
    AxlSmbusTransportOps ops = { 0 };

    if (axl_smbus_hc_open(&ops) != 0 &&
        axl_smbus_i2c_open(&ops) != 0)
    {
        axl_debug("SMBus: no controller (neither HC nor I2C Master)");
        return NULL;
    }

    AxlSmbus *s = axl_malloc(sizeof(AxlSmbus));
    if (s == NULL) {
        if (ops.close != NULL) {
            ops.close(ops.ctx);
        }
        return NULL;
    }
    s->ops = ops;

    axl_info("SMBus: %s transport ready",
             axl_smbus_transport_string(ops.kind));
    return s;
}

void
axl_smbus_free(AxlSmbus *s)
{
    if (s == NULL) {
        return;
    }
    if (s->ops.close != NULL) {
        s->ops.close(s->ops.ctx);
    }
    axl_free(s);
}

AxlSmbusTransport
axl_smbus_transport(const AxlSmbus *s)
{
    if (s == NULL) {
        return AXL_SMBUS_TRANSPORT_UNKNOWN;
    }
    return s->ops.kind;
}

int
axl_smbus_read_block(AxlSmbus *s,
                     uint8_t   slave,
                     uint8_t   command,
                     uint8_t  *buf,
                     size_t   *len)
{
    if (s == NULL || buf == NULL || len == NULL) {
        return -1;
    }
    return s->ops.read_block(s->ops.ctx, slave, command, buf, len);
}

int
axl_smbus_write_block(AxlSmbus       *s,
                      uint8_t         slave,
                      uint8_t         command,
                      const uint8_t  *buf,
                      size_t          len)
{
    if (s == NULL || buf == NULL) {
        return -1;
    }
    if (len > AXL_SMBUS_BLOCK_MAX) {
        return -1;
    }
    return s->ops.write_block(s->ops.ctx, slave, command, buf, len);
}
