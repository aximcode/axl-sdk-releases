/* axl-debug-info.S — PE CodeView debug directory for GCC+objcopy builds.
 *
 * Emits a .dbgdir section containing:
 *   1. EFI_IMAGE_DEBUG_DIRECTORY_ENTRY (28 bytes)
 *   2. CodeView RSDS entry (24 bytes fixed header + 32 bytes name = 56 bytes)
 *   3. Module name placeholder (patched by pe-set-debug)
 *
 * The pe-set-debug post-link tool patches the PE data directory [6]
 * to point to this section and fills in the module name.
 *
 * Linked into every .efi binary via the CRT0 object set.
 */

    .section .dbgdir, "a"
    .balign 4

/* EFI_IMAGE_DEBUG_DIRECTORY_ENTRY (28 bytes) */
_debug_dir_entry:
    .4byte  0           /* Characteristics */
    .4byte  0x10d1a884  /* TimeDateStamp (fixed, matches iPXE/EDK2) */
    .2byte  0           /* MajorVersion */
    .2byte  0           /* MinorVersion */
    .4byte  2           /* Type = IMAGE_DEBUG_TYPE_CODEVIEW */
    .4byte  _debug_rsds_end - _debug_rsds  /* SizeOfData */
    .4byte  0           /* AddressOfRawData (RVA — patched by pe-set-debug) */
    .4byte  0           /* PointerToRawData (file offset — patched by pe-set-debug) */

/* CodeView RSDS entry */
_debug_rsds:
    .4byte  0x53445352  /* Signature = "RSDS" */
    .4byte  0, 0, 0, 0  /* GUID (16 bytes, zeroed) */
    .4byte  1           /* Age */
_debug_name:
    .ascii  "unknown.efi"  /* Module name — patched by pe-set-debug */
    .space  21, 0          /* Padding to 32 bytes total for name field */
_debug_rsds_end:

#if defined(__ELF__) && defined(__linux__)
    .section .note.GNU-stack, "", %progbits
#endif
