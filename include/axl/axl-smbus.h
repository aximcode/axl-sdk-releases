/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-smbus.h:
 *
 * SMBus / I2C block access for UEFI applications. Dispatches on the
 * first available firmware-provided transport:
 *
 *   1. EFI_SMBUS_HC_PROTOCOL  — full SMBus host controller.
 *   2. EFI_I2C_MASTER_PROTOCOL — raw I2C master; framing is built here.
 *
 * Consumers (AxlIpmi SSIF today, AxlSpd tomorrow) use `AxlSmbus` as
 * an opaque session object and never have to know which backend the
 * platform exposes. All framing lives behind the session — callers
 * pass the SMBus command byte and payload bytes and get back the
 * SMBus-stripped payload.
 */

#ifndef AXL_SMBUS_H
#define AXL_SMBUS_H

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/**
 * AxlSmbus:
 *
 * Opaque handle for an SMBus controller session. Created via
 * axl_smbus_new() and freed with axl_smbus_free().
 */
typedef struct AxlSmbus AxlSmbus;

/**
 * @brief Transport kind currently driving a session.
 */
typedef enum {
    AXL_SMBUS_TRANSPORT_UNKNOWN = 0,  ///< No controller available
    AXL_SMBUS_TRANSPORT_HC,           ///< EFI_SMBUS_HC_PROTOCOL
    AXL_SMBUS_TRANSPORT_I2C,          ///< EFI_I2C_MASTER_PROTOCOL (framed here)
    AXL_SMBUS_TRANSPORT_PIIX4         ///< AMD FCH / Intel PIIX4 direct I/O
} AxlSmbusTransport;

/**
 * @brief Open a session against the first available SMBus controller.
 *
 * Probes EFI_SMBUS_HC_PROTOCOL first (full block protocol provided by
 * firmware); falls back to EFI_I2C_MASTER_PROTOCOL (this module builds
 * the SMBus block-transfer framing on top).
 *
 * @return session handle, or NULL if no controller is available.
 */
AxlSmbus *
axl_smbus_new(void);

/**
 * @brief Open the FIRST SMBus controller where @p probe returns true.
 *
 * Auto-detect via `axl_smbus_new()` uses `LocateProtocol`, which
 * returns only one EFI_SMBUS_HC_PROTOCOL or EFI_I2C_MASTER_PROTOCOL
 * instance. On multi-segment platforms (server AMD EPYC boards
 * commonly publish 1 SMBus HC + a dozen I2C masters) the first
 * instance is rarely the bus carrying DIMM SPDs. This walker enumerates
 * EVERY published handle of both protocols and runs the caller's
 * probe against each opened session — useful for pickers like
 * AxlSpd that need a specific slave address to respond.
 *
 * @p probe receives the candidate session and is expected to attempt
 * a single read/write at the slave address it cares about. Return
 * true to claim the session; the caller will receive it as the
 * axl_smbus_new return value. Return false to discard and try
 * the next handle. @p user is forwarded verbatim.
 *
 * Sessions discarded by the probe are freed before the next attempt.
 *
 * @return session handle, or NULL if no enumeration step succeeded.
 */
typedef bool (*AxlSmbusProbeFn)(AxlSmbus *s, void *user);

AxlSmbus *
axl_smbus_new_with_probe(
    AxlSmbusProbeFn  probe,    ///< called per candidate session
    void            *user      ///< opaque pointer forwarded to probe
    );

/**
 * @brief Visit every published SMBus controller (HC + I2C master)
 *     for diagnostic / inventory purposes.
 *
 * Unlike `axl_smbus_new_with_probe`, this never claims a session.
 * Each controller is opened, handed to @p visit, then freed before
 * the next iteration. Useful for scanners that want to report the
 * full topology rather than pick a single bus.
 *
 * @return number of controllers visited (0 means none published).
 */
typedef void (*AxlSmbusVisitFn)(
    AxlSmbus     *s,       ///< transient session for this controller
    size_t        index,   ///< 0-based visit order
    void         *user     ///< opaque pointer forwarded to visit
    );

size_t
axl_smbus_visit_all(
    AxlSmbusVisitFn  visit,   ///< called per controller
    void            *user     ///< opaque pointer forwarded to visit
    );

/**
 * @brief Free an SMBus session. NULL-safe.
 */
void
axl_smbus_free(
    AxlSmbus  *s   ///< session to free (NULL-safe)
    );

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlSmbus, axl_smbus_free)
#endif

/**
 * @brief Report which transport a session selected.
 */
AxlSmbusTransport
axl_smbus_transport(
    const AxlSmbus  *s   ///< session (NULL returns UNKNOWN)
    );

/**
 * @brief SMBus "Receive Byte" transaction (§5.5.3).
 *
 * Wire format: address + R bit, then the slave returns one byte.
 * NO command byte is sent — different from axl_smbus_read_byte
 * which writes a command then re-reads. Linux's `i2cdetect` uses
 * Receive Byte as the safest probe for EEPROM-prone address ranges
 * (0x30..0x37, 0x50..0x5F) because writing a stray command byte
 * could trigger device behavior — e.g., a register-pointer reset
 * or, on some EEPROMs, a partial erase.
 *
 * @return AXL_OK on success, AXL_ERR on transport error or invalid
 *     arguments.
 */
int
axl_smbus_receive_byte(
    AxlSmbus  *s,
    uint8_t    slave,
    uint8_t   *out
    );

/**
 * @brief SMBus QUICK probe — address-only ACK check.
 *
 * Sends @a slave + R/W bit and returns whether the slave ACKed.
 * No command byte, no data. This is what Linux's `i2cdetect`
 * uses by default — the safest way to detect "is something at
 * this address?" without triggering register reads or writes.
 *
 * @a is_read selects the R/W bit (true = read direction). Some
 * EEPROMs at 0x50..0x5F can be erased by stray writes, so prefer
 * is_read=true for those address ranges.
 *
 * @return AXL_OK if the slave acknowledged; AXL_ERR if it
 *     NACKed, the bus errored, or the transport doesn't
 *     implement QUICK (currently all three transports do).
 */
int
axl_smbus_quick(
    AxlSmbus  *s,
    uint8_t    slave,
    bool       is_read
    );

/**
 * @brief Per-instance human-readable identity, filled by the
 *     backend at session-open time.
 *
 * Examples:
 *   - "EFI SMBus HC"               (every HC handle, no further detail)
 *   - "EFI I2C Master"             (every I2C Master handle)
 *   - "AMD FCH PIIX4 port 0 at 0xB00"  (MAIN controller)
 *   - "AMD FCH PIIX4 port 1 at 0xB20"  (AUX controller)
 *
 * Returned pointer lives as long as the session — do not free,
 * do not retain across axl_smbus_free. NULL only if @p s is
 * NULL.
 */
const char *
axl_smbus_describe(
    const AxlSmbus  *s   ///< session (NULL returns NULL)
    );

// ---------------------------------------------------------------------------
// Block transfers (SMBus 2.0 §5.5.7/5.5.8)
// ---------------------------------------------------------------------------

/// SMBus block transfer payload limit — the count byte is one byte wide
/// and the spec caps block length at 32 data bytes.
#define AXL_SMBUS_BLOCK_MAX  32

/**
 * @brief SMBus block read.
 *
 * Issues an SMBus block-read transaction: the command byte is written
 * to @a slave, the device responds with a byte count followed by up to
 * @c AXL_SMBUS_BLOCK_MAX payload bytes. The byte count is stripped
 * by this function; the caller sees only the payload.
 *
 * @param s        open session.
 * @param slave    7-bit SMBus slave address (bit 0 is the R/W bit and
 *                 is supplied by the transport).
 * @param command  SMBus command byte.
 * @param buf      (out) receives the payload.
 * @param len      (in/out) buffer capacity on entry; bytes written on
 *                 success (clamped to capacity if the device returned
 *                 more than requested).
 *
 * @return AXL_OK on success, AXL_ERR on transport error or invalid arguments.
 */
int
axl_smbus_read_block(
    AxlSmbus       *s,
    uint8_t         slave,
    uint8_t         command,
    uint8_t        *buf,
    size_t         *len
    );

/**
 * @brief SMBus block write.
 *
 * Issues an SMBus block-write transaction: command byte, byte count,
 * then @a len payload bytes. The byte-count prefix is inserted by
 * this function.
 *
 * @param s        open session.
 * @param slave    7-bit SMBus slave address.
 * @param command  SMBus command byte.
 * @param buf      payload bytes.
 * @param len      payload length; must not exceed @c AXL_SMBUS_BLOCK_MAX.
 *
 * @return AXL_OK on success, AXL_ERR on transport error or invalid arguments.
 */
int
axl_smbus_write_block(
    AxlSmbus       *s,
    uint8_t         slave,
    uint8_t         command,
    const uint8_t  *buf,
    size_t          len
    );

// ---------------------------------------------------------------------------
// Byte transfers (SMBus 2.0 §5.5.4 / §5.5.5)
// ---------------------------------------------------------------------------

/**
 * @brief SMBus "Read Byte" transaction.
 *
 * Per spec §5.5.5: write @a command to @a slave, repeated start, read
 * one data byte. The wire format is plain (no count prefix), which
 * matches 24Cxx-style EEPROMs that auto-increment from a written
 * offset — including the JEDEC SPD EEPROMs at 0x50–0x57 (DDR3/4 use
 * 1-byte offsets; DDR5 SPD5118 hubs use this op for register reads
 * after a page select via MR11).
 *
 * @param s        open session.
 * @param slave    7-bit SMBus slave address.
 * @param command  SMBus command byte (register/offset).
 * @param out      (out) receives the data byte.
 *
 * @return AXL_OK on success, AXL_ERR on transport error or invalid arguments.
 */
int
axl_smbus_read_byte(
    AxlSmbus  *s,
    uint8_t    slave,
    uint8_t    command,
    uint8_t   *out
    );

/**
 * @brief SMBus "Write Byte" transaction.
 *
 * Per spec §5.5.4: write @a command followed by @a value to @a slave.
 * Used for SPD5118 hub page selection (write page to MR11 / register
 * 0x0B) before reading from the page window.
 *
 * @param s        open session.
 * @param slave    7-bit SMBus slave address.
 * @param command  SMBus command byte (register/offset).
 * @param value    data byte to write.
 *
 * @return AXL_OK on success, AXL_ERR on transport error or invalid arguments.
 */
int
axl_smbus_write_byte(
    AxlSmbus  *s,
    uint8_t    slave,
    uint8_t    command,
    uint8_t    value
    );

// ---------------------------------------------------------------------------
// Formatting helpers (string table lookups; no allocation)
// ---------------------------------------------------------------------------

/**
 * @brief Human-readable name for an AxlSmbusTransport value.
 *
 * @return static string; never NULL.
 */
const char *
axl_smbus_transport_string(
    AxlSmbusTransport  kind
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_SMBUS_H */
