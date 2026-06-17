/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-scsi.c
    SCSI/SAS logical-unit enumeration + CDB pass-thru, over
    EFI_EXT_SCSI_PASS_THRU_PROTOCOL.

    A device is (controller, 16-byte target, LUN). The set is built once with a
    GetNextTargetLun walk and cached for the image lifetime; axl_scsi_next
    returns opaque AxlScsiDev pointers into that cache. The typed readers
    (INQUIRY, READ CAPACITY, LOG SENSE health) issue a CDB via scsi_exec() and
    hand the buffer to the pure decoders in axl-scsi-decode.c. scsi_exec bounces
    data through an IoAlign-satisfying buffer.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_EXT_SCSI_PASS_THRU_PROTOCOL (extra) */
#include <axl/axl-atexit.h>
#include <axl/axl-driver.h>  /* axl_driver_connect */
#include <axl/axl-mem.h>
#include <axl/axl-str.h>     /* axl_memcpy / axl_memset */
#include <axl/axl-scsi.h>

#define SCSI_TIMEOUT_NS  100000000ULL  /* 10 s in 100 ns units */
#define SCSI_SENSE_LEN   64u

/* Backstop for the target/LUN walk against a non-conforming HBA that never
   reports EFI_NOT_FOUND (the GetNext* lesson from AxlAta: rely on NOT_FOUND
   for termination, never value-equality, but cap the loop). */
#define SCSI_MAX_DEVICES  4096u

struct AxlScsiDev {
    EFI_EXT_SCSI_PASS_THRU_PROTOCOL *p;
    uint8_t                          target[AXL_SCSI_TARGET_LEN];
    uint64_t                         lun;
};

static struct AxlScsiDev *g_devs   = NULL;
static size_t             g_count  = 0;
static bool               g_inited = false;

static int
scsi_exec(const AxlScsiDev *d, const uint8_t *cdb, size_t cdb_len,
          AxlScsiDir dir, void *data, size_t data_len,
          size_t *transferred, uint8_t *sense, size_t *sense_len);

/* True if a device is connected at (target, lun): a standard INQUIRY
   succeeds and reports peripheral qualifier 000b (byte 0, bits 7..5). */
static bool
scsi_lun_present(EFI_EXT_SCSI_PASS_THRU_PROTOCOL *p,
                 const uint8_t *target, uint64_t lun)
{
    struct AxlScsiDev probe = { .p = p, .lun = lun };
    axl_memcpy(probe.target, target, AXL_SCSI_TARGET_LEN);

    uint8_t std[36] = { 0 };
    uint8_t cdb[6]  = { 0x12, 0x00, 0x00, 0x00, (uint8_t)sizeof(std), 0x00 };
    return scsi_exec(&probe, cdb, sizeof(cdb), AXL_SCSI_DATA_IN,
                     std, sizeof(std), NULL, NULL, NULL) == AXL_OK
        && (std[0] >> 5) == 0;
}

// ---------------------------------------------------------------------------
// Enumeration cache (image-lifetime)
// ---------------------------------------------------------------------------

static void
scsi_cleanup(void *ctx)
{
    (void)ctx;
    axl_free(g_devs);
    g_devs = NULL;
    g_count = 0;
}

static void
scsi_add(EFI_EXT_SCSI_PASS_THRU_PROTOCOL *p, const uint8_t *target, uint64_t lun)
{
    struct AxlScsiDev *grown =
        axl_realloc(g_devs, (g_count + 1) * sizeof(*g_devs));
    if (grown == NULL) {
        return;
    }
    g_devs = grown;
    g_devs[g_count].p = p;
    axl_memcpy(g_devs[g_count].target, target, AXL_SCSI_TARGET_LEN);
    g_devs[g_count].lun = lun;
    g_count++;
}

static void
scsi_ensure(void)
{
    if (g_inited) {
        return;
    }
    g_inited = true;

    /* Ext SCSI pass-thru handles exist only once the SCSI HBA is bound. A
       normal boot connects controllers during BDS; a minimally-booted app may
       not have, so make it explicit (mirrors AxlAta / AxlNvme). */
    axl_driver_connect(NULL);

    EFI_HANDLE *handles = NULL;
    size_t      n = 0;
    if (EFI_ERROR(axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
            ByProtocol, &gEfiExtScsiPassThruProtocolGuid, NULL, &n, &handles))
        || n == 0) {
        return;
    }

    for (size_t i = 0; i < n; i++) {
        EFI_EXT_SCSI_PASS_THRU_PROTOCOL *p = NULL;
        if (EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3,
                handles[i], &gEfiExtScsiPassThruProtocolGuid, (void **)&p))
            || p == NULL) {
            continue;
        }

        /* Walk (target, LUN) pairs. The first call uses an all-0xFF target
           (the firmware's "give me the first" sentinel — a real target is
           never all-0xFF); termination is by EFI_NOT_FOUND, with a cap as the
           backstop. The firmware updates the 16-byte buffer in place. */
        uint8_t  target[AXL_SCSI_TARGET_LEN];
        axl_memset(target, 0xFF, sizeof(target));
        uint8_t *tp  = target;
        uint64_t lun = 0;
        for (unsigned guard = 0; guard < SCSI_MAX_DEVICES; guard++) {
            if (EFI_ERROR(axl_efi_call(p->GetNextTargetLun, 3, p, &tp, &lun))) {
                break;
            }
            /* GetNextTargetLun reports a LUN range that may include addresses
               with no device behind them (virtio-scsi, LSI). Probe with a
               standard INQUIRY and add only LUNs whose peripheral qualifier is
               000b (a device is connected) — otherwise the listing fills with
               phantom "not connected" units. */
            if (scsi_lun_present(p, tp, lun)) {
                scsi_add(p, tp, lun);
            }
        }
    }

    axl_backend_free(handles);
    if (g_count > 0) {
        axl_atexit(scsi_cleanup, NULL);
    }
}

AxlScsiDev *
axl_scsi_next(AxlScsiDev *prev)
{
    scsi_ensure();
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
axl_scsi_get_address(const AxlScsiDev *dev,
                     uint8_t target[AXL_SCSI_TARGET_LEN], uint64_t *lun)
{
    if (dev == NULL) {
        return AXL_ERR;
    }
    if (target != NULL) {
        axl_memcpy(target, dev->target, AXL_SCSI_TARGET_LEN);
    }
    if (lun != NULL) {
        *lun = dev->lun;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

/* Map an AxlScsiDir to the EFI data-direction constant (NO_DATA rides the
   READ direction with a zero-length transfer). */
static uint8_t
scsi_efi_dir(AxlScsiDir dir)
{
    return (dir == AXL_SCSI_DATA_OUT) ? EFI_EXT_SCSI_DATA_DIRECTION_WRITE
                                      : EFI_EXT_SCSI_DATA_DIRECTION_READ;
}

/* Issue one CDB. Bounces @p data through an IoAlign-satisfying buffer. Fills
   @p transferred / @p sense when non-NULL. Returns AXL_OK on adapter success
   with target GOOD status. */
static int
scsi_exec(const AxlScsiDev *d, const uint8_t *cdb, size_t cdb_len,
          AxlScsiDir dir, void *data, size_t data_len,
          size_t *transferred, uint8_t *sense, size_t *sense_len)
{
    EFI_EXT_SCSI_PASS_THRU_PROTOCOL *p = d->p;
    bool data_in  = (dir == AXL_SCSI_DATA_IN);
    bool data_out = (dir == AXL_SCSI_DATA_OUT);

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
            uintptr_t a = ((uintptr_t)raw + (align - 1)) & ~((uintptr_t)align - 1);
            xfer = (uint8_t *)a;
        }
        if (data_out) {
            axl_memcpy(xfer, data, data_len);
        } else {
            axl_memset(xfer, 0, data_len);
        }
    }

    uint8_t local_sense[SCSI_SENSE_LEN];
    axl_memset(local_sense, 0, sizeof(local_sense));

    EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET pkt = { 0 };
    pkt.Timeout         = SCSI_TIMEOUT_NS;
    pkt.Cdb             = (void *)cdb;          /* firmware reads it */
    pkt.CdbLength       = (uint8_t)cdb_len;
    pkt.DataDirection   = scsi_efi_dir(dir);
    pkt.SenseData       = local_sense;
    pkt.SenseDataLength = sizeof(local_sense);
    if (data_len > 0) {
        if (data_in) {
            pkt.InDataBuffer     = xfer;
            pkt.InTransferLength = (uint32_t)data_len;
        } else {
            pkt.OutDataBuffer     = xfer;
            pkt.OutTransferLength = (uint32_t)data_len;
        }
    }

    EFI_STATUS s = axl_efi_call(p->PassThru, 5, p,
                                (uint8_t *)d->target, d->lun, &pkt, NULL);

    if (transferred != NULL) {
        *transferred = data_in ? pkt.InTransferLength : pkt.OutTransferLength;
    }
    if (sense != NULL && sense_len != NULL) {
        size_t got = (pkt.TargetStatus
                          == EFI_EXT_SCSI_STATUS_TARGET_CHECK_CONDITION)
                         ? pkt.SenseDataLength : 0;
        if (got > *sense_len) {
            got = *sense_len;
        }
        if (got > 0) {
            axl_memcpy(sense, local_sense, got);
        }
        *sense_len = got;
    }

    int rc = (!EFI_ERROR(s)
              && pkt.HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK
              && pkt.TargetStatus == EFI_EXT_SCSI_STATUS_TARGET_GOOD)
                 ? AXL_OK : AXL_ERR;
    if (rc == AXL_OK && data_in && data_len > 0) {
        size_t got = pkt.InTransferLength;
        if (got > data_len) {
            got = data_len;
        }
        axl_memcpy(data, xfer, got);
    }
    axl_free(raw);
    return rc;
}

/* Big-endian store into a CDB field. */
static void
cdb_put_be(uint8_t *cdb, size_t off, uint64_t v, size_t nbytes)
{
    for (size_t i = 0; i < nbytes; i++) {
        cdb[off + nbytes - 1 - i] = (uint8_t)(v >> (8 * i));
    }
}

int
axl_scsi_passthru(AxlScsiDev *dev, const uint8_t *cdb, size_t cdb_len,
                  AxlScsiDir dir, void *data, size_t data_len,
                  size_t *data_transferred, uint8_t *sense, size_t *sense_len)
{
    if (dev == NULL || cdb == NULL || cdb_len == 0
        || (sense == NULL) != (sense_len == NULL)) {
        return AXL_ERR;
    }
    return scsi_exec(dev, cdb, cdb_len, dir, data, data_len,
                     data_transferred, sense, sense_len);
}

// ---------------------------------------------------------------------------
// Typed readers
// ---------------------------------------------------------------------------

int
axl_scsi_inquiry(AxlScsiDev *dev, AxlScsiInquiry *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    /* Standard INQUIRY. */
    uint8_t std[96] = { 0 };
    uint8_t cdb[6]  = { 0x12, 0x00, 0x00, 0x00, (uint8_t)sizeof(std), 0x00 };
    if (scsi_exec(dev, cdb, sizeof(cdb), AXL_SCSI_DATA_IN,
                  std, sizeof(std), NULL, NULL, NULL) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_scsi_decode_inquiry(std, sizeof(std), out) != AXL_OK) {
        return AXL_ERR;
    }

    /* Unit Serial Number VPD page (0x80) — best-effort. */
    uint8_t vpd[256]  = { 0 };
    uint8_t cdb80[6]  = { 0x12, 0x01, 0x80, 0x00, 0xFF, 0x00 };
    if (scsi_exec(dev, cdb80, sizeof(cdb80), AXL_SCSI_DATA_IN,
                  vpd, sizeof(vpd), NULL, NULL, NULL) == AXL_OK) {
        axl_scsi_decode_serial(vpd, sizeof(vpd), out->serial, sizeof(out->serial));
    }
    return AXL_OK;
}

int
axl_scsi_read_capacity(AxlScsiDev *dev, AxlScsiCapacity *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    uint8_t data[32] = { 0 };
    uint8_t cdb[16]  = { 0 };
    cdb[0] = 0x9E;            /* SERVICE ACTION IN (16) */
    cdb[1] = 0x10;           /* READ CAPACITY (16) */
    cdb_put_be(cdb, 10, sizeof(data), 4);   /* allocation length */
    if (scsi_exec(dev, cdb, sizeof(cdb), AXL_SCSI_DATA_IN,
                  data, sizeof(data), NULL, NULL, NULL) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_scsi_decode_capacity(data, sizeof(data), out);
}

/* LOG SENSE one page into @p buf. PC=1 (cumulative values). */
static int
scsi_log_sense(AxlScsiDev *dev, uint8_t page, uint8_t *buf, size_t buf_len)
{
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x4D;                  /* LOG SENSE */
    cdb[2] = (uint8_t)(0x40 | (page & 0x3F));   /* PC=1 | page code */
    cdb_put_be(cdb, 7, buf_len, 2); /* allocation length */
    return scsi_exec(dev, cdb, sizeof(cdb), AXL_SCSI_DATA_IN,
                     buf, buf_len, NULL, NULL, NULL);
}

int
axl_scsi_health(AxlScsiDev *dev, AxlScsiHealth *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));
    out->temperature_c = INT32_MIN;

    /* Informational Exceptions page (0x2F) — the pass/fail verdict. */
    uint8_t ie[64] = { 0 };
    if (scsi_log_sense(dev, 0x2F, ie, sizeof(ie)) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_scsi_decode_health(ie, sizeof(ie), out) != AXL_OK) {
        return AXL_ERR;
    }

    /* Temperature page (0x0D) — fallback when the IE page omitted it. */
    if (out->temperature_c == INT32_MIN) {
        uint8_t tp[32] = { 0 };
        if (scsi_log_sense(dev, 0x0D, tp, sizeof(tp)) == AXL_OK) {
            int32_t t = INT32_MIN;
            if (axl_scsi_decode_temperature(tp, sizeof(tp), &t) == AXL_OK) {
                out->temperature_c = t;
            }
        }
    }
    return AXL_OK;
}
