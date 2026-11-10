/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-efi-pack.c
    Host packer for compressed self-extracting .efi images.

    Reads the PE Machine of an input image, picks the matching branch filter,
    builds the packed payload through the format module and decides whether
    wrapping is worth it. THE WRAP THRESHOLD LIVES HERE AND NOWHERE ELSE:
    callers (the CMake build, axl-cc) pass no tuning flags.

      wrap iff  raw - final >= 16384  AND  final <= 0.80 * raw
      final  =  stub_size + align8(40 + packed)

    Exit status: 0 wrapped (OUT.pack written), 3 kept raw (nothing written),
    2 refusal or error.

    Usage:
      axl-efi-pack [--force] --stub-size BYTES -o OUT.pack IN.efi
      axl-efi-pack --verify OUT.pack IN.efi        (test hook; not documented)
**/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/pack/axl-pack-format.h"

#define EXIT_WRAPPED  0
#define EXIT_REFUSED  2
#define EXIT_KEPT     3

#define WRAP_MIN_SAVING   16384LL
#define WRAP_RATIO_NUM    80LL    /* final must be <= 80% of raw */
#define WRAP_RATIO_DEN    100LL

/* --stub-size range: a stub is never empty, and 1 GiB is far past any image
   (AXL_PACK_MAX_RAW is 64 MiB) while keeping every size sum in range. */
#define STUB_SIZE_MIN     1LL
#define STUB_SIZE_MAX     (1LL << 30)

#define PE_OFF_LFANEW     0x3Cu
#define PE_MIN_HDR        0x40u

#define USAGE "usage: axl-efi-pack [--force] --stub-size BYTES -o OUT.pack IN.efi"

static void *tool_alloc(void *ctx, size_t n);
static void  tool_free(void *ctx, void *p);
static int   read_file(const char *path, uint8_t **out, size_t *len);
static int   write_file(const char *path, const void *buf, size_t len);
static const char *base_name(const char *path);
static int   pe_machine(const uint8_t *buf, size_t len, uint16_t *machine);
static int   machine_filter(uint16_t machine, AxlPackFilter *filter);
static int   parse_stub_size(const char *arg, long long *out);
static int   same_file(const char *a, const char *b);
static int   do_pack(int force, long long stub, const char *out, const char *in);
static int   do_verify(const char *packfile, const char *in);

static const AxlPackAlloc g_alloc = { tool_alloc, tool_free, NULL };

static void *
tool_alloc(
    void   *ctx,
    size_t  n)
{
    (void)ctx;
    return malloc(n);
}

static void
tool_free(
    void *ctx,
    void *p)
{
    (void)ctx;
    free(p);
}

static int
read_file(
    const char  *path,
    uint8_t    **out,
    size_t      *len)
{
    FILE *f = fopen(path, "rb");
    long  n;
    uint8_t *buf;

    if (f == NULL) {
        fprintf(stderr, "axl-efi-pack: %s: cannot open\n", path);
        return -1;
    }
    n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "axl-efi-pack: %s: cannot size\n", path);
        fclose(f);
        return -1;
    }
    buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "axl-efi-pack: %s: read failed\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *len = (size_t)n;
    return 0;
}

static int
write_file(
    const char *path,
    const void *buf,
    size_t      len)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL || fwrite(buf, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "axl-efi-pack: %s: write failed\n", path);
        remove(path);   /* never leave a partial pack for a make rule to trust */
        return -1;
    }
    return 0;
}

static const char *
base_name(
    const char *path)
{
    const char *s = strrchr(path, '/');
    return s != NULL ? s + 1 : path;
}

/* e_lfanew + "PE\0\0" + Machine; nothing else is read. */
static int
pe_machine(
    const uint8_t *buf,
    size_t         len,
    uint16_t      *machine)
{
    uint32_t pe;

    if (len < PE_MIN_HDR || buf[0] != 'M' || buf[1] != 'Z') {
        return -1;
    }
    pe = (uint32_t)buf[PE_OFF_LFANEW] | (uint32_t)buf[PE_OFF_LFANEW + 1] << 8 |
         (uint32_t)buf[PE_OFF_LFANEW + 2] << 16 | (uint32_t)buf[PE_OFF_LFANEW + 3] << 24;
    if ((uint64_t)pe + 6 > len || memcmp(buf + pe, "PE\0\0", 4) != 0) {
        return -1;
    }
    *machine = (uint16_t)(buf[pe + 4] | buf[pe + 5] << 8);
    return 0;
}

static int
machine_filter(
    uint16_t       machine,
    AxlPackFilter *filter)
{
    if (machine == AXL_PACK_MACHINE_X64) {
        *filter = AXL_PACK_FILTER_X86;
    } else if (machine == AXL_PACK_MACHINE_AA64) {
        *filter = AXL_PACK_FILTER_ARM64;
    } else {
        return -1;
    }
    return 0;
}

/* A whole decimal byte count in [STUB_SIZE_MIN, STUB_SIZE_MAX]: digits only
   (no sign, no trailing junk), then the range. The range check also refuses
   the two values strtoll invents: 0 for the empty string, and LLONG_MAX for
   an overflow (C11 7.22.1.4 saturates), so no ERANGE test is needed -- which
   matters, because this tool is built with -Ideps/lzma, whose errno.h is the
   UEFI stand-in and has no errno. */
static int
parse_stub_size(
    const char *arg,
    long long  *out)
{
    size_t    n = 0;
    long long v = 0;

    while (arg[n] >= '0' && arg[n] <= '9') {
        n++;
    }
    if (arg[n] != '\0') {
        return -1;
    }
    v = strtoll(arg, NULL, 10);
    if (v < STUB_SIZE_MIN || v > STUB_SIZE_MAX) {
        return -1;
    }
    *out = v;
    return 0;
}

/* 1 when both paths name one existing file (any spelling, or a hard link). */
static int
same_file(
    const char *a,
    const char *b)
{
    struct stat sa;
    struct stat sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0
        && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int
do_pack(
    int         force,
    long long   stub,
    const char *out,
    const char *in)
{
    uint8_t      *raw = NULL;
    void         *pk = NULL;
    size_t        raw_len = 0;
    size_t        pk_len = 0;
    uint16_t      machine = 0;
    AxlPackFilter filter;
    AxlPackStatus st;
    long long     rawn;
    long long     final;
    long long     saved;
    int           rc = EXIT_REFUSED;

    /* Checked before the remove below, which would otherwise delete the
       input before reading it. */
    if (same_file(out, in)) {
        fprintf(stderr, "axl-efi-pack: -o %s is the input file %s\n", out, in);
        return EXIT_REFUSED;
    }
    /* OUT.pack exists after this run only if THIS run wrapped: a stale pack
       from an earlier wrap must not survive a keep or a refusal. */
    remove(out);
    if (read_file(in, &raw, &raw_len) != 0) {
        return EXIT_REFUSED;
    }
    if (pe_machine(raw, raw_len, &machine) != 0) {
        fprintf(stderr, "axl-efi-pack: %s: not a PE/COFF image\n", in);
        goto done;
    }
    if (machine_filter(machine, &filter) != 0) {
        fprintf(stderr, "axl-efi-pack: %s: unsupported PE machine 0x%04x\n", in, machine);
        goto done;
    }
    st = axl_pack_build(raw, raw_len, machine, filter, &g_alloc, &pk, &pk_len);
    if (st != AXL_PACK_OK) {
        fprintf(stderr, "axl-efi-pack: %s: %s\n", in, axl_pack_status_str(st));
        goto done;
    }
    rawn = (long long)raw_len;
    final = stub + (((long long)pk_len + 7) / 8) * 8;
    saved = rawn - final;
    if (force || (saved >= WRAP_MIN_SAVING && final * WRAP_RATIO_DEN <= rawn * WRAP_RATIO_NUM)) {
        if (write_file(out, pk, pk_len) != 0) {
            goto done;
        }
        /* Integer percent, rounded down; a forced loss prints as +N%. */
        if (saved >= 0) {
            printf("axl-efi-pack: %s: wrap %lld -> %lld (-%lld%%)\n", base_name(in), rawn, final,
                   saved * 100 / rawn);
        } else {
            printf("axl-efi-pack: %s: wrap %lld -> %lld (+%lld%%)\n", base_name(in), rawn, final,
                   (-saved) * 100 / rawn);
        }
        rc = EXIT_WRAPPED;
    } else {
        printf("axl-efi-pack: %s: keep raw %lld (saves %lld bytes, under the threshold)\n",
               base_name(in), rawn, saved);
        rc = EXIT_KEPT;
    }
done:
    free(pk);
    free(raw);
    return rc;
}

static int
do_verify(
    const char *packfile,
    const char *in)
{
    uint8_t *pk = NULL;
    uint8_t *raw = NULL;
    void    *back = NULL;
    size_t   pk_len = 0;
    size_t   raw_len = 0;
    size_t   back_len = 0;
    uint16_t machine = 0;
    AxlPackStatus st;
    int      rc = EXIT_REFUSED;

    if (read_file(packfile, &pk, &pk_len) != 0 || read_file(in, &raw, &raw_len) != 0) {
        goto done;
    }
    if (pe_machine(raw, raw_len, &machine) != 0) {
        fprintf(stderr, "axl-efi-pack: %s: not a PE/COFF image\n", in);
        goto done;
    }
    st = axl_pack_unpack(pk, pk_len, machine, &g_alloc, &back, &back_len);
    if (st != AXL_PACK_OK) {
        fprintf(stderr, "axl-efi-pack: verify failed: %s\n", axl_pack_status_str(st));
        goto done;
    }
    if (back_len != raw_len || memcmp(back, raw, raw_len) != 0) {
        fprintf(stderr, "axl-efi-pack: verify failed: unpacked bytes differ from %s\n", in);
        goto done;
    }
    printf("axl-efi-pack: verify ok\n");
    rc = EXIT_WRAPPED;
done:
    free(back);
    free(raw);
    free(pk);
    return rc;
}

int
main(
    int    argc,
    char **argv)
{
    int         force = 0;
    long long   stub = -1;
    const char *out = NULL;
    const char *in = NULL;
    int         i;

    if (argc == 4 && strcmp(argv[1], "--verify") == 0) {
        return do_verify(argv[2], argv[3]);
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--stub-size") == 0 && i + 1 < argc) {
            const char *arg = argv[++i];
            if (parse_stub_size(arg, &stub) != 0) {
                fprintf(stderr, "axl-efi-pack: --stub-size '%s': not a byte count in [%lld, %lld]\n",
                        arg, STUB_SIZE_MIN, STUB_SIZE_MAX);
                return EXIT_REFUSED;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (argv[i][0] != '-' && in == NULL) {
            in = argv[i];
        } else {
            fprintf(stderr, "%s\n", USAGE);
            return EXIT_REFUSED;
        }
    }
    if (stub < 0 || out == NULL || in == NULL) {
        fprintf(stderr, "%s\n", USAGE);
        return EXIT_REFUSED;
    }
    return do_pack(force, stub, out, in);
}
