/* mkrd-blob.S — Embedded RamDiskDxe.efi blob for the mkrd tool.
 *
 * `.incbin` reads the binary file at assembly time and emits its
 * bytes verbatim into .rodata. The path is supplied at build time
 * via `-DRAMDISK_BLOB_PATH=...` (see Makefile). Per-arch picking
 * happens there, so this source stays arch-agnostic.
 *
 * The C side (tools/mkrd.c) declares:
 *
 *   extern const unsigned char axl_embedded_ramdiskdxe[];
 *   extern const unsigned char axl_embedded_ramdiskdxe_end[];
 *
 * and gets the length as a pointer-difference at runtime.
 *
 * Replaces the xxd -i + sed Makefile dance — same payload, no
 * generated C header, no parse cost on the multi-MB blob, fewer
 * moving parts in the build graph.
 */

#ifndef RAMDISK_BLOB_PATH
#  error "RAMDISK_BLOB_PATH must be defined (see Makefile)"
#endif

/* Two-step stringify so a bare token like
   `third_party/edk2/RamDiskDxe-x64.efi` becomes a quoted string
   literal acceptable to .incbin. */
#define _AXL_STR(x) #x
#define _AXL_XSTR(x) _AXL_STR(x)

    .section .rodata
    .balign 8
    .globl axl_embedded_ramdiskdxe
    .globl axl_embedded_ramdiskdxe_end
axl_embedded_ramdiskdxe:
    .incbin _AXL_XSTR(RAMDISK_BLOB_PATH)
axl_embedded_ramdiskdxe_end:

#if defined(__ELF__) && defined(__linux__)
    .section .note.GNU-stack, "", %progbits
#endif
