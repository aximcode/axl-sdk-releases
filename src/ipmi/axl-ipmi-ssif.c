/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-ssif.c
    SSIF (SMBus System Interface) IPMI transport.

    Framing follows IPMI v2.0 Section 12. Uses an AxlSmbus session
    for block transfers; AxlSmbus auto-selects EFI_SMBUS_HC_PROTOCOL
    or EFI_I2C_MASTER_PROTOCOL.

    Transport hazards (enforced here, not in callers):

      - 60 ms inter-command delay after every completed transaction.
        Without it, bulk operations (SDR list on iDRAC / Nvidia Grace)
        overwhelm the BMC's SSIF interface and it stalls.
      - Write retries up to 5 times at 60 ms intervals.
      - Read retries up to 10 times with exponential backoff starting
        at 60 ms.
      - Multi-part framing for responses that exceed 32 bytes.
**/

#include "axl-ipmi-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-wait.h>

AXL_LOG_DOMAIN("ipmi-ssif");

// ---------------------------------------------------------------------------
// SSIF constants (IPMI v2.0 Section 12)
// ---------------------------------------------------------------------------

#define SSIF_BLOCK_MAX                32   ///< Max payload per SMBus block
#define SSIF_MSG_MAX                  256  ///< Max full IPMI request (2 header + data)

// SMBus command codes (Table 12-4)
#define SSIF_CMD_SINGLE_PART_WRITE    0x02
#define SSIF_CMD_MULTI_PART_WR_START  0x06
#define SSIF_CMD_MULTI_PART_WR_MIDDLE 0x07
#define SSIF_CMD_MULTI_PART_WR_END    0x08
#define SSIF_CMD_SINGLE_PART_READ     0x03
#define SSIF_CMD_MULTI_PART_RD_MIDDLE 0x09

// Multi-part read framing bytes
#define SSIF_MPR_START_MAGIC1         0x00
#define SSIF_MPR_START_MAGIC2         0x01
#define SSIF_MPR_END_PATTERN          0xFF

// Retry + timing (microseconds)
#define SSIF_RETRY_DELAY_US           60000
#define SSIF_WRITE_MAX_RETRIES        5
#define SSIF_READ_MAX_RETRIES         10
#define SSIF_INTER_CMD_DELAY_US       60000

// ---------------------------------------------------------------------------
// Per-session state
// ---------------------------------------------------------------------------

typedef struct {
    AxlSmbus  *smbus;       ///< owned: freed by ssif_close
    uint8_t    slave_addr;
} SsifCtx;

// ---------------------------------------------------------------------------
// Write phase — single- or multi-part, with retries
// ---------------------------------------------------------------------------

static int
ssif_try_write(SsifCtx *s, const uint8_t *msg, size_t msg_len)
{
    if (msg_len <= SSIF_BLOCK_MAX) {
        return axl_smbus_write_block(
            s->smbus, s->slave_addr, SSIF_CMD_SINGLE_PART_WRITE,
            msg, msg_len);
    }

    //
    // Multi-part: start block (32 bytes) + optional middles + end.
    //
    size_t off = 0;
    if (axl_smbus_write_block(
            s->smbus, s->slave_addr, SSIF_CMD_MULTI_PART_WR_START,
            &msg[off], SSIF_BLOCK_MAX) != 0)
    {
        return -1;
    }
    off += SSIF_BLOCK_MAX;

    while (off + SSIF_BLOCK_MAX < msg_len) {
        if (axl_smbus_write_block(
                s->smbus, s->slave_addr, SSIF_CMD_MULTI_PART_WR_MIDDLE,
                &msg[off], SSIF_BLOCK_MAX) != 0)
        {
            return -1;
        }
        off += SSIF_BLOCK_MAX;
    }

    return axl_smbus_write_block(
        s->smbus, s->slave_addr, SSIF_CMD_MULTI_PART_WR_END,
        &msg[off], msg_len - off);
}

static int
ssif_write_with_retries(SsifCtx *s, const uint8_t *msg, size_t msg_len)
{
    for (size_t w = 0; w < SSIF_WRITE_MAX_RETRIES; w++) {
        if (ssif_try_write(s, msg, msg_len) == 0) {
            return 0;
        }
        axl_debug("SSIF write retry %zu/%d",
                  w + 1, SSIF_WRITE_MAX_RETRIES);
        axl_msleep(SSIF_RETRY_DELAY_US / 1000);
    }
    axl_error("SSIF write failed after %d retries", SSIF_WRITE_MAX_RETRIES);
    return -1;
}

// ---------------------------------------------------------------------------
// Read phase — exponential backoff, then multi-part reassembly
// ---------------------------------------------------------------------------

static int
ssif_read_first_block(SsifCtx *s, uint8_t *buf, size_t *len)
{
    size_t  delay = SSIF_RETRY_DELAY_US;
    for (size_t r = 0; r < SSIF_READ_MAX_RETRIES; r++) {
        axl_msleep(delay / 1000);
        size_t cap = *len;
        if (axl_smbus_read_block(
                s->smbus, s->slave_addr, SSIF_CMD_SINGLE_PART_READ,
                buf, &cap) == 0)
        {
            *len = cap;
            return 0;
        }
        axl_debug("SSIF read retry %zu/%d (delay=%zu us)",
                  r + 1, SSIF_READ_MAX_RETRIES, delay);
        delay *= 2;
    }
    axl_error("SSIF read failed after %d retries", SSIF_READ_MAX_RETRIES);
    return -1;
}

/**
 * Copy @a copy_len bytes from @a src into the caller's response buffer
 * at the current offset, bumping @a *total. Bytes that overflow the
 * caller's buffer are dropped silently (matches uefi-ipmitool).
 */
static void
ssif_append(uint8_t *resp, size_t resp_cap,
            size_t *total,
            const uint8_t *src, size_t copy_len)
{
    if (*total + copy_len <= resp_cap) {
        axl_memcpy(&resp[*total], src, copy_len);
    }
    *total += copy_len;
}

static int
ssif_read_response(SsifCtx *s, uint8_t *resp, size_t *resp_len)
{
    uint8_t  buf[SSIF_BLOCK_MAX + 2];
    size_t   buf_len = sizeof(buf);

    if (ssif_read_first_block(s, buf, &buf_len) != 0) {
        return -1;
    }

    size_t  cap   = *resp_len;
    size_t  total = 0;

    //
    // Multi-part start marker: first two bytes 0x00 0x01. If present,
    // the payload begins at byte 2 and continues across additional
    // MULTI_PART_READ_MIDDLE reads until a block prefixed with 0xFF
    // arrives (the end marker).
    //
    bool multi = (buf_len >= 2 &&
                  buf[0] == SSIF_MPR_START_MAGIC1 &&
                  buf[1] == SSIF_MPR_START_MAGIC2);

    if (multi) {
        ssif_append(resp, cap, &total, &buf[2], buf_len - 2);

        //
        // Bound the middle-block read loop. An IPMI response is at
        // most ~260 bytes, so ~16 middle blocks is already more than
        // any real BMC would ever emit. The cap defends against a
        // confused or malicious BMC that keeps returning non-0xFF
        // blocks — without it a tight spin pins the CPU with
        // ssif_append silently dropping bytes past resp_cap.
        //
        const size_t max_middle_blocks = 16;
        size_t blocks = 0;
        bool   terminated = false;
        while (blocks < max_middle_blocks) {
            buf_len = sizeof(buf);
            if (axl_smbus_read_block(
                    s->smbus, s->slave_addr, SSIF_CMD_MULTI_PART_RD_MIDDLE,
                    buf, &buf_len) != 0)
            {
                return -1;
            }
            if (buf_len == 0) {
                return -1;
            }
            //
            // Per spec: byte 0 is a block sequence number (0x02, 0x03,
            // ...) on middle blocks and 0xFF on the final block. Strip
            // it before appending either way.
            //
            bool last = (buf[0] == SSIF_MPR_END_PATTERN);
            ssif_append(resp, cap, &total, &buf[1], buf_len - 1);
            blocks++;
            if (last) {
                terminated = true;
                break;
            }
        }
        if (!terminated) {
            axl_warning("SSIF multi-part read did not terminate after %zu blocks",
                        max_middle_blocks);
            return -1;
        }

        //
        // Multi-part payload begins with NetFn + Cmd echoes; strip
        // them so the caller sees [CompletionCode, data...] starting
        // at resp[0], matching the single-part path. A multi-part
        // response with fewer than 2 total bytes is malformed (no
        // room for NetFn/Cmd echo, let alone a completion code) —
        // fail rather than returning garbage to the caller.
        //
        if (total < 2) {
            axl_warning("SSIF multi-part response too short (%zu bytes) to strip NetFn/Cmd",
                        total);
            *resp_len = 0;
            return -1;
        }
        total -= 2;
        if (total <= cap) {
            //
            // Shift in place. Safe: source and dest overlap but
            // dest is lower — memmove semantics required, which
            // axl_memcpy does not guarantee. Use a byte loop.
            //
            for (size_t i = 0; i < total; i++) {
                resp[i] = resp[i + 2];
            }
        }
    } else if (buf_len > 2) {
        //
        // Single-part response: [NetFn, Cmd, CompletionCode, Data...]
        // Strip the 2-byte header.
        //
        ssif_append(resp, cap, &total, &buf[2], buf_len - 2);
    }

    *resp_len = total;
    //
    // A valid IPMI response is at least one byte (the completion code).
    // Empty means the BMC is confused — treat as transport error.
    //
    return (total == 0) ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Send-raw entry point (vtable method)
// ---------------------------------------------------------------------------

static int
ssif_send_raw(void *vctx,
              uint8_t netfn, uint8_t cmd,
              const uint8_t *req, size_t req_len,
              uint8_t *resp, size_t *resp_len)
{
    SsifCtx *s = (SsifCtx *)vctx;
    uint8_t  msg[SSIF_MSG_MAX];
    size_t   msg_len;

    if (resp == NULL || resp_len == NULL) {
        return -1;
    }

    msg_len = 2 + req_len;
    if (msg_len > sizeof(msg)) {
        return -1;
    }
    msg[0] = (uint8_t)(netfn << 2);  // LUN 0
    msg[1] = cmd;
    if (req_len > 0) {
        axl_memcpy(&msg[2], req, req_len);
    }

    if (ssif_write_with_retries(s, msg, msg_len) != 0) {
        return -1;
    }

    int rc = ssif_read_response(s, resp, resp_len);

    //
    // Inter-command delay runs regardless of outcome so a failing
    // command doesn't cascade into the next one hitting the BMC
    // before it has recovered.
    //
    axl_msleep(SSIF_INTER_CMD_DELAY_US / 1000);
    return rc;
}

static void
ssif_close(void *vctx)
{
    SsifCtx *s = (SsifCtx *)vctx;
    if (s == NULL) {
        return;
    }
    axl_smbus_free(s->smbus);
    axl_free(s);
}

// ---------------------------------------------------------------------------
// Public opener (called by auto-detect in axl-ipmi.c)
// ---------------------------------------------------------------------------

int
axl_ipmi_ssif_open(AxlIpmiTransportOps *ops,
                   AxlSmbus            *smbus,
                   uint8_t              slave_addr)
{
    if (ops == NULL || smbus == NULL) {
        return -1;
    }

    SsifCtx *s = axl_malloc(sizeof(SsifCtx));
    if (s == NULL) {
        return -1;
    }
    s->smbus      = smbus;
    s->slave_addr = slave_addr;

    ops->kind     = AXL_IPMI_TRANSPORT_SSIF;
    ops->send_raw = ssif_send_raw;
    ops->close    = ssif_close;
    ops->ctx      = s;

    axl_info("IPMI SSIF transport ready (slave=0x%02x, via %s)",
             (unsigned)slave_addr,
             axl_smbus_transport_string(axl_smbus_transport(smbus)));
    return 0;
}
