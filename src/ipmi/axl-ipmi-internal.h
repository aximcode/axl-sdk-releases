/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-internal.h
    Private types for the AxlIpmi module.

    Not shipped to SDK consumers. The module files in src/ipmi/
    share these definitions; everything else goes through the
    public header <axl/axl-ipmi.h>.
**/

#ifndef AXL_IPMI_INTERNAL_H
#define AXL_IPMI_INTERNAL_H

#include <axl/axl-ipmi.h>
#include <axl/axl-mem.h>
#include <axl/axl-smbus.h>

#include "../backend/axl-backend.h"

// ---------------------------------------------------------------------------
// Transport vtable
// ---------------------------------------------------------------------------

/**
 * Per-transport send_raw entry point. Each transport
 * implementation (KCS, SSIF, EDKII, Dell) provides one.
 *
 * Same in/out semantics as axl_ipmi_raw(): resp/resp_len are
 * filled with the completion code + response data on success.
 */
typedef int (*AxlIpmiSendRawFn)(
    void           *ctx,
    uint8_t         netfn,
    uint8_t         cmd,
    const uint8_t  *req,
    size_t          req_len,
    uint8_t        *resp,
    size_t         *resp_len);

/**
 * Per-transport cleanup; frees whatever ctx held.
 * Called during axl_ipmi_session_free().
 */
typedef void (*AxlIpmiCloseFn)(void *ctx);

/**
 * Transport operations bundle. A session holds exactly one.
 */
typedef struct {
    AxlIpmiTransport   kind;
    AxlIpmiSendRawFn   send_raw;
    AxlIpmiCloseFn     close;
    void              *ctx;
} AxlIpmiTransportOps;

struct AxlIpmiSession {
    AxlIpmiTransportOps  ops;
};

// ---------------------------------------------------------------------------
// Per-transport constructors (called by axl-ipmi.c's auto-detect)
//
// Each returns 0 and fills @a ops on success, or -1 if the transport
// isn't available (e.g. SMBIOS Type 38 absent, or LocateProtocol
// fails for a vendor protocol).
// ---------------------------------------------------------------------------

int axl_ipmi_kcs_open(AxlIpmiTransportOps *ops,
                      uint16_t             data_port,
                      uint16_t             cmd_port);

/**
 * SSIF opener takes ownership of @a smbus on success (it becomes part
 * of the session context and is freed by the transport's close).
 * On failure, the caller retains ownership.
 */
int axl_ipmi_ssif_open(AxlIpmiTransportOps *ops,
                       AxlSmbus            *smbus,
                       uint8_t              slave_addr);

/**
 * Fail-fast IPMI Get Device ID round-trip on (@a smbus, @a addr), used by
 * the SSIF opener to discover which controller and slave address actually
 * host the BMC (multi-bus platforms like Nvidia Grace publish several I2C
 * masters, and the SMBIOS slave-address encoding is ambiguous across
 * platforms). Does NOT take ownership of @a smbus.
 *
 * A write ACK alone only proves *some* device is on the bus, so a full
 * write AND read is required. Returns true iff the BMC answered Get Device
 * ID with completion code 0x00.
 */
bool axl_ipmi_ssif_probe_get_device_id(AxlSmbus *smbus, uint8_t addr);

int axl_ipmi_edkii_open(AxlIpmiTransportOps *ops);

int axl_ipmi_dell_open(AxlIpmiTransportOps *ops);

#endif /* AXL_IPMI_INTERNAL_H */
