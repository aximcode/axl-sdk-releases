/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ata.c
    ATA/SATA device enumeration + task-file pass-thru, over
    EFI_ATA_PASS_THRU_PROTOCOL.

    A device is (controller, port, port-multiplier port). The set is built
    once with a two-level walk (LocateHandleBuffer -> GetNextPort ->
    GetNextDevice) and cached for the image lifetime; axl_ata_next returns
    opaque AxlAtaDev pointers into that cache. The typed readers (IDENTIFY,
    SMART, self-test) issue a command via ata_exec() and hand the buffer to
    the pure decoders in axl-ata-decode.c. ata_exec bounces data through an
    IoAlign-satisfying buffer.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_ATA_PASS_THRU_PROTOCOL (extra) */
#include <axl/axl-atexit.h>
#include <axl/axl-driver.h>  /* axl_driver_connect */
#include <axl/axl-mem.h>
#include <axl/axl-str.h>     /* axl_memcpy / axl_memset */
#include <axl/axl-ata.h>

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_SMART           0xB0
#define ATA_SMART_READ_DATA     0xD0
#define ATA_SMART_READ_THRESH   0xD1
#define ATA_SMART_EXEC_OFFLINE  0xD4
#define ATA_SMART_CYL_LOW       0x4F   /* SMART command signature */
#define ATA_SMART_CYL_HIGH      0xC2
#define ATA_STATUS_ERR          0x01   /* Status register ERR bit */
#define ATA_SECTOR_LEN          512u
#define ATA_TIMEOUT_NS          100000000ULL  /* 10 s in 100 ns units */

/* Backstop for the per-port device walk against a non-conforming
   controller that never reports EFI_NOT_FOUND. A SATA port multiplier
   tops out at 15 devices; this is generous headroom. */
#define ATA_MAX_DEVICES_PER_PORT  4096u

struct AxlAtaDev {
    EFI_ATA_PASS_THRU_PROTOCOL *p;
    uint16_t                    port;
    uint16_t                    pmport;
};

static struct AxlAtaDev *g_devs  = NULL;
static size_t            g_count = 0;
static bool              g_inited = false;

// ---------------------------------------------------------------------------
// Enumeration cache (two-level walk, image-lifetime)
// ---------------------------------------------------------------------------

static void
ata_cleanup(void *ctx)
{
    (void)ctx;
    axl_free(g_devs);
    g_devs = NULL;
    g_count = 0;
}

static void
ata_add(EFI_ATA_PASS_THRU_PROTOCOL *p, uint16_t port, uint16_t pmport)
{
    struct AxlAtaDev *grown =
        axl_realloc(g_devs, (g_count + 1) * sizeof(*g_devs));
    if (grown == NULL) {
        return;
    }
    g_devs = grown;
    g_devs[g_count].p = p;
    g_devs[g_count].port = port;
    g_devs[g_count].pmport = pmport;
    g_count++;
}

static void
ata_ensure(void)
{
    if (g_inited) {
        return;
    }
    g_inited = true;

    /* ATA pass-thru handles exist only once the AHCI/IDE controllers are
       bound. A normal boot connects them during BDS, but a minimally
       booted app may not have, so connect all controllers first (the
       AxlBlock/AxlNvme walks rely on firmware having done this; storage
       discovery makes it explicit). */
    axl_driver_connect(NULL);

    EFI_HANDLE *handles = NULL;
    size_t      n = 0;
    if (EFI_ERROR(axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
            ByProtocol, &gEfiAtaPassThruProtocolGuid, NULL, &n, &handles))
        || n == 0) {
        return;
    }

    for (size_t i = 0; i < n; i++) {
        EFI_ATA_PASS_THRU_PROTOCOL *p = NULL;
        if (EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3,
                handles[i], &gEfiAtaPassThruProtocolGuid, (void **)&p))
            || p == NULL) {
            continue;
        }
        uint16_t port = 0xFFFF;
        for (;;) {
            uint16_t prev_port = port;
            if (EFI_ERROR(axl_efi_call(p->GetNextPort, 2, p, &port))
                || port == prev_port) {
                break;   /* end, or a non-advancing controller */
            }
            /* Walk this port's devices. A directly-attached SATA device
               reports PortMultiplierPort 0xFFFF — the SAME value as the
               "first device" input sentinel — so we must NOT treat a
               returned 0xFFFF as "no advance" (that silently drops every
               direct disk). Termination is by EFI_NOT_FOUND: the firmware
               tracks per-port iteration state internally and reports it
               when the port is exhausted, exactly as EDK2's AtaBus
               enumeration relies on. The iteration cap backstops a
               non-conforming controller that never reports NOT_FOUND. */
            uint16_t pmp = 0xFFFF;
            for (unsigned guard = 0; guard < ATA_MAX_DEVICES_PER_PORT; guard++) {
                if (EFI_ERROR(axl_efi_call(p->GetNextDevice, 3, p, port, &pmp))) {
                    break;
                }
                ata_add(p, port, pmp);
            }
        }
    }

    axl_backend_free(handles);
    if (g_count > 0) {
        axl_atexit(ata_cleanup, NULL);
    }
}

AxlAtaDev *
axl_ata_next(AxlAtaDev *prev)
{
    ata_ensure();
    if (g_count == 0) {
        return NULL;
    }
    if (prev == NULL) {
        return &g_devs[0];
    }
    for (size_t i = 0; i < g_count; i++) {
        if (&g_devs[i] == prev) {
            return (i + 1 < g_count) ? &g_devs[i + 1] : NULL;
        }
    }
    return &g_devs[0];   /* foreign pointer: rewind */
}

int
axl_ata_get_address(const AxlAtaDev *dev, uint16_t *port, uint16_t *pmport)
{
    if (dev == NULL) {
        return AXL_ERR;
    }
    if (port != NULL) {
        *port = dev->port;
    }
    if (pmport != NULL) {
        *pmport = dev->pmport;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

/* Issue one task-file command. @p efi_proto is the EFI pass-thru protocol
   (PIO_DATA_IN / PIO_DATA_OUT / NON_DATA); the data direction is derived
   from it. Bounces @p data through an IoAlign-satisfying buffer. Fills
   @p asb (status block). Returns AXL_OK on transport success with the ATA
   ERR bit clear. */
static int
ata_exec(const AxlAtaDev *d, EFI_ATA_COMMAND_BLOCK *acb, uint8_t efi_proto,
         void *data, size_t data_len, EFI_ATA_STATUS_BLOCK *asb)
{
    EFI_ATA_PASS_THRU_PROTOCOL *p = d->p;
    bool data_in  = (efi_proto == EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN);
    bool data_out = (efi_proto == EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT);

    uint8_t *raw  = NULL;
    uint8_t *xfer = NULL;
    if (data_len > 0) {
        uint32_t align = (p->Mode != NULL && p->Mode->IoAlign > 1)
                             ? p->Mode->IoAlign : 1;
        raw = axl_malloc(data_len + align);
        if (raw == NULL) {
            return AXL_ERR;
        }
        xfer = raw;
        if (align > 1) {
            uintptr_t a = ((uintptr_t)raw + (align - 1))
                          & ~((uintptr_t)align - 1);
            xfer = (uint8_t *)a;
        }
        if (data_out) {
            axl_memcpy(xfer, data, data_len);
        } else {
            axl_memset(xfer, 0, data_len);
        }
    }

    EFI_ATA_PASS_THRU_COMMAND_PACKET pkt = { 0 };
    pkt.Asb      = asb;
    pkt.Acb      = acb;
    pkt.Timeout  = ATA_TIMEOUT_NS;
    pkt.Protocol = efi_proto;
    if (data_len > 0) {
        pkt.Length = EFI_ATA_PASS_THRU_LENGTH_BYTES;
        if (data_in) {
            pkt.InDataBuffer    = xfer;
            pkt.InTransferLength = (uint32_t)data_len;
        } else {
            pkt.OutDataBuffer    = xfer;
            pkt.OutTransferLength = (uint32_t)data_len;
        }
    } else {
        pkt.Length = EFI_ATA_PASS_THRU_LENGTH_NO_DATA_TRANSFER;
    }

    EFI_STATUS s = axl_efi_call(p->PassThru, 5, p, d->port, d->pmport,
                                &pkt, NULL);

    int rc = (!EFI_ERROR(s) && (asb->AtaStatus & ATA_STATUS_ERR) == 0)
                 ? AXL_OK : AXL_ERR;
    if (rc == AXL_OK && data_in && data_len > 0) {
        axl_memcpy(data, xfer, data_len);
    }
    axl_free(raw);
    return rc;
}

/* Fill an Acb for a SMART subcommand (feature) with the SMART signature. */
static void
ata_smart_acb(EFI_ATA_COMMAND_BLOCK *acb, uint8_t feature, uint8_t lba_low)
{
    axl_memset(acb, 0, sizeof(*acb));
    acb->AtaCommand      = ATA_CMD_SMART;
    acb->AtaFeatures     = feature;
    acb->AtaSectorNumber = lba_low;            /* subcommand for EXEC OFFLINE */
    acb->AtaCylinderLow  = ATA_SMART_CYL_LOW;
    acb->AtaCylinderHigh = ATA_SMART_CYL_HIGH;
    acb->AtaSectorCount  = 1;
}

// ---------------------------------------------------------------------------
// Typed readers
// ---------------------------------------------------------------------------

int
axl_ata_identify(AxlAtaDev *dev, AxlAtaIdentify *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    EFI_ATA_COMMAND_BLOCK acb = { 0 };
    EFI_ATA_STATUS_BLOCK  asb = { 0 };
    acb.AtaCommand     = ATA_CMD_IDENTIFY;
    acb.AtaSectorCount = 1;
    uint8_t buf[ATA_SECTOR_LEN];
    if (ata_exec(dev, &acb, EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN,
                 buf, sizeof(buf), &asb) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_ata_decode_identify(buf, sizeof(buf), out);
}

int
axl_ata_smart(AxlAtaDev *dev, AxlAtaSmart *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    EFI_ATA_COMMAND_BLOCK acb = { 0 };
    EFI_ATA_STATUS_BLOCK  asb = { 0 };
    uint8_t data[ATA_SECTOR_LEN];
    uint8_t thresh[ATA_SECTOR_LEN];

    ata_smart_acb(&acb, ATA_SMART_READ_DATA, 0);
    if (ata_exec(dev, &acb, EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN,
                 data, sizeof(data), &asb) != AXL_OK) {
        return AXL_ERR;
    }
    ata_smart_acb(&acb, ATA_SMART_READ_THRESH, 0);
    if (ata_exec(dev, &acb, EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN,
                 thresh, sizeof(thresh), &asb) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_ata_decode_smart(data, thresh, ATA_SECTOR_LEN, out);
}

int
axl_ata_self_test_start(AxlAtaDev *dev, AxlAtaSelfTest kind)
{
    if (dev == NULL) {
        return AXL_ERR;
    }
    uint8_t subcmd;
    switch (kind) {
    case AXL_ATA_SELF_TEST_ABORT:    subcmd = 0x7F; break;
    case AXL_ATA_SELF_TEST_SHORT:    subcmd = 0x01; break;
    case AXL_ATA_SELF_TEST_EXTENDED: subcmd = 0x02; break;
    default:                         return AXL_ERR;
    }
    EFI_ATA_COMMAND_BLOCK acb = { 0 };
    EFI_ATA_STATUS_BLOCK  asb = { 0 };
    ata_smart_acb(&acb, ATA_SMART_EXEC_OFFLINE, subcmd);
    return ata_exec(dev, &acb, EFI_ATA_PASS_THRU_PROTOCOL_ATA_NON_DATA,
                    NULL, 0, &asb);
}

int
axl_ata_self_test_result(AxlAtaDev *dev, AxlAtaSelfTestResult *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    EFI_ATA_COMMAND_BLOCK acb = { 0 };
    EFI_ATA_STATUS_BLOCK  asb = { 0 };
    uint8_t data[ATA_SECTOR_LEN];
    ata_smart_acb(&acb, ATA_SMART_READ_DATA, 0);
    if (ata_exec(dev, &acb, EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN,
                 data, sizeof(data), &asb) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_ata_decode_self_test(data, sizeof(data), out);
}

// ---------------------------------------------------------------------------
// Raw task-file pass-through
// ---------------------------------------------------------------------------

int
axl_ata_passthru(AxlAtaDev *dev, const AxlAtaCmd *cmd, AxlAtaProtocol proto,
                 void *data, size_t data_len, AxlAtaResult *out_result)
{
    if (dev == NULL || cmd == NULL
        || (data == NULL && data_len != 0)
        || (proto == AXL_ATA_NO_DATA && data_len != 0)) {
        return AXL_ERR;
    }
    uint8_t efi_proto;
    switch (proto) {
    case AXL_ATA_NO_DATA: efi_proto = EFI_ATA_PASS_THRU_PROTOCOL_ATA_NON_DATA; break;
    case AXL_ATA_PIO_IN:  efi_proto = EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN; break;
    case AXL_ATA_PIO_OUT: efi_proto = EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT; break;
    default:              return AXL_ERR;
    }

    EFI_ATA_COMMAND_BLOCK acb = { 0 };
    EFI_ATA_STATUS_BLOCK  asb = { 0 };
    acb.AtaCommand        = cmd->command;
    acb.AtaFeatures       = cmd->features;
    acb.AtaSectorCount    = cmd->sector_count;
    acb.AtaSectorNumber   = cmd->lba_low;
    acb.AtaCylinderLow    = cmd->lba_mid;
    acb.AtaCylinderHigh   = cmd->lba_high;
    acb.AtaDeviceHead     = cmd->device;
    acb.AtaFeaturesExp    = cmd->features_exp;
    acb.AtaSectorCountExp = cmd->sector_count_exp;
    acb.AtaSectorNumberExp = cmd->lba_low_exp;
    acb.AtaCylinderLowExp  = cmd->lba_mid_exp;
    acb.AtaCylinderHighExp = cmd->lba_high_exp;

    int rc = ata_exec(dev, &acb, efi_proto, data, data_len, &asb);

    if (out_result != NULL) {
        out_result->status       = asb.AtaStatus;
        out_result->error        = asb.AtaError;
        out_result->sector_count = asb.AtaSectorCount;
        out_result->lba_low      = asb.AtaSectorNumber;
        out_result->lba_mid      = asb.AtaCylinderLow;
        out_result->lba_high     = asb.AtaCylinderHigh;
        out_result->device       = asb.AtaDeviceHead;
    }
    return rc;
}
