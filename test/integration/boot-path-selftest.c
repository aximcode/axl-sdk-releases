/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * boot-path-selftest.c — self-relative file access with NO shell.
 *
 * When BdsDxe launches an app directly as \EFI\BOOT\BOOTx64.EFI there is no
 * shell of any kind: neither EFI_SHELL_PROTOCOL nor the EFI 1.x
 * SHELL_ENVIRONMENT is published (measured — both absent, on X64 and
 * AARCH64). Before the fix, axl_app_image_path() got its "fsN:" prefix
 * ONLY from EFI_SHELL_PROTOCOL->GetMapFromDevicePath, so it fell back to
 * the bare FILEPATH:
 *
 *   RED    axl_app_image_path()   = "\EFI\BOOT\BOOTX64.EFI"   (volume-less)
 *          axl_app_boot_path(...) = AXL_ERR
 *   GREEN  axl_app_image_path()   = "fs0:\EFI\BOOT\BOOTX64.EFI"
 *          axl_app_boot_path(...) = "fs0:\<name>"
 *
 * and — because axl_backend_file_open had only two branches, both
 * shell-dependent — no on-disk file could be opened at all. So a
 * volume-prefixed STRING is only half the contract: the checks below also
 * write, read back and delete a real file, and read the running image's own
 * bytes through the path the API reported.
 *
 * The same binary is the regression guard for the shell case: run under the
 * shell it asserts the shell's own mapping still comes through verbatim
 * (uppercase "FS0:", the alias GetMapFromDevicePath returns), so the added
 * fallback cannot displace the shell's naming.
 *
 * Emits "BOOTPATH: <N> passed, <M> failed" as the final line, then powers
 * off (nothing else runs in the boot-option case).
 */

#include <axl.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(
    bool         cond,
    const char  *label
    )
{
    if (cond) {
        g_pass++;
        axl_printf("PASS: %s\n", label);
    } else {
        g_fail++;
        axl_printf("FAIL: %s\n", label);
    }
}

/* Report the compared strings on failure — an exact-string assertion that
   just says "false" costs a whole QEMU round-trip to diagnose. */
static void
check_str(
    const char  *got,
    const char  *want,
    const char  *label
    )
{
    bool ok = (got != NULL) && (axl_strcmp(got, want) == 0);
    check(ok, label);
    if (!ok) {
        axl_printf("       want=\"%s\"\n       got =\"%s\"\n",
                   want, got ? got : "(null)");
    }
}

/* The name BdsDxe loads from the removable-media boot slot. */
#if defined(__aarch64__)
#define BOOT_SLOT_PATH  "\\EFI\\BOOT\\BOOTAA64.EFI"
#else
#define BOOT_SLOT_PATH  "\\EFI\\BOOT\\BOOTX64.EFI"
#endif

#define SEED_NAME       "boot-seed.txt"
#define SEED_CONTENT    "axl boot-path seed\n"

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    AxlShellKind kind = axl_shell_kind();
    bool         no_shell = (kind == AXL_SHELL_KIND_NONE);

    axl_printf("BOOTPATH: shell_kind=%d (%s)\n", (int)kind,
               no_shell ? "none - boot-option case"
                        : "shell present - regression case");

    const char *self = axl_app_image_path();
    axl_printf("BOOTPATH: image_path=%s\n", self ? self : "(null)");

    /* ---- 1. the image path carries a volume ---------------------- */
    if (no_shell) {
        /* Exactly one SimpleFileSystem volume exists in the boot-option
           fixture, so the handle-matched index is deterministically 0. */
        check_str(self, "fs0:" BOOT_SLOT_PATH,
                  "no-shell image path is volume-qualified");
    } else {
        /* Under the shell the mapping comes from GetMapFromDevicePath
           verbatim — uppercase, as the shell spells its own alias. The
           fixture is launched from the volume root by startup.nsh. */
        check_str(self, "FS0:\\boot-path-selftest.efi",
                  "shell image path is unchanged by the no-shell fallback");
    }

    /* ---- 2. axl_app_boot_path resolves ---------------------------- */
    char boot[256] = { 0 };
    int  rc = axl_app_boot_path(SEED_NAME, boot, sizeof(boot));
    check(rc == AXL_OK, "axl_app_boot_path succeeds");
    axl_printf("BOOTPATH: boot_path=%s (rc=%d)\n", rc == AXL_OK ? boot : "", rc);
    if (rc == AXL_OK) {
        check_str(boot, no_shell ? "fs0:\\" SEED_NAME : "FS0:\\" SEED_NAME,
                  "axl_app_boot_path names the boot volume");
    } else {
        check(false, "axl_app_boot_path names the boot volume");
    }

    /* ---- 3. a real write/read/delete round-trip ------------------- */
    /* The string being right is not the contract — opening it is. With no
       shell, axl_backend_file_open had no branch that could resolve this. */
    if (rc == AXL_OK) {
        check(axl_file_set_contents(boot, SEED_CONTENT,
                                    axl_strlen(SEED_CONTENT)) == AXL_OK,
              "write a file next to the running image");

        void   *data = NULL;
        size_t  len  = 0;
        bool    read_ok = (axl_file_get_contents(boot, &data, &len) == AXL_OK);
        check(read_ok && len == axl_strlen(SEED_CONTENT)
                  && data != NULL
                  && axl_memcmp(data, SEED_CONTENT, len) == 0,
              "read it back byte-for-byte");
        axl_free(data);

        check(axl_file_delete(boot) == AXL_OK, "delete it again");
    } else {
        check(false, "write a file next to the running image");
        check(false, "read it back byte-for-byte");
        check(false, "delete it again");
    }

    /* ---- 4. the reported image path opens the running image ------- */
    /* Pins the prefix to the volume the image actually came from: a path
       naming the WRONG volume would still parse and could still open some
       file, but not one starting "MZ" at this name. */
    if (self != NULL) {
        void   *img = NULL;
        size_t  ilen = 0;
        bool    got  = (axl_file_get_contents(self, &img, &ilen) == AXL_OK);
        check(got && ilen > 2 && ((const char *)img)[0] == 'M'
                  && ((const char *)img)[1] == 'Z',
              "the reported image path opens this very image");
        axl_free(img);
    } else {
        check(false, "the reported image path opens this very image");
    }

    axl_printf("BOOTPATH: %d passed, %d failed\n", g_pass, g_fail);

    /* The boot-option case has no shell to return to — power off so the
       harness doesn't sit until its timeout. Under the shell, startup.nsh
       owns the shutdown, so return normally. */
    if (no_shell) {
        axl_reset(AXL_RESET_SHUTDOWN);
    }
    return (g_fail == 0) ? 0 : 1;
}
