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
      mkrd.efi <label> [-s size] [-a alias]  Create RAM disk (default 4 MB).
                                    Maps it as <label>: (or <alias>: with -a, or
                                    a free fsN) via SetMap, sets var %<label>%.
      mkrd.efi (no args)            Print this help
      mkrd.efi -l                   List existing RAM disks
      mkrd.efi -d <label>           Destroy RAM disk by label (+ unmap its alias)
      mkrd.efi --driver <name>      Override embedded RamDiskDxe with
                                    a copy named <name> on disk
      mkrd.efi -h                   Help

    Scriptable create (non-interactive startup.nsh, read-only boot media) —
    ONE call, no `map -r`:
      mkrd.efi RAMDISK              -> maps as RAMDISK: (the label) + sets %RAMDISK%
      %RAMDISK%:                    -> switch to the writable RAM disk (bare
                                       `<name>:` verb, NOT `cd` — cd is intra-fs)
      echo ... > last.nsh           -> write where the boot FD is read-only
      -- or pin a different alias --
      mkrd.efi SCRATCH -a RD        -> maps as RD: (fails if RD: already taken)
      RD:                           -> switch to it

    The label vs the alias: the positional <label> is the FAT volume label AND
    the shell var %<label>%; the map ALIAS (what you type to switch to it) is,
    by default, the label itself — so `mkrd RD` gives you `RD:` with no `-a`.
    `-a <alias>` overrides the alias explicitly.

    Alias selection (no `-a`): the label is used as the alias when it is a
    clean, free custom name; it FALLS BACK to the lowest free fsN (with a note)
    when the label
      - matches the shell's reserved `fs<digits>` namespace, case- and
        `:`-insensitive (so `mkrd fs2` / `FS2` never shadows or clobbers a
        real fs2 / the boot volume),
      - is already in use by another volume (never clobbers it), or
      - is too long / has shell-unsafe characters (only [A-Za-z0-9_-]).
    An explicit `-a fsN` (a reserved name) is a hard error, not a fallback.

    Why no `map -r`: mkrd assigns the shell map name itself via
    EFI_SHELL_PROTOCOL.SetMap, which targets the shell's global map (not a
    nested Execute), so the name is usable by the launching script
    immediately — the same approach the old EFI Toolkit MKRAMDISK took by
    accepting the map name as an argument. Idempotent on the label AND its map:
    a re-run reuses the disk's existing alias in ANY form (label, fsN, or a
    custom -a name) — no drift, no duplicate alias — and republishes %<label>%,
    so a startup.nsh may re-run it each boot. A `-a <alias>` on an already-mapped
    disk keeps the existing alias and reports that <name> was not added.
    Auto-mapping needs EFI_SHELL_PROTOCOL; without it the disk is still created
    (mount via `map -r`) and mkrd says so.

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
    { .name = "alias",   .short_name = 'a', .type = AXL_ARG_STRING,
      .help = "Shell map alias override (e.g. RD); default: the label itself, "
              "else a free fsN" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "label", .type = AXL_ARG_STRING,
      .help = "Volume label + %VAR% + default map alias for the new RAM disk" },
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
    /* Capture the disk's current shell alias BEFORE destroying it — the
       device path is freed on destroy, so we must read it first. After the
       disk is gone we remove the now-dangling mapping (SetMap leaves the alias
       pointing at freed memory otherwise, so a later bare `<alias>:` would
       dereference it). Best-effort: no alias / no shell just skips cleanup. */
    char  alias[MKRD_MAP_NAME_CAP];
    bool  have_alias = false;
    void *dev_path   = NULL;
    if (axl_ramdisk_find(label, &dev_path) == AXL_OK
            && axl_volume_map_alias(dev_path, alias, sizeof(alias)) == AXL_OK) {
        have_alias = true;
    }

    if (axl_ramdisk_destroy(label) != AXL_OK) {
        axl_printf("MkRd: no RAM disk with label \"%s\" found.\n", label);
        return 1;
    }
    if (have_alias) {
        (void)axl_volume_unmap(alias);   /* drop the now-dangling shell alias */
        axl_printf("RAM disk \"%s\" destroyed (unmapped \"%s:\").\n",
                   label, alias);
    } else {
        axl_printf("RAM disk \"%s\" destroyed.\n", label);
    }
    return 0;
}

/* Highest fsN the auto-pick scan will consider when hunting a free slot. */
#define MKRD_MAX_FS_SCAN  256

/* Is @p name a reserved shell fsN name? Normalizes case AND an optional
   trailing ':' so "fs9", "FS9", "fs9:", "FS9:" all match. The `fs<digits>`
   space is the shell's own auto-assigned namespace: mkrd auto-picks a FREE
   fsN as a fallback, but never lets a caller CLAIM a specific one (as a label
   default or an explicit -a) — that could shadow a real fsN or repoint the
   boot volume at the RAM disk. Bare "fs" / "fs:" (no digits) is NOT reserved. */
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

/* Is @p label a clean custom map name — safe to use as the disk's shell alias?
   Rejects the reserved fs<digits> namespace (is_reserved_fsn_name), anything
   that won't fit the map-name buffer, and any name with shell-unsafe
   characters (only [A-Za-z0-9_-], so the alias is a single clean token the
   user can type). Rejected labels fall back to an auto-picked fsN. */
static bool
label_usable_as_map_name(
    const char *label
    )
{
    size_t n = axl_strlen(label);
    if (n == 0 || n >= MKRD_MAP_NAME_CAP || is_reserved_fsn_name(label)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = label[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Choose the shell map name for a freshly-created (unmapped) disk, in order:
     1. an explicit -a NAME (already length/reserved/clash-validated), else
     2. the LABEL itself when it is a clean custom name (label_usable_as_map_name)
        AND not already taken by another volume — so `mkrd RD` maps as `RD:`
        with no `-m` needed, else
     3. the lowest free fsN (the label is fs-pattern, taken, or shell-unsafe).
   Writes the bare name (no ':') to @p out and, when it falls back for a taken
   or unsafe label (not merely fs-pattern), notes why. Returns AXL_OK, or
   AXL_ERR when no free fsN slot was found. */
static int
choose_map_name(
    const char *requested,
    const char *label,
    char       *out,
    size_t      out_size
    )
{
    if (requested != NULL) {
        axl_snprintf(out, out_size, "%s", requested);
        return AXL_OK;
    }
    /* Default the alias to the label when it is a clean, free, non-reserved
       name; otherwise fall back to a free fsN and say WHY (a temporary RAM
       disk should never fail on a harmless label — just explain the fallback
       so %<label>% resolving to an fsN isn't a surprise). */
    if (label_usable_as_map_name(label)) {
        if (!axl_volume_map_taken(label)) {
            axl_snprintf(out, out_size, "%s", label);
            return AXL_OK;
        }
        axl_printf("MkRd: \"%s:\" is already in use by another volume; "
                   "mapping this disk to a free fsN instead.\n", label);
    } else if (is_reserved_fsn_name(label)) {
        axl_printf("MkRd: label \"%s\" looks like a reserved fsN name; the "
                   "alias falls back to a free fsN (fsN names belong to the "
                   "shell). Use -a for a custom alias.\n", label);
    } else {
        axl_printf("MkRd: label \"%s\" is not usable as an alias (max %d chars, "
                   "letters/digits/_/- only); mapping to a free fsN instead.\n",
                   label, MKRD_MAP_NAME_CAP - 1);
    }
    /* Fall back to the lowest free fsN. Mirrors the shell's own fsN namespace,
       so existing volumes keep their numbers and the disk gets the next slot. */
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

/* Validate a caller-requested -a/--alias BEFORE the disk is created, so a bad
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
        axl_printf("MkRd: alias \"%s\" is too long (max %d characters).\n",
                   name, MKRD_MAP_NAME_CAP - 1);
        return AXL_ERR;
    }
    /* Reserved fsN names belong to the shell's auto-namespace — a caller must
       not CLAIM a specific one (case/':'-insensitive: fs9 / FS9 / fs9:). */
    if (is_reserved_fsn_name(name)) {
        axl_printf("MkRd: alias \"%s\" is a reserved fsN name; pick a custom "
                   "alias, or omit -a to auto-assign a free fsN.\n", name);
        return AXL_ERR;
    }
    if (axl_volume_map_taken(name)) {
        axl_printf("MkRd: shell map alias \"%s\" is already in use.\n", name);
        return AXL_ERR;
    }
    return AXL_OK;
}

static int
cli_create(
    const char *label,
    size_t      size_mb,
    const char *alias
    )
{
    /* Validate a requested -a/--alias (length + reserved + not-taken) BEFORE
       allocating the disk, so a bad/clashing name never leaks a RAM disk.
       (The label-default / auto-pick paths need no pre-check.) */
    if (validate_requested_map_name(alias) != AXL_OK) {
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
    char existing[MKRD_MAP_NAME_CAP];
    bool reused = false;
    /* Idempotent on the label AND its shell map. If this (possibly pre-existing)
       disk already carries a shell alias — in ANY form: an fsN, its label, or a
       custom -a name — REUSE that alias instead of assigning a second one. This
       reads the disk's CURRENT alias (axl_volume_map_alias, not the fsN-only
       map_name) so a re-run of any prior mapping style is deduplicated: without
       it, a same-session re-run would drift %<label>% and leave a duplicate
       mapping (the "re-run each boot" idempotency the docs promise). A freshly
       created disk has no shell alias yet (create connects it but does not touch
       the shell's global map), so this resolves to AXL_ERR and the fresh-map
       path below runs unchanged. */
    if (axl_volume_map_alias(dev_path, existing, sizeof(existing)) == AXL_OK) {
        axl_snprintf(name, sizeof(name), "%s", existing);
        reused = true;
        /* An explicit -a NAME on an already-mapped disk: keep the existing alias
           (idempotent) rather than multiplying aliases; note the ignored request
           so the caller isn't surprised the name didn't take. NAME is always a
           different, free name here — a -a NAME equal to the current alias is a
           taken name, which validate_requested_map_name rejected before create. */
        if (alias != NULL) {
            axl_printf("MkRd: \"%s\" is already mapped as \"%s:\"; keeping it "
                       "(\"%s:\" not added).\n", label, existing, alias);
        }
    } else {
        /* No shell alias yet (fresh create, or a prior map that didn't stick):
           assign the -a name, else the label (when it's a clean custom name),
           else the next-free fsN — then publish it via SetMap. */
        if (choose_map_name(alias, label, name, sizeof(name)) != AXL_OK) {
            return 1;
        }
        /* Distinguish WHY the auto-map failed: AXL_UNSUPPORTED means no
           EFI_SHELL_PROTOCOL was locatable (get_shell() == NULL — the shell's
           map is unreachable, which also makes pick_map_name see every fsN as
           free and pick fs0); AXL_ERR means the shell is present but rejected
           SetMap. The distinction tells a real-hardware report which half broke.
           Either way the disk itself exists and is mountable via 'map -r'. */
        int map_rc = axl_volume_set_map(dev_path, name);
        if (map_rc != AXL_OK) {
            const char *why = (map_rc == AXL_UNSUPPORTED)
                ? "no EFI_SHELL_PROTOCOL on this firmware (shell map unavailable)"
                : "the shell rejected SetMap for this name";
            axl_printf("MkRd: RAM disk \"%s\" created (%zu MB %s) but could not "
                       "auto-map it as \"%s:\" - %s. The disk exists; run 'map -r' "
                       "to mount it, then use its fsN.\n",
                       label, size_mb, fs_type, name, why);
            return 1;
        }
    }
    /* Also publish %<label>% = the map name, so a script can use either the
       fixed `%<label>%:` or the literal `<name>:`. */
    bool var_ok = (axl_setenv(label, name, true) == AXL_OK);

    /* Success summary — label, size/format, shell mapping, backing device
       path (VirtualDisk(<phys>,...), varies per boot), and the shell var. */
    AXL_AUTO_FREE char *dp_text = axl_device_path_to_text(dev_path);
    axl_printf("%s\n", reused ? "RAM disk reused (already mapped):"
                              : "RAM disk created:");
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
    return cli_create(label, size_mb, axl_args_get_string(a, "alias"));
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
