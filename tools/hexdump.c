/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file Hexdump.c
    UEFI Shell application — hex/ASCII file viewer (xxd-style).

    Build with axl-cc:
      axl-cc Hexdump.c -o Hexdump.efi

    Usage:
      Hexdump.efi [-o offset] [-n length] [-v] [-h|-?] file
**/

#include <axl.h>

static const AxlConfigDesc descs[] = {
    { "offset",  AXL_CFG_STRING, NULL,    'o', "Start offset (hex 0x prefix, or decimal)", 0, 0 },
    { "length",  AXL_CFG_STRING, NULL,    'n', "Number of bytes to dump (default: all)",   0, 0 },
    { "verbose", AXL_CFG_BOOL,   "false", 'v', "Show file info header",                    0, 0 },
    { "help",    AXL_CFG_BOOL,   "false", 'h', "Show this help",                           0, 0 },
    { 0 }
};

static void
print_hex_line(
    uint64_t       addr,
    const uint8_t *data,
    size_t         count
    )
{
    size_t i;

    axl_printf("%08llx: ", (unsigned long long)addr);

    for (i = 0; i < 16; i++) {
        if (i < count) {
            axl_printf("%02x", data[i]);
        } else {
            axl_printf("  ");
        }
        if (i % 2 == 1) {
            axl_printf(" ");
        }
    }

    axl_printf(" ");

    for (i = 0; i < count; i++) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) {
            axl_printf("%c", data[i]);
        } else {
            axl_printf(".");
        }
    }

    axl_printf("\n");
}

int
main(
    int    argc,
    char **argv
    )
{
    AxlStream  *file;
    AxlFileInfo fi;
    uint64_t    offset;
    uint64_t    length;
    uint64_t    file_size;
    bool        has_length;
    bool        verbose;
    uint8_t     buf[16];
    uint64_t    bytes_read;

    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL || axl_config_parse_args(cfg, argc, argv) != 0) {
        axl_printf("Hexdump: invalid option\n");
        axl_config_usage(cfg, "Hexdump", "[-o offset] [-n length] [-v] file");
        return 1;
    }

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "Hexdump", "[-o offset] [-n length] [-v] file");
        return 0;
    }

    verbose = axl_config_get_bool(cfg, "verbose");
    offset = axl_strtou64(axl_config_get(cfg, "offset"));

    const char *length_str = axl_config_get(cfg, "length");
    has_length = (length_str != NULL);
    length = axl_strtou64(length_str);

    const char *path = axl_config_pos(cfg, 0);
    if (path == NULL) {
        axl_printf("Hexdump: file path required\n");
        axl_config_usage(cfg, "Hexdump", "[-o offset] [-n length] [-v] file");
        return 1;
    }

    /* Get file size */
    file_size = 0;
    if (axl_file_info(path, &fi) == 0) {
        file_size = fi.size;
    }

    if (!has_length) {
        length = (offset < file_size) ? file_size - offset : 0;
    }

    /* Open file */
    file = axl_fopen(path, "r");
    if (file == NULL) {
        axl_printf("Hexdump: cannot open '%s'\n", path);
        return 1;
    }

    if (verbose) {
        axl_printf("File:   %s\n", path);
        axl_printf("Size:   %llu bytes (0x%llx)\n",
                   (unsigned long long)file_size,
                   (unsigned long long)file_size);
        if (offset > 0) {
            axl_printf("Offset: %llu (0x%llx)\n",
                       (unsigned long long)offset,
                       (unsigned long long)offset);
        }
        axl_printf("Dump:   %llu bytes\n\n", (unsigned long long)length);
    }

    if (offset > file_size) {
        axl_printf("Hexdump: offset 0x%llx exceeds file size 0x%llx\n",
                   (unsigned long long)offset,
                   (unsigned long long)file_size);
        axl_fclose(file);
        return 1;
    }

    /* Read and dump */
    bytes_read = 0;
    while (bytes_read < length) {
        size_t want = 16;
        if (bytes_read + want > length) {
            want = (size_t)(length - bytes_read);
        }

        axl_ssize_t got = axl_pread(file, buf, want, (size_t)(offset + bytes_read));
        if (got <= 0) {
            break;
        }

        print_hex_line(offset + bytes_read, buf, (size_t)got);
        bytes_read += (uint64_t)got;
    }

    axl_fclose(file);
    return 0;
}
