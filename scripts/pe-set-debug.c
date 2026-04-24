/**
 * pe-set-debug.c — Patch PE debug data directory to point to .dbgdir section.
 *
 * The .dbgdir section (from axl-debug-info.S) contains a pre-formatted
 * DebugDirEntry + CodeView RSDS entry. This tool:
 *   1. Finds the .dbgdir section in the PE section table
 *   2. Sets the PE data directory [6] (DEBUG) to point to it
 *   3. Patches the DebugDirEntry's RVA and file offset
 *   4. Writes the module name into the RSDS filename field
 *
 * Usage: pe-set-debug <file.efi> [module-name]
 *
 * Build: cc -o pe-set-debug pe-set-debug.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* PE/COFF constants */
#define MZ_MAGIC                0x5A4D
#define PE_MAGIC                0x00004550
#define PE32PLUS_MAGIC          0x20b
#define DEBUG_DIR_INDEX         6
#define IMAGE_DEBUG_TYPE_CODEVIEW 2
#define RSDS_SIGNATURE          0x53445352

/* Offsets within PE32+ optional header (from start of optional header) */
#define OPT64_NUM_DATA_DIRS_OFF 108  /* offset of NumberOfRvaAndSizes */
#define OPT64_DATA_DIR_OFF      112  /* offset of first DataDirectory entry */
#define DATA_DIR_ENTRY_SIZE     8    /* {VirtualAddress, Size} */

/* Section header size */
#define SECTION_HEADER_SIZE     40

/* Debug directory entry layout (28 bytes) */
#define DDE_TYPE_OFF            12
#define DDE_SIZE_OF_DATA_OFF    16
#define DDE_RVA_OFF             20
#define DDE_FILE_OFF_OFF        24
#define DDE_SIZE                28

/* RSDS layout */
#define RSDS_SIG_OFF            0
#define RSDS_NAME_OFF           24   /* after signature(4) + guid(16) + age(4) */

static uint16_t
r16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
r32(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static void
w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: pe-set-debug <file.efi> [module-name]\n");
        return 1;
    }

    const char *path = argv[1];

    /* Derive module name from filename if not specified */
    const char *mod_name;
    char name_buf[64];
    if (argc >= 3) {
        mod_name = argv[2];
    } else {
        const char *slash = strrchr(path, '/');
        const char *base = (slash != NULL) ? slash + 1 : path;
        snprintf(name_buf, sizeof(name_buf), "%s", base);
        mod_name = name_buf;
    }

    /* Read file */
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(file_size);
    if (data == NULL || fread(data, 1, file_size, f) != (size_t)file_size) {
        fprintf(stderr, "failed to read %s\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Parse DOS header */
    if (file_size < 64 || r16(data) != MZ_MAGIC) {
        fprintf(stderr, "%s: not a PE file\n", path);
        free(data);
        return 1;
    }

    uint32_t pe_off = r32(data + 60);
    if (pe_off + 4 + 20 > (uint32_t)file_size || r32(data + pe_off) != PE_MAGIC) {
        fprintf(stderr, "%s: bad PE signature\n", path);
        free(data);
        return 1;
    }

    /* COFF header */
    uint8_t *coff = data + pe_off + 4;
    uint16_t num_sections = r16(coff + 2);
    uint16_t opt_size = r16(coff + 16);

    /* Optional header */
    uint8_t *opt = coff + 20;
    if (r16(opt) != PE32PLUS_MAGIC) {
        fprintf(stderr, "%s: not PE32+\n", path);
        free(data);
        return 1;
    }

    uint32_t num_dd = r32(opt + OPT64_NUM_DATA_DIRS_OFF);
    if (num_dd <= DEBUG_DIR_INDEX) {
        fprintf(stderr, "%s: too few data directories (%u)\n", path, num_dd);
        free(data);
        return 1;
    }

    /* Data directory [6] = DEBUG */
    uint8_t *dd_debug = opt + OPT64_DATA_DIR_OFF + DEBUG_DIR_INDEX * DATA_DIR_ENTRY_SIZE;

    /* Section table starts after optional header */
    uint8_t *sections = coff + 20 + opt_size;

    if (sections + num_sections * SECTION_HEADER_SIZE > data + file_size) {
        fprintf(stderr, "%s: section table extends past end of file\n", path);
        free(data);
        return 1;
    }

    /* Find .dbgdir section */
    uint32_t debug_rva = 0;
    uint32_t debug_file_off = 0;
    uint32_t debug_raw_size = 0;
    int found = 0;

    for (uint16_t i = 0; i < num_sections; i++) {
        uint8_t *sh = sections + i * SECTION_HEADER_SIZE;
        if (memcmp(sh, ".dbgdir\0\0", 8) == 0) {
            debug_rva = r32(sh + 12);       /* VirtualAddress */
            debug_raw_size = r32(sh + 16);   /* SizeOfRawData */
            debug_file_off = r32(sh + 20);   /* PointerToRawData */
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "%s: no .dbgdir section found\n", path);
        free(data);
        return 1;
    }

    if (debug_raw_size < DDE_SIZE + RSDS_NAME_OFF + 1) {
        fprintf(stderr, "%s: .dbgdir section too small (%u bytes)\n",
                path, debug_raw_size);
        free(data);
        return 1;
    }

    /* Patch the PE data directory [6] to point to the DebugDirEntry */
    w32(dd_debug + 0, debug_rva);       /* VirtualAddress */
    w32(dd_debug + 4, DDE_SIZE);        /* Size = one DebugDirEntry */

    /* Patch the DebugDirEntry inside the .dbgdir section */
    uint8_t *dde = data + debug_file_off;

    /* Verify it looks like our pre-formatted entry */
    if (r32(dde + DDE_TYPE_OFF) != IMAGE_DEBUG_TYPE_CODEVIEW) {
        fprintf(stderr, "%s: .dbgdir section doesn't contain a CodeView entry\n",
                path);
        free(data);
        return 1;
    }

    /* RSDS data starts right after the DebugDirEntry */
    uint32_t rsds_rva = debug_rva + DDE_SIZE;
    uint32_t rsds_file_off = debug_file_off + DDE_SIZE;

    w32(dde + DDE_RVA_OFF, rsds_rva);
    w32(dde + DDE_FILE_OFF_OFF, rsds_file_off);

    /* Verify RSDS signature */
    uint8_t *rsds = data + rsds_file_off;
    if (r32(rsds + RSDS_SIG_OFF) != RSDS_SIGNATURE) {
        fprintf(stderr, "%s: bad RSDS signature in .dbgdir section\n", path);
        free(data);
        return 1;
    }

    /* Write module name into RSDS filename field */
    uint8_t *name_field = rsds + RSDS_NAME_OFF;
    uint32_t name_space = debug_raw_size - DDE_SIZE - RSDS_NAME_OFF;
    size_t name_len = strlen(mod_name);
    if (name_len >= name_space) {
        name_len = name_space - 1;
    }
    memset(name_field, 0, name_space);
    memcpy(name_field, mod_name, name_len);

    /* Write back */
    f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        free(data);
        return 1;
    }

    if (fwrite(data, 1, file_size, f) != (size_t)file_size) {
        fprintf(stderr, "write error\n");
        fclose(f);
        free(data);
        return 1;
    }

    fclose(f);
    free(data);
    return 0;
}
