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
#include "../runtime/axl-signal-internal.h"

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
            _axl_poll_break();
        }
    }
    axl_error("KCS IBF-clear timeout");
    return -1;
}

// An earlier response-read implementation lived here — a single-byte
// read+prime helper (kcs_read_byte) and its OBF-set spin-wait
// (kcs_wait_obf_set). Both were superseded by the inline read loop in
// kcs_recv below (which handles the last-byte-arrives-in-IDLE corner
// case), left unused by that refactor, and removed as dead code.

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

    //
    // Pre-flight: ensure BMC is in IDLE before starting a new
    // transaction. the BMC observed during testing was wedging in ERROR
    // state mid-test; without this, every subsequent command fails
    // with "BMC not idle". Linux's ipmi_si state machine does the
    // equivalent check + abort recovery on every entry.
    //
    {
        uint8_t status_initial;
        if (kcs_read_status(k, &status_initial) != 0) {
            return -1;
        }
        if ((status_initial & KCS_STATE_MASK) != KCS_STATE_IDLE
            || (status_initial & KCS_OBF))
        {
            axl_debug("KCS pre-flight: BMC not idle (status=0x%02x), "
                      "issuing abort", status_initial);
            //
            // IPMI spec §9.10 abort: write Get_Status (0x60) to cmd,
            // wait IBF clear, write 0x00 to data; BMC responds with
            // status byte then enters IDLE.
            //
            kcs_wait_ibf_clear(k);
            if (kcs_write_cmd(k, KCS_CTRL_GET_STATUS) != 0) {
                return -1;
            }
            kcs_wait_ibf_clear(k);
            /* Drain any stale OBF before issuing the abort data byte. */
            uint8_t s_after_cmd;
            if (kcs_read_status(k, &s_after_cmd) == 0
                && (s_after_cmd & KCS_OBF))
            {
                uint8_t drop;
                kcs_read_data(k, &drop);
            }
            if (kcs_write_data(k, 0x00) != 0) {
                return -1;
            }
            /* Drain the abort response (1 byte status + transition to IDLE). */
            for (int i = 0; i < 4; i++) {
                uint8_t s_drain;
                if (kcs_wait_ibf_clear(k) != 0
                    || kcs_read_status(k, &s_drain) != 0)
                {
                    break;
                }
                if (s_drain & KCS_OBF) {
                    uint8_t drop;
                    kcs_read_data(k, &drop);
                    if ((s_drain & KCS_STATE_MASK) == KCS_STATE_READ) {
                        kcs_write_data(k, KCS_CTRL_READ);
                    }
                }
                if ((s_drain & KCS_STATE_MASK) == KCS_STATE_IDLE) {
                    break;
                }
            }
        }
    }

    //
    // Linux's ipmi_kcs_sm.c calls clear_obf() — drain stale data
    // byte if OBF=1 — at every state transition. that BMC sets OBF
    // during the WRITE_START echo phase; without draining we leave
    // BMC with OBF=1 while we drive the next write, which BMC
    // treats as a protocol violation and transitions to ERROR.
    //
    #define KCS_CLEAR_OBF_IF_SET()                                   \
        do {                                                          \
            uint8_t _s;                                               \
            if (kcs_read_status(k, &_s) == 0 && (_s & KCS_OBF)) {     \
                uint8_t _drop;                                        \
                kcs_read_data(k, &_drop);                       \
            }                                                         \
        } while (0)

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
        if (kcs_wait_ibf_clear(k) != 0) {
            return -1;
        }
        KCS_CLEAR_OBF_IF_SET();
        if (kcs_write_data(k, payload[i]) != 0) {
            return -1;
        }
    }

    //
    // WRITE_END + final data byte.
    //
    if (kcs_wait_ibf_clear(k) != 0) {
        return -1;
    }
    KCS_CLEAR_OBF_IF_SET();
    if (kcs_write_cmd(k, KCS_CTRL_WRITE_END) != 0 ||
        kcs_wait_ibf_clear(k) != 0)
    {
        return -1;
    }
    KCS_CLEAR_OBF_IF_SET();
    if (kcs_write_data(k, payload[payload_len - 1]) != 0) {
        return -1;
    }

    //
    // Read loop. Linux's ipmi_kcs_sm WAIT_READ pattern:
    //   - check state: must be READ or IDLE (anything else = error)
    //   - if READ + OBF: read byte, write KCS_READ_BYTE to ack
    //   - if IDLE: drain any final OBF byte, transaction complete
    //
    // Critical: the LAST response byte routinely arrives with state
    // already IDLE (BMC set the byte then immediately transitioned
    // because there's nothing more to send). Treating that as a state
    // mismatch loses the byte AND wastes 5 s waiting for a state==READ
    // that won't come.
    //
    // We use an internal staging buffer because every KCS response
    // begins with a 2-byte [NetFn|LUN, Cmd] echo before the CC byte
    // the caller actually wants — sizing solely off resp_len would
    // truncate the user's data by 2. that BMC also emits trailing
    // vendor-specific bytes past the spec body for some commands;
    // truncating those silently matches Linux ipmi_kcs_sm.
    //
    uint8_t  staging[2 + 256];      /* 2 echo + max IPMI response */
    size_t   stage_cap = sizeof(staging);
    size_t   stage_out = 0;
    while (1) {
        uint8_t status;
        if (kcs_wait_ibf_clear(k) != 0
            || kcs_read_status(k, &status) != 0)
        {
            return -1;
        }
        uint8_t state = status & KCS_STATE_MASK;
        if (state == KCS_STATE_ERROR) {
            axl_warning("KCS error state during read (status=0x%02x)",
                        status);
            return -1;
        }
        if (state == KCS_STATE_IDLE) {
            /* Final dummy byte per spec — some BMCs emit, others don't */
            if (status & KCS_OBF) {
                uint8_t discard;
                kcs_read_data(k, &discard);
            }
            break;
        }
        if (state != KCS_STATE_READ) {
            axl_warning("KCS unexpected state during read (status=0x%02x)",
                        status);
            return -1;
        }
        /* state == READ; wait for OBF if not yet set */
        if (!(status & KCS_OBF)) {
            axl_backend_stall(KCS_POLL_INTERVAL_US);
            continue;
        }
        uint8_t byte;
        if (kcs_read_data(k, &byte) != 0) {
            return -1;
        }
        if (stage_out < stage_cap) {
            staging[stage_out++] = byte;
        }
        if (kcs_write_data(k, KCS_CTRL_READ) != 0) {
            return -1;
        }
    }

    //
    // Strip the 2-byte echo and copy as much of the body (CC + data)
    // as the caller's buffer can hold. Anything past resp_len falls
    // off — fine, callers' command-specific decoders ignore trailing
    // bytes anyway.
    //
    if (stage_out < 2) {
        return -1;
    }
    size_t body_len = stage_out - 2;
    size_t copy = (body_len < *resp_len) ? body_len : *resp_len;
    for (size_t i = 0; i < copy; i++) {
        resp[i] = staging[2 + i];
    }
    *resp_len = copy;
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
        return AXL_ERR;
    }

    //
    // Sanity-probe: one status read. Catches "port I/O not available"
    // on AArch64 without committing to the whole FSM.
    //
    uint8_t probe;
    if (axl_backend_io_read8(cmd_port, &probe) != AXL_OK) {
        return AXL_ERR;
    }

    //
    // Phantom-BMC guard: a status byte of 0xFF on idle usually means
    // the bus is floating high — port I/O works but nothing is mapped
    // there. Real KCS hardware in any valid state has at least one bit
    // clear (idle = 0x00, write/read/error states never set all four
    // state-machine bits at once with both IBF and OBF set).
    //
    // Some BMCs nonetheless idle at 0xFF (or get there after a partial
    // prior transaction), then come alive on a Get Status command.
    // Try one before declaring the port dead — write 0x60 to the cmd
    // port, give the BMC a moment, re-read. If the byte changes to a
    // plausible value, the port IS mapped; proceed.
    //
    if (probe == 0xFF) {
        if (axl_backend_io_write8(cmd_port, KCS_CTRL_GET_STATUS) != AXL_OK) {
            return AXL_ERR;
        }
        axl_backend_stall(1000);   /* 1 ms — enough for BMC IBF latch */
        uint8_t reprobe;
        if (axl_backend_io_read8(cmd_port, &reprobe) != AXL_OK) {
            return AXL_ERR;
        }
        if (reprobe == 0xFF) {
            /* Truly floating — no BMC behind this port. */
            return AXL_ERR;
        }
    }

    KcsCtx *k = axl_malloc(sizeof(KcsCtx));
    if (k == NULL) {
        return AXL_ERR;
    }
    k->data_port = data_port;
    k->cmd_port  = cmd_port;

    ops->kind     = AXL_IPMI_TRANSPORT_KCS;
    ops->send_raw = kcs_send_raw;
    ops->close    = kcs_close;
    ops->ctx      = k;

    axl_info("IPMI KCS transport ready (data=0x%x, cmd=0x%x)",
             (unsigned)data_port, (unsigned)cmd_port);
    return AXL_OK;
}
