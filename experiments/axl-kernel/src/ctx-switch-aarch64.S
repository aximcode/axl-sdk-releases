/* ctx-switch-aarch64.S — cooperative coroutine context switch (AArch64).
 *
 * ABI: AAPCS64. UEFI Boot Services use MS-style x64/AArch64 conventions
 * via axl_efi_call; intra-coroutine switches are pure AAPCS64.
 *
 * Callee-saved registers preserved: x19-x28, x29 (FP), x30 (LR), sp.
 * FP/SIMD (v8-v15 lower 64b) not saved — freestanding scheduler path
 * doesn't use SIMD. Extend if that changes.
 *
 * void axlk_ctx_switch(AxlkCtx *from, AxlkCtx *to);
 *   x0 = from (save target), x1 = to (restore target)
 *
 * AxlkCtx layout must match src/kernel.c (aarch64 branch):
 *   offset   0: x19
 *   offset   8: x20
 *   offset  16: x21
 *   offset  24: x22
 *   offset  32: x23
 *   offset  40: x24
 *   offset  48: x25
 *   offset  56: x26
 *   offset  64: x27
 *   offset  72: x28
 *   offset  80: x29 (FP)
 *   offset  88: x30 (LR) — `ret` branches here
 *   offset  96: sp
 */

    .text
    .global axlk_ctx_switch
    .type   axlk_ctx_switch, @function

axlk_ctx_switch:
    /* Save callee-saved regs into *from (x0). stp stores two 8-byte
     * registers at once; offset form is byte offset from base. */
    stp     x19, x20, [x0,  #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]    /* FP + LR */
    mov     x9, sp
    str     x9,      [x0, #96]

    /* Restore callee-saved regs from *to (x1). */
    ldp     x19, x20, [x1,  #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9,       [x1, #96]
    mov     sp, x9

    /* `ret` branches to LR (x30) — either the saved return from the
     * target's last ctx_switch call, or the trampoline address
     * prepped by axlk_spawn on first entry. SP is 16-byte aligned
     * by construction. */
    ret
    .size axlk_ctx_switch, .-axlk_ctx_switch

/* Mark stack non-executable. */
    .section .note.GNU-stack,"",%progbits
