/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-reloc.c
    ELF dynamic relocation for GCC→objcopy EFI builds.

    When building with GCC, the EFI image contains ELF RELA relocations
    (not PE base relocations). This function processes them at startup
    before any code accesses global data. Called from axl-crt0-native.c
    for GCC builds only.

    Based on gnu-efi's reloc_x86_64.c / reloc_aarch64.c logic.
**/

#include <stdint.h>
#include <stddef.h>

/* ELF dynamic section tags */
#define DT_NULL     0
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9

/* ELF RELA entry */
typedef struct {
    uint64_t  offset;
    uint64_t  info;
    int64_t   addend;
} Elf64Rela;

/* ELF dynamic entry */
typedef struct {
    int64_t   tag;
    uint64_t  val;
} Elf64Dyn;

/* Relocation types */
#if defined(__x86_64__)
#define R_RELATIVE  8   /* R_X86_64_RELATIVE */
#elif defined(__aarch64__)
#define R_RELATIVE  1027 /* R_AARCH64_RELATIVE */
#else
#error "Unsupported architecture for ELF relocation"
#endif

/* Linker-provided symbols */
extern Elf64Dyn _DYNAMIC[];
extern char     ImageBase[];

/**
 * Process ELF RELA relocations at image load time.
 *
 * Walks the _DYNAMIC section to find the RELA table, then applies
 * each R_*_RELATIVE entry by adding the load-time base address delta.
 *
 * @param image_base  actual load address of the image
 * @param dynamic     pointer to the ELF .dynamic section
 * @return 0 on success, nonzero on error
 */
uint64_t
_axl_relocate(uint64_t image_base, Elf64Dyn *dynamic)
{
    Elf64Rela *rela = NULL;
    uint64_t   rela_size = 0;
    uint64_t   rela_ent = 0;
    uint64_t   i;

    /* Parse .dynamic section */
    for (i = 0; dynamic[i].tag != DT_NULL; i++) {
        switch (dynamic[i].tag) {
        case DT_RELA:
            rela = (Elf64Rela *)(image_base + dynamic[i].val);
            break;
        case DT_RELASZ:
            rela_size = dynamic[i].val;
            break;
        case DT_RELAENT:
            rela_ent = dynamic[i].val;
            break;
        default:
            /* Other tags (DT_STRTAB, DT_SYMTAB, etc.) are irrelevant
               to a freestanding EFI image — skip them. */
            break;
        }
    }

    if (rela == NULL || rela_ent == 0) {
        return 0;  /* no relocations needed */
    }

    /* Apply relocations */
    for (i = 0; i < rela_size; i += rela_ent) {
        Elf64Rela *entry = (Elf64Rela *)((uint8_t *)rela + i);
        uint32_t type = (uint32_t)(entry->info & 0xFFFFFFFF);

        if (type == R_RELATIVE) {
            uint64_t *target = (uint64_t *)(image_base + entry->offset);
            *target = image_base + (uint64_t)entry->addend;
        }
    }

    return 0;
}
