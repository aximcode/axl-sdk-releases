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
                 uint8_t  base_addr_modifier,
                 uint16_t *out_data,
                 uint16_t *out_cmd)
{
    uint64_t io_addr = base_addr & ~1ULL;
    //
    // SMBIOS Type 38 stores the *data* port for KCS. The cmd/status
    // port is data + register_spacing. The spacing is encoded in
    // BaseAddressModifier bits 7..6 per the SMBIOS spec (DSP0134
    // 3.3.39.5) — verified against dmidecode source
    // (dmi_ipmi_register_spacing reads `(data[0x10] >> 6) & 0x03`)
    // and Linux's `ipmi_si_intf.c`:
    //   0b00 → 1-byte stride (cmd = data + 1) — most x86 systems
    //   0b01 → 4-byte stride (cmd = data + 4) — DWord-mapped
    //                                            (some OEM BMCs
    //                                            use this:
    //                                            modifier=0x4A,
    //                                            bits 7..6 = 01b)
    //   0b10 → 16-byte stride (cmd = data + 16)
    //   0b11 → reserved; treat as 1-byte fallback
    //
    static const uint8_t reg_spacings[4] = { 1, 4, 16, 1 };
    uint8_t spacing = reg_spacings[(base_addr_modifier >> 6) & 0x03];

    *out_data = (uint16_t)io_addr;
    *out_cmd  = (uint16_t)(io_addr + spacing);
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
        //
        // Try the spec-encoded spacing first; if that cmd port reads
        // 0xFF (unmapped), retry with the other two valid spacings.
        // Some platforms (observed on some OEM BMC firmware) publish a modifier
        // that doesn't match the actual hardware layout. Linux's
        // ipmi_si_intf does similar probing fallbacks.
        //
        uint16_t data_port, cmd_port;
        const uint8_t  spec_spacings[3] = { 1, 4, 16 };
        const uint8_t  spec_modifier   = raw[16];
        const uint8_t  primary_idx     = spec_modifier & 0x03;

        decode_kcs_ports(base, spec_modifier, &data_port, &cmd_port);
        axl_info("SMBIOS Type 38: KCS @ 0x%x/0x%x (modifier=0x%02x)",
                 (unsigned)data_port, (unsigned)cmd_port,
                 (unsigned)spec_modifier);
        if (axl_ipmi_kcs_open(ops, data_port, cmd_port) == 0) {
            return 0;
        }
        for (size_t i = 0; i < 3; i++) {
            if (i == primary_idx) {
                continue;       /* already tried */
            }
            uint8_t spacing = spec_spacings[i];
            uint16_t alt_data = (uint16_t)(base & ~1ULL);
            uint16_t alt_cmd  = (uint16_t)(alt_data + spacing);
            axl_info("SMBIOS Type 38: KCS retry @ 0x%x/0x%x "
                     "(spacing=%u, modifier mismatch fallback)",
                     (unsigned)alt_data, (unsigned)alt_cmd,
                     (unsigned)spacing);
            if (axl_ipmi_kcs_open(ops, alt_data, alt_cmd) == 0) {
                return 0;
            }
        }
        //
        // Last-resort: try swapping data/cmd ports. Some OEM
        // platforms appear to publish the cmd port as Type 38
        // BaseAddress (instead of the data port that Linux
        // ipmi_si and the IPMI spec assume).
        //
        for (size_t i = 0; i < 3; i++) {
            uint8_t spacing = spec_spacings[i];
            uint16_t alt_cmd  = (uint16_t)(base & ~1ULL);
            uint16_t alt_data = (uint16_t)(alt_cmd + spacing);
            axl_info("SMBIOS Type 38: KCS swapped retry @ 0x%x/0x%x "
                     "(spacing=%u, base-is-cmd fallback)",
                     (unsigned)alt_data, (unsigned)alt_cmd,
                     (unsigned)spacing);
            if (axl_ipmi_kcs_open(ops, alt_data, alt_cmd) == 0) {
                return 0;
            }
        }
        return -1;
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
    return axl_ipmi_session_new_with_transport(AXL_IPMI_TRANSPORT_UNKNOWN);
}

AxlIpmiSession *
axl_ipmi_session_new_with_transport(AxlIpmiTransport hint)
{
    AXL_AUTOPTR(AxlIpmiSession) s = axl_calloc(1, sizeof(AxlIpmiSession));
    if (s == NULL) {
        return NULL;
    }

    int rc = -1;
    switch (hint) {
    case AXL_IPMI_TRANSPORT_EDKII:
        rc = axl_ipmi_edkii_open(&s->ops);
        break;
    case AXL_IPMI_TRANSPORT_DELL:
        rc = axl_ipmi_dell_open(&s->ops);
        break;
    case AXL_IPMI_TRANSPORT_KCS:
        //
        // Prefer SMBIOS-Type-38-detected ports — some OEM platforms advertise
        // 0xCA8/0xCA9 which differs from the x86 default. The linux kernel
        // ipmi_si driver uses these same SMBIOS ports successfully on
        // that BMC firmware, where the vendor EFI protocol misbehaves. If
        // Type 38 says SSIF instead, refuse rather than silently
        // downgrading — caller asked for KCS specifically.
        //
        rc = try_smbios_detect(&s->ops);
        if (rc == 0 && s->ops.kind != AXL_IPMI_TRANSPORT_KCS) {
            if (s->ops.close != NULL) {
                s->ops.close(s->ops.ctx);
            }
            rc = -1;
        }
        if (rc != 0) {
            rc = try_default_kcs(&s->ops);
        }
        break;
    case AXL_IPMI_TRANSPORT_SSIF:
        rc = try_smbios_detect(&s->ops);
        if (rc == 0 && s->ops.kind != AXL_IPMI_TRANSPORT_SSIF) {
            //
            // SMBIOS Type 38 advertised KCS, not SSIF. Caller asked for
            // SSIF specifically; refuse rather than silently downgrading.
            //
            if (s->ops.close != NULL) {
                s->ops.close(s->ops.ctx);
            }
            rc = -1;
        }
        break;
    case AXL_IPMI_TRANSPORT_UNKNOWN:
    default:
        //
        // Auto-detect priority (IPMI spec puts SMBIOS Type 38 first;
        // vendor protocols are convenience layers on top, used only
        // as fallback). Real-world reason this order matters: the vendor
        // EFI_IPMI_TRANSPORT on some firmware advertises itself but Get
        // Device ID returns garbage, so trusting it over the spec
        // path silently broke ipmi info / mc info / sel list etc.
        //
        //   1. SMBIOS Type 38 — KCS or SSIF per InterfaceType.
        //      The Linux kernel ipmi_si driver also keys off this.
        //   2. EDKII IPMI_PROTOCOL — firmware-mediated fallback.
        //   3. Dell EFI_IPMI_TRANSPORT — vendor fallback.
        //   4. Last-resort x86 default KCS ports (0x0CA2 / 0x0CA3).
        //
        // Override with axl_ipmi_session_new_with_transport(hint).
        //
        if (try_smbios_detect(&s->ops)   != 0 &&
            axl_ipmi_edkii_open(&s->ops) != 0 &&
            axl_ipmi_dell_open(&s->ops)  != 0 &&
            try_default_kcs(&s->ops)     != 0)
        {
            rc = -1;
        } else {
            rc = 0;
        }
        break;
    }

    if (rc != 0) {
        axl_warning("No IPMI transport available (hint=%d)", (int)hint);
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
        return AXL_ERR;
    }
    if (req == NULL && req_len != 0) {
        return AXL_ERR;
    }
    if (session->ops.send_raw == NULL) {
        return AXL_ERR;
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
        return AXL_ERR;
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
    // SMBus HC: LocateProtocol only returns one instance, but some OEM
    // platforms typically publish multiple HCs (one per bus segment
    // — DIMMs are usually on a different segment than NIC EEPROMs
    // etc.). Count handles so multi-segment platforms surface here,
    // and so memspd can pick the right HC for SPD scanning.
    //
    EFI_GUID hc_guid = gEfiSmbusHcProtocolGuid;
    UINTN    hc_count = 0;
    EFI_HANDLE *hc_buf = NULL;
    EFI_STATUS hc_st = gBS->LocateHandleBuffer(
        ByProtocol, &hc_guid, NULL, &hc_count, &hc_buf);
    if (!EFI_ERROR(hc_st)) {
        out->smbus_hc_handle_count = (size_t)hc_count;
        if (hc_buf != NULL) {
            gBS->FreePool(hc_buf);
        }
    }

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

    return AXL_OK;
}
