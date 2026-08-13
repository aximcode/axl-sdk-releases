/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stack-guard.c
    The stack-smashing detector: canary value and failure handler.

    `-fstack-protector-strong` places a canary between a function's
    locals and its saved return address, and checks it before
    returning. A linear buffer overflow that would have overwritten
    the return address hits the canary first, and control never
    reaches the corrupted address.

    @par Why this needs a file at all

    On x86-64 GCC reads the canary from `%fs:0x28` — glibc's TLS
    block. UEFI sets up no TLS and does not load `%fs`, so that read
    is garbage or a fault, and this is the real (and only) reason the
    SDK built with `-fno-stack-protector` for so long. It reads as
    "the protector does not work on firmware", which is false.

    `-mstack-protector-guard=global` redirects the read to the plain
    symbol below instead, and AArch64 already defaults to exactly
    that. With the flag and these two definitions the protector works
    under UEFI — verified by overflowing a buffer under QEMU and
    watching it halt here rather than return.

    Lives in `libaxl.a` rather than `libaxl-cxx.a` because the symbols
    are C-linkage and every C consumer needs them too. Some prebuilt
    `libstdc++.a` members reference `__stack_chk_fail` regardless of
    how we compile, so a hosted C++ link needs it even when the
    protector is off.

    @par The canary is a fixed value, deliberately

    It contains a NUL byte, which is the one property that matters
    most: the overflows this catches are usually string operations,
    and a NUL in the canary means `strcpy` and friends cannot write a
    valid one past it.

    It is NOT randomized per boot, and that is a scoped decision
    rather than an oversight. Randomizing means writing the guard
    after `EFI_RNG_PROTOCOL` is reachable, which is well after C code
    starts running — and every frame already on the stack at that
    moment captured the OLD value and would fail its check on return.
    Making that safe means guaranteeing `_start`, `_AxlEntry` and
    `_axl_init` are all unprotected, an invariant nothing enforces and
    that a later refactor would break in a way indistinguishable from
    a real smash. Detection is identical either way; only
    predictability of the value differs.
**/

#include <stdint.h>

#include <axl.h>

/* High byte NUL on purpose -- see the file docs. */
uintptr_t __stack_chk_guard = 0x00a5b7c3d1e9f200ULL;

/**
 * @brief Canary mismatch: the stack was corrupted. Halts.
 *
 * Not itself protected: the stack is already known bad, and a
 * handler that checked its own canary on the way out could recurse
 * instead of reporting.
 *
 * The return address names the frame that was smashed. Both build
 * modes emit DWARF into the side-by-side `.debug` file, so it
 * `addr2line`s against the artifact a user already has.
 */
AXL_NORETURN
__attribute__((no_stack_protector)) void
__stack_chk_fail(void)
{
    axl_printf("[axl] *** stack smashing detected in the function returning "
               "to %p -- halting\r\n", __builtin_return_address(0));
    axl_exit(1);
    __builtin_unreachable();
}
