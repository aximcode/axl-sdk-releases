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
    AXL_SMBUS_TRANSPORT_I2C           ///< EFI_I2C_MASTER_PROTOCOL (framed here)
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
