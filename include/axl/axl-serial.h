/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-serial.h
    Serial-port enumeration and line-setting readout.

    Enumerates the handles publishing the firmware's serial-I/O
    protocol and reads each port's line settings (baud, framing,
    timeout, FIFO depth) and modem control/status lines. This is a
    read-only descriptor probe for inventory and diagnostics — it
    does not open a port, transmit, or receive. (Console byte I/O is
    `<axl/axl-stream.h>`; this is the lower-level port descriptor.)

    Cursor-style iteration matches the other platform readers and
    returns the firmware `AxlHandle` directly:

    @code
    AxlHandle h = NULL;
    while ((h = axl_serial_next(h)) != NULL) {
        AxlSerialMode m;
        if (axl_serial_get_mode(h, &m) == AXL_OK) {
            // ... report Uart(baud, data, parity, stop) ...
        }
    }
    @endcode

    Device-path text needs no new API: the same `AxlHandle` resolves
    through the existing `axl_handle_get_protocol(h, "device-path",
    ...)` + `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`).

    Line-setting fields are raw readouts; the consumer names the
    parity/stop-bit codes (e.g. "N", "8N1").
**/

#ifndef AXL_SERIAL_H
#define AXL_SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>   /* AxlHandle */

typedef struct AxlLoop AxlLoop;   /* forward-decl for axl_serial_read_async */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Line settings for a serial port.
 *
 * Typed projection of the firmware's `SERIAL_IO_MODE` (current
 * attributes). `parity` and `stop_bits` are raw enum codes the
 * consumer maps to names; the firmware fields are:
 *   - parity:    0 Default, 1 None, 2 Even, 3 Odd, 4 Mark, 5 Space
 *   - stop_bits: 0 Default, 1 OneStopBit, 2 OneFive, 3 Two
 */
typedef struct {
    uint32_t baud_rate;            ///< current baud rate; 0 = device's designed speed (firmware field is UINT64, narrowed to 32 bits — every real UART rate fits)
    uint32_t data_bits;            ///< data bits per character (5-8; 0 = device default)
    uint8_t  parity;               ///< parity code (EFI_PARITY_TYPE raw)
    uint8_t  stop_bits;            ///< stop-bits code (EFI_STOP_BITS_TYPE raw)
    uint32_t timeout;              ///< receive/transmit timeout in microseconds (0 = device default)
    uint32_t receive_fifo_depth;   ///< receive FIFO depth in bytes
} AxlSerialMode;

/**
 * @brief Modem control/status lines for a serial port.
 *
 * Decoded from the protocol's GetControl bitmask. These are the
 * status/handshake lines a diagnostic view reports; the consumer
 * formats them.
 */
typedef struct {
    bool cts;               ///< Clear To Send asserted
    bool dsr;               ///< Data Set Ready asserted
    bool ri;                ///< Ring Indicate asserted
    bool dcd;               ///< Data Carrier Detect asserted
    bool hw_flow_control;   ///< hardware flow control enabled
} AxlSerialControl;

/**
 * @brief Iterate handles publishing the serial-I/O protocol.
 *
 * Cursor-style enumeration: pass NULL to get the first serial
 * handle, then pass each returned handle back to get the next.
 * Returns NULL once exhausted (including when no serial ports
 * exist).
 *
 * The handle set is located once and cached for the image lifetime
 * (like AxlBlock / AxlUsb) — a port that appears afterward will not
 * show up; the cache mirrors the boot device set. Position is
 * recovered from the handle
 * you pass back, not a hidden shared cursor: passing NULL — or any
 * handle not in the cached set — starts again from the first port,
 * and independent walks do not interfere. The returned handle is
 * firmware-owned (do not free) and valid to pass to the readers
 * below and to `axl_handle_get_protocol(h, "device-path", ...)`.
 *
 * @return next serial-I/O handle, or NULL at end of enumeration.
 */
AxlHandle
axl_serial_next(
    AxlHandle prev   ///< previous handle, or NULL to start
);

/**
 * @brief Read a serial port's current line settings.
 *
 * @return AXL_OK on success, AXL_ERR if @p handle does not publish
 *     the serial-I/O protocol or @p out is NULL.
 */
AXL_WARN_UNUSED int
axl_serial_get_mode(
    AxlHandle      handle,   ///< handle from axl_serial_next
    AxlSerialMode *out       ///< [out] populated on success
);

/**
 * @brief Read a serial port's modem control/status lines.
 *
 * Calls the protocol's GetControl and decodes the handshake bits.
 *
 * @return AXL_OK on success, AXL_ERR if @p handle does not publish
 *     the serial-I/O protocol, the GetControl call fails, or @p out
 *     is NULL.
 */
AXL_WARN_UNUSED int
axl_serial_get_control(
    AxlHandle         handle,   ///< handle from axl_serial_next
    AxlSerialControl *out       ///< [out] populated on success
);

// ---------------------------------------------------------------------------
// Byte I/O — open a port and transmit/receive (Serial-Over-LAN, UART bridges)
// ---------------------------------------------------------------------------
//
// The readers above are a read-only descriptor probe. This layer opens a
// serial port for actual byte traffic over EFI_SERIAL_IO_PROTOCOL — the
// substrate a consumer needs to bridge a UART (e.g. Serial-Over-LAN: pump a
// host port <-> a network terminal). EFI_SERIAL_IO has no completion event
// (Read returns whatever is buffered immediately), so the loop-integrated
// receive is a timer POLL, not an event wake — fine for UART byte rates.

/// Opaque open serial port. Owns no firmware resource beyond the borrowed
/// protocol pointer (the port is firmware-owned); axl_serial_close frees the
/// wrapper and removes any async-read source.
typedef struct AxlSerial AxlSerial;

/// Async receive callback — invoked from the loop with the bytes read this
/// poll tick (@p len > 0). @p data is owned by the library (valid only for
/// the call); copy what you need.
typedef void (*AxlSerialReadFn)(
    const void *data,  ///< received bytes (library-owned, valid for the call)
    size_t      len,   ///< number of bytes (always > 0)
    void       *user   ///< caller context
) AXL_CB_NOEXCEPT;

/**
 * @brief Open a serial port for byte I/O, EXCLUSIVELY.
 *
 * @p handle is a serial-I/O handle from axl_serial_next. The port's current
 * line settings are left as-is (call axl_serial_set_mode to change them).
 *
 * The open is exclusive: while this image holds the port, a second
 * axl_serial_open (or axl_serial_open_shared) for the same handle fails with
 * AXL_BUSY. Two subsystems each holding a "port" that is the same UART is
 * not merely interleaved output — whichever one calls
 * axl_driver_disconnect_handle first detaches the port's drivers underneath
 * a protocol pointer the other is still writing through. That failure is
 * silent, so sharing must be asked for by name: see axl_serial_open_shared.
 *
 * @warning Scope is THIS IMAGE. The claim lives in the library's static
 *     state, so it cannot see a port another loaded image has open. It
 *     resolves collisions between subsystems of one program, which is where
 *     they are both most likely and least visible.
 *
 * @return AXL_OK with @p out set; AXL_BUSY if the port is already open in
 *     this image (@p out is left NULL); AXL_ERR on NULL args, a handle that
 *     does not publish the serial-I/O protocol, or allocation failure.
 */
int
axl_serial_open(
    AxlHandle   handle,   ///< handle from axl_serial_next
    AxlSerial **out       ///< [out] open port on success
);

/**
 * @brief Open a serial port that other subsystems may also hold.
 *
 * Identical to axl_serial_open except that it coexists with other SHARED
 * opens of the same handle. Use it only where sharing is deliberate and the
 * writers coordinate — a diagnostic log sink alongside a console, say.
 *
 * Still refuses (AXL_BUSY) when an EXCLUSIVE open holds the port: the holder
 * asked for exclusivity and gets it. Sharing is therefore all-or-nothing per
 * port, established by the first open.
 *
 * @return AXL_OK with @p out set; AXL_BUSY if an exclusive open holds the
 *     port; AXL_ERR as for axl_serial_open.
 */
int
axl_serial_open_shared(
    AxlHandle   handle,   ///< handle from axl_serial_next
    AxlSerial **out       ///< [out] open port on success
);

/**
 * @brief Is this handle already open as a serial port in this image?
 *
 * Lets a consumer decline, or share deliberately, instead of discovering the
 * collision as corrupted output. Note that a check followed by an open is
 * two steps: prefer acting on axl_serial_open's AXL_BUSY, which cannot go
 * stale between the question and the answer.
 *
 * @return true while any open (exclusive or shared) is outstanding.
 */
bool
axl_serial_is_open(
    AxlHandle handle   ///< handle from axl_serial_next
);

/**
 * @brief The handle an open port was opened from.
 *
 * Every pre-open query in this header takes an AxlHandle (axl_serial_get_mode,
 * axl_serial_get_control), so without this a caller holding only the open port
 * had to retain the enumeration index purely to read back — which a
 * read-modify-write of one line setting requires, since axl_serial_set_mode
 * takes the whole struct and reads a zero field as "device default".
 *
 * @return the handle, or NULL if @p s is NULL.
 */
AxlHandle
axl_serial_handle(
    const AxlSerial *s   ///< open port (NULL-safe)
);

/**
 * @brief Close a serial port opened by axl_serial_open.
 *
 * Removes any axl_serial_read_async source and frees the wrapper. Does not
 * reset the underlying firmware port. NULL-safe.
 *
 * @warning Do not close the same port twice. Two guards make the common
 *     case a harmless no-op -- a closed port's protocol pointer is cleared,
 *     and a pointer absent from the open-port list is never freed -- but
 *     NEITHER survives the allocator recycling the address. If a later
 *     axl_serial_open lands on the freed address, a stale second close
 *     matches the NEW port, unlinks it and frees it, silently releasing a
 *     live port's claim.
 *
 *     That residual hazard cannot be closed while the API hands out a raw
 *     pointer: any check has to read through the very pointer whose
 *     validity is in question. A generation-tagged opaque handle would fix
 *     it properly and is tracked in docs/ROADMAP.md. Until then, treat the
 *     pointer as dead the moment this returns.
 */
void
axl_serial_close(
    AxlSerial *s   ///< port (NULL-safe)
);

/**
 * @brief Set the port's line settings (baud / framing / timeout).
 *
 * Maps to the protocol's SetAttributes. A zero field requests the device
 * default (per the UEFI spec). @p mode uses the same field encoding as
 * axl_serial_get_mode (parity / stop_bits raw codes).
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or a SetAttributes failure
 *     (e.g. an unsupported baud/framing combination).
 */
int
axl_serial_set_mode(
    AxlSerial           *s,     ///< open port
    const AxlSerialMode *mode   ///< desired settings
);

/**
 * @brief Write bytes to the port (best-effort within the port's timeout).
 *
 * Writes up to @p len bytes; @p out_written (optional) receives the count
 * actually transmitted. A short write (firmware timeout) is not an error —
 * @p out_written tells the caller to retry the remainder.
 *
 * @return AXL_OK on success (possibly short — see @p out_written); AXL_ERR on
 *     NULL @p s / @p buf or a device error.
 */
int
axl_serial_write(
    AxlSerial  *s,            ///< open port
    const void *buf,          ///< bytes to send
    size_t      len,          ///< number of bytes
    size_t     *out_written   ///< [out] bytes transmitted (NULL = don't care)
);

/**
 * @brief Read available bytes from the port (non-blocking).
 *
 * Returns immediately with whatever is buffered (up to @p cap). @p out_read
 * receives the count — zero is a normal "nothing available right now" result,
 * not an error. For a continuous receive, prefer axl_serial_read_async.
 *
 * @return AXL_OK (with @p out_read possibly 0); AXL_ERR on NULL @p s / @p buf
 *     / @p out_read, zero @p cap, or a device error.
 */
int
axl_serial_read(
    AxlSerial *s,          ///< open port
    void      *buf,        ///< [out] receive buffer
    size_t     cap,        ///< buffer capacity in bytes
    size_t    *out_read    ///< [out] bytes read (0 = none available)
);

/**
 * @brief Start a loop-integrated receive: poll the port and deliver bytes.
 *
 * Registers a @p poll_ms timer on @p loop; each tick drains the port and, if
 * any bytes arrived, invokes @p cb with them. EFI_SERIAL_IO exposes no
 * receive event, so this is a poll (not an interrupt) — pick @p poll_ms to
 * suit the UART rate (e.g. 5-10 ms for an interactive console). Only one
 * async receive per port; a second call replaces the first. The source is
 * removed by axl_serial_close.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args, zero @p poll_ms, or if the
 *     timer source could not be added.
 */
int
axl_serial_read_async(
    AxlSerial      *s,        ///< open port
    AxlLoop        *loop,     ///< event loop
    size_t          poll_ms,  ///< poll interval in milliseconds (> 0)
    AxlSerialReadFn cb,       ///< receive callback
    void           *user      ///< caller context
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SERIAL_H */
