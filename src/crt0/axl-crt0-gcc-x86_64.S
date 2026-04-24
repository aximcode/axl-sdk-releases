/* axl-crt0-gcc-x86_64.S — GCC EFI startup for x86_64.
 *
 * Entry point that applies ELF relocations (via _axl_relocate)
 * before jumping to the C entry point (_AxlEntry). Uses RIP-relative
 * addressing so it works before relocation is applied.
 *
 * Inspired by gnu-efi crt0-efi-x86_64.S (BSD-3-Clause, HP/Intel).
 */

    .text
    .align 4

    .globl _start
    .type  _start, %function
_start:
    subq    $8, %rsp
    pushq   %rcx                /* save ImageHandle (ms_abi arg1) */
    pushq   %rdx                /* save SystemTable (ms_abi arg2) */

    /* _axl_relocate(ImageBase, _DYNAMIC) — RIP-relative, works pre-reloc */
    lea     ImageBase(%rip), %rdi
    lea     _DYNAMIC(%rip), %rsi
    call    _axl_relocate

    /* _AxlEntry(ImageHandle, SystemTable) — globals are now valid */
    popq    %rdx
    popq    %rcx
    call    _AxlEntry

    addq    $8, %rsp
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
