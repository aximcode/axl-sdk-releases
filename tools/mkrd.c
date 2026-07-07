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
      mkrd.efi <label> [-s size]    Create RAM disk (default 4 MB). Maps it so a
                                    bare `map` shows it immediately as a native
                                    volume: `FS<n>: Alias(s):<label>:`.
      mkrd.efi (no args)            Print this help
      mkrd.efi -l                   List RAM disks (mapping, label/alias, size, fstype)
      mkrd.efi -d <label>           Destroy RAM disk by label (+ unmap its names)
      mkrd.efi --driver <name>      Override embedded RamDiskDxe with
                                    a copy named <name> on disk
      mkrd.efi -h                   Help

    Scriptable create (non-interactive startup.nsh, read-only boot media) —
    ONE call, no `map -r`:
      mkrd.efi SCRATCH             -> shows in `map` as `FS<n>: Alias(s):SCRATCH:`
      FS<n>:  (or)  SCRATCH:       -> switch to the writable RAM disk by either
                                       name (bare `<name>:` verb, NOT `cd`)
      echo ... > last.nsh          -> write where the boot FD is read-only

    How the mapping works: mkrd makes TWO EFI_SHELL_PROTOCOL.SetMap calls on the
    disk's device path — the next-free `FS<n>` name FIRST (so it becomes the
    PRIMARY name a bare `map` displays, matching the shell's own fsN convention)
    and the volume LABEL second (so it lands in the `Alias(s):` column). Both
    `FS<n>:` and `<label>:` are usable paths with no `map -r`. SetMap targets the
    shell's GLOBAL map (not a nested Execute), so the names are usable by the
    launching script immediately. The label is the stable handle to script
    against; the fsN is the native display name.

    The fsN number is the lowest free `FS<n>` at create time — a best-effort
    guess at what the shell would assign, mirroring its namespace. It never
    reuses a taken slot (so the boot volume's fs0 is untouched) and never claims
    an fsN when the shell is unreachable (SetMap fails cleanly; the disk is still
    created — mount via `map -r`).

    Label as alias: the LABEL doubles as the friendly alias when it is a clean
    map token. It is NOT set as an alias (the disk is reachable as `FS<n>:` only,
    with a note) when the label matches the reserved `fs<digits>` namespace, is
    too long, or has characters outside [A-Za-z0-9_-]. There is no `-a/--alias`
    option and no `%<label>%` env var — the label IS the alias.

    Idempotent: a re-run of a still-mapped disk reuses its existing fsN handle —
    no drift, no duplicate — so a startup.nsh may re-run it each boot.

    Caveat: a later `map -r` rebuilds the shell map from scratch and drops the
    SetMap'd names (the disk re-emerges as a firmware-assigned fsN). The one-call
    workflow above never runs `map -r`, so this is a non-issue there.

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
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "label", .type = AXL_ARG_STRING,
      .help = "Volume label (also the shell alias) for the new RAM disk" },
    {0}
};

// ---------------------------------------------------------------------------
// CLI handlers — thin wrappers over the library + presentation
// ---------------------------------------------------------------------------

/* Highest fsN the auto-pick scan will consider when hunting a free slot. */
#define MKRD_MAX_FS_SCAN  256

/* Is @p name a reserved shell fsN name? Normalizes case AND an optional
   trailing ':' so "fs9", "FS9", "fs9:", "FS9:" all match. The `fs<digits>`
   space is the shell's own auto-assigned namespace: mkrd auto-picks a FREE
   fsN for the PRIMARY mapping, but never uses one as the disk's LABEL alias
   (that would put a second fsN-looking name in the map). Bare "fs" / "fs:"
   (no digits) is NOT reserved. */
static bool
is_reserved_fsn_name(
    const char *name
    )
{
    if (name == NULL) {
        return false;
    }
    size_t n = axl_strlen(name);
    if (n > 0 && name[n - 1] == ':') {   /* tolerate a trailing ':' */
        n--;
    }
    if (n < 3
            || (name[0] != 'f' && name[0] != 'F')
            || (name[1] != 's' && name[1] != 'S')) {
        return false;   /* need "fs" + at least one digit */
    }
    for (size_t i = 2; i < n; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

/* Why @p label can't double as the disk's shell alias, or NULL when it can.
   The label is a usable alias only when it is a single clean token the user
   can type as a path: non-empty, not in the shell's reserved fs<digits>
   namespace, short enough for the map-name buffer, and [A-Za-z0-9_-] only. A
   rejected label just isn't set as an alias — the disk is still reachable via
   its fsN primary. */
static const char *
label_alias_reject_reason(
    const char *label
    )
{
    size_t n = (label != NULL) ? axl_strlen(label) : 0;
    if (n == 0) {
        return "empty label";
    }
    if (is_reserved_fsn_name(label)) {
        return "reserved fsN pattern";
    }
    if (n >= MKRD_MAP_NAME_CAP) {
        return "too long for a shell map name";
    }
    for (size_t i = 0; i < n; i++) {
        char c = label[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return "not a clean map token (letters/digits/_/- only)";
        }
    }
    return NULL;
}

/* Pick the lowest free FS<n> shell name (uppercase, matching the shell's own
   FS0/FS1 display) into @p out. Skips taken slots — so the boot volume's fs0 is
   never reused — mirroring how the shell numbers a fresh volume. Returns
   AXL_OK, or AXL_ERR when no slot below MKRD_MAX_FS_SCAN is free. */
static int
pick_free_fsn(
    char   *out,
    size_t  out_size
    )
{
    for (uint32_t i = 0; i < MKRD_MAX_FS_SCAN; i++) {
        char cand[MKRD_MAP_NAME_CAP];
        axl_snprintf(cand, sizeof(cand), "FS%u", i);
        if (!axl_volume_map_taken(cand)) {
            axl_snprintf(out, out_size, "%s", cand);
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

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

    axl_printf("%-11s  %-13s  %7s  %s\n",
               "MAPPING", "LABEL/ALIAS", "SIZE", "FSTYPE");
    axl_printf("-----------  -------------  -------  ------\n");
    size_t shown = (n < sizeof(disks) / sizeof(disks[0]))
                 ? n : sizeof(disks) / sizeof(disks[0]);
    for (size_t i = 0; i < shown; i++) {
        uint64_t    size_mb = disks[i].size_bytes / (1024 * 1024);
        /* FAT16 below the 512 MB cutover, FAT32 at/above it (mirrors create). */
        const char *fs      = (size_mb <= 512) ? "FAT16" : "FAT32";
        /* MAPPING: the disk's current PRIMARY shell name (the fsN we SetMap
           first), or "(unmapped)" if it has none (no shell, or a `map -r`
           dropped the SetMap'd names). */
        char mapping[MKRD_MAP_NAME_CAP + 2];
        char primary[MKRD_MAP_NAME_CAP];
        if (axl_volume_map_alias(disks[i].device_path, primary, sizeof(primary))
                == AXL_OK) {
            axl_snprintf(mapping, sizeof(mapping), "%s:", primary);
        } else {
            axl_snprintf(mapping, sizeof(mapping), "(unmapped)");
        }
        /* LABEL/ALIAS: the FAT label, shown with a trailing ':' only when it is
           CURRENTLY a live shell alias (a clean token AND actually in the map) —
           so it reads as a path you can type right now. A label that can't be an
           alias, or one a `map -r` has since dropped, shows plain; "(unlabeled)"
           for none. */
        char label_disp[MKRD_MAP_NAME_CAP + 2];
        if (disks[i].label[0] == '\0') {
            axl_snprintf(label_disp, sizeof(label_disp), "(unlabeled)");
        } else if (label_alias_reject_reason(disks[i].label) == NULL
                       && axl_volume_map_taken(disks[i].label)) {
            axl_snprintf(label_disp, sizeof(label_disp), "%s:", disks[i].label);
        } else {
            axl_snprintf(label_disp, sizeof(label_disp), "%s", disks[i].label);
        }
        char size_disp[16];
        axl_snprintf(size_disp, sizeof(size_disp), "%lluMB",
                     (unsigned long long)size_mb);
        axl_printf("%-11s  %-13s  %7s  %s\n", mapping, label_disp, size_disp, fs);
    }
    return 0;
}

static int
cli_destroy(
    const char *label
    )
{
    /* Remove every shell map name pointing at this disk's device BEFORE freeing
       it (the device path is freed on destroy, and a lingering name would then
       dangle at freed memory). mkrd may have set two names (the fsN primary + the
       LABEL alias). We drop them by re-reading the device's OWN current name each
       pass (map_alias matches by device path), so we only ever unmap names that
       belong to THIS device — never another volume's same-spelled entry — however
       many there are. The cap is a runaway guard, not an expected limit. */
    void *dev_path = NULL;
    bool  unmapped = false;
    if (axl_ramdisk_find(label, &dev_path) == AXL_OK) {
        char name[MKRD_MAP_NAME_CAP];
        for (int i = 0; i < 8
                && axl_volume_map_alias(dev_path, name, sizeof(name)) == AXL_OK;
                i++) {
            if (axl_volume_unmap(name) != AXL_OK) {
                break;   /* couldn't remove it — stop rather than spin */
            }
            unmapped = true;
        }
    }

    if (axl_ramdisk_destroy(label) != AXL_OK) {
        axl_printf("MkRd: no RAM disk with label \"%s\" found.\n", label);
        return 1;
    }
    if (unmapped) {
        axl_printf("RAM disk \"%s\" destroyed (unmapped shell alias).\n", label);
    } else {
        axl_printf("RAM disk \"%s\" destroyed.\n", label);
    }
    return 0;
}

static int
cli_create(
    const char *label,
    size_t      size_mb
    )
{
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

    /* Does this (possibly pre-existing) disk already carry a shell mapping? A
       fresh create does not touch the shell's global map, so map_alias resolves
       to AXL_ERR and we assign names below; a re-run of a still-mapped disk
       reuses its existing fsN primary (idempotent — no drift, no duplicate). */
    char primary[MKRD_MAP_NAME_CAP];
    bool reused = (axl_volume_map_alias(dev_path, primary, sizeof(primary))
                       == AXL_OK);

    /* Whether the LABEL is (or becomes) a usable alias of THIS disk, resolved
       against real map state — never advertised in the summary unless actually
       set. `reject` explains why not, for the note. */
    const char *reject      = label_alias_reject_reason(label);
    bool        label_alias = false;

    if (reused) {
        /* Re-run: names already set. The label is an alias iff it is a valid
           token AND still present in the map (a prior `map -r` may have dropped
           it, or the original label SetMap may have been skipped/failed). */
        label_alias = (reject == NULL) && axl_volume_map_taken(label);
    } else {
        /* Two SetMap calls, ORDER-SENSITIVE: the next-free FS<n> FIRST so it is
           the PRIMARY name a bare `map` shows (the shell displays the
           first-inserted name as primary), then the LABEL so it lands in the
           Alias(s) column. Both target the shell's global map, so both are
           usable immediately — no `map -r`. */
        if (pick_free_fsn(primary, sizeof(primary)) != AXL_OK) {
            axl_printf("MkRd: no free fsN slot below %u.\n",
                       (unsigned)MKRD_MAX_FS_SCAN);
            return 1;
        }
        /* Distinguish WHY the map failed: AXL_UNSUPPORTED means no
           EFI_SHELL_PROTOCOL was locatable (get_shell() == NULL — the shell's
           map is unreachable, which also makes pick_free_fsn see every fsN as
           free); AXL_ERR means the shell is present but rejected SetMap. The
           distinction tells a real-hardware report which half broke. Either way
           the disk exists and is mountable via 'map -r'. */
        int map_rc = axl_volume_set_map(dev_path, primary);
        if (map_rc != AXL_OK) {
            const char *why = (map_rc == AXL_UNSUPPORTED)
                ? "no EFI_SHELL_PROTOCOL on this firmware (shell map unavailable)"
                : "the shell rejected SetMap for this name";
            axl_printf("MkRd: RAM disk \"%s\" created (%zu MB %s) but could not "
                       "auto-map it as \"%s:\" - %s. The disk exists; run 'map -r' "
                       "to mount it.\n", label, size_mb, fs_type, primary, why);
            return 1;
        }
        /* Second name = the LABEL, in the Alias(s) column — but only when it is
           a clean token AND not already mapped to another volume (SetMap does an
           unconditional insert, so it would silently repoint that name at this
           disk). Reflect the real outcome in label_alias so the summary never
           advertises a name we didn't actually set. */
        if (reject == NULL) {
            if (axl_volume_map_taken(label)) {
                reject = "name already mapped to another volume";
            } else if (axl_volume_set_map(dev_path, label) != AXL_OK) {
                reject = "the shell rejected SetMap for the label";
            } else {
                label_alias = true;
            }
        }
    }

    /* Success summary — label, size/format, the shell mapping (fsN primary, with
       the label alias when set), and the backing device path (varies per boot). */
    AXL_AUTO_FREE char *dp_text = axl_device_path_to_text(dev_path);
    axl_printf("%s\n", reused ? "RAM disk reused (already mapped):"
                              : "RAM disk created:");
    axl_printf("  label   : %s\n", label);
    axl_printf("  size    : %zu MB (%s)\n", size_mb, fs_type);
    if (label_alias) {
        axl_printf("  mapping : %s: (alias %s:)\n", primary, label);
    } else {
        axl_printf("  mapping : %s:\n", primary);
    }
    if (dp_text != NULL) {
        axl_printf("  device  : %s\n", dp_text);
    }
    /* A label that can't be an alias: say so, so it isn't a surprise that only
       the fsN works as a path. (reject is NULL on the reused path when the label
       is a valid token that just isn't currently mapped — no note needed then.) */
    if (!label_alias && reject != NULL) {
        axl_printf("MkRd: label \"%s\" is not a usable shell alias (%s); "
                   "reachable as %s: only.\n", label, reject, primary);
    }
    if (label_alias) {
        axl_printf("Ready: use  %s:  or  %s:  now - no 'map -r' needed.\n",
                   primary, label);
    } else {
        axl_printf("Ready: use  %s:  now - no 'map -r' needed.\n", primary);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_mkrd(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");

    /* Keep the tool quiet by default: suppress library INFO/DEBUG chatter — e.g.
       the driver-ensure "loaded '<embedded>' handle=..." line from
       axl_ramdisk_ensure_driver — so a plain `mkrd <label>` prints only its own
       result. -v opens the library log up to DEBUG so that diagnostic is
       available on demand (alongside the -v protocol probes below). */
    axl_log_set_level(verbose ? AXL_LOG_DEBUG : AXL_LOG_WARNING);

    /* AXL_DIAG-gated cross-tool startup dump (no-op when env unset). */
    axl_diag_startup(g_argc, g_argv);

    /* Resolve the mode up front. With no action requested (no label, no
       --list, no --destroy) print the help and stop — WITHOUT loading the RAM
       disk driver, which only the real modes below need. (Usage error: bare
       `mkrd` returns non-zero; explicit `-h` is handled by the args framework
       and returns 0.) */
    bool        want_list     = axl_args_get_bool(a, "list");
    const char *destroy_label = axl_args_get_string(a, "destroy");
    const char *label         = axl_args_get_string(a, "label");
    if (!want_list && destroy_label == NULL && label == NULL) {
        axl_args_print_help(a);
        return 1;
    }

    if (verbose) {
        /* The EFI_RAM_DISK_PROTOCOL state at entry tells you whether
         * ensure-with-embedded had work to do or the firmware already
         * published it. For library-level debug, set AXL_LOG_LEVEL=debug. */
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL");
    }

    /* All real modes (list / destroy / create) need EFI_RAM_DISK_PROTOCOL:
     * firmware → --driver override / disk search → embedded RamDiskDxe blob. */
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

    if (want_list) {
        return cli_list();
    }
    if (destroy_label != NULL) {
        return cli_destroy(destroy_label);
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
    /* Quiet by default BEFORE arg parsing, so even framework-handled paths
       (-h / usage errors) don't emit library INFO/DEBUG chatter. run_mkrd
       re-affirms this and bumps to DEBUG when -v is given. */
    axl_log_set_level(AXL_LOG_WARNING);
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "MkRd",
        .help         = "Create / list / destroy RAM disks via EFI_RAM_DISK_PROTOCOL",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_mkrd,
    });
}
