/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* sd-sibling-probe.c — probe for the sibling-locate hard-fail +
 * default-search sibling-first reorder
 * (docs/superpowers/specs/2026-07-04-shared-driver-sibling-locate-design.md).
 *
 * A plain shell app (int main), public headers only — no <uefi/axl-uefi.h>.
 *
 * argv[1] selects the scenario:
 *
 *   hardfail — calls axl_shared_driver_locate_sibling() FIRST (cold; nothing
 *              resident yet) and prints its rc as a symbolic token
 *              (SDSIB:sibling=OK/NOTFOUND/INVALID/ERR). Then calls the
 *              multi-path axl_shared_driver_locate() and prints
 *              SDSIB:multi=OK/NOTFOUND/ERR. Order matters: sibling first,
 *              else the multi-path load would make the driver resident and
 *              the sibling call would warm-hit instead of hard-failing.
 *
 *   reorder  — calls axl_shared_driver_locate() (multi-path) and, on
 *              success, invokes the resolved vtable's run() so the driver
 *              prints its own "SDSIB:tag=<A|B>" marker (proves WHICH copy
 *              answered). Prints SDSIB:tag=NONE if locate itself failed.
 *
 *   beside   — driver-A staged ONLY beside the probe (no /drivers/ copy at
 *              all). Calls axl_shared_driver_locate_sibling() and prints
 *              SDSIB:sibling=<token>; on success, invokes the resolved
 *              vtable's run() so the driver prints its own
 *              "SDSIB:tag=<TAG>" marker. Proves locate_sibling's POSITIVE
 *              path — it resolves and dispatches a driver that IS staged
 *              beside the launcher (the hardfail scenario only proves the
 *              negative: it hard-fails when the driver is NOT beside it).
 *
 * Always prints SDSIB:done last, regardless of scenario outcome.
 */
#include <axl.h>

#include "sd-sibling.h"

/* Symbolic token for an AxlStatus rc — the runner asserts these exact
 * strings, never a raw int (ints drift silently; the enum shape is the
 * contract we're testing). */
static const char *
rc_token(int rc)
{
    if (rc == AXL_OK)        return "OK";
    if (rc == AXL_NOT_FOUND) return "NOTFOUND";
    if (rc == AXL_INVALID)   return "INVALID";
    return "ERR";
}

static void
scenario_hardfail(void)
{
    AxlSharedDriverVtable *vt = NULL;
    int rc = axl_shared_driver_locate_sibling(
        SDSIB_NAME, "sd-sibling-driver.efi", (void **)&vt);
    axl_printf("SDSIB:sibling=%s\n", rc_token(rc));

    AxlSharedDriverVtable *vt2 = NULL;
    int rc2 = axl_shared_driver_locate(
        SDSIB_NAME, "sd-sibling-driver.efi", NULL, 0, (void **)&vt2);
    axl_printf("SDSIB:multi=%s\n", rc_token(rc2));
}

static void
scenario_reorder(void)
{
    AxlSharedDriverVtable *vt = NULL;
    int rc = axl_shared_driver_locate(
        SDSIB_NAME, "sd-sibling-driver.efi", NULL, 0, (void **)&vt);
    if (rc == AXL_OK && vt != NULL && vt->run != NULL) {
        vt->run(0, NULL);   /* driver prints its own SDSIB:tag=<TAG> */
    } else {
        axl_printf("SDSIB:tag=NONE\n");
    }
}

static void
scenario_beside(void)
{
    AxlSharedDriverVtable *vt = NULL;
    int rc = axl_shared_driver_locate_sibling(
        SDSIB_NAME, "sd-sibling-driver.efi", (void **)&vt);
    axl_printf("SDSIB:sibling=%s\n", rc_token(rc));
    if (rc == AXL_OK && vt != NULL && vt->run != NULL) {
        vt->run(0, NULL);   /* driver prints its own SDSIB:tag=<TAG> */
    } else {
        axl_printf("SDSIB:tag=NONE\n");
    }
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && argv[1] != NULL && axl_strcmp(argv[1], "hardfail") == 0) {
        scenario_hardfail();
    } else if (argc >= 2 && argv[1] != NULL
               && axl_strcmp(argv[1], "reorder") == 0)
    {
        scenario_reorder();
    } else if (argc >= 2 && argv[1] != NULL
               && axl_strcmp(argv[1], "beside") == 0)
    {
        scenario_beside();
    } else {
        axl_printf("SDSIB:badscenario\n");
    }
    axl_printf("SDSIB:done\n");
    return 0;
}
