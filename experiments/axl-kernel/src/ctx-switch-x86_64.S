/* ctx-switch-x86_64.S — cooperative coroutine context switch.
 *
 * ABI: x86-64 SysV (gcc default on freestanding). UEFI Boot Services
 * use MS x64 via axl_efi_call; intra-coroutine switches are pure SysV.
 *
 * Callee-saved registers preserved: rbx, rbp, r12, r13, r14, r15, rsp.
 * FPU/SSE state is not touched — freestanding builds don't generate
 * SSE code. If that changes we add fxsave/fxrstor here.
 *
 * void axlk_ctx_switch(AxlkCtx *from, AxlkCtx *to);
 *   rdi = from (save target), rsi = to (restore target)
 *
 * AxlkCtx layout must match src/kernel.c:
 *   offset 0:  rbx
 *   offset 8:  rbp
 *   offset 16: r12
 *   offset 24: r13
 *   offset 32: r14
 *   offset 40: r15
 *   offset 48: rsp
 */

    .text
    .global axlk_ctx_switch
    .type   axlk_ctx_switch, @function

axlk_ctx_switch:
    /* Save callee-saved regs into *from. */
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    movq    %rsp, 48(%rdi)

    /* Restore callee-saved regs from *to. */
    movq     0(%rsi), %rbx
    movq     8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15
    movq    48(%rsi), %rsp

    /* ret pops the target's saved return address (or the trampoline
     * address prepped by axl_spawn) into rip. Stack is 16-byte aligned
     * by construction. */
    retq
    .size axlk_ctx_switch, .-axlk_ctx_switch

/* Mark stack non-executable to keep modern linkers happy. */
    .section .note.GNU-stack,"",@progbits
