/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbus-piix4.c
    Direct-I/O SMBus backend for the AMD FCH SMBus controller (and
    Intel PIIX4-compatible chipsets that follow the same register
    layout).

    Backend is opportunistic: on platforms where the firmware
    declines to expose the SMBus controller carrying DIMM SPDs
    via EFI_SMBUS_HC_PROTOCOL or EFI_I2C_MASTER_PROTOCOL, this
    backend offers a direct-I/O fallback so AxlSpd can still
    enumerate.

    AMD FCH SMBus has TWO controllers per chipset:
      - MAIN at I/O 0xB00 — port-multiplexed (subports 0..3
        selected via SB800 index/data pair at 0xCD6/0xCD7),
        carries the chipset-internal traffic + IMC arbitration
        semaphore at offset +8 (SMBSLVCNT).
      - AUX  at I/O 0xB20 — single port, NOT
        port-multiplexed, NO SMBSLVCNT semaphore. DIMM SPDs
        typically live here on AMD server boards.
    Linux's i2c_piix4 uses different algorithms for the two:
    `piix4_access_sb800` (with SMBSLVCNT acquire + port-mux)
    for MAIN, plain `piix4_access` for AUX. We mirror the same
    architectural split — `is_aux` flag in the per-port context
    skips IMC arbitration on the AUX path.

    NOTE — port-mux on MAIN is not currently implemented; we
    talk to whatever subport firmware left selected. For SPD
    discovery on boards that route SPDs to AUX this doesn't
    matter. Add port-mux when a future platform exposes SPDs
    on a MAIN subport that BIOS doesn't pre-select.

    KNOWN LIMITATION — AUX false-ACK quirk on some AMD server
    boards. Empirically on AMD FCH 1022:790b rev 0x71: every
    probe address on the AUX appears to ACK in QUICK / READ /
    Receive Byte modes, BUT completion status reads 0x00 (no
    INTR, no DEV_ERR, no BUS_COLLI, no FAILED) and a 256-byte
    dump returns all zeros. Linux's i2c_piix4 driver explicitly
    warns about this in piix4_add_adapter() (l. 968-974 of
    upstream drivers/i2c/busses/i2c-piix4.c):

      "The AUX bus can not be probed as on some platforms it
       reports all devices present and all reads return '0'.
       This would allow the ee1004 to be probed incorrectly."

    Mechanism (proven via cross-referencing OEM source): on
    affected platforms the DDR5 SPDs are NOT physically wired
    to the AUX SMBus controller — they're routed through an
    OEM-specific CPLD ("FPGA hub PLD") that fronts a private
    memory map. BIOS reads SPDs from the CPLD via a vendor
    UEFI protocol and publishes the results into SMBIOS Type
    17. The AUX SMBus controller is electrically present but
    has no real slaves; the "all-ACK + zero-data" pattern is
    what an empty FCH AUX returns by chipset design. Verified
    against Dell's reference DellCpldSmbusDxe / Dell17gCpldSmbus
    sources, which target a CPLD slave at SMBus address 0xC4
    via SSIF — entirely separate from the FCH SMBus.

    Linux works around the false-positive by skipping the
    standard ee1004 probe on the AUX adapter. We hold the line
    by gating piix4_run on the INTR completion bit: transactions
    that complete without INTR are reported as failures rather
    than masquerading as "all addresses ACK with zero data."
    Verified empirically: with the INTR guard temporarily
    lifted, every address from 0x03..0x77 reports ACK and
    slave 0x50 returns 256 bytes of all-zero — matching Linux
    exactly.

    Practical consequence: memspd cannot read DDR5 SPDs via
    this backend on affected platforms because the SPDs aren't
    on the bus we're talking to. memspd correctly falls back to
    SMBIOS Type 17 (BIOS-populated from the OEM CPLD path);
    same data path Linux's `dmidecode -t 17` uses.

    The backend remains in tree because (a) it works on
    platforms where SPDs are wired to the MAIN controller,
    (b) it cleanly reports "unavailable" rather than masking
    the chipset's false-ACK quirk, and (c) a future vendor
    consumer (e.g., a Dell CPLD adapter parallel to the
    existing ipmi-dell.c) could wrap the OEM SMBus protocol
    if richer-than-Type-17 telemetry is needed.

    Reference layouts:
      - Linux drivers/i2c/busses/i2c-piix4.c
      - AMD FCH BIOS Developer's Guide, "SMBus Controller" chapter

    Register layout per port (offsets from port base):
      0x00  HOST_STS    (R/W1C — write 1 to clear status bits)
      0x01  HOST_SLVSTS
      0x02  HOST_CNT    (W — control + start)
      0x03  HOST_CMD    (W — register/command code)
      0x04  HOST_ADDR   (W — slave address << 1 | R/W)
      0x05  HOST_DAT0   (R/W)
      0x06  HOST_DAT1   (R/W)
      0x07  HOST_BLOCK  (R/W — block FIFO, auto-incrementing)

    AMD FCH layout: ports are 0x20-byte spaced from the same I/O
    base (0xB00), so:
      port 0 — 0xB00..0xB1F  (MAIN; chipset BMC/PMBus typically)
      port 1 — 0xB20..0xB3F  (AUX; DIMM SPDs on AMD server boards)
**/

#include "axl-smbus-internal.h"

#include <axl/axl-log.h>
#include <axl/axl-pci.h>
#include <axl/axl-str.h>
#include <axl/axl-wait.h>

AXL_LOG_DOMAIN("smbus-piix4");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define PIIX4_PORT_STRIDE    0x20

#define PIIX4_REG_HOST_STS   0x00
#define PIIX4_REG_HOST_CNT   0x02
#define PIIX4_REG_HOST_CMD   0x03
#define PIIX4_REG_HOST_ADDR  0x04
#define PIIX4_REG_HOST_DAT0  0x05
#define PIIX4_REG_HOST_DAT1  0x06
#define PIIX4_REG_HOST_BLOCK 0x07
#define PIIX4_REG_SLV_CNT    0x08    /* IMC arbitration semaphore */

/* SMBSLVCNT bits — write to acquire / release the SMBus from the
 * IMC (Integrated MicroController) which runs in parallel managing
 * fan/thermal sensors on the same physical bus. Per Linux's
 * piix4_access_sb800 the semaphore protocol is:
 *   request: write current_value | 0x10, then re-read; bit 0x10 set
 *            in the read-back means we own the bus.
 *   release: write current_value | 0x20.
 * Without this IMC collisions silently corrupt our transactions
 * (DEV_ERR / BUS_COLLI in HOST_STS). */
#define PIIX4_SLV_REQ        0x10
#define PIIX4_SLV_REL        0x20

/* HOST_STS bits — write 1 to clear */
#define PIIX4_STS_HOST_BUSY  0x01
#define PIIX4_STS_INTR       0x02
#define PIIX4_STS_DEV_ERR    0x04
#define PIIX4_STS_BUS_COLLI  0x08
#define PIIX4_STS_FAILED     0x10
#define PIIX4_STS_CLEAR_ALL  0x1F

/* HOST_CNT command-type bits 4..2 */
#define PIIX4_CMD_QUICK      (0x0 << 2)
#define PIIX4_CMD_BYTE       (0x1 << 2)
#define PIIX4_CMD_BYTE_DATA  (0x2 << 2)
#define PIIX4_CMD_WORD_DATA  (0x3 << 2)
#define PIIX4_CMD_BLOCK_DATA (0x5 << 2)
#define PIIX4_CMD_START      0x40

/* HOST_ADDR bit 0: 0=write 1=read */
#define PIIX4_ADDR_READ      0x01

/* AMD FCH default I/O base — well-known constant from chipset docs.
 * Linux's i2c_piix4 derives this from PMx00 via SB800 SMBus index/
 * data pair (register 0x2c-0x2d at I/O 0xCD6/0xCD7); we hardcode
 * since axl-sdk has no need to support boards that move it. */
#define AMD_FCH_SMBUS_BASE   0x0B00

/* AMD FCH SMBus PCI ID (rev-agnostic) */
#define AMD_FCH_SMBUS_VID    0x1022
#define AMD_FCH_SMBUS_DID    0x790B

/* Two AMD FCH SMBus controllers — MAIN at base, AUX at base+0x20.
 * port_index 0 → MAIN (0xB00), port_index 1 → AUX (0xB20). */
#define PIIX4_PORTS_MAX  2

/* Poll cadence — 50 us per iter, 100 ms total. SPD reads complete
 * in <1 ms typically; long timeout covers the case where another
 * agent (BIOS thermal monitor) holds the bus briefly. */
#define PIIX4_POLL_INTERVAL_US  50
#define PIIX4_POLL_TIMEOUT_US   (100 * 1000)

/* IMC semaphore retry budget — 2000 × 1 ms = 2 seconds total. Linux's
 * MAX_TIMEOUT for the equivalent loop is around 500 ms; 2 s is
 * conservative for the rare case where the IMC is busy. The 1 ms
 * inter-retry sleep is event-driven (axl_msleep, not axl_backend_stall)
 * so the host CPU idles while we wait — busy-spinning was a 100% CPU
 * waste in the worst case. */
#define PIIX4_IMC_RETRY_MAX        2000
#define PIIX4_IMC_RETRY_SLEEP_MS   1

// ---------------------------------------------------------------------------
// Per-session state
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t  base;        /* I/O base of THIS port (e.g. 0xB20) */
    bool      is_aux;      /* true on AUX controller (0xB20) — skip IMC */
} Piix4Ctx;

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

static inline int
io_w(uint16_t port, uint8_t value)
{
    return axl_backend_io_write8(port, value);
}

static inline int
io_r(uint16_t port, uint8_t *out)
{
    return axl_backend_io_read8(port, out);
}

/**
 * Wait HOST_BUSY clear, then read final status. Returns the status
 * byte via @a final_status; returns -1 on timeout or I/O error.
 *
 * Per AMD FCH docs the controller may also return after clearing
 * HOST_BUSY but with DEV_ERR / BUS_COLLI / FAILED set — caller checks
 * those bits.
 *
 * Per PIIX4 errata (echoed in Linux's piix4_transaction) we MUST
 * stall before the first poll — the controller takes a few hundred
 * us to assert BUSY after the START write. Without the pre-stall,
 * STS reads as 0x00 (idle, never started) and we incorrectly
 * conclude the transaction completed silently. This was the
 * port-1 silent-failure mode on AMD server boards (AUX controller
 * at 0xB20): first poll caught the controller mid-arming, returned
 * STS=0, piix4_run then saw no INTR and returned -1 with zero
 * diagnostic output.
 */
static int
piix4_wait_complete(Piix4Ctx *p, uint8_t *final_status)
{
    /* Pre-stall — give the controller time to assert BUSY. */
    axl_backend_stall(500);

    uint8_t status;
    for (size_t elapsed = 0; elapsed < PIIX4_POLL_TIMEOUT_US;
         elapsed += PIIX4_POLL_INTERVAL_US)
    {
        if (io_r(p->base + PIIX4_REG_HOST_STS, &status) != AXL_OK) {
            return -1;
        }
        if (!(status & PIIX4_STS_HOST_BUSY)) {
            *final_status = status;
            return 0;
        }
        axl_backend_stall(PIIX4_POLL_INTERVAL_US);
    }
    return -1;
}

/**
 * Acquire the IMC semaphore. Returns 0 on success and the *original*
 * SMBSLVCNT value via @a save (caller must pass it back unchanged
 * on release). -1 on timeout — IMC has held the bus for too long.
 *
 * Per Linux piix4_access_sb800 the protocol is: write current|0x10,
 * re-read; bit 0x10 set in the read-back means we own the bus. Some
 * boards take ~10 ms to grant; we retry up to 50 ms.
 *
 * SMBSLVCNT is the MAIN controller's slave-control register; on the
 * AUX controller (0xB20) the same offset is RESERVED and writing to
 * it puts the controller into an undefined state where START is
 * silently ignored. So this function is a no-op on AUX — Linux's
 * piix4_access (used for AUX) skips it entirely.
 */
static int
piix4_imc_acquire(Piix4Ctx *p, uint8_t *save)
{
    *save = 0;
    if (p->is_aux) {
        return 0;
    }
    uint8_t cur;
    if (io_r(p->base + PIIX4_REG_SLV_CNT, &cur) != AXL_OK) {
        return -1;
    }
    *save = cur;
    for (int i = 0; i < PIIX4_IMC_RETRY_MAX; i++) {
        if (io_w(p->base + PIIX4_REG_SLV_CNT,
                 (uint8_t)(cur | PIIX4_SLV_REQ)) != AXL_OK) {
            return -1;
        }
        if (io_r(p->base + PIIX4_REG_SLV_CNT, &cur) != AXL_OK) {
            return -1;
        }
        if (cur & PIIX4_SLV_REQ) {
            return 0;
        }
        axl_msleep(PIIX4_IMC_RETRY_SLEEP_MS);
    }
    axl_debug("piix4 IMC semaphore acquire timeout @ port 0x%x (last=0x%02x)",
              (unsigned)p->base, cur);
    return -1;
}

static void
piix4_imc_release(Piix4Ctx *p, uint8_t save)
{
    if (p->is_aux) {
        return;
    }
    if (io_w(p->base + PIIX4_REG_SLV_CNT, save | PIIX4_SLV_REL) != AXL_OK) {
        axl_warning("piix4 IMC release write failed @ port 0x%x",
                    (unsigned)p->base);
    }
}

/**
 * Setup + start a transaction. Caller pre-fills DAT0 (for writes)
 * or DAT0/BLOCK (for block writes) before calling. This stages
 * ADDR/CMD/CNT, then runs Linux's piix4_transaction protocol:
 * pre-check STS (write-back to clear, bail with EBUSY if it
 * doesn't go to 0x00), kick START via OR-in of bit 0x40, stall
 * 500us per PIIX4 errata, poll BUSY clear, check status.
 *
 * @return 0 on success (status has INTR set, no errors), -1 on
 *     timeout / DEV_ERR / BUS_COLLI / FAILED / pre-flight EBUSY.
 *
 * Caller must hold the IMC semaphore (piix4_imc_acquire / release)
 * around any sequence that includes piix4_run AND any pre-fill of
 * DATA0/DATA1/BLOCK regs on the MAIN controller.
 */
static int
piix4_run(Piix4Ctx *p, uint8_t cmd_byte, uint8_t addr_rw, uint8_t cnt_kind)
{
    /* Stage ADDR / CMD / CNT (without start bit) — matches Linux's
     * order in piix4_access. */
    if (io_w(p->base + PIIX4_REG_HOST_ADDR, addr_rw) != AXL_OK ||
        io_w(p->base + PIIX4_REG_HOST_CMD,  cmd_byte) != AXL_OK ||
        io_w(p->base + PIIX4_REG_HOST_CNT,  cnt_kind) != AXL_OK)
    {
        return -1;
    }

    /* Pre-flight STS check (matches Linux's piix4_transaction). Write
     * back the READ value, not a hardcoded 0x1F — some bits behave
     * differently across MAIN and AUX, and writing only what we read
     * avoids touching reserved bits. */
    uint8_t pre;
    if (io_r(p->base + PIIX4_REG_HOST_STS, &pre) != AXL_OK) {
        return -1;
    }
    if (pre != 0x00) {
        if (io_w(p->base + PIIX4_REG_HOST_STS, pre) != AXL_OK) {
            return -1;
        }
        if (io_r(p->base + PIIX4_REG_HOST_STS, &pre) != AXL_OK) {
            return -1;
        }
        if (pre != 0x00) {
            axl_debug("piix4 pre-flight EBUSY status=0x%02x @ port 0x%x",
                      pre, (unsigned)p->base);
            return -1;
        }
    }

    /* Kick START — OR bit 0x40 into CNT. Matches Linux exactly. */
    if (io_w(p->base + PIIX4_REG_HOST_CNT,
             (uint8_t)(cnt_kind | PIIX4_CMD_START)) != AXL_OK)
    {
        return -1;
    }

    uint8_t status;
    if (piix4_wait_complete(p, &status) != 0) {
        axl_debug("piix4 timeout @ port 0x%x", (unsigned)p->base);
        return -1;
    }
    if (status & (PIIX4_STS_DEV_ERR | PIIX4_STS_BUS_COLLI | PIIX4_STS_FAILED)) {
        /* DEV_ERR is normal during slave-address probing; BUS_COLLI /
         * FAILED are bus problems. All at debug. */
        axl_debug("piix4 status=0x%02x @ port 0x%x slave=0x%02x cmd=0x%02x rw=%u",
                  status, (unsigned)p->base, addr_rw>>1, cmd_byte, addr_rw & 1);
        return -1;
    }
    if (!(status & PIIX4_STS_INTR)) {
        /* No completion flag, no error bits — AUX false-ACK quirk
         * (see file comment). Refuse to expose this to the caller.
         * Lifting the guard would make every address appear to ACK
         * with all-zero data — see Linux's piix4_add_adapter comment
         * (l. 968-974) which describes the same quirk. */
        axl_debug("piix4 no-intr status=0x%02x @ port 0x%x slave=0x%02x cmd=0x%02x rw=%u (pre=0x%02x)",
                  status, (unsigned)p->base, addr_rw>>1, cmd_byte, addr_rw & 1, pre);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Vtable methods
// ---------------------------------------------------------------------------

static int
piix4_read_byte(void *vctx,
                uint8_t slave, uint8_t command,
                uint8_t *out)
{
    Piix4Ctx *p = (Piix4Ctx *)vctx;
    uint8_t save;
    if (piix4_imc_acquire(p, &save) != 0) {
        return -1;
    }
    int rc = -1;
    uint8_t addr_rw = (uint8_t)((slave << 1) | PIIX4_ADDR_READ);
    if (piix4_run(p, command, addr_rw, PIIX4_CMD_BYTE_DATA) == 0) {
        rc = (io_r(p->base + PIIX4_REG_HOST_DAT0, out) == AXL_OK) ? 0 : -1;
    }
    piix4_imc_release(p, save);
    return rc;
}

static int
piix4_write_byte(void *vctx,
                 uint8_t slave, uint8_t command,
                 uint8_t value)
{
    Piix4Ctx *p = (Piix4Ctx *)vctx;
    uint8_t save;
    if (piix4_imc_acquire(p, &save) != 0) {
        return -1;
    }
    int rc = -1;
    if (io_w(p->base + PIIX4_REG_HOST_DAT0, value) == AXL_OK) {
        uint8_t addr_rw = (uint8_t)((slave << 1) | 0);
        rc = piix4_run(p, command, addr_rw, PIIX4_CMD_BYTE_DATA);
    }
    piix4_imc_release(p, save);
    return rc;
}

static int
piix4_read_block(void *vctx,
                 uint8_t slave, uint8_t command,
                 uint8_t *buf, size_t *len)
{
    Piix4Ctx *p = (Piix4Ctx *)vctx;
    uint8_t save;
    if (piix4_imc_acquire(p, &save) != 0) {
        return -1;
    }
    int rc = -1;

    /* Reset the block FIFO position by reading HOST_CNT (per spec). */
    uint8_t throwaway;
    io_r(p->base + PIIX4_REG_HOST_CNT, &throwaway);

    uint8_t addr_rw = (uint8_t)((slave << 1) | PIIX4_ADDR_READ);
    if (piix4_run(p, command, addr_rw, PIIX4_CMD_BLOCK_DATA) == 0) {
        uint8_t count;
        if (io_r(p->base + PIIX4_REG_HOST_DAT0, &count) == AXL_OK) {
            if (count > AXL_SMBUS_BLOCK_MAX) count = AXL_SMBUS_BLOCK_MAX;
            if (count > *len)                count = (uint8_t)*len;
            rc = 0;
            for (uint8_t i = 0; i < count; i++) {
                if (io_r(p->base + PIIX4_REG_HOST_BLOCK, &buf[i]) != AXL_OK) {
                    rc = -1;
                    break;
                }
            }
            if (rc == 0) {
                *len = count;
            }
        }
    }
    piix4_imc_release(p, save);
    return rc;
}

static int
piix4_write_block(void *vctx,
                  uint8_t slave, uint8_t command,
                  const uint8_t *buf, size_t len)
{
    Piix4Ctx *p = (Piix4Ctx *)vctx;
    if (len == 0 || len > AXL_SMBUS_BLOCK_MAX) {
        return -1;
    }
    uint8_t save;
    if (piix4_imc_acquire(p, &save) != 0) {
        return -1;
    }
    int rc = -1;

    /* Reset block-FIFO index by reading HOST_CNT (spec). */
    uint8_t throwaway;
    io_r(p->base + PIIX4_REG_HOST_CNT, &throwaway);

    if (io_w(p->base + PIIX4_REG_HOST_DAT0, (uint8_t)len) == AXL_OK) {
        rc = 0;
        for (size_t i = 0; i < len && rc == 0; i++) {
            if (io_w(p->base + PIIX4_REG_HOST_BLOCK, buf[i]) != AXL_OK) {
                rc = -1;
            }
        }
        if (rc == 0) {
            uint8_t addr_rw = (uint8_t)((slave << 1) | 0);
            rc = piix4_run(p, command, addr_rw, PIIX4_CMD_BLOCK_DATA);
        }
    }
    piix4_imc_release(p, save);
    return rc;
}

static int
piix4_receive_byte(void *vctx, uint8_t slave, uint8_t *out)
{
    Piix4Ctx *p = (Piix4Ctx *)vctx;
    uint8_t save;
    if (piix4_imc_acquire(p, &save) != 0) {
        return AXL_ERR;
    }
    /* PIIX4_CMD_BYTE = (0x1 << 2) selects the BYTE protocol — no
     * command byte sent, just address+R then 1 data byte read.
     * Different from PIIX4_CMD_BYTE_DATA which sends a command. */
    int rc = AXL_ERR;
    uint8_t addr_rw = (uint8_t)((slave << 1) | PIIX4_ADDR_READ);
    if (piix4_run(p, 0, addr_rw, PIIX4_CMD_BYTE) == 0) {
        rc = (io_r(p->base + PIIX4_REG_HOST_DAT0, out) == AXL_OK)
             ? AXL_OK : AXL_ERR;
    }
    piix4_imc_release(p, save);
    return rc;
}

static int
piix4_quick(void *vctx, uint8_t slave, bool is_read)
{
    Piix4Ctx *p = (Piix4Ctx *)vctx;
    uint8_t save;
    if (piix4_imc_acquire(p, &save) != 0) {
        return AXL_ERR;
    }
    /* QUICK uses HOST_ADDR alone — no command, no data. The R/W bit
     * is bit 0 of HOST_ADDR (matches Linux's piix4_access for
     * I2C_SMBUS_QUICK). PIIX4_CMD_QUICK = (0x0 << 2) selects the
     * quick command type in HOST_CNT. cmd_byte is unused at the
     * controller level for quick but we pass 0 for cleanliness. */
    uint8_t addr_rw = (uint8_t)((slave << 1) | (is_read ? 0x01 : 0x00));
    int rc = piix4_run(p, 0, addr_rw, PIIX4_CMD_QUICK);
    piix4_imc_release(p, save);
    return rc == 0 ? AXL_OK : AXL_ERR;
}

static void
piix4_close(void *vctx)
{
    axl_free(vctx);
}

// ---------------------------------------------------------------------------
// Discovery + open
// ---------------------------------------------------------------------------

/**
 * Return true if the AMD FCH SMBus PCI device is present in this
 * system, false otherwise. Used by the smbus walker to decide whether
 * to advertise the PIIX4 ports as candidates.
 *
 * On non-x86 architectures port I/O isn't possible, so the backend
 * is unreachable regardless of what PCI says — bail early without
 * walking PCI.
 *
 * Result is memoized: this is called by both port_count and
 * open_port, plus port_count is invoked per visit_all /
 * new_with_probe pass. A full PCI scan per call would be wasteful.
 */
static bool
amd_fch_smbus_present(void)
{
#if !defined(__x86_64__) && !defined(__i386__)
    return false;
#else
    static bool s_checked = false;
    static bool s_present = false;
    if (s_checked) {
        return s_present;
    }
    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        uint16_t vid, did;
        if (axl_pci_read_config_16(*p, 0x00, &vid) != 0 ||
            axl_pci_read_config_16(*p, 0x02, &did) != 0)
        {
            continue;
        }
        if (vid == AMD_FCH_SMBUS_VID && did == AMD_FCH_SMBUS_DID) {
            s_present = true;
            break;
        }
    }
    s_checked = true;
    return s_present;
#endif
}

int
axl_smbus_piix4_open_port(AxlSmbusTransportOps *ops, size_t port_index)
{
    if (ops == NULL || port_index >= PIIX4_PORTS_MAX) {
        return AXL_ERR;
    }
    if (!amd_fch_smbus_present()) {
        return AXL_ERR;
    }

    Piix4Ctx *p = axl_malloc(sizeof(*p));
    if (p == NULL) {
        return AXL_ERR;
    }
    p->base = (uint16_t)(AMD_FCH_SMBUS_BASE
                         + (uint16_t)(port_index * PIIX4_PORT_STRIDE));
    p->is_aux = (port_index == 1);

    /* Quick sanity probe: reading HOST_STS returning 0xFF means the
     * I/O range is unmapped — refuse to open rather than silently
     * accepting writes that go nowhere. */
    uint8_t s;
    if (io_r(p->base + PIIX4_REG_HOST_STS, &s) != AXL_OK || s == 0xFF) {
        axl_debug("piix4 port %zu (base 0x%x) probe rejected status=0x%02x",
                  port_index, (unsigned)p->base, s);
        axl_free(p);
        return AXL_ERR;
    }
    axl_debug("piix4 port %zu opened (base 0x%x, status=0x%02x)",
              port_index, (unsigned)p->base, s);

    ops->kind        = AXL_SMBUS_TRANSPORT_PIIX4;
    ops->read_block  = piix4_read_block;
    ops->write_block = piix4_write_block;
    ops->read_byte   = piix4_read_byte;
    ops->write_byte  = piix4_write_byte;
    ops->quick       = piix4_quick;
    ops->receive_byte = piix4_receive_byte;
    ops->close       = piix4_close;
    ops->ctx         = p;
    /* Identity matches Linux's i2cdetect -l labelling — port number
     * is the FCH-internal label (0 = MAIN, 1 = AUX), and the I/O
     * base lets users correlate with /proc/ioports on the same box. */
    axl_snprintf(ops->desc, sizeof(ops->desc),
                 "AMD FCH PIIX4 %s port %zu at 0x%X",
                 p->is_aux ? "AUX" : "MAIN",
                 port_index, (unsigned)p->base);
    return AXL_OK;
}

size_t
axl_smbus_piix4_port_count(void)
{
    return amd_fch_smbus_present() ? PIIX4_PORTS_MAX : 0;
}
