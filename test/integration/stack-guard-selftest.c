/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file stack-guard-selftest.c
    Smash the stack on purpose and require the image to halt.

    `-fstack-protector-strong` is only worth carrying if it actually
    fires under UEFI, and the reason it was off for so long is that
    the x86-64 DEFAULT form cannot: GCC reads the canary from
    `%fs:0x28`, glibc's TLS block, which firmware never sets up. The
    build now passes `-mstack-protector-guard=global` so the read
    comes from a real symbol instead.

    A `volatile` byte count keeps the compiler from proving the
    overflow at compile time and turning this into a fixture that
    exercises nothing. The safe write runs FIRST so a failure to
    reach the smash is distinguishable from the smash being missed.
**/

#include <axl.h>

/* noinline: -Os would otherwise fold it into main, and a
 * canary check in main's frame is a different thing to test. */
__attribute__((noinline)) void
smash_by(
    int n   ///< bytes to write into a 16-byte buffer
)
{
    char buf[16];

    for (int i = 0; i < n; i++) {
        buf[i] = (char) ('A' + (i % 26));
    }
    axl_printf("stackguard: wrote %d bytes, buf[0]=%c\r\n", n, buf[0]);
}

int
main(void)
{
    volatile int safe  = 8;
    volatile int smash = 96;

    axl_print("stackguard: begin\r\n");
    smash_by(safe);
    axl_print("stackguard: overflowing now\r\n");
    smash_by(smash);
    axl_print("stackguard: UNDETECTED - returned from a smashed frame\r\n");
    return 0;
}
