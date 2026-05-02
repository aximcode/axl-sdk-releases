/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-port.c
    x86 architectural I/O port access — `in`/`out` instruction wrappers.

    The translation unit is empty on non-x86 targets; the public header
    omits the declarations there and any caller fails to link.
**/

#include <axl/axl-port.h>

#if defined(__x86_64__) || defined(__i386__)

uint8_t
axl_io_port_read8(uint16_t port)
{
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint16_t
axl_io_port_read16(uint16_t port)
{
    uint16_t v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint32_t
axl_io_port_read32(uint16_t port)
{
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void
axl_io_port_write8(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

void
axl_io_port_write16(uint16_t port, uint16_t value)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

void
axl_io_port_write32(uint16_t port, uint32_t value)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

#endif /* x86 */
