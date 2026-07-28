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
#include <uefi/axl-uefi.h>

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

/*
 * Walk every published EFI_SMBUS_HC_PROTOCOL + EFI_I2C_MASTER_PROTOCOL
 * handle, opening each as a session and handing it to the caller's
 * probe. First true wins.
 */
static AxlSmbus *
wrap_ops_into_session(const AxlSmbusTransportOps *ops)
{
    AxlSmbus *s = axl_malloc(sizeof(AxlSmbus));
    if (s == NULL) {
        if (ops->close != NULL) {
            ops->close(ops->ctx);
        }
        return NULL;
    }
    s->ops = *ops;
    return s;
}

static AxlSmbus *
try_each_handle(const EFI_GUID *guid,
                int (*open_handle)(AxlSmbusTransportOps *, void *),
                AxlSmbusProbeFn probe, void *user)
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    EFI_STATUS  st = gBS->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)guid, NULL, &count, &handles);
    if (EFI_ERROR(st) || count == 0) {
        return NULL;
    }

    AxlSmbus *winner = NULL;
    for (UINTN i = 0; i < count && winner == NULL; i++) {
        AxlSmbusTransportOps ops = { 0 };
        if (open_handle(&ops, handles[i]) != AXL_OK) {
            continue;
        }
        AxlSmbus *cand = wrap_ops_into_session(&ops);
        if (cand == NULL) {
            continue;
        }
        if (probe == NULL || probe(cand, user)) {
            winner = cand;
        } else {
            axl_smbus_free(cand);
        }
    }
    gBS->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    return winner;
}

/*
 * Try every PIIX4 port advertised by axl-smbus-piix4 (only nonzero
 * when AMD FCH SMBus is detected). Same probe-claims-first contract
 * as the protocol-handle walker.
 */
static AxlSmbus *
try_piix4_ports(AxlSmbusProbeFn probe, void *user)
{
    size_t n = axl_smbus_piix4_port_count();
    for (size_t i = 0; i < n; i++) {
        AxlSmbusTransportOps ops = { 0 };
        if (axl_smbus_piix4_open_port(&ops, i) != AXL_OK) {
            continue;
        }
        AxlSmbus *cand = wrap_ops_into_session(&ops);
        if (cand == NULL) {
            continue;
        }
        if (probe == NULL || probe(cand, user)) {
            return cand;
        }
        axl_smbus_free(cand);
    }
    return NULL;
}

AxlSmbus *
axl_smbus_new_with_probe(AxlSmbusProbeFn probe, void *user)
{
    /* Try SMBus HC handles first (firmware-built block transfers are
     * less ambiguous than our hand-rolled I2C framing). Fall through
     * to I2C masters — on AMD EPYC the SPDs typically live there. */
    AxlSmbus *s = try_each_handle(&gEfiSmbusHcProtocolGuid,
                                  axl_smbus_hc_open_handle, probe, user);
    if (s != NULL) {
        axl_info("SMBus: HC transport ready (probe-selected)");
        return s;
    }

    EFI_GUID i2c_guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    s = try_each_handle(&i2c_guid,
                        axl_smbus_i2c_open_handle, probe, user);
    if (s != NULL) {
        axl_info("SMBus: I2C transport ready (probe-selected)");
        return s;
    }

    /* Last resort: AMD FCH PIIX4 direct I/O. Some AMD server
     * boards expose the DIMM SPD bus only here — firmware keeps
     * EFI_SMBUS_HC_PROTOCOL pointing at a different controller. */
    s = try_piix4_ports(probe, user);
    if (s != NULL) {
        axl_info("SMBus: PIIX4 transport ready (probe-selected)");
        return s;
    }

    axl_debug("SMBus: no controller passed probe");
    return NULL;
}

static size_t
visit_each_handle(const EFI_GUID *guid,
                  int (*open_handle)(AxlSmbusTransportOps *, void *),
                  AxlSmbusVisitFn visit, void *user, size_t start_index)
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    EFI_STATUS  st = gBS->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)guid, NULL, &count, &handles);
    if (EFI_ERROR(st) || count == 0) {
        return 0;
    }

    size_t visited = 0;
    for (UINTN i = 0; i < count; i++) {
        AxlSmbusTransportOps ops = { 0 };
        if (open_handle(&ops, handles[i]) != AXL_OK) {
            continue;
        }
        AxlSmbus *cand = wrap_ops_into_session(&ops);
        if (cand == NULL) {
            continue;
        }
        visit(cand, start_index + visited, user);
        axl_smbus_free(cand);
        visited++;
    }
    gBS->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    return visited;
}

size_t
axl_smbus_visit_all(AxlSmbusVisitFn visit, void *user)
{
    if (visit == NULL) {
        return 0;
    }
    size_t n = visit_each_handle(&gEfiSmbusHcProtocolGuid,
                                 axl_smbus_hc_open_handle,
                                 visit, user, 0);
    EFI_GUID i2c_guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    n += visit_each_handle(&i2c_guid,
                           axl_smbus_i2c_open_handle,
                           visit, user, n);
    /* PIIX4 ports — only present on AMD FCH boards (port_count == 0
     * elsewhere). Visit so memspd scan can show what's at each port. */
    size_t piix4_count = axl_smbus_piix4_port_count();
    for (size_t i = 0; i < piix4_count; i++) {
        AxlSmbusTransportOps ops = { 0 };
        if (axl_smbus_piix4_open_port(&ops, i) != AXL_OK) {
            continue;
        }
        AxlSmbus *cand = wrap_ops_into_session(&ops);
        if (cand == NULL) {
            continue;
        }
        visit(cand, n + i, user);
        axl_smbus_free(cand);
    }
    n += piix4_count;
    return n;
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

const char *
axl_smbus_describe(const AxlSmbus *s)
{
    if (s == NULL) {
        return NULL;
    }
    /* desc[] is zero-initialized; if a backend forgot to fill it,
     * fall back to the kind label. */
    if (s->ops.desc[0] == '\0') {
        return axl_smbus_transport_string(s->ops.kind);
    }
    return s->ops.desc;
}

int
axl_smbus_read_block(AxlSmbus *s,
                     uint8_t   slave,
                     uint8_t   command,
                     uint8_t  *buf,
                     size_t   *len)
{
    if (s == NULL || buf == NULL || len == NULL) {
        return AXL_ERR;
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
    /* len == 0 isn't a legal SMBus block write (the count byte must
       be 1..32 per spec §5.5.7); reject it here so transports don't
       have to. This also stops a 1-op 2-byte I2C transaction from
       being misinterpreted as a Byte Write by transports that
       discriminate write shape on length. */
    if (s == NULL || buf == NULL || len == 0 || len > AXL_SMBUS_BLOCK_MAX) {
        return AXL_ERR;
    }
    return s->ops.write_block(s->ops.ctx, slave, command, buf, len);
}

int
axl_smbus_read_byte(AxlSmbus *s,
                    uint8_t   slave,
                    uint8_t   command,
                    uint8_t  *out)
{
    if (s == NULL || out == NULL) {
        return AXL_ERR;
    }
    return s->ops.read_byte(s->ops.ctx, slave, command, out);
}

int
axl_smbus_write_byte(AxlSmbus *s,
                     uint8_t   slave,
                     uint8_t   command,
                     uint8_t   value)
{
    if (s == NULL) {
        return AXL_ERR;
    }
    return s->ops.write_byte(s->ops.ctx, slave, command, value);
}

int
axl_smbus_quick(AxlSmbus *s,
                uint8_t   slave,
                bool      is_read)
{
    if (s == NULL || s->ops.quick == NULL) {
        return AXL_ERR;
    }
    return s->ops.quick(s->ops.ctx, slave, is_read);
}

int
axl_smbus_receive_byte(AxlSmbus *s,
                       uint8_t   slave,
                       uint8_t  *out)
{
    if (s == NULL || out == NULL || s->ops.receive_byte == NULL) {
        return AXL_ERR;
    }
    return s->ops.receive_byte(s->ops.ctx, slave, out);
}
