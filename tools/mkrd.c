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
    disk's device path — a free `FS<n>` name, FS20 or above, FIRST (so it
    becomes the PRIMARY name a bare `map` displays, matching the shell's own fsN
    convention) and the volume LABEL second (so it lands in the `Alias(s):`
    column). Both `FS<n>:` and `<label>:` are usable paths with no `map -r`.
    SetMap targets the shell's GLOBAL map (not a nested Execute), so the names
    are usable by the launching script immediately. The label is the stable
    handle to script against; the fsN is the native display name.

    The fsN number comes from axl_volume_pick_free_fsn: the lowest free
    `FS<n>` at or above FS20, not the lowest free overall, so the shell's own
    `map -u` does not hand the disk's name to the next volume it names (why:
    AXL_VOLUME_FSN_FLOOR in <axl/axl-fs.h>). A system that boots with twenty
    or more volumes has no such margin. mkrd never reuses a taken slot and
    never claims an fsN when the shell is unreachable (SetMap fails cleanly;
    the disk is still created — mount via `map -r`).

    No state is kept between runs: each create asks the shell's live map which
    names are taken (GetDevicePathFromMap), so a second disk gets FS21 and a
    number freed by `mkrd -d` is reused.

    Old / no shell: the SetMap path above needs the EDK2 EFI_SHELL_PROTOCOL. The
    old EFI 1.x shell has no programmatic SetMap for a custom name, so there is
    nothing to name the disk with: mkrd creates and connects it, prints
    `Run 'map -r' to mount the disk`, and exits 0. It does NOT run the shell's
    `map -r` for you (it once did, through SHELL_ENVIRONMENT.Execute). A map
    refresh is not a local operation: it first probes every physical block
    device in the system (ShellPkg Map.c, ProbeForMediaChange), letting each
    report a media change, and a device that does has its BlockIo re-installed
    — which re-enumerates removable and virtual media and invalidates file
    handles a running script or an open editor still holds. Whoever wants the
    shell's map rebuilt says so. A re-run once that `map -r` has mounted the
    disk reports the fsN it got (GetMap reverse lookup) and asks for nothing.

    Label as alias: the LABEL doubles as the friendly alias when it is a clean
    map token. It is NOT set as an alias (the disk is reachable as `FS<n>:` only,
    with a note) when the label matches the reserved `fs<digits>` namespace, is
    too long, or has characters outside [A-Za-z0-9_-]. There is no `-a/--alias`
    option and no `%<label>%` env var — the label IS the alias.

    Idempotent: a re-run of an existing disk says "reused", keeps the names it
    still has — no drift, no duplicate — and gives back any that are missing:
    an fsN if it has none (after a `map -d`, or when `map -u` gave it to another
    volume), and the label alias if the label is not its name. So a startup.nsh
    may re-run it each boot.

    Caveat: a later `map -r` rebuilds the shell map from scratch and drops the
    SetMap'd names (the disk re-emerges as a firmware-assigned fsN); re-running
    `mkrd <label>` gives the label back. The one-call workflow above never runs
    `map -r`, so this is a non-issue there.

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

/* RAM disks `mkrd -l` lists, and a re-run searches for its disk's size. */
#define MKRD_LIST_MAX         16

/* Longest shell name mkrd reads back to advise with: far above any the shell
   generates, so a user's long alias still reads as a name. */
#define MKRD_NAME_READ_MAX    128

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

static int
cli_list(void)
{
    AxlRamDisk disks[MKRD_LIST_MAX];
    size_t     n = 0;
    if (axl_ramdisk_list(disks, MKRD_LIST_MAX, &n) != AXL_OK) {
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
           CURRENTLY this disk's shell alias (a clean token AND mapped to it) —
           so it reads as a path you can type right now. A label that can't be an
           alias, or one a `map -r` has since dropped, shows plain; "(unlabeled)"
           for none. */
        char label_disp[MKRD_MAP_NAME_CAP + 2];
        if (disks[i].label[0] == '\0') {
            axl_snprintf(label_disp, sizeof(label_disp), "(unlabeled)");
        } else if (label_alias_reject_reason(disks[i].label) == NULL
                       && axl_volume_map_is(disks[i].label,
                                            disks[i].device_path)) {
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
    /* Remove every shell name for this disk BEFORE freeing it, while its
       device path is still valid to match on: the fsN primary and the LABEL
       alias mkrd set, and any a user or a `map -r` added since. Matching by
       device path never touches another volume's same-spelled name. A name
       that survives (a shell with no programmatic unmap, e.g. old EFI 1.x,
       keeps the fsN its own `map -r` assigned) is read back for the advice
       below. */
    void  *dev_path     = NULL;
    size_t unmapped     = 0;
    bool   names_remain = false;
    bool   left_known   = false;
    char   left[MKRD_MAP_NAME_CAP];
    if (axl_ramdisk_find(label, &dev_path) == AXL_OK
            && axl_volume_unmap_device(dev_path, &unmapped) != AXL_OK) {
        names_remain = true;
        left_known   = (axl_volume_map_alias(dev_path, left, sizeof(left))
                            == AXL_OK);
    }

    if (axl_ramdisk_destroy(label) != AXL_OK) {
        axl_printf("MkRd: no RAM disk with label \"%s\" found.\n", label);
        return 1;
    }
    if (unmapped > 0) {
        axl_printf("RAM disk \"%s\" destroyed (unmapped shell alias).\n", label);
    } else {
        axl_printf("RAM disk \"%s\" destroyed.\n", label);
    }
    /* A name we could not clear now points at a disk that no longer exists,
       so say which command clears it — but only when one actually survived,
       so a clean destroy stays quiet. mkrd names the command; it does not run
       `map -r` itself, for the reasons in axl_ramdisk_create. On the modern
       shell, say why not `map -u`: ShellCommandUpdateMapping's "remove
       mappings that represent removed devices" is a comment with no code
       under it, and `map -d` walks live handles, so neither can clear a
       mapping whose device is gone. The old EFI 1.x shell has no `map -u` at
       all ("map: Unknown flag -u", measured), so there it goes unsaid. */
    if (names_remain) {
        const char *not_u = (axl_shell_kind() == AXL_SHELL_KIND_UEFI)
                          ? " ('map -u' will not)" : "";
        if (left_known) {
            axl_printf("The shell still maps \"%s\" to the destroyed disk; "
                       "run 'map -r' to clear it%s.\n", left, not_u);
        } else {
            axl_printf("The shell still maps a name to the destroyed disk; "
                       "run 'map -r' to clear it%s.\n", not_u);
        }
    }
    return 0;
}

/* The size, in MB, of the existing disk at @p dev_path, read from the RAM
   disk list. Its device paths come from the same volume enumeration as
   axl_ramdisk_find's, so the pointers are the same firmware-owned ones.
   Returns false if the disk is not among the first MKRD_LIST_MAX listed. */
static bool
existing_size_mb(
    const void *dev_path,
    size_t     *mb
    )
{
    AxlRamDisk disks[MKRD_LIST_MAX];
    size_t     n = 0;
    if (axl_ramdisk_list(disks, MKRD_LIST_MAX, &n) != AXL_OK) {
        return false;
    }
    size_t shown = (n < MKRD_LIST_MAX) ? n : MKRD_LIST_MAX;
    for (size_t i = 0; i < shown; i++) {
        if (disks[i].device_path == dev_path) {
            *mb = (size_t)(disks[i].size_bytes / (1024 * 1024));
            return true;
        }
    }
    return false;
}

/* The summary a create prints: whether the disk is new, the disk itself, and
   its shell mapping when it has one (@p mapping is the text after
   "mapping :", or NULL). For a disk that already existed the size is the
   disk's own, not the one asked for: axl_ramdisk_create hands the existing
   disk back unchanged. @p requested_mb is the explicit -s, or 0. */
static void
print_summary(
    bool        existed,
    const char *label,
    size_t      size_mb,
    size_t      requested_mb,
    const char *mapping,
    const void *dev_path
    )
{
    axl_printf("RAM disk %s:\n", existed ? "reused" : "created");
    axl_printf("  label   : %s\n", label);
    size_t disk_mb = size_mb;
    if (existed && !existing_size_mb(dev_path, &disk_mb)) {
        axl_printf("  size    : (not found in the RAM disk list)\n");
    } else {
        /* FAT16 below the 512 MB cutover, FAT32 at/above it (mirrors the
           library's choice). */
        axl_printf("  size    : %zu MB (%s)\n", disk_mb,
                   (disk_mb <= 512) ? "FAT16" : "FAT32");
    }
    if (existed && requested_mb != 0 && requested_mb != disk_mb) {
        axl_printf("  note    : -s %zu ignored; the disk already exists "
                   "('mkrd -d %s' first to resize it)\n", requested_mb, label);
    }
    if (mapping != NULL) {
        axl_printf("  mapping : %s\n", mapping);
    }
    AXL_AUTO_FREE char *dp_text = axl_device_path_to_text(dev_path);
    if (dp_text != NULL) {
        axl_printf("  device  : %s\n", dp_text);
    }
}

/* Uppercase an fs<n> name in place, to the spelling the modern shell's `map`
   shows (axl_volume_map_name reports it lowercased). */
static void
fsn_to_display(
    char *name
    )
{
    for (char *p = name; *p != '\0'; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
}

static int
cli_create(
    const char *label,
    size_t      requested_mb   ///< explicit -s, or 0 for the default
    )
{
    size_t size_mb = (requested_mb != 0) ? requested_mb : MKRD_DEFAULT_SIZE_MB;
    /* axl_ramdisk_create is idempotent on the label (a startup.nsh may re-run
       mkrd every boot) and does not say whether it made a disk or found one,
       so ask first: both the summary and the advice depend on it. */
    void       *dev_path = NULL;
    bool        existed  = (axl_ramdisk_find(label, &dev_path) == AXL_OK);
    const char *verb     = existed ? "reused" : "created";
    if (axl_ramdisk_create(label, size_mb, &dev_path) != AXL_OK) {
        axl_printf("MkRd: failed to create RAM disk \"%s\" (%zu MB).\n",
                   label, size_mb);
        return 1;
    }

    /* Shells with no programmatic map — the old EFI 1.x shell (its
       SHELL_ENVIRONMENT exposes only a read-only GetMap; no SetMap/AddMap)
       or no shell at all — can't be handed an fsN/label name. The disk is
       already created AND connected (its BlockIo / SimpleFileSystem are
       published), so the shell enumerates it as an fsN on the next `map -r`.

       mkrd does NOT drive that `map -r` itself. Running the shell's map
       refresh from inside a create probes every physical block device in the
       system (ShellPkg Map.c ProbeForMediaChange), letting each report a media
       change; one that does has its BlockIo re-installed, which invalidates
       handles a running script or an open editor still holds. Naming a disk is
       not worth disturbing every other volume, so this names the one manual
       step and exits 0 — the disk WAS created (a shell-less create is a
       partial success, and a non-zero exit reads as EFI_ABORTED to a caller,
       which would abort the script at this line).

       A re-run once that `map -r` has mounted the disk must not ask for
       another: the old shell's GetMap reverse lookup finds the fsN it gave
       the disk, so report it and ask for nothing. Only on that shell: with
       no shell at all the lookup falls back to a positional fs<n>, which is
       not what a shell started later would call the disk. */
    AxlShellKind kind = axl_shell_kind();
    if (kind != AXL_SHELL_KIND_UEFI) {
        char fsn[MKRD_MAP_NAME_CAP];
        char mapping[MKRD_MAP_NAME_CAP + 2];
        bool mounted = existed && kind == AXL_SHELL_KIND_EFI_1X
            && axl_volume_map_name(dev_path, fsn, sizeof(fsn)) == AXL_OK;
        if (mounted) {
            axl_snprintf(mapping, sizeof(mapping), "%s:", fsn);
        }
        print_summary(existed, label, size_mb, requested_mb,
                      mounted ? mapping : NULL, dev_path);
        if (mounted) {
            axl_printf("Ready: use  %s:  now.\n", fsn);
        } else {
            axl_printf("Run 'map -r' to mount the disk (it appears as an fsN).\n");
        }
        return 0;
    }

    /* Name the disk the way a fresh create does, and on a re-run give back
       whatever is missing: an fsN (FS20 and up) if it has none — after a
       `map -d`, or when `map -u` gave its fsN to another volume — and the
       LABEL alias if the label is not this disk's name — after a `map -r`,
       which also gives the disk an fsN of the shell's own, kept as is. Names
       the disk still has are kept: no drift, no duplicate. `reject` explains
       why the label is not an alias, for the note. */
    const char *reject     = label_alias_reject_reason(label);
    bool        label_ours = (reject == NULL)
                             && axl_volume_map_is(label, dev_path);
    char        fsn[MKRD_MAP_NAME_CAP];
    bool        set_fsn    = false;
    bool        set_label  = false;

    if (axl_volume_map_name(dev_path, fsn, sizeof(fsn)) != AXL_OK) {
        /* Distinguish WHY no fsN could be set, because it decides which
           command to name. Every old or absent shell already returned at the
           axl_shell_kind() check above, so reaching here means
           EFI_SHELL_PROTOCOL was located and `map -u` exists (EDK2's Level-2
           map registers it unconditionally): AXL_ERR is a modern shell that
           merely rejected this name, and no free name is the same situation.
           AXL_UNSUPPORTED can then only mean the protocol went away between
           that check and this call, or that it exposes no SetMap — no shell to
           run anything in, so -r is named as the conservative answer. The
           distinction also tells a real-hardware report which half broke. */
        bool picked = (axl_volume_pick_free_fsn(fsn, sizeof(fsn)) == AXL_OK);
        int  map_rc = picked ? axl_volume_set_map(dev_path, fsn) : AXL_ERR;
        if (map_rc != AXL_OK) {
            char        why_buf[64];
            const char *why = why_buf;
            if (!picked) {
                axl_snprintf(why_buf, sizeof(why_buf),
                             "no free fsN from FS%u to FS%u",
                             (unsigned)AXL_VOLUME_FSN_FLOOR,
                             (unsigned)(AXL_VOLUME_FSN_LIMIT - 1));
            } else if (map_rc == AXL_UNSUPPORTED) {
                why = "the shell's SetMap is unavailable";
            } else {
                axl_snprintf(why_buf, sizeof(why_buf),
                             "the shell rejected SetMap for \"%s:\"", fsn);
            }
            /* `map -u` names any volume that has none, skipping devices that
               already have a mapping of any kind (ShellPkg
               ShellCommandUpdateMapping), and it does NOT empty the map list,
               blank %path% or drop the per-mapping current directories the
               way `map -r` does. That is why it is the better follow-up for a
               disk with no name to lose. A disk that still has a name — its
               label, or an alias a user gave it — is one `map -u` skips, so
               that one is pointed at the name it has, and at `map -r` for an
               fsN, which drops that name. Both flags probe media, so neither
               is free. The old EFI 1.x shell answers "map: Unknown flag -u"
               (measured), and AXL_UNSUPPORTED leaves no SetMap to name the
               disk with, so both keep naming -r. */
            char held[MKRD_NAME_READ_MAX];
            if (axl_volume_map_alias(dev_path, held, sizeof(held)) == AXL_OK) {
                bool is_label = (axl_strcasecmp(held, label) == 0);
                axl_printf("MkRd: RAM disk \"%s\" %s but could not give it an "
                           "fsN - %s. It is reachable as %s:. 'map -r' gives it "
                           "an fsN but drops %s:%s%s%s; 'map -u' skips a disk "
                           "that already has a name.\n",
                           label, verb, why, held, held,
                           is_label ? " (re-run 'mkrd " : "",
                           is_label ? label : "",
                           is_label ? "' afterwards to get it back)" : "");
            } else {
                const char *how = (map_rc == AXL_UNSUPPORTED)
                    ? "run 'map -r' to mount it"
                    : "run 'map -u' to mount it (it names volumes that have "
                      "none, without rebuilding the map)";
                axl_printf("MkRd: RAM disk \"%s\" %s but could not name it - "
                           "%s. The disk exists; %s.\n", label, verb, why, how);
            }
            /* Exit 0: the disk WAS created (and connected) — a disk without a
               name is a partial success, not a failure. A non-zero exit here
               reads as EFI_ABORTED to a caller, aborting a
               `mkrd X; map -u; X:` script at the first step; the advisory
               above already names the follow-up. */
            return 0;
        }
        set_fsn = true;
        /* The fsN must be the disk's FIRST name — the shell displays the
           first-inserted one as primary — so a label left from before goes
           behind it: remove it here and set it again below. Only now that
           the fsN is in place, so a failure above left the label usable. */
        if (label_ours && axl_volume_unmap(label) == AXL_OK) {
            label_ours = false;
        }
    }
    fsn_to_display(fsn);

    /* The LABEL, in the Alias(s) column — only when it is a clean token and
       not already another volume's name (SetMap removes a name from its holder
       first, so it would silently take the name from that volume). */
    if (reject == NULL && !label_ours) {
        if (axl_volume_map_taken(label)) {
            reject = "name already mapped to another volume";
        } else if (axl_volume_set_map(dev_path, label) != AXL_OK) {
            reject = "the shell rejected SetMap for the label";
        } else {
            label_ours = true;
            set_label  = true;
        }
    }

    char mapping[2 * MKRD_MAP_NAME_CAP + 16];
    if (label_ours) {
        axl_snprintf(mapping, sizeof(mapping), "%s: (alias %s:)", fsn, label);
    } else {
        axl_snprintf(mapping, sizeof(mapping), "%s:", fsn);
    }
    print_summary(existed, label, size_mb, requested_mb, mapping, dev_path);
    /* A label that can't be an alias: say so, so it isn't a surprise that only
       the fsN works as a path. */
    if (!label_ours && reject != NULL) {
        axl_printf("MkRd: label \"%s\" is not a usable shell alias (%s); "
                   "reachable as %s: only.\n", label, reject, fsn);
    }
    if (label_ours) {
        axl_printf("Ready: use  %s:  or  %s:  now - no 'map -r' needed.\n",
                   fsn, label);
    } else {
        axl_printf("Ready: use  %s:  now - no 'map -r' needed.\n", fsn);
    }
    /* Say it once, here, rather than leave it to the docs: the names mkrd set
       in this run are its own, and the shell's `map -r` rebuilds its map from
       scratch. Only names set in this run: an fsN the shell assigned itself is
       what a `map -r` produces anyway. A re-run gives the label back. The note
       does not offer `map -u` as a way to keep them: the FS20 floor only keeps
       them out of its counter's way until that counter gets there (file
       header). test-ramdisk-map-qemu.sh (check 13) pins the typical case: two
       creates, then unmapping the SECOND disk's names so it is an unmapped
       volume again, then `map -u` — the shell names that volume FS1 and the
       first disk keeps its FS20. Before the floor, mkrd took FS1 itself, and
       `map -u` moved it onto the second disk. */
    if (set_fsn && label_ours) {
        axl_printf("Note: a later 'map -r' replaces these names with a "
                   "firmware-assigned fsN; re-run 'mkrd %s' to get %s: back.\n",
                   label, label);
    } else if (set_fsn) {
        axl_printf("Note: a later 'map -r' replaces this name with a "
                   "firmware-assigned fsN ('mkrd -l' finds it).\n");
    } else if (set_label) {
        axl_printf("Note: a later 'map -r' drops %s:; re-run 'mkrd %s' to get "
                   "it back.\n", label, label);
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

    /* 0 when -s was not given: cli_create picks the default, and a re-run
       without -s does not report a size as ignored. */
    return cli_create(label, (size_t)axl_args_get_uint(a, "size"));
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
