/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file Hexdump.c
    UEFI Shell application — hex/ASCII file viewer (xxd-style).

    Build with axl-cc:
      axl-cc Hexdump.c -o Hexdump.efi

    Usage:
      Hexdump.efi [-o offset] [-n length] [-v] [-h|-?] [file]

    With no file argument, reads from stdin — works as the right-hand
    side of a UEFI Shell pipe (`some-tool | Hexdump.efi`) on shells
    that publish EFI_SHELL_PARAMETERS_PROTOCOL.
**/

#include <axl.h>

static const AxlArgDesc flags[] = {
    { .name = "offset",  .short_name = 'o', .type = AXL_ARG_U64, .base = 0,
      .help = "Start offset (hex 0x prefix, or decimal)" },
    { .name = "length",  .short_name = 'n', .type = AXL_ARG_U64, .base = 0,
      .help = "Number of bytes to dump (default: all)" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Show file info header" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "file", .type = AXL_ARG_STRING,
      .help = "Path to the file to dump (omit to read from stdin)" },
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
run_hexdump_stdin(uint64_t offset, uint64_t length,
                  bool has_length, bool verbose)
{
    /* Sequential read from axl_stdin — no seek, no pread. The caller's
       --offset is honored by reading and discarding `offset` bytes
       first; --length caps total bytes dumped. */
    if (verbose) {
        axl_printf("File:   <stdin>\n");
        if (offset > 0) {
            axl_printf("Offset: %llu (skipped before dumping)\n",
                       (unsigned long long)offset);
        }
        if (has_length) {
            axl_printf("Dump:   up to %llu bytes\n\n",
                       (unsigned long long)length);
        } else {
            axl_printf("Dump:   until EOF\n\n");
        }
    }

    /* Skip `offset` bytes by reading and discarding. */
    uint8_t skip_buf[256];
    while (offset > 0) {
        size_t want = (offset < sizeof(skip_buf)) ? (size_t)offset
                                                  : sizeof(skip_buf);
        axl_ssize_t got = axl_read(axl_stdin, skip_buf, want);
        if (got <= 0) break;
        offset -= (uint64_t)got;
    }

    /* Dump 16 bytes at a time until EOF or length cap. */
    uint8_t  buf[16];
    uint64_t bytes_dumped = 0;
    /* stdin doesn't have meaningful absolute offsets like a file does;
       address shown is just the position within the dumped region. */
    while (!has_length || bytes_dumped < length) {
        size_t want = 16;
        if (has_length && bytes_dumped + want > length) {
            want = (size_t)(length - bytes_dumped);
        }
        axl_ssize_t got = axl_read(axl_stdin, buf, want);
        if (got <= 0) break;
        print_hex_line(bytes_dumped, buf, (size_t)got);
        bytes_dumped += (uint64_t)got;
    }
    return 0;
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
    if (path == NULL) {
        return run_hexdump_stdin(offset, length, has_length, verbose);
    }

    /* Get file size */
    file_size = 0;
    if (axl_file_info(path, &fi) == AXL_OK) {
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

AXL_TOOL_MAIN(hexdump)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "Hexdump",
        .help         = "Hex+ASCII file dump",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_hexdump,
    });
}
