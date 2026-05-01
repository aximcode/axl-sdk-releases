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

static const AxlArgDesc kFlags[] = {
    { .name = "offset",  .short_name = 'o', .type = AXL_ARG_U64, .base = 0,
      .help = "Start offset (hex 0x prefix, or decimal)" },
    { .name = "length",  .short_name = 'n', .type = AXL_ARG_U64, .base = 0,
      .help = "Number of bytes to dump (default: all)" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Show file info header" },
    {0}
};

static const AxlArgDesc kPositional[] = {
    { .name = "file", .type = AXL_ARG_STRING, .required = true,
      .help = "Path to the file to dump" },
    {0}
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

static int
run_hexdump(AxlArgs *a)
{
    AxlStream  *file;
    AxlFileInfo fi;
    uint64_t    file_size;
    bool        has_length;
    uint8_t     buf[16];
    uint64_t    bytes_read;

    bool     verbose = axl_args_get_bool(a, "verbose");
    uint64_t offset  = axl_args_get_uint(a, "offset");
    uint64_t length  = axl_args_get_uint(a, "length");
    has_length = (axl_args_get_string(a, "length") != NULL);

    const char *path = axl_args_get_string(a, "file");

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

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsApp){
        .name         = "Hexdump",
        .help         = "Hex+ASCII file dump",
        .global_flags = kFlags,
        .positionals  = kPositional,
        .handler      = run_hexdump,
    });
}
