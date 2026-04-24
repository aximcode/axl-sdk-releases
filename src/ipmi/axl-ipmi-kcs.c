/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-kcs.c
    KCS (Keyboard Controller Style) IPMI transport.

    Classic polled FSM over two x86 I/O ports: data (default 0xCA2)
    and command/status (default 0xCA3). Ported from uefi-ipmitool
    (IpmiToolPkg/Library/IpmiTransportLib/IpmiKcs.c) with axl-backend
    I/O primitives in place of EDK2 IoLib.
**/

#include "axl-ipmi-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-runtime.h>

AXL_LOG_DOMAIN("ipmi-kcs");

// ---------------------------------------------------------------------------
// KCS register bits and states (IPMI 2.0, Section 9.7)
// ---------------------------------------------------------------------------

#define KCS_OBF             0x01  ///< Output buffer full
#define KCS_IBF             0x02  ///< Input buffer full
#define KCS_STATE_MASK      0xC0
#define KCS_STATE_IDLE      0x00
#define KCS_STATE_READ      0x40
#define KCS_STATE_WRITE     0x80
#define KCS_STATE_ERROR     0xC0

#define KCS_CTRL_WRITE_START  0x61
#define KCS_CTRL_WRITE_END    0x62
#define KCS_CTRL_READ         0x68
#define KCS_CTRL_GET_STATUS   0x60

#define KCS_POLL_INTERVAL_US  100
#define KCS_POLL_TIMEOUT_US   (5 * 1000 * 1000)  // 5 s

// ---------------------------------------------------------------------------
// Per-session state
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t  data_port;
    uint16_t  cmd_port;
} KcsCtx;

// ---------------------------------------------------------------------------
// Low-level register access
// ---------------------------------------------------------------------------

static int
kcs_read_status(KcsCtx *k, uint8_t *status)
{
    return axl_backend_io_read8(k->cmd_port, status);
}

static int
kcs_read_data(KcsCtx *k, uint8_t *data)
{
    return axl_backend_io_read8(k->data_port, data);
}

static int
kcs_write_data(KcsCtx *k, uint8_t data)
{
    return axl_backend_io_write8(k->data_port, data);
}

static int
kcs_write_cmd(KcsCtx *k, uint8_t cmd)
{
    return axl_backend_io_write8(k->cmd_port, cmd);
}

// ---------------------------------------------------------------------------
// Polling helpers
// ---------------------------------------------------------------------------

/**
 * Spin until IBF clears (firmware has consumed the byte we wrote).
 * Returns 0 on success, -1 on timeout or I/O error.
 */
static int
kcs_wait_ibf_clear(KcsCtx *k)
{
    uint8_t status;
    size_t  iters = 0;
    for (size_t elapsed = 0; elapsed < KCS_POLL_TIMEOUT_US;
         elapsed += KCS_POLL_INTERVAL_US)
    {
        if (kcs_read_status(k, &status) != 0) {
            return -1;
        }
        if ((status & KCS_IBF) == 0) {
            return 0;
        }
        /* 100 us cadence sits below firmware timer resolution
           (gBS timer events typically snap to 100us - 1ms), so a
           timer-based wait would add unbounded jitter here. Keep
           this as a busy-wait — the total budget is only 5 s and
           KCS traffic is infrequent. Yield every 100 iters (~10 ms)
           so Ctrl-C during a stuck-BMC 5 s poll is responsive. */
        axl_backend_stall(KCS_POLL_INTERVAL_US);
        if ((++iters % 100) == 0) {
            axl_yield();
        }
    }
    axl_error("KCS IBF-clear timeout");
    return -1;
}

/**
 * Spin until OBF is set (firmware has a byte ready for us) AND the
 * state is the one we expect. State mismatches return error immediately.
 */
static int
kcs_wait_obf_set(KcsCtx *k, uint8_t expected_state)
{
    uint8_t status;
    size_t  iters = 0;
    for (size_t elapsed = 0; elapsed < KCS_POLL_TIMEOUT_US;
         elapsed += KCS_POLL_INTERVAL_US)
    {
        if (kcs_read_status(k, &status) != 0) {
            return -1;
        }
        if ((status & KCS_STATE_MASK) == KCS_STATE_ERROR) {
            axl_warning("KCS error state (status=0x%02x)", status);
            return -1;
        }
        if ((status & KCS_STATE_MASK) != expected_state) {
            axl_warning("KCS state mismatch (got 0x%02x want 0x%02x)",
                        status & KCS_STATE_MASK, expected_state);
            return -1;
        }
        if (status & KCS_OBF) {
            return 0;
        }
        /* 100 us cadence: see note in kcs_wait_ibf_clear — too fine
           for firmware timer resolution, so busy-wait is correct.
           Yield every 100 iters (~10 ms) so Ctrl-C is observed
           during a stuck-BMC 5 s poll. */
        axl_backend_stall(KCS_POLL_INTERVAL_US);
        if ((++iters % 100) == 0) {
            axl_yield();
        }
    }
    axl_error("KCS OBF-set timeout");
    return -1;
}

/**
 * After we read a byte from the data port, the firmware expects us
 * to write KCS_CTRL_READ into the data port to prime the next byte.
 */
static int
kcs_read_byte(KcsCtx *k, uint8_t *byte)
{
    if (kcs_wait_obf_set(k, KCS_STATE_READ) != 0) {
        return -1;
    }
    if (kcs_read_data(k, byte) != 0) {
        return -1;
    }
    //
    // Prime the next read unless the firmware has moved us to IDLE
    // (which would mean "done, don't ask for more").
    //
    uint8_t status;
    if (kcs_read_status(k, &status) != 0) {
        return -1;
    }
    if ((status & KCS_STATE_MASK) == KCS_STATE_READ) {
        if (kcs_write_data(k, KCS_CTRL_READ) != 0) {
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Send-raw entry point (vtable method)
// ---------------------------------------------------------------------------

static int
kcs_send_raw(void *vctx,
             uint8_t netfn, uint8_t cmd,
             const uint8_t *req, size_t req_len,
             uint8_t *resp, size_t *resp_len)
{
    KcsCtx *k = (KcsCtx *)vctx;

    if (resp == NULL || resp_len == NULL || *resp_len < 1) {
        return -1;
    }

    //
    // Write phase, IPMI v2.0 §9.7: build the full payload
    // [netfn<<2, cmd, data...] and send it as
    //   WRITE_START, (len-1) bytes in WRITE state, WRITE_END, final byte.
    //
    // The byte that follows WRITE_END *is* the last data byte — the
    // BMC commits to processing the request once it sees that final
    // data write. Zero-data commands (Get Device ID, Chassis Status,
    // BMC Reset, ...) have payload_len == 2, so exactly one byte is
    // written in WRITE state and the cmd byte becomes the
    // post-WRITE_END final byte.
    //
    // Earlier revision of this code sent both netfn and cmd in WRITE
    // state and then a dummy trailing 0 after WRITE_END, which made
    // zero-data commands arrive as [netfn, cmd, 0x00] — a 3-byte
    // request that strict BMCs reject with CC 0xC7 "Request data
    // length invalid." Confirmed against uefi-ipmitool's
    // IpmiKcs.c:146-173 reference.
    //
    uint8_t  payload[34];                 // 2 header + 32 SMBus-limit data
    size_t   payload_len = 2 + req_len;
    if (payload_len > sizeof(payload)) {
        return -1;
    }
    payload[0] = (uint8_t)(netfn << 2);   // LUN 0
    payload[1] = cmd;
    for (size_t i = 0; i < req_len; i++) {
        payload[2 + i] = req[i];
    }

    if (kcs_wait_ibf_clear(k) != 0 ||
        kcs_write_cmd(k, KCS_CTRL_WRITE_START) != 0)
    {
        return -1;
    }

    //
    // Send (payload_len - 1) bytes in WRITE state. payload_len is
    // always >= 2 so the lower bound is 1 — no empty-loop case.
    //
    for (size_t i = 0; i + 1 < payload_len; i++) {
        if (kcs_wait_ibf_clear(k) != 0 ||
            kcs_write_data(k, payload[i]) != 0)
        {
            return -1;
        }
    }

    //
    // WRITE_END + final data byte.
    //
    if (kcs_wait_ibf_clear(k) != 0 ||
        kcs_write_cmd(k, KCS_CTRL_WRITE_END) != 0 ||
        kcs_wait_ibf_clear(k) != 0 ||
        kcs_write_data(k, payload[payload_len - 1]) != 0)
    {
        return -1;
    }

    //
    // Read phase: skip the two bytes of NetFn/Cmd echo, then copy
    // the rest (CompletionCode + response data) into the caller's
    // buffer until the state machine goes IDLE.
    //
    uint8_t discard;
    for (size_t i = 0; i < 2; i++) {
        if (kcs_read_byte(k, &discard) != 0) {
            return -1;
        }
    }

    size_t  cap = *resp_len;
    size_t  out = 0;
    uint8_t status = 0;

    while (out < cap) {
        uint8_t byte;
        if (kcs_read_byte(k, &byte) != 0) {
            return -1;
        }
        resp[out++] = byte;
        if (kcs_read_status(k, &status) != 0) {
            return -1;
        }
        if ((status & KCS_STATE_MASK) == KCS_STATE_IDLE) {
            break;
        }
    }

    if ((status & KCS_STATE_MASK) != KCS_STATE_IDLE) {
        axl_warning("KCS response overflow (cap=%zu)", cap);
        return -1;
    }

    *resp_len = out;
    return 0;
}

static void
kcs_close(void *vctx)
{
    axl_free(vctx);
}

// ---------------------------------------------------------------------------
// Public opener (called by auto-detect in axl-ipmi.c)
// ---------------------------------------------------------------------------

int
axl_ipmi_kcs_open(AxlIpmiTransportOps *ops,
                  uint16_t             data_port,
                  uint16_t             cmd_port)
{
    if (ops == NULL) {
        return -1;
    }

    //
    // Sanity-probe: one status read. Catches "port I/O not available"
    // on AArch64 without committing to the whole FSM.
    //
    uint8_t probe;
    if (axl_backend_io_read8(cmd_port, &probe) != 0) {
        return -1;
    }

    KcsCtx *k = axl_malloc(sizeof(KcsCtx));
    if (k == NULL) {
        return -1;
    }
    k->data_port = data_port;
    k->cmd_port  = cmd_port;

    ops->kind     = AXL_IPMI_TRANSPORT_KCS;
    ops->send_raw = kcs_send_raw;
    ops->close    = kcs_close;
    ops->ctx      = k;

    axl_info("IPMI KCS transport ready (data=0x%x, cmd=0x%x)",
             (unsigned)data_port, (unsigned)cmd_port);
    return 0;
}
