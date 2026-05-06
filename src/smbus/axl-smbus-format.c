/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbus-format.c
    Enum-to-string helpers for AxlSmbus. No allocation.
**/

#include <axl/axl-smbus.h>

const char *
axl_smbus_transport_string(AxlSmbusTransport kind)
{
    switch (kind) {
    case AXL_SMBUS_TRANSPORT_HC:    return "SMBus HC";
    case AXL_SMBUS_TRANSPORT_I2C:   return "I2C Master";
    case AXL_SMBUS_TRANSPORT_PIIX4: return "PIIX4";
    case AXL_SMBUS_TRANSPORT_UNKNOWN:
    default:                        return "unknown";
    }
}
