/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi.c
    AxlIpmi — core dispatcher + session lifecycle + transport
    auto-detection.

    Phase 2 scope: KCS transport only (SMBIOS Type 38 auto-detect,
    with an x86 default fallback). SSIF + EDKII + Dell land in
    later phases and plug into the same transport vtable.
**/

#include "axl-ipmi-internal.h"

#include <axl/axl-log.h>
#include <axl/axl-smbios.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("ipmi");

// ---------------------------------------------------------------------------
// Protocol GUIDs — IPMI-adjacent, for axl_ipmi_probe()
// ---------------------------------------------------------------------------

//
// These are the same GUIDs uefi-ipmitool's `probe` subcommand walks.
// We deliberately probe some that AxlIpmi does not itself use as a
// transport (AMI variants, Intel, Mu) because the point of probe is
// to diagnose an unknown platform — knowing that AMI's SMM IPMI
// protocol is present but we haven't wired it up yet is useful
// information.
//
static const EFI_GUID ami_dxe_ipmi_guid = {
    0x4a1d0e66, 0x5271, 0x4e22,
    {0x83, 0xfe, 0x90, 0x92, 0x1b, 0x74, 0x82, 0x13}
};
static const EFI_GUID ami_smm_ipmi_guid = {
    0x1dbd1503, 0x0a60, 0x4230,
    {0xaa, 0xa3, 0x80, 0x16, 0xd8, 0xc3, 0xde, 0x2f}
};
static const EFI_GUID intel_sm_ipmi_guid = {
    0xdbbb21ce, 0xd92e, 0x4add,
    {0xb4, 0xc0, 0xc4, 0x62, 0xd4, 0xb0, 0x96, 0x02}
};
static const EFI_GUID mu_ipmi_transport2_guid = {
    0x40e28370, 0x7247, 0x4bce,
    {0x94, 0x18, 0x77, 0x7f, 0xd5, 0x11, 0x12, 0xf0}
};

static bool
probe_has_protocol(const EFI_GUID *guid)
{
    VOID *iface = NULL;
    EFI_GUID local = *guid;
    EFI_STATUS s = gBS->LocateProtocol(&local, NULL, &iface);
    return !EFI_ERROR(s) && iface != NULL;
}

// ---------------------------------------------------------------------------
// SMBIOS Type 38 interface-type constants (IPMI spec Table 38-1)
// ---------------------------------------------------------------------------

#define IPMI_SMBIOS_IFACE_UNKNOWN  0
#define IPMI_SMBIOS_IFACE_KCS      1
#define IPMI_SMBIOS_IFACE_SMIC     2
#define IPMI_SMBIOS_IFACE_BT       3
#define IPMI_SMBIOS_IFACE_SSIF     4

// Default KCS I/O ports when SMBIOS Type 38 is absent on x86 hosts.
#define KCS_DEFAULT_DATA_PORT  0x0CA2
#define KCS_DEFAULT_CMD_PORT   0x0CA3

// ---------------------------------------------------------------------------
// Auto-detect: walk SMBIOS Type 38, fall back to x86 default KCS
// ---------------------------------------------------------------------------

/**
 * Interpret SMBIOS Type 38's BaseAddress per IPMI spec:
 *   bit 0: 0 = memory-mapped, 1 = I/O-mapped
 *   bits 1..: the address itself (bit 0 cleared for KCS/SMIC/BT)
 *
 * For KCS we return the command-port address (data port is
 * cmd_port - 1 on x86 systems per the IPMI v2.0 spec 9.5).
 */
static void
decode_kcs_ports(uint64_t base_addr,
                 uint16_t *out_data,
                 uint16_t *out_cmd)
{
    uint64_t io_addr = base_addr & ~1ULL;
    //
    // SMBIOS Type 38 stores the *data* port for KCS. The command/
    // status port is data + 1 on standard platforms.
    //
    *out_data = (uint16_t)io_addr;
    *out_cmd  = (uint16_t)(io_addr + 1);
}

//
// SMBIOS Type 38 declared length per the IPMI spec (Table 38-1).
// Older firmware might publish short records; validate before reading.
//
#define SMBIOS_TYPE38_MIN_LEN  0x10

static int
try_smbios_detect(AxlIpmiTransportOps *ops)
{
    //
    // SMBIOS_TABLE_TYPE38 lives in include/uefi/axl-uefi-extra.h
    // (DMTF SMBIOS spec, not UEFI/PI), so we can't pull it from the
    // AxlSmbios public header. Walk via axl_smbios_find() and cast
    // the header pointer to the known layout.
    //
    AxlSmbiosHeader *hdr = axl_smbios_find(38);
    if (hdr == NULL) {
        return -1;
    }
    if (hdr->Length < SMBIOS_TYPE38_MIN_LEN) {
        axl_warning("SMBIOS Type 38 length too short: %u (need >= %u)",
                    (unsigned)hdr->Length, SMBIOS_TYPE38_MIN_LEN);
        return -1;
    }

    //
    // SMBIOS_TABLE_TYPE38 layout (see axl-uefi-extra.h):
    //   +0 standard 4-byte header (type, length, handle)
    //   +4 InterfaceType
    //   +5 IPMISpecificationRevision
    //   +6 I2CSlaveAddress
    //   +7 NVStorageDeviceAddress
    //   +8 BaseAddress (u64, unaligned)
    //  +16 BaseAddressModifier
    //  +17 InterruptNumber
    //
    // Access via byte pointer + memcpy to dodge the unaligned u64
    // read — SMBIOS structs use #pragma pack(1).
    //
    const uint8_t *raw = (const uint8_t *)hdr;
    uint8_t  iface = raw[4];
    uint64_t base;
    axl_memcpy(&base, &raw[8], sizeof(base));

    switch (iface) {
    case IPMI_SMBIOS_IFACE_KCS: {
        uint16_t data_port, cmd_port;
        decode_kcs_ports(base, &data_port, &cmd_port);
        axl_info("SMBIOS Type 38: KCS @ 0x%x/0x%x",
                 (unsigned)data_port, (unsigned)cmd_port);
        return axl_ipmi_kcs_open(ops, data_port, cmd_port);
    }
    case IPMI_SMBIOS_IFACE_SSIF: {
        //
        // SMBIOS Type 38 I2CSlaveAddress is the 8-bit wire address
        // (bit 0 = R/W). Shift out the R/W bit to get the 7-bit
        // device address SMBus/I2C APIs expect.
        //
        uint8_t slave = (uint8_t)(raw[6] >> 1);
        axl_info("SMBIOS Type 38: SSIF (slave=0x%02x)", (unsigned)slave);

        AxlSmbus *smbus = axl_smbus_new();
        if (smbus == NULL) {
            axl_warning("SSIF: no SMBus controller — cannot reach BMC");
            return -1;
        }
        int rc = axl_ipmi_ssif_open(ops, smbus, slave);
        if (rc != 0) {
            axl_smbus_free(smbus);
        }
        return rc;
    }
    case IPMI_SMBIOS_IFACE_BT:
        axl_info("SMBIOS Type 38: BT interface — not supported");
        return -1;
    case IPMI_SMBIOS_IFACE_SMIC:
        axl_info("SMBIOS Type 38: SMIC interface — not supported");
        return -1;
    default:
        axl_warning("SMBIOS Type 38: unknown interface type %u",
                    (unsigned)iface);
        return -1;
    }
}

static int
try_default_kcs(AxlIpmiTransportOps *ops)
{
    axl_debug("SMBIOS Type 38 absent; trying default KCS 0x%x/0x%x",
              KCS_DEFAULT_DATA_PORT, KCS_DEFAULT_CMD_PORT);
    return axl_ipmi_kcs_open(ops,
                             KCS_DEFAULT_DATA_PORT,
                             KCS_DEFAULT_CMD_PORT);
}

// ---------------------------------------------------------------------------
// Session lifecycle (public API)
// ---------------------------------------------------------------------------

AxlIpmiSession *
axl_ipmi_session_new(void)
{
    AXL_AUTOPTR(AxlIpmiSession) s = axl_calloc(1, sizeof(AxlIpmiSession));
    if (s == NULL) {
        return NULL;
    }

    //
    // Auto-detect priority:
    //   1. EDKII IPMI_PROTOCOL — firmware-mediated, preferred when
    //      available because it handles platform quirks and reaches
    //      BMCs we can't always address directly (e.g., no port I/O
    //      access from this execution environment).
    //   2. Dell EFI_IPMI_TRANSPORT — vendor-specific; nearly as good
    //      as EDKII on Dell hardware.
    //   3. SMBIOS Type 38 — pick KCS or SSIF based on InterfaceType.
    //   4. Last-resort x86 default KCS ports (0x0CA2 / 0x0CA3).
    //
    if (axl_ipmi_edkii_open(&s->ops) != 0 &&
        axl_ipmi_dell_open(&s->ops)  != 0 &&
        try_smbios_detect(&s->ops)   != 0 &&
        try_default_kcs(&s->ops)     != 0)
    {
        axl_warning("No IPMI transport available");
        return NULL;
    }

    return axl_steal_pointer(&s);
}

void
axl_ipmi_session_free(AxlIpmiSession *session)
{
    if (session == NULL) {
        return;
    }
    if (session->ops.close != NULL) {
        session->ops.close(session->ops.ctx);
    }
    axl_free(session);
}

AxlIpmiTransport
axl_ipmi_session_transport(const AxlIpmiSession *session)
{
    if (session == NULL) {
        return AXL_IPMI_TRANSPORT_UNKNOWN;
    }
    return session->ops.kind;
}

// ---------------------------------------------------------------------------
// Caller-provided transport (testing + pluggable transports)
// ---------------------------------------------------------------------------

typedef struct {
    AxlIpmiSendRaw  send_raw;
    void           *user_data;
} CallbackCtx;

static int
callback_send_raw(void *vctx,
                  uint8_t netfn, uint8_t cmd,
                  const uint8_t *req, size_t req_len,
                  uint8_t *resp, size_t *resp_len)
{
    CallbackCtx *c = (CallbackCtx *)vctx;
    return c->send_raw(c->user_data, netfn, cmd,
                       req, req_len, resp, resp_len);
}

static void
callback_close(void *vctx)
{
    axl_free(vctx);
}

AxlIpmiSession *
axl_ipmi_session_new_with_callback(AxlIpmiTransport kind,
                                   AxlIpmiSendRaw send_raw,
                                   void *user_data)
{
    if (send_raw == NULL) {
        return NULL;
    }
    AXL_AUTOPTR(AxlIpmiSession) s = axl_calloc(1, sizeof(AxlIpmiSession));
    if (s == NULL) {
        return NULL;
    }
    CallbackCtx *c = axl_malloc(sizeof(CallbackCtx));
    if (c == NULL) {
        return NULL;
    }
    c->send_raw  = send_raw;
    c->user_data = user_data;

    s->ops.kind     = kind;
    s->ops.send_raw = callback_send_raw;
    s->ops.close    = callback_close;
    s->ops.ctx      = c;
    return axl_steal_pointer(&s);
}

int
axl_ipmi_raw(AxlIpmiSession *session,
             uint8_t netfn, uint8_t cmd,
             const uint8_t *req, size_t req_len,
             uint8_t *resp, size_t *resp_len)
{
    if (session == NULL || resp == NULL || resp_len == NULL) {
        return -1;
    }
    if (req == NULL && req_len != 0) {
        return -1;
    }
    if (session->ops.send_raw == NULL) {
        return -1;
    }
    return session->ops.send_raw(session->ops.ctx,
                                 netfn, cmd,
                                 req, req_len,
                                 resp, resp_len);
}

// ---------------------------------------------------------------------------
// Probe — diagnostic snapshot
// ---------------------------------------------------------------------------

int
axl_ipmi_probe(AxlIpmiProbe *out)
{
    if (out == NULL) {
        return -1;
    }
    //
    // Zero-fill first — every field has a meaningful "not found"
    // value so the caller doesn't need to track which probe steps
    // succeeded.
    //
    for (size_t i = 0; i < sizeof(*out); i++) {
        ((uint8_t *)out)[i] = 0;
    }

    out->edkii_ipmi_protocol     = probe_has_protocol(&gIpmiProtocolGuid);
    out->dell_ipmi_transport     = probe_has_protocol(&gDellIpmiProtocolGuid);
    out->ami_dxe_ipmi_transport  = probe_has_protocol(&ami_dxe_ipmi_guid);
    out->ami_smm_ipmi_transport  = probe_has_protocol(&ami_smm_ipmi_guid);
    out->intel_sm_ipmi_transport = probe_has_protocol(&intel_sm_ipmi_guid);
    out->mu_ipmi_transport2      = probe_has_protocol(&mu_ipmi_transport2_guid);
    out->smbus_hc_protocol       = probe_has_protocol(&gEfiSmbusHcProtocolGuid);

    //
    // I2C Master: LocateProtocol only catches one instance. Also
    // count matching handles so multi-bus platforms show up.
    //
    EFI_GUID i2c_guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    UINTN    handle_count = 0;
    EFI_HANDLE *handle_buf = NULL;
    EFI_STATUS s = gBS->LocateHandleBuffer(
        ByProtocol, &i2c_guid, NULL, &handle_count, &handle_buf);
    if (!EFI_ERROR(s)) {
        out->i2c_master_protocol = (handle_count > 0);
        out->i2c_master_handle_count = (size_t)handle_count;
        if (handle_buf != NULL) {
            gBS->FreePool(handle_buf);
        }
    }

    //
    // SMBIOS Type 38 — reuse the same byte-level walk as the auto-
    // detect path so we don't duplicate struct-offset knowledge.
    //
    AxlSmbiosHeader *hdr = axl_smbios_find(38);
    if (hdr != NULL && hdr->Length >= SMBIOS_TYPE38_MIN_LEN) {
        const uint8_t *raw = (const uint8_t *)hdr;
        out->smbios_type38_present  = true;
        out->smbios_interface_type  = raw[4];
        out->smbios_i2c_slave       = raw[6];
        axl_memcpy(&out->smbios_base_address, &raw[8], sizeof(uint64_t));
    }

    return 0;
}
