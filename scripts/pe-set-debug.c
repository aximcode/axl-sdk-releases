/**
 * pe-set-debug.c — Patch PE debug data directory to point to .dbgdir section.
 *
 * The .dbgdir section (from axl-debug-info.S) contains a pre-formatted
 * DebugDirEntry + CodeView RSDS entry. This tool:
 *   1. Finds the .dbgdir section in the PE section table
 *   2. Sets the PE data directory [6] (DEBUG) to point to it
 *   3. Patches the DebugDirEntry's RVA and file offset
 *   4. Writes the module name into the RSDS filename field
 *   5. Sets the NX_COMPAT bit in DllCharacteristics (the images are built
 *      W^X-clean — R-X .text, RW .data — so they should advertise NX
 *      compatibility for firmware that enforces memory protection, which is
 *      the norm under Secure Boot). Verified by scripts/check-pe-nx.py.
 *   6. Drops a COFF string table objcopy left for a long section name,
 *      truncating the name to eight bytes as the PE spec says an image's
 *      must be (drop_string_table). Verified by scripts/check-pe-stripped.py.
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
#define OPT64_DLLCHARACTERISTICS_OFF 70  /* offset of DllCharacteristics */
#define OPT64_NUM_DATA_DIRS_OFF 108  /* offset of NumberOfRvaAndSizes */
#define OPT64_DATA_DIR_OFF      112  /* offset of first DataDirectory entry */
#define DATA_DIR_ENTRY_SIZE     8    /* {VirtualAddress, Size} */

/* DllCharacteristics bit: image is compatible with NX / memory protection. */
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT 0x0100

/* Offsets within the COFF file header (after the "PE\0\0" signature) */
#define COFF_PTR_TO_SYMTAB_OFF  8
#define COFF_NUM_SYMBOLS_OFF    12

/* Section header size, and its name field */
#define SECTION_HEADER_SIZE     40
#define SECTION_NAME_LEN        8

/* Debug directory entry layout (28 bytes) */
#define DDE_TYPE_OFF            12
#define DDE_SIZE_OF_DATA_OFF    16
#define DDE_RVA_OFF             20
#define DDE_FILE_OFF_OFF        24
#define DDE_SIZE                28

/* RSDS layout */
#define RSDS_SIG_OFF            0
#define RSDS_GUID_OFF           4    /* the 16-byte CodeView GUID */
#define RSDS_GUID_LEN           16
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

static void
w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

/* build_id -- a deterministic 128-bit identity for the image.
 *
 * WHY THE CODEVIEW GUID. Our .efi files carried NO build identity at all:
 * TimeDateStamp is 0 on every image this SDK produces, and axl-debug-info.S
 * emits the RSDS record with sixteen literal zero bytes. So two builds of the
 * same tool were indistinguishable, and an .efi of ours landing in RSOD triage
 * could not be matched to the build it came from. The GUID field is already
 * there, already carried through objcopy in .dbgdir, and rsod-decode.py
 * already reads it -- so filling it in costs no new section, no new flag and
 * no format.
 *
 * FNV-1a over the whole file with the GUID field itself ZEROED. Zeroing is
 * what makes it idempotent: the id is written INTO the image, so hashing the
 * file as-is would hash a file containing its own hash and give a different
 * answer on every run -- the build would never reach a fixed point.
 *
 * FNV-1a, not a cryptographic digest, and the distinction is deliberate: this
 * identifies a build, it does not authenticate one. Nothing here resists an
 * adversary choosing colliding images, and nothing should rely on it to. What
 * it does guarantee is that the same bytes always give the same id and that a
 * one-byte change gives a different one, which is the whole job.
 */
static void
build_id(const uint8_t *data, size_t len, size_t guid_off, uint8_t out[16])
{
    /* FNV-1a 128-bit: offset basis and prime from the FNV spec. */
    unsigned __int128 h = ((unsigned __int128)0x6c62272e07bb0142ULL << 64)
                          | 0x62b821756295c58dULL;
    const unsigned __int128 prime = ((unsigned __int128)1 << 88)
                                    | ((unsigned __int128)1 << 8) | 0x3b;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (i >= guid_off && i < guid_off + RSDS_GUID_LEN) {
            b = 0;      /* the field we are about to fill -- see above */
        }
        h ^= (unsigned __int128)b;
        h *= prime;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = (uint8_t)(h >> (8 * (15 - i)));   /* big-endian */
    }
}

/* drop_string_table -- an image carries no COFF string table.
 *
 * WHY. objcopy's ELF -> PE step writes a section name longer than eight
 * bytes into a COFF string table after the last section, names the section
 * `/N` (its offset there), and points PointerToSymbolTable at the table with
 * NumberOfSymbols zero: --strip-all removes the symbols, not the table a long
 * name needs. aa64's `.rela.dyn` is nine bytes (elf_aarch64_efi.lds says why
 * it must be that name), so every aa64 image carried one, and
 * check-pe-stripped failed on it.
 *
 * THE RULE, PE/COFF spec section 3.4: "Executable images do not use a string
 * table and do not support section names longer than 8 characters. Long
 * names in object files are truncated if they are emitted to an executable
 * file." So each `/N` name becomes the first eight bytes of its string, the
 * pointer becomes zero, and the table -- the file's tail -- is cut off.
 * Nothing reads a section by name at load time (the crt0 finds relocations
 * through DT_RELA in .dynamic).
 *
 * Only a table with NO symbols: one with symbols is check-pe-stripped's to
 * report, and its names live in the table too. A table that is not the
 * file's tail is a layout nothing here produces, so it is refused rather
 * than cut with bytes left after it.
 *
 * Returns 0 (done, or nothing to do) or 1 (refused; *file_size unchanged).
 */
static int
drop_string_table(const char *path, uint8_t *coff, uint8_t *sections,
                  uint16_t num_sections, uint8_t *data, long *file_size)
{
    uint32_t sym_ptr = r32(coff + COFF_PTR_TO_SYMTAB_OFF);
    uint32_t num_syms = r32(coff + COFF_NUM_SYMBOLS_OFF);
    if (sym_ptr == 0 || num_syms != 0) {
        return 0;
    }
    /* The table must lie after every section's raw data: a pointer into a
     * section whose length happens to reach the end of the file would
     * otherwise cut that section's bytes off. */
    for (uint16_t i = 0; i < num_sections; i++) {
        const uint8_t *sh = sections + i * SECTION_HEADER_SIZE;
        uint64_t raw_end = (uint64_t)r32(sh + 20) + r32(sh + 16);  /* PointerToRawData + SizeOfRawData */
        if (r32(sh + 16) != 0 && raw_end > sym_ptr) {
            fprintf(stderr, "%s: COFF string table at 0x%x overlaps section %u's "
                    "data -- refusing to cut it\n", path, sym_ptr, i);
            return 1;
        }
    }
    /* ...and be the file's tail. */
    if ((uint64_t)sym_ptr + 4 > (uint64_t)*file_size
        || (uint64_t)sym_ptr + r32(data + sym_ptr) != (uint64_t)*file_size) {
        fprintf(stderr, "%s: COFF string table at 0x%x is not the end of the "
                "file -- refusing to cut it\n", path, sym_ptr);
        return 1;
    }
    const uint8_t *strtab = data + sym_ptr;
    uint32_t strtab_len = r32(strtab);

    for (uint16_t i = 0; i < num_sections; i++) {
        uint8_t *name = sections + i * SECTION_HEADER_SIZE;
        if (name[0] != '/') {
            continue;
        }
        uint32_t off = 0;
        int digits = 0;
        for (int k = 1; k < SECTION_NAME_LEN && name[k] != '\0'; k++) {
            if (name[k] < '0' || name[k] > '9') {
                digits = 0;
                break;
            }
            off = off * 10 + (uint32_t)(name[k] - '0');
            digits++;
        }
        if (digits == 0 || off < 4 || off >= strtab_len) {
            fprintf(stderr, "%s: section %u's long name '%.8s' does not index "
                    "the string table -- refusing to cut it\n", path, i, name);
            return 1;
        }
        size_t len = strnlen((const char *)strtab + off, strtab_len - off);
        if (len > SECTION_NAME_LEN) {
            len = SECTION_NAME_LEN;
        }
        memset(name, 0, SECTION_NAME_LEN);
        memcpy(name, strtab + off, len);
    }

    w32(coff + COFF_PTR_TO_SYMTAB_OFF, 0);
    *file_size = (long)sym_ptr;
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: pe-set-debug <file.efi> [module-name]\n");
        return 1;
    }

    /* Read-only query: print the id an image already carries and change
     * nothing. Kept separate from the patching path so the build stays silent
     * -- 34 tools x 2 arches means a line per link is noise, but being unable
     * to ask "which build is this .efi?" is the problem being fixed. */
    int print_only = 0;
    if (strcmp(argv[1], "--print-build-id") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: pe-set-debug --print-build-id <file.efi>\n");
            return 1;
        }
        print_only = 1;
        argv++; argc--;
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

    /* Advertise NX-compatibility. The images are linked W^X-clean (R-X .text,
       RW non-executable .data), so set IMAGE_DLLCHARACTERISTICS_NX_COMPAT so
       firmware that enforces memory protection (the norm under Secure Boot)
       applies it instead of warning on or rejecting the image. */
    uint8_t *dllchar = opt + OPT64_DLLCHARACTERISTICS_OFF;
    w16(dllchar, (uint16_t)(r16(dllchar) | IMAGE_DLLCHARACTERISTICS_NX_COMPAT));

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

    if (print_only) {
        const uint8_t *g = rsds + RSDS_GUID_OFF;
        for (int i = 0; i < RSDS_GUID_LEN; i++) {
            printf("%02x", g[i]);
        }
        printf("\n");
        free(data);
        return 0;
    }

    if (drop_string_table(path, coff, sections, num_sections, data, &file_size) != 0) {
        free(data);
        return 1;
    }

    /* Write module name into RSDS filename field */
    uint8_t *name_field = rsds + RSDS_NAME_OFF;
    uint32_t name_space = debug_raw_size - DDE_SIZE - RSDS_NAME_OFF;
    /* The whole field zeroed first -- the padding after the name is hashed
     * into the build id below, so it must not be whatever was there -- then
     * the name, truncated to fit and always NUL-terminated. */
    memset(name_field, 0, name_space);
    snprintf((char *)name_field, name_space, "%s", mod_name);

    /* The build identity, LAST: it must cover the module name written just
     * above and every other patch this tool makes, or two images differing
     * only in a patched field would share an id. */
    {
        uint8_t id[RSDS_GUID_LEN];
        size_t guid_off = (size_t)(rsds - data) + RSDS_GUID_OFF;
        build_id(data, (size_t)file_size, guid_off, id);
        memcpy(rsds + RSDS_GUID_OFF, id, RSDS_GUID_LEN);
    }

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
