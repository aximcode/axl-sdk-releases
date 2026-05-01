/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-io-port.h
    x86 architectural I/O port access.

    Public wrappers around the `in`/`out` instruction family. These are
    primitive accessors — IO port reads on x86 cannot fail at the
    architectural level, so they return the value directly rather than
    a status code.

    Build-gated to x86 only. On AARCH64 (or any non-x86 target) the
    declarations expand to nothing, so call sites that haven't been
    arch-gated themselves fail at compile time rather than silently
    returning a no-op value at runtime — wrong-arch usage surfaces at
    build time, which is what we want.

    Typical use is for legacy hardware that hasn't moved to MMIO:
    SuperIO config (0x2E/0x2F), CMOS (0x70/0x71), 8042 keyboard
    controller, ACPI PM block GPE registers (where the FADT advertises
    them as port-based), IPMI KCS BMC interfaces.
**/

#ifndef AXL_IO_PORT_H
#define AXL_IO_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__x86_64__) || defined(__i386__)

/**
 * @brief Read a byte from an x86 I/O port.
 *
 * @return the byte read.
 */
uint8_t
axl_io_port_read8(
    uint16_t  port  ///< I/O port address
);

/**
 * @brief Read a 16-bit word from an x86 I/O port.
 *
 * @return the value read. The port should be 16-bit aligned for
 *     well-defined behavior on most chipsets.
 */
uint16_t
axl_io_port_read16(
    uint16_t  port  ///< I/O port address
);

/**
 * @brief Read a 32-bit doubleword from an x86 I/O port.
 *
 * @return the value read. The port should be 32-bit aligned.
 */
uint32_t
axl_io_port_read32(
    uint16_t  port  ///< I/O port address
);

/**
 * @brief Write a byte to an x86 I/O port.
 */
void
axl_io_port_write8(
    uint16_t  port,  ///< I/O port address
    uint8_t   value  ///< byte to write
);

/**
 * @brief Write a 16-bit word to an x86 I/O port.
 */
void
axl_io_port_write16(
    uint16_t  port,  ///< I/O port address
    uint16_t  value  ///< word to write
);

/**
 * @brief Write a 32-bit doubleword to an x86 I/O port.
 */
void
axl_io_port_write32(
    uint16_t  port,  ///< I/O port address
    uint32_t  value  ///< doubleword to write
);

#endif /* x86 */

#ifdef __cplusplus
}
#endif

#endif /* AXL_IO_PORT_H */
