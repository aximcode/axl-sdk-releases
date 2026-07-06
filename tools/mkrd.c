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
      mkrd.efi <label> [-s size] [-m name]  Create RAM disk (default 4 MB).
                                    Maps it as <name>: (or the next free fsN)
                                    via SetMap, and sets shell var %<label>%.
      mkrd.efi -l                   List existing RAM disks
      mkrd.efi -d <label>           Destroy RAM disk by label
      mkrd.efi --driver <name>      Override embedded RamDiskDxe with
                                    a copy named <name> on disk
      mkrd.efi -h                   Help

    Scriptable create (non-interactive startup.nsh, read-only boot media) —
    ONE call, no `map -r`:
      mkrd.efi RAMDISK              -> maps to the next free fsN + sets %RAMDISK%
      %RAMDISK%:                    -> switch to the writable RAM disk (bare
                                       `fsN:` verb, NOT `cd` — cd is intra-fs)
      echo ... > last.nsh           -> write where the boot FD is read-only
      -- or pin a fixed name --
      mkrd.efi RAMDISK -m RD        -> maps as RD: (fails if RD: already taken)
      RD:                           -> switch to it

    Why no `map -r`: mkrd assigns the shell map name itself via
    EFI_SHELL_PROTOCOL.SetMap, which targets the shell's global map (not a
    nested Execute), so the name is usable by the launching script
    immediately — the same approach the old EFI Toolkit MKRAMDISK took by
    accepting the map name as an argument. `-m <name>` fails if the name is
    already in use; without it, the lowest free fsN is chosen. Idempotent on
    the label, so a startup.nsh may re-run it each boot.

    Caveat: a later `map -r` in the same script rebuilds the shell map from
    scratch and may drop or renumber the SetMap'd name. The one-call workflow
    above never runs `map -r`, so this is a non-issue there — but if you mix
    mkrd with a subsequent `map -r`, don't assume the assigned name survives it.

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

/* Shell map-name buffer capacity; the usable bare name is one less (NUL). */
#define MKRD_MAP_NAME_CAP     16

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
    { .name = "map",     .short_name = 'm', .type = AXL_ARG_STRING,
      .help = "Shell map name for the new disk (e.g. RD); default: next free fsN" },
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

/* Highest fsN the auto-pick scan will consider when hunting a free slot. */
#define MKRD_MAX_FS_SCAN  256

/* Choose the shell map name for the new disk: the caller's --map name if given
   (already length- and clash-validated by the caller before the disk was
   created), else the lowest free fsN. Writes the chosen bare name (no ':') to
   @p out. Returns AXL_OK, or AXL_ERR when no free fsN was found. */
static int
pick_map_name(
    const char *requested,
    char       *out,
    size_t      out_size
    )
{
    if (requested != NULL) {
        axl_snprintf(out, out_size, "%s", requested);
        return AXL_OK;
    }
    /* Auto-pick the lowest free fsN (mirrors the shell's own fsN namespace, so
       existing volumes keep their numbers and the disk gets the next slot).
       Bounded scan; in practice the first free slot is fs0..fs2. */
    for (uint32_t i = 0; i < MKRD_MAX_FS_SCAN; i++) {
        char cand[MKRD_MAP_NAME_CAP];
        axl_snprintf(cand, sizeof(cand), "fs%u", i);
        if (!axl_volume_map_taken(cand)) {
            axl_snprintf(out, out_size, "%s", cand);
            return AXL_OK;
        }
    }
    axl_printf("MkRd: no free fsN slot below %u.\n", (unsigned)MKRD_MAX_FS_SCAN);
    return AXL_ERR;
}

/* Validate a caller-requested --map name BEFORE the disk is created, so a bad
   or taken name never leaks a RAM disk. Returns AXL_OK if usable. */
static int
validate_requested_map_name(
    const char *name
    )
{
    if (name == NULL) {
        return AXL_OK;   /* auto-pick path; nothing to validate */
    }
    /* Must fit the map-name buffer un-truncated (bare name + NUL). */
    if (axl_strlen(name) >= MKRD_MAP_NAME_CAP) {
        axl_printf("MkRd: map name \"%s\" is too long (max %d characters).\n",
                   name, MKRD_MAP_NAME_CAP - 1);
        return AXL_ERR;
    }
    if (axl_volume_map_taken(name)) {
        axl_printf("MkRd: shell map name \"%s\" is already in use.\n", name);
        return AXL_ERR;
    }
    return AXL_OK;
}

static int
cli_create(
    const char *label,
    size_t      size_mb,
    const char *map_name
    )
{
    /* Validate a requested --map name (length + not-taken) BEFORE allocating
       the disk, so a bad/clashing name never leaks a RAM disk. (Auto-pick
       can't clash, so it needs no pre-check.) */
    if (validate_requested_map_name(map_name) != AXL_OK) {
        return 1;
    }

    void *dev_path = NULL;
    /* Idempotent on the label: an existing same-label disk returns AXL_OK and
       its device path, so a startup.nsh that re-runs mkrd every boot is safe. */
    if (axl_ramdisk_create(label, size_mb, &dev_path) != AXL_OK) {
        axl_printf("MkRd: failed to create RAM disk \"%s\" (%zu MB).\n",
                   label, size_mb);
        return 1;
    }
    /* FAT16 below the 512 MB cutover, FAT32 at/above it (mirrors the
       library's choice). */
    const char *fs_type = (size_mb <= 512) ? "FAT16" : "FAT32";

    /* Make the disk usable from a non-interactive .nsh in ONE call: assign it a
       shell map name via EFI_SHELL_PROTOCOL.SetMap. Unlike a firmware connect +
       `map -r` cycle, SetMap targets the shell's global map, so the name works
       in the launching script IMMEDIATELY — no `map -r`, no non-deterministic
       fsN to discover (this is what the old EFI Toolkit MKRAMDISK did by taking
       the map name as an argument). */
    char name[MKRD_MAP_NAME_CAP];
    if (pick_map_name(map_name, name, sizeof(name)) != AXL_OK) {
        return 1;
    }
    if (axl_volume_set_map(dev_path, name) != AXL_OK) {
        axl_printf("MkRd: RAM disk \"%s\" created (%zu MB %s) but could not map "
                   "it as \"%s:\" (no shell / SetMap). Run 'map -r' and locate "
                   "it manually.\n", label, size_mb, fs_type, name);
        return 1;
    }
    /* Also publish %<label>% = the map name, so a script can use either the
       fixed `%<label>%:` or the literal `<name>:`. */
    bool var_ok = (axl_setenv(label, name, true) == AXL_OK);

    /* Success summary — label, size/format, shell mapping, backing device
       path (VirtualDisk(<phys>,...), varies per boot), and the shell var. */
    AXL_AUTO_FREE char *dp_text = axl_device_path_to_text(dev_path);
    axl_printf("RAM disk created:\n");
    axl_printf("  label   : %s\n", label);
    axl_printf("  size    : %zu MB (%s)\n", size_mb, fs_type);
    axl_printf("  mapping : %s:%s\n", name,
               var_ok ? "" : "   (note: %VAR% not set)");
    if (var_ok) {
        axl_printf("  var     : %%%s%% = %s\n", label, name);
    }
    if (dp_text != NULL) {
        axl_printf("  device  : %s\n", dp_text);
    }
    axl_printf("Ready: use  %s:  now - no 'map -r' needed.\n", name);
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
    return cli_create(label, size_mb, axl_args_get_string(a, "map"));
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
