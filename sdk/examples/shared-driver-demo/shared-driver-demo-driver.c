/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-driver.c — resident driver image, built ENTIRELY
 * from the turnkey AXL_SHARED_DRIVER macro (see the bottom of this
 * file). No vtable definition, no axl_shared_driver_publish/_unpublish
 * call, no AXL_DRIVER — three plain functions are the whole driver.
 *
 * TEACHING: what the SDK does behind the scenes so a shell command
 * like `shared-driver-demo.efi status` turns into a call to demo_run()
 * below, running in THIS image's address space, with stdin / stdout /
 * stderr / exit-status all wired up:
 *
 *   1. Resolve.    The launcher's AXL_SHARED_DRIVER_LAUNCHER macro
 *      (shared-driver-demo-launcher.c) calls axl_shared_driver_run(),
 *      which tries, in order: an already-resident driver (a
 *      LocateProtocol short-circuit — the common case after the first
 *      invocation this boot) -> on-disk "shared-driver-demo-dxe.efi"
 *      -> the blob embedded in the launcher at build time. First hit
 *      wins. demo_init() below runs exactly ONCE per boot, the moment
 *      any of those three paths first loads this image — every later
 *      launcher invocation within the same boot skips straight to
 *      step 3.
 *   2. Stdio bridge.  Before dispatching, the SDK installs a bridge so
 *      THIS image's axl_stdin / axl_stdout / axl_stderr reflect the
 *      LAUNCHER's shell handles for this one invocation. Without it a
 *      resident DXE driver has no shell parameters of its own to read
 *      or write — it would see EOF on stdin and its output would go
 *      nowhere a human (or a redirect) could see.
 *   3. Dispatch.  demo_run(argc, argv) is called with the launcher's
 *      own argv, VERBATIM — demo_run() IS the canonical `int main`:
 *      argv[0] is the program name, so the verb lives at argv[1],
 *      exactly like any single-image tool's argument parsing.
 *   4. Exit status.  If demo_run() calls axl_set_exit_status(), that
 *      value crosses the bridge back into the LAUNCHER's own exit
 *      status, so `%lasterror%` after `shared-driver-demo.efi status`
 *      reflects what THIS driver decided — not just demo_run's plain
 *      C return value (which only ever becomes EFI_SUCCESS/ABORTED).
 *
 * Per-stream behavior, from the driver verb's point of view (the full
 * table, including the raw/binary variants this demo doesn't use, is
 * in docs/AXL-Shared-Driver-Recipe.md):
 *
 *   | Stream       | Read/write with            | Redirection honored     |
 *   |--------------|-----------------------------|---------------------------|
 *   | stdin, text  | axl_stdin_text()            | `<file`, default `\|` pipe, interactive |
 *   | stdout, text | axl_print (alias axl_printf)| `>file`                  |
 *   | stderr, text | axl_printerr / axl_warning  | `2>file`, **not** `>file`|
 *
 * Two caveats worth knowing before you rely on either:
 *   - The UEFI reference Shell strips bit 63 (MAX_BIT) from an
 *     *error-class* exit status before exposing it as `%lasterror%`;
 *     small-int / success-class values (like the demo's 0x2A below)
 *     survive unchanged.
 *   - Redirected stdout/stderr (`>`, `2>`) is shell-encoded (UCS-2
 *     with a BOM, or ASCII for `>a`), not UTF-8 — a verb that needs a
 *     UTF-8 file should use axl_fopen directly instead of relying on
 *     shell redirection.
 *
 * Build: see the sibling CMakeLists.txt (axl_add_driver + axl_add_app).
 */

#include <axl.h>
#include "shared-driver-demo.h"
#include "shared-driver-demo-format.h"   /* demo_print_banner (driver-only TU) */

AXL_LOG_DOMAIN("shared-driver-demo-dxe");

/* Static-first, macro-last: AXL_SHARED_DRIVER forward-declares
 * demo_init/demo_run/demo_unload with external linkage. A `static`
 * DEFINITION written after that forward declaration fails to compile
 * ("static declaration follows non-static declaration") — declaring
 * and defining all three `static` here, before the macro invocation
 * at the bottom of the file, sidesteps the ordering hazard entirely.
 * See AXL_SHARED_DRIVER's own docstring in <axl.h> for the full
 * explanation. */
static int demo_init(void);
static int demo_run(int argc, char **argv);
static int demo_unload(void);

static int
demo_init(void)
{
    /* Heavy per-boot setup goes here: sidecar parsing, opening
     * firmware protocols, building caches — whatever the tool's
     * verbs need repeatedly. This runs EXACTLY ONCE per boot, no
     * matter how many times `shared-driver-demo.efi <verb>` is
     * invoked afterward — amortizing that cost is the entire point
     * of the shared-driver pattern. A nonzero return here aborts the
     * driver load (DriverEntry fails), so a real consumer would guard
     * actual setup with an error check instead of the demo's
     * unconditional success.
     *
     * The demo has nothing heavy to do, so this call is a stand-in —
     * a real driver might do e.g. `axl_pci_ids_load(NULL)` or open an
     * SMBIOS/NVMe protocol here instead. */
    demo_print_banner("driver resident (demo_init ran once this boot)");
    return 0;
}

static int
demo_run(int argc, char **argv)
{
    /* demo_run IS the canonical `int main`: the SDK forwards the
     * launcher's argv here verbatim (argv[0] is the program name,
     * exactly as int main sees it), so the verb lives at argv[1] just
     * like any single-image tool's argument parsing. */
    const char *verb = (argc >= 2) ? argv[1] : NULL;

    if (verb != NULL && axl_strcmp(verb, "echotext") == 0) {
        /* --- STDIN + STDOUT --------------------------------------
         * axl_stdin_text() opens a FRESH text-decoding wrapper over
         * the bridged axl_stdin for THIS dispatch only — create one
         * per call, never cache across invocations, since a resident
         * driver that held on to one would replay a stale
         * invocation's buffered input on the next call. It
         * transparently decodes the shell's default `|` pipe
         * (UCS-2), `<file` redirection, and interactive typed input
         * all to UTF-8, so one axl_readline() call handles every
         * input shape without the caller needing to know which one
         * it got.
         *
         * Try it: `echo hello | shared-driver-demo.efi echotext` —
         * "hello" shows up here because the SDK's launcher-side
         * stdio bridge (step 2 above) makes THIS image, the resident
         * driver, see the LAUNCHER's piped stdin. Without that bridge
         * a driver image has no shell parameters of its own to read
         * at all. */
        AxlStream *in = axl_stdin_text();
        char *line = (in != NULL) ? axl_readline(in) : NULL;
        if (line != NULL) {
            size_t n = axl_strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = '\0';
            }
        }
        /* axl_print (alias axl_printf) writes to stdout — captured
         * by `>file`, shown on the console otherwise. */
        axl_print("echo: %s\n", line != NULL ? line : "<EOF>");
        axl_free(line);
        if (in != NULL) {
            axl_fclose(in);
        }
        return 0;
    }

    if (verb != NULL && axl_strcmp(verb, "stderr") == 0) {
        /* --- STDERR -------------------------------------------------
         * axl_printerr (and the axl_warning/axl_log family) write to
         * gST->StdErr, not gST->ConOut. Try:
         *   shared-driver-demo.efi stderr > out.txt
         * out.txt ends up EMPTY — plain stdout redirection does NOT
         * capture either line below. Only `2> err.txt` does. */
        axl_printerr("shared-driver-demo: diagnostic written to stderr\n");
        axl_warning("same destination - axl_warning/axl_log route to stderr too");
        axl_print("(this line is stdout, for comparison; only the two above land in 2>)\n");
        return 0;
    }

    if (verb != NULL && axl_strcmp(verb, "status") == 0) {
        /* --- EXIT STATUS --------------------------------------------
         * axl_set_exit_status() arms the EXACT EFI_STATUS an image's
         * own CRT0 hands back on exit — but under the shared-driver
         * split, "this image" is the DRIVER, and the driver never
         * runs its own exit path per dispatch (a resident driver
         * doesn't call main/CRT0 on every invocation). What actually
         * happens: the launcher's AXL_SHARED_DRIVER_LAUNCHER macro
         * drains this armed value INTO the LAUNCHER's own exit status
         * right after demo_run() returns (step 4 above), so the
         * launcher's CRT0 — the one that actually returns to the
         * firmware — hands it to the shell verbatim.
         *
         * 0x2A is bit-63-clear (not built with an ENCODE_ERROR()-style
         * helper), so it survives the UEFI reference Shell's
         * MAX_BIT-stripping and shows up unchanged in `%lasterror%`
         * after `shared-driver-demo.efi status`. An error-class status
         * (top bit set) would still cross the launcher/driver bridge
         * with its full 64 bits intact, but the reference Shell masks
         * bit 63 off before exposing %lasterror% to shell scripts —
         * only a programmatic EFI_STATUS reader (e.g. the return value
         * of gBS->StartImage) sees the untruncated value. */
        axl_set_exit_status((AxlEfiStatus)0x2A);
        axl_print("status: armed exit status 0x2A (check %%lasterror%% after this returns)\n");
        return 0;
    }

    axl_print("usage: shared-driver-demo.efi <echotext|stderr|status>\n");
    return 1;
}

static int
demo_unload(void)
{
    /* Teardown: release whatever demo_init acquired. This demo holds
     * no heap allocation and no opened protocol, so there's nothing
     * to release — a real consumer would mirror demo_init's setup
     * here (close protocols opened BY_DRIVER, free cached sidecar
     * data). Held-protocol cleanup matters: axl_driver_unload (or a
     * firmware-side UnloadImage) fails with EFI_ACCESS_DENIED if a
     * BY_DRIVER-attributed OpenProtocol call is never closed. */
    demo_print_banner("driver unloading");
    return 0;
}

AXL_SHARED_DRIVER(SHARED_DRIVER_DEMO_NAME, demo_init, demo_run, demo_unload)
