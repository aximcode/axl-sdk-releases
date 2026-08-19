/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * cxx-ctor-test.c — launcher for the C++-driver global-constructor contract
 * (test-cxx-driver-ctors-qemu.sh).
 *
 * Loads each of the five C++ driver images built from cxx-ctor-driver.cpp and
 * drives the lifecycle the contract is about:
 *
 *   1. AXL_DRIVER          load -> start -> unload      (ctors, then dtors)
 *   2. AXL_DRIVER again    load -> start -> unload      (RELOAD: runs must be
 *                                                        1 again, not 2)
 *   3. AXL_SHARED_DRIVER   load -> start -> DISPATCH -> unload
 *   4. AXL_SERVICE_DRIVER  load -> start -> unload
 *   5. entry-failure       load -> start REFUSED        (firmware auto-unloads
 *                                                        without calling
 *                                                        Unload -- dtors must
 *                                                        still have run)
 *   6. unload-failure      load -> start -> unload REFUSED, then retried
 *                                                       (no dtors on the
 *                                                        refusal; dtors on the
 *                                                        retry)
 *
 * Plain C on purpose. The launcher is not what is under test, and keeping it C
 * means a failure here cannot be blamed on the C++ layer.
 */

#include <axl.h>
#include <axl/axl-shared-driver.h>

#define CTOR_DRV_DRIVER  "cxx-ctor-driver.efi"
#define CTOR_DRV_SHARED  "cxx-ctor-sd-driver.efi"
#define CTOR_DRV_SERVICE "cxx-ctor-svc-driver.efi"
#define CTOR_DRV_FAILENT "cxx-ctor-fe-driver.efi"
#define CTOR_DRV_FAILUNL "cxx-ctor-fu-driver.efi"

/* Must match the name the shared-driver fixture publishes under. */
#define CTOR_SD_NAME     "axl/cxx-ctor"

/* Load, start and unload one driver image, optionally dispatching it in
 * between. Every step is reported so a failure names which of the three images
 * and which step, rather than leaving the test to infer it from a missing
 * line.
 *
 * @a dispatch_name non-NULL means "this is a shared driver": locate the vtable
 * it published and call its `run`. That is the only way to observe a global
 * from a DISPATCH rather than from the load, which is the shared-driver
 * pattern's whole promise — the image stays resident and `run` is called
 * against state built once at load. */
/* What the caller expects this cycle to do. Anything other than CTOR_EXPECT_OK
 * is a failure path being deliberately driven, so "start failed" or "unload
 * failed" is the PASS condition there and its absence is the defect. */
typedef enum {
    CTOR_EXPECT_OK,           /* load, start, (dispatch), unload -- all succeed */
    CTOR_EXPECT_START_FAIL,   /* DriverEntry refuses; firmware auto-unloads */
    CTOR_EXPECT_UNLOAD_FAIL   /* first unload refuses, second succeeds */
} CtorExpect;

static int
cycle_driver(
    const char *dir,
    const char *name,
    const char *label,
    const char *dispatch_name,
    int         argc,
    char      **argv,
    CtorExpect  expect
    )
{
    AXL_AUTO_FREE char *path = axl_path_join(dir != NULL ? dir : "", name);
    if (path == NULL) {
        axl_printf("CTOR: FAIL cannot build path for %s\n", name);
        return AXL_ERR;
    }

    AxlDriverHandle drv = NULL;
    if (axl_driver_load(path, &drv) != AXL_OK || drv == NULL) {
        axl_printf("CTOR: FAIL load %s\n", label);
        return AXL_ERR;
    }
    int start_rc = axl_driver_start(drv);
    if (expect == CTOR_EXPECT_START_FAIL) {
        /* The firmware has already reclaimed the image (CoreStartImage ->
           CoreUnloadAndCloseImage), so `drv` must not be unloaded again. */
        if (start_rc == AXL_OK) {
            axl_printf("CTOR: FAIL %s started but was expected to refuse\n",
                       label);
            return AXL_ERR;
        }
        axl_printf("CTOR: refused %s\n", label);
        return AXL_OK;
    }
    if (start_rc != AXL_OK) {
        axl_printf("CTOR: FAIL start %s\n", label);
        return AXL_ERR;
    }
    axl_printf("CTOR: started %s\n", label);

    if (dispatch_name != NULL) {
        /* Already resident (we just started it), so this resolves from the
           published protocol rather than loading anything — NULL/0 for the
           embedded blob is the thin-launcher shape. */
        void *iface = NULL;
        if (axl_shared_driver_locate(dispatch_name, name, NULL, 0, &iface)
                != AXL_OK || iface == NULL) {
            axl_printf("CTOR: FAIL locate %s\n", label);
            return AXL_ERR;
        }
        if (axl_shared_driver_dispatch((const AxlSharedDriverVtable *)iface,
                                       argc, argv) != 0) {
            axl_printf("CTOR: FAIL dispatch %s\n", label);
            return AXL_ERR;
        }
    }

    if (expect == CTOR_EXPECT_UNLOAD_FAIL) {
        /* First attempt must be REFUSED — the fixture's unload returns
           non-zero, so the firmware keeps the image loaded and AXL must not
           have destructed its globals. */
        if (axl_driver_unload(drv) == AXL_OK) {
            axl_printf("CTOR: FAIL %s unloaded but was expected to refuse\n",
                       label);
            return AXL_ERR;
        }
        axl_printf("CTOR: refused-unload %s\n", label);
        /* Second attempt succeeds — proving a retry still drains, which is
           what makes success-only the right trade rather than a leak. */
        if (axl_driver_unload(drv) != AXL_OK) {
            axl_printf("CTOR: FAIL retry-unload %s\n", label);
            return AXL_ERR;
        }
        axl_printf("CTOR: unloaded %s\n", label);
        return AXL_OK;
    }

    if (axl_driver_unload(drv) != AXL_OK) {
        axl_printf("CTOR: FAIL unload %s\n", label);
        return AXL_ERR;
    }
    axl_printf("CTOR: unloaded %s\n", label);
    return AXL_OK;
}

int
main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

    /* Resolve the drivers beside us, so the test works wherever the harness
       stages the set. Same approach as image-path-test.c. */
    const char *self = axl_app_image_path();
    if (self == NULL) {
        axl_printf("CTOR: FAIL launcher has no image path\n");
        axl_printf("CTOR_DONE\n");
        return 1;
    }
    AXL_AUTO_FREE char *dir = axl_path_get_dirname(self);

    int rc = cycle_driver(dir, CTOR_DRV_DRIVER, "AXL_DRIVER",
                          NULL, argc, argv, CTOR_EXPECT_OK);

    /* RELOAD. A second load of the SAME image: the firmware loads a fresh
       copy and AXL's own _start zeroes .bss before DriverEntry, so the
       constructor must run again against zeroed storage — runs=1, not 2. */
    if (rc == AXL_OK) {
        rc = cycle_driver(dir, CTOR_DRV_DRIVER, "AXL_DRIVER-reload",
                          NULL, argc, argv, CTOR_EXPECT_OK);
    }
    /* The shared driver is the one that gets DISPATCHED as well as loaded. */
    if (rc == AXL_OK) {
        rc = cycle_driver(dir, CTOR_DRV_SHARED, "AXL_SHARED_DRIVER",
                          CTOR_SD_NAME, argc, argv, CTOR_EXPECT_OK);
    }
    if (rc == AXL_OK) {
        rc = cycle_driver(dir, CTOR_DRV_SERVICE, "AXL_SERVICE_DRIVER",
                          NULL, argc, argv, CTOR_EXPECT_OK);
    }
    /* The two failure paths. Neither was reachable before: every other
       fixture's entry and unload return 0, so the entry-failure drain and the
       success-only guard were both asserted by prose and nothing else. */
    if (rc == AXL_OK) {
        rc = cycle_driver(dir, CTOR_DRV_FAILENT, "entry-failure",
                          NULL, argc, argv, CTOR_EXPECT_START_FAIL);
    }
    if (rc == AXL_OK) {
        rc = cycle_driver(dir, CTOR_DRV_FAILUNL, "unload-failure",
                          NULL, argc, argv, CTOR_EXPECT_UNLOAD_FAIL);
    }

    axl_printf("CTOR_DONE\n");
    return (rc == AXL_OK) ? 0 : 1;
}
