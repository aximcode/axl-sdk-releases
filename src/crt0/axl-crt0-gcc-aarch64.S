/* axl-crt0-gcc-aarch64.S — GCC EFI startup for aarch64.
 *
 * Entry point that applies ELF relocations (via _axl_relocate)
 * before jumping to the C entry point (_AxlEntry). Uses PC-relative
 * addressing so it works before relocation is applied.
 *
 * aarch64 UEFI uses AAPCS64 (same as Linux), so no calling
 * convention translation is needed — x0=ImageHandle, x1=SystemTable.
 */

    .text
    .align 4

    .globl _start
    .type  _start, %function
_start:
    stp     x29, x30, [sp, #-32]!  /* save caller's FP and LR */
    mov     x29, sp
    stp     x0, x1, [sp, #16]      /* save ImageHandle, SystemTable */

    /* _axl_relocate(ImageBase, _DYNAMIC) — PC-relative, works pre-reloc */
    adrp    x0, ImageBase
    add     x0, x0, :lo12:ImageBase
    adrp    x1, _DYNAMIC
    add     x1, x1, :lo12:_DYNAMIC
    bl      _axl_relocate

    /* _AxlEntry(ImageHandle, SystemTable) — globals are now valid */
    ldp     x0, x1, [sp, #16]
    bl      _AxlEntry

    ldp     x29, x30, [sp], #32    /* restore caller's FP and LR */
    ret

    /* Dummy .reloc section — PE loader needs this to recognize the
       image as relocatable. Two dummy IMAGE_REL_ABSOLUTE entries. */
    .data
dummy0: .4byte  0
dummy1: .4byte  0

    .section .reloc, "a", %progbits
    .4byte  dummy1 - dummy0     /* Page RVA */
    .4byte  12                  /* Block size (2*4 + 2*2) */
    .2byte  (0 << 12) + 0      /* IMAGE_REL_ABSOLUTE for dummy */
    .2byte  (0 << 12) + 0      /* IMAGE_REL_ABSOLUTE for dummy */

#if defined(__ELF__) && defined(__linux__)
    .section .note.GNU-stack, "", %progbits
#endif
