/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-nvme.c
    NVMe controller enumeration + admin pass-thru, over
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL.

    Enumeration mirrors axl-block (AxlHandleIter over the pass-thru GUID,
    cached for the image lifetime). The typed readers (Identify, SMART,
    Device Self-test) issue one admin command via nvme_exec() and hand the
    returned buffer to the pure decoders in axl-nvme-decode.c. nvme_exec
    bounces the data through an IoAlign-satisfying buffer so a caller's
    buffer need not be aligned.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL (extra) */
#include "../util/axl-handle-iter.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>     /* axl_memcpy / axl_memset */
#include <axl/axl-nvme.h>

#define NVME_LOG_SMART       0x02u
#define NVME_LOG_SELF_TEST   0x06u
#define NVME_ID_LEN          4096u
#define NVME_SMART_LEN       512u
#define NVME_SELF_TEST_LEN   564u
#define NVME_NSID_ALL        0xFFFFFFFFu

/* Get Log Page Cdw10: log id in bits 7:0, NUMDL (dwords, 0-based) in
   bits 27:16. @p len must be a positive multiple of 4 (all call sites pass
   a compile-time log-page size: 512 -> NUMD 127, 564 -> NUMD 140). */
#define NVME_LOG_CDW10(lid, len)  ((uint32_t)(lid) | ((((uint32_t)(len) / 4u) - 1u) << 16))

static AxlHandleIter nvme_iter = {
    .guid = &gEfiNvmExpressPassThruProtocolGuid,
    .what = "NVMe controller"
};

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

static EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *
nvme_proto(AxlHandle h)
{
    if (h == NULL) {
        return NULL;
    }
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = NULL;
    EFI_STATUS s = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)h, &gEfiNvmExpressPassThruProtocolGuid, (void **)&p);
    return EFI_ERROR(s) ? NULL : p;
}

/* Submit one admin command. Bounces @p data through an IoAlign-satisfying
   buffer (the pass-thru requires aligned transfer buffers); copies in
   before DATA_OUT, out after DATA_IN. Returns AXL_OK only when the
   transport succeeded and the NVMe Status Field is zero. */
static int
nvme_exec(EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p,
          const EFI_NVM_EXPRESS_COMMAND *cmd_in,
          AxlNvmeDataDir dir, void *data, size_t data_len,
          uint32_t *cqe_dw0, uint16_t *cqe_status)
{
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
        if (dir == AXL_NVME_DATA_OUT) {
            axl_memcpy(xfer, data, data_len);
        } else {
            axl_memset(xfer, 0, data_len);
        }
    }

    EFI_NVM_EXPRESS_COMMAND                  cmd = *cmd_in;
    EFI_NVM_EXPRESS_COMPLETION               cpl = { 0 };
    EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET pkt = { 0 };
    pkt.CommandTimeout = 50000000ULL;        /* 5 s in 100 ns units */
    pkt.TransferBuffer = (data_len > 0) ? xfer : NULL;
    pkt.TransferLength = (uint32_t)data_len;
    pkt.QueueType      = NVME_ADMIN_QUEUE;
    pkt.NvmeCmd        = &cmd;
    pkt.NvmeCompletion = &cpl;

    EFI_STATUS s = axl_efi_call(p->PassThru, 4, p, cmd.Nsid, &pkt, NULL);

    /* CQE Dword 3: Status Field is bits 31:17 (0 = success). */
    uint16_t sf = (uint16_t)((cpl.DW3 >> 17) & 0x7FFFu);
    if (cqe_status != NULL) {
        *cqe_status = sf;
    }
    if (cqe_dw0 != NULL) {
        *cqe_dw0 = cpl.DW0;
    }

    int rc = (!EFI_ERROR(s) && sf == 0) ? AXL_OK : AXL_ERR;
    if (rc == AXL_OK && data_len > 0 && dir == AXL_NVME_DATA_IN) {
        axl_memcpy(data, xfer, data_len);
    }
    axl_free(raw);
    return rc;
}

/* Read a fixed-length log page (DATA_IN) into @p out. */
static int
nvme_get_log(EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p, uint8_t lid,
             void *out, size_t len)
{
    EFI_NVM_EXPRESS_COMMAND cmd = { 0 };
    cmd.Cdw0.Opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.Nsid        = NVME_NSID_ALL;
    cmd.Cdw10       = NVME_LOG_CDW10(lid, len);
    cmd.Flags       = NVME_CDW10_VALID;
    return nvme_exec(p, &cmd, AXL_NVME_DATA_IN, out, len, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

AxlHandle
axl_nvme_next(AxlHandle prev)
{
    return axl_handle_iter_next(&nvme_iter, prev);
}

uint32_t
axl_nvme_namespace_next(AxlHandle ctrl, uint32_t prev_nsid)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL || p->GetNextNamespace == NULL) {
        return 0;
    }
    /* Spec: feed 0xFFFFFFFF to get the first id, then the prior id. */
    uint32_t id = (prev_nsid == 0) ? NVME_NSID_ALL : prev_nsid;
    EFI_STATUS s = axl_efi_call(p->GetNextNamespace, 2, p, &id);
    /* End on error, the all-ones sentinel, or a non-advancing result
       (a misbehaving controller that returns SUCCESS without moving the
       id would otherwise hang the caller's walk). */
    if (EFI_ERROR(s) || id == 0 || id == NVME_NSID_ALL || id == prev_nsid) {
        return 0;
    }
    return id;
}

// ---------------------------------------------------------------------------
// Identify
// ---------------------------------------------------------------------------

int
axl_nvme_identify_controller(AxlHandle ctrl, AxlNvmeController *out)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint8_t buf[NVME_ID_LEN];
    EFI_NVM_EXPRESS_COMMAND cmd = { 0 };
    cmd.Cdw0.Opcode = NVME_ADMIN_IDENTIFY_OPC;
    cmd.Nsid        = 0;
    cmd.Cdw10       = 1;   /* CNS 1 = Identify Controller */
    cmd.Flags       = NVME_CDW10_VALID;
    if (nvme_exec(p, &cmd, AXL_NVME_DATA_IN, buf, sizeof(buf), NULL, NULL)
            != AXL_OK) {
        return AXL_ERR;
    }
    return axl_nvme_decode_identify_controller(buf, sizeof(buf), out);
}

int
axl_nvme_identify_namespace(AxlHandle ctrl, uint32_t nsid,
                            AxlNvmeNamespace *out)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL || out == NULL || nsid == 0) {
        return AXL_ERR;
    }
    uint8_t buf[NVME_ID_LEN];
    EFI_NVM_EXPRESS_COMMAND cmd = { 0 };
    cmd.Cdw0.Opcode = NVME_ADMIN_IDENTIFY_OPC;
    cmd.Nsid        = nsid;
    cmd.Cdw10       = 0;   /* CNS 0 = Identify Namespace */
    cmd.Flags       = NVME_CDW10_VALID;
    if (nvme_exec(p, &cmd, AXL_NVME_DATA_IN, buf, sizeof(buf), NULL, NULL)
            != AXL_OK) {
        return AXL_ERR;
    }
    return axl_nvme_decode_identify_namespace(buf, sizeof(buf), nsid, out);
}

// ---------------------------------------------------------------------------
// SMART / Health
// ---------------------------------------------------------------------------

int
axl_nvme_smart(AxlHandle ctrl, AxlNvmeSmart *out)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint8_t buf[NVME_SMART_LEN];
    if (nvme_get_log(p, NVME_LOG_SMART, buf, sizeof(buf)) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_nvme_decode_smart(buf, sizeof(buf), out);
}

// ---------------------------------------------------------------------------
// Device Self-test
// ---------------------------------------------------------------------------

int
axl_nvme_self_test_start(AxlHandle ctrl, AxlNvmeSelfTest kind)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL) {
        return AXL_ERR;
    }
    uint32_t stc;
    switch (kind) {
    case AXL_NVME_SELF_TEST_ABORT:    stc = 0xF; break;
    case AXL_NVME_SELF_TEST_SHORT:    stc = 0x1; break;
    case AXL_NVME_SELF_TEST_EXTENDED: stc = 0x2; break;
    default:                          return AXL_ERR;
    }
    EFI_NVM_EXPRESS_COMMAND cmd = { 0 };
    cmd.Cdw0.Opcode = NVME_ADMIN_DEVICE_SELF_TEST;
    cmd.Nsid        = NVME_NSID_ALL;
    cmd.Cdw10       = stc;
    cmd.Flags       = NVME_CDW10_VALID;
    return nvme_exec(p, &cmd, AXL_NVME_NO_DATA, NULL, 0, NULL, NULL);
}

int
axl_nvme_self_test_result(AxlHandle ctrl, AxlNvmeSelfTestResult *out)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint8_t buf[NVME_SELF_TEST_LEN];
    if (nvme_get_log(p, NVME_LOG_SELF_TEST, buf, sizeof(buf)) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_nvme_decode_self_test_log(buf, sizeof(buf), out);
}

// ---------------------------------------------------------------------------
// Raw admin pass-through
// ---------------------------------------------------------------------------

int
axl_nvme_admin_passthru(AxlHandle ctrl, const AxlNvmeAdminCmd *c,
                        AxlNvmeDataDir dir, void *data, size_t data_len,
                        uint32_t *cqe_dw0, uint16_t *cqe_status)
{
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *p = nvme_proto(ctrl);
    if (p == NULL || c == NULL
        || (data == NULL && data_len != 0)
        || (dir == AXL_NVME_NO_DATA && data_len != 0)) {
        return AXL_ERR;
    }
    EFI_NVM_EXPRESS_COMMAND cmd = { 0 };
    cmd.Cdw0.Opcode = c->opcode;
    cmd.Nsid        = c->nsid;
    cmd.Cdw10       = c->cdw10;
    cmd.Cdw11       = c->cdw11;
    cmd.Cdw12       = c->cdw12;
    cmd.Cdw13       = c->cdw13;
    cmd.Cdw14       = c->cdw14;
    cmd.Cdw15       = c->cdw15;
    /* Program all caller-supplied command Dwords (10..15). */
    cmd.Flags = NVME_CDW10_VALID | NVME_CDW11_VALID | NVME_CDW12_VALID
              | NVME_CDW13_VALID | NVME_CDW14_VALID | NVME_CDW15_VALID;
    return nvme_exec(p, &cmd, dir, data, data_len, cqe_dw0, cqe_status);
}
