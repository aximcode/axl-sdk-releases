/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * volume-map-test.c — regression test for axl_volume_enumerate deriving
 * AxlVolume.name from the UEFI Shell's fsN map, not the LocateHandle index.
 *
 * The bug: axl_volume_enumerate named each volume "fs<i>" from its position
 * in LocateHandle(SimpleFileSystem) order. That order need not match the
 * shell's own fsN assignment, so the SDK bound the wrong handle/device-path
 * to a name — `do fd` printed type+label for the wrong volume after the FS
 * set changed (e.g. `mkrd`). This test runs with a --mount volume present,
 * which reliably makes the LocateHandle order differ from the shell's fsN
 * order (the host filesystem and the boot ESP come back in the opposite
 * order to how the shell numbers them).
 *
 * Invariant asserted: for every enumerated volume that the shell has a map
 * entry for, AxlVolume.name equals the shell's own alias for that volume's
 * device path (i.e. GetMapFromDevicePath(device_path)'s FSn token). The
 * buggy index-naming violates this whenever the two orders diverge; the fix
 * makes it hold by construction.
 *
 * Scenario mirrors the report: enumerate, create a ramdisk (mkrd), enumerate
 * again — the pre-existing volumes must not swap.
 */

#include <axl.h>
#include <axl/axl-ramdisk.h>
#include <axl/axl-embed.h>
#include <uefi/axl-uefi.h>

/* Vendored EDK2 RamDiskDxe baked in (same as mkrd) so the mkrd phase works
 * even when the firmware doesn't ship EFI_RAM_DISK_PROTOCOL. */
AXL_EMBED_DECLARE(ramdiskdxe);

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool ok, const char *msg)
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static EFI_SHELL_PROTOCOL *
get_shell(void)
{
    EFI_SHELL_PROTOCOL *sh = NULL;
    EFI_GUID g = gEfiShellProtocolGuid;
    if (EFI_ERROR(gBS->LocateProtocol(&g, NULL, (void **)&sh))) {
        return NULL;
    }
    return sh;
}

/* Extract the first "fs<digits>" alias from a shell map string (a ';'-separated
 * UTF-8 list like "FS1:;F1:"), lowercased, colon stripped, into out. Returns
 * true if an FSn token was found. */
static bool
extract_fsn(const char *aliases, char *out, size_t out_size)
{
    const char *p = aliases;
    while (*p != '\0') {
        const char *tok = p;
        while (*p != '\0' && *p != ';') {
            p++;
        }
        size_t toklen = (size_t)(p - tok);
        if (toklen >= 3
            && (tok[0] == 'f' || tok[0] == 'F')
            && (tok[1] == 's' || tok[1] == 'S')
            && tok[2] >= '0' && tok[2] <= '9') {
            size_t j = 0;
            for (size_t k = 0; k < toklen && j + 1 < out_size; k++) {
                if (tok[k] == ':') {
                    break;
                }
                char c = tok[k];
                out[j++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            out[j] = '\0';
            return j > 0;
        }
        if (*p == ';') {
            p++;
        }
    }
    return false;
}

/* Shell's own FSn alias for a volume's device path, or NULL if unmapped.
 * Caller frees. */
static char *
shell_fsn_for(EFI_SHELL_PROTOCOL *sh, void *device_path)
{
    if (sh == NULL || sh->GetMapFromDevicePath == NULL || device_path == NULL) {
        return NULL;
    }
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const unsigned short *map = sh->GetMapFromDevicePath(&dp);
    if (map == NULL) {
        return NULL;
    }
    char *utf8 = axl_ucs2_to_utf8(map);
    if (utf8 == NULL) {
        return NULL;
    }
    char fsn[16];
    bool ok = extract_fsn(utf8, fsn, sizeof(fsn));
    axl_free(utf8);
    return ok ? axl_strdup(fsn) : NULL;
}

/* Assert the name<->shell-alias invariant for every mapped volume. Returns the
 * number of volumes that HAD a shell mapping (so the caller can require the
 * setup actually exercised divergence). */
static int
assert_invariant(EFI_SHELL_PROTOCOL *sh, const char *phase)
{
    AxlVolume vols[16];
    size_t    n = 0;
    if (axl_volume_enumerate(vols, 16, &n) != AXL_OK) {
        check(false, "enumerate succeeds");
        return 0;
    }

    int mapped = 0;
    for (size_t i = 0; i < n; i++) {
        char *fsn = shell_fsn_for(sh, vols[i].device_path);
        if (fsn == NULL) {
            /* Unmapped (e.g. a just-created ramdisk before the shell remaps) —
             * the documented fallback; not part of the invariant. */
            axl_printf("INFO: %s vol[%zu] name=%s shell=<unmapped>\n",
                       phase, i, vols[i].name);
            continue;
        }
        mapped++;
        /* One real assertion per mapped volume — the pass count reflects
         * actual name<->shell comparisons, not a summary marker. */
        char buf[80];
        axl_snprintf(buf, sizeof(buf),
                     "%s: vol[%zu] name '%s' == shell alias '%s'",
                     phase, i, vols[i].name, fsn);
        check(axl_strcasecmp(vols[i].name, fsn) == 0, buf);
        axl_free(fsn);
    }
    return mapped;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("volume-map-test: start\n");

    EFI_SHELL_PROTOCOL *sh = get_shell();
    check(sh != NULL, "EFI_SHELL_PROTOCOL present");
    if (sh == NULL) {
        goto done;
    }

    /* Phase 1 — with the --mount volume present, LocateHandle order diverges
     * from the shell's fsN order, so this catches the index-naming bug. Each
     * mapped volume is asserted inside assert_invariant. */
    int mapped = assert_invariant(sh, "before");
    check(mapped >= 2,
          "at least two volumes have a shell map (divergence is exercised)");

    size_t n_before = 0;
    axl_volume_enumerate(NULL, 0, &n_before);

    /* Phase 2 — mkrd: create a ramdisk, then re-check. The pre-existing
     * volumes must not swap. */
    (void)axl_ramdisk_ensure_driver(AXL_EMBED_DATA(ramdiskdxe),
                                    AXL_EMBED_SIZE(ramdiskdxe), NULL);
    void *dp = NULL;
    if (axl_ramdisk_create("VMAPTEST", 16, &dp) == AXL_OK) {
        size_t n_after = 0;
        axl_volume_enumerate(NULL, 0, &n_after);
        check(n_after == n_before + 1, "ramdisk adds exactly one volume");

        /* The pre-existing volumes must keep their correct shell names after
         * the FS set changed (a fresh ramdisk perturbs the handle DB — the
         * exact trigger in the report). assert_invariant re-checks each mapped
         * volume; the ramdisk itself is unmapped until the shell remaps on its
         * next command, so it falls back to a positional name and is skipped. */
        (void)assert_invariant(sh, "after");
    } else {
        /* Ramdisk creation needs EFI_RAM_DISK_PROTOCOL; if the firmware lacks
         * it and the embedded RamDiskDxe won't load, skip the mkrd half rather
         * than fail (the phase-1 assertions already cover the fix). */
        axl_printf("INFO: ramdisk_create unavailable; skipping mkrd phase\n");
    }

done:
    axl_printf("volume-map-test: %d passed, %d failed\n", g_pass, g_fail);
    /* Power off so the QEMU harness exits promptly instead of idling to its
     * timeout (serial output above is already flushed synchronously). */
    axl_reset(AXL_RESET_SHUTDOWN);
    return g_fail == 0 ? 0 : 1;
}
