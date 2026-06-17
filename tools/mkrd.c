/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file mkrd.c
    Create/list/destroy FAT16/FAT32 RAM disks — a thin CLI over the
    AxlRamDisk library (`<axl/axl-ramdisk.h>`). The create/format/
    register/enumerate orchestration lives in the library so other
    consumers reuse it; this tool is argument parsing + presentation.

    Build with axl-cc:
      axl-cc mkrd.c -o mkrd.efi

    Usage:
      mkrd.efi <label> [-s size]    Create RAM disk (default 4 MB)
      mkrd.efi -l                   List existing RAM disks
      mkrd.efi -d <label>           Destroy RAM disk by label
      mkrd.efi --driver <name>      Override embedded RamDiskDxe with
                                    a copy named <name> on disk
      mkrd.efi -h                   Help

    Driver discovery: EFI_RAM_DISK_PROTOCOL is optional in UEFI 2.6+
    and absent on some firmware. When it isn't already registered,
    `axl_ramdisk_ensure_driver` auto-loads RamDiskDxe — firmware
    short-circuit, then a disk search (or the --driver override), then
    the embedded copy baked into mkrd at build time
    (third_party/edk2/RamDiskDxe-<arch>.efi). The embedded fallback
    makes mkrd self-contained on minimal firmware.
**/

#include <axl.h>
#include <uefi/axl-uefi.h>   /* EFI_RAM_DISK_PROTOCOL_GUID for the -v probe */

/* Embedded RamDiskDxe.efi blob — emitted into .rodata by
 * tools/mkrd-blob.S via the GNU assembler's `.incbin` directive
 * (the per-arch path is supplied by the Makefile). Passed to
 * axl_ramdisk_ensure_driver as the embedded fallback. */
AXL_EMBED_DECLARE(ramdiskdxe);

#define MKRD_DEFAULT_SIZE_MB  4
#define MKRD_MIN_SIZE_MB      1
#define MKRD_MAX_SIZE_MB      32768

static bool   verbose = false;
static int    g_argc;
static char **g_argv;

static const AxlArgDesc flags[] = {
    { .name = "size",    .short_name = 's', .type = AXL_ARG_U32, .base = 0,
      .min = MKRD_MIN_SIZE_MB, .max = MKRD_MAX_SIZE_MB,
      .help = "Size in MB (default 4)" },
    { .name = "destroy", .short_name = 'd', .type = AXL_ARG_STRING,
      .help = "Destroy RAM disk by volume label" },
    { .name = "list",    .short_name = 'l', .type = AXL_ARG_BOOL,
      .help = "List existing RAM disks" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output" },
    { .name = "driver",  .type = AXL_ARG_STRING,
      .help = "Driver name to load instead of embedded RamDiskDxe" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "label", .type = AXL_ARG_STRING,
      .help = "Volume label for the new RAM disk (create mode)" },
    {0}
};

// ---------------------------------------------------------------------------
// CLI handlers — thin wrappers over the library + presentation
// ---------------------------------------------------------------------------

static int
cli_list(void)
{
    AxlRamDisk disks[16];
    size_t     n = 0;
    if (axl_ramdisk_list(disks, sizeof(disks) / sizeof(disks[0]), &n) != AXL_OK) {
        axl_printf("No filesystems found.\n");
        return 1;
    }
    if (n == 0) {
        axl_printf("No RAM disks found.\n");
        return 0;
    }

    axl_printf("LABEL        SIZE     \n");
    axl_printf("-----------  -------\n");
    size_t shown = (n < sizeof(disks) / sizeof(disks[0]))
                 ? n : sizeof(disks) / sizeof(disks[0]);
    for (size_t i = 0; i < shown; i++) {
        const char *label = (disks[i].label[0] != '\0')
                          ? disks[i].label : "(unlabeled)";
        axl_printf("%-11s  %4llu MB\n", label,
                   (unsigned long long)(disks[i].size_bytes / (1024 * 1024)));
    }
    return 0;
}

static int
cli_destroy(
    const char *label
    )
{
    if (axl_ramdisk_destroy(label) != AXL_OK) {
        axl_printf("MkRd: no RAM disk with label \"%s\" found.\n", label);
        return 1;
    }
    axl_printf("RAM disk \"%s\" destroyed.\n", label);
    return 0;
}

static int
cli_create(
    const char *label,
    size_t      size_mb
    )
{
    void *dev_path = NULL;
    if (axl_ramdisk_create(label, size_mb, &dev_path) != AXL_OK) {
        axl_printf("MkRd: failed to create RAM disk \"%s\" (%zu MB).\n",
                   label, size_mb);
        return 1;
    }
    /* FAT16 below the 512 MB cutover, FAT32 at/above it (mirrors the
       library's choice). */
    const char *fs_type = (size_mb <= 512) ? "FAT16" : "FAT32";
    axl_printf("RAM disk \"%s\" created: %zu MB %s\n", label, size_mb, fs_type);
    axl_printf("Run 'map -r' to refresh drive mappings.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_mkrd(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");

    /* AXL_DIAG-gated cross-tool startup dump (no-op when env unset). */
    axl_diag_startup(g_argc, g_argv);

    if (verbose) {
        /* The EFI_RAM_DISK_PROTOCOL state at entry tells you whether
         * ensure-with-embedded had work to do or the firmware already
         * published it. For library-level debug, set AXL_LOG_LEVEL=debug. */
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL");
    }

    /* All non-help modes need EFI_RAM_DISK_PROTOCOL: firmware →
     * --driver override / disk search → embedded RamDiskDxe blob. */
    const char *driver_override = axl_args_get_string(a, "driver");
    if (axl_ramdisk_ensure_driver(
            AXL_EMBED_DATA(ramdiskdxe), AXL_EMBED_SIZE(ramdiskdxe),
            driver_override) != AXL_OK) {
        if (driver_override != NULL) {
            axl_printf("MkRd: '%s' not found on any mounted volume "
                       "(--driver overrides embedded fallback).\n",
                       driver_override);
        } else {
            axl_printf("MkRd: failed to register EFI_RAM_DISK_PROTOCOL "
                       "(disk search and embedded fallback both failed).\n");
        }
        return 1;
    }

    if (verbose) {
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL (post-ensure)");
    }

    if (axl_args_get_bool(a, "list")) {
        return cli_list();
    }
    const char *destroy_label = axl_args_get_string(a, "destroy");
    if (destroy_label != NULL) {
        return cli_destroy(destroy_label);
    }

    const char *label = axl_args_get_string(a, "label");
    if (label == NULL) {
        axl_printf("MkRd: label required (or use --list / --destroy)\n");
        return 1;
    }
    uint32_t size_mb = (uint32_t)axl_args_get_uint(a, "size");
    if (size_mb == 0) {
        size_mb = MKRD_DEFAULT_SIZE_MB;
    }
    return cli_create(label, size_mb);
}

AXL_TOOL_MAIN(mkrd)
{
    g_argc = argc;
    g_argv = argv;
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "MkRd",
        .help         = "Create / list / destroy RAM disks via EFI_RAM_DISK_PROTOCOL",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_mkrd,
    });
}
