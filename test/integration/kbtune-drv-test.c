/* kbtune-drv-test.c — hazard-safe lifecycle test for the kbtune-drv resident
 * console-conditioning shim.
 *
 * A plain shell app (int main), public headers only. Exercises the RISKY parts
 * of the driver — load (installs the ConIn wrap), the published {get,set} config
 * channel, and unload (must cleanly unpublish + restore the console) — and
 * asserts only SAFE negatives per feedback_uefi_firmware_test_hazards. It does
 * NOT drive real keystrokes through the wrap (that timing is non-deterministic
 * under QEMU sendkey and is real-HW territory); the pure conditioning logic is
 * covered by the axl-input debounce/gate unit tests.
 *
 * Run in isolation with its own timeout (test-kbtune-driver-qemu.sh) so a wedge
 * on load/unload can't starve other tests.
 */

#include <axl.h>
#include "kbtune-shared.h"

static int g_pass;
static int g_fail;

static void
check(bool ok, const char *msg)
{
    axl_printf("%s: %s\r\n", ok ? "PASS" : "FAIL", msg);
    if (ok) { g_pass++; } else { g_fail++; }
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    axl_printf("=== kbtune-drv lifecycle test ===\r\n");

    /* 1. Load (installs the ConIn/ConInEx wrap) + resolve the vtable. A wedge
          here — a bad wrap install — is caught by the harness timeout. */
    void *iface = NULL;
    int rc = axl_shared_driver_locate_sibling(KBTUNE_SHARED_NAME, "kbtune-drv.efi",
                                              &iface);
    check(rc == AXL_OK && iface != NULL, "locate_sibling loads + resolves kbtune-drv");
    if (iface == NULL) {
        axl_printf("=== Results: %d passed, %d failed ===\r\n", g_pass, g_fail);
        return 1;
    }
    KbTuneVtable *vt = (KbTuneVtable *)iface;
    check(vt->version == KBTUNE_VTABLE_VERSION, "vtable version matches");
    check(vt->get != NULL && vt->set != NULL, "vtable get/set present");

    /* 2. Default config: installed but disabled (transparent relay). */
    AxlKbTuneConfig cfg = {0};
    check(vt->get(&cfg) == AXL_OK, "get() succeeds");
    check(cfg.version == KBTUNE_CONFIG_VERSION, "default config carries the version");
    check(!cfg.enabled, "default config is disabled (transparent relay)");

    /* 3. Commit a config and read it back. */
    AxlKbTuneConfig want = {
        .version        = KBTUNE_CONFIG_VERSION,
        .enabled        = true,
        .debounce_ms    = 40,
        .min_gap_ms     = 25,
        .printable_only = true,
    };
    check(vt->set(&want) == AXL_OK, "set() succeeds");
    AxlKbTuneConfig got = {0};
    vt->get(&got);
    check(got.enabled && got.debounce_ms == 40 && got.min_gap_ms == 25
              && got.printable_only,
          "get() round-trips the committed config");

    /* 4. Safe negatives — the driver's own validation, never a firmware misuse. */
    AxlKbTuneConfig bad = want;
    bad.version = 999;
    check(vt->set(&bad) == AXL_ERR, "set() rejects a wrong-version config");
    check(vt->set(NULL) == AXL_ERR, "set(NULL) rejected");
    check(vt->get(NULL) == AXL_ERR, "get(NULL) rejected");

    /* 5. A non-blocking read must return promptly with no key queued (no hang,
          no crash) — smoke-exercises the read path with the wrap installed. */
    AxlKey k;
    check(axl_console_read_key(0, &k) != AXL_OK,
          "non-blocking read returns no-key with the wrap active (no hang)");

    /* 6. Unload: the driver's unload callback must unpublish + restore the
          console. Assert the vtable is gone afterward (proves unpublish ran, and
          restore is its paired step in the same unload callback).

          NOTE: we deliberately do NOT read the console again after unload. Step 5
          read the console while the wrap was active, which memoizes the wrapped
          ConInEx in the backend's Ex cache; that pointer dangles once UnloadImage
          frees the driver, so a post-unload Ex read in THIS image would deref
          freed memory. The resident product never does this (the launcher leaves
          the driver loaded), so it is a test-only hazard we simply avoid. */
    check(axl_shared_driver_unload(KBTUNE_SHARED_NAME) == AXL_OK,
          "unload succeeds");
    AxlGuid g;
    void   *iface2 = NULL;
    axl_shared_driver_guid(KBTUNE_SHARED_NAME, &g);
    check(axl_protocol_find_guid(&g, &iface2) != AXL_OK || iface2 == NULL,
          "vtable is gone after unload (driver unpublished)");

    axl_printf("=== Results: %d passed, %d failed ===\r\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
