/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-test.c — does an image give its C heap back when it goes?
 * Observer for test-heap-release-qemu.sh.
 *
 *   heap-release-test.efi app    <child.efi> <N>
 *   heap-release-test.efi driver <child.efi> <N>
 *   heap-release-test.efi load   <child.efi> <N>
 *   heap-release-test.efi fail   <child.efi> <N> [service-name]
 *   heap-release-test.efi hold   <child.efi>
 *
 * Runs the child once to warm up (first-launch firmware state is not a leak),
 * samples free memory, runs it N more times -- an app through axl_image_run,
 * a driver through load -> start -> unload, `load` through load ->
 * unload with no start, and `fail` through load -> a start that must FAIL
 * (the firmware unloads the image itself) -- samples again, and prints
 *
 *   HEAPREL <child> launches=<N> per_launch=<pages> total=<pages>
 *
 * With a service name, `fail` also reports whether that service's protocol
 * outlived its image:
 *
 *   HEAPSVC <name> installed=yes|no
 *
 * `hold` instead loads and starts a driver once, samples WHILE it is
 * resident, unloads it, samples again, and prints
 *
 *   HEAPHOLD <child> held=<pages> after_unload=<pages>
 *
 * -- for a driver that must still own its heap while it lives.
 *
 * THE DEFECT. newlib's dlmalloc grows into a region _sbrk takes with
 * AllocatePages (src/cxxrt/axl-cxxrt-alloc.c), and nothing returned it. UEFI
 * does not reclaim an image's page allocations when it unloads, so every
 * launch of any image that ever called malloc -- including every C++ image,
 * whose libsupc++ emergency-pool constructor mallocs before main -- left its
 * 1 MiB arena behind. Measured before the fix: 256 pages per launch on both
 * arches, for C malloc, C++ and C++ --no-eh-frame alike.
 *
 * WHY `load` EXISTS. A driver load/unload cycle reads ~0.4 pages of its own
 * even when the image never starts (30 pages over 80 cycles, RELEASE, C and
 * C++ alike). That is NOT a leak, and it is not the load path: the EDK2
 * Shell parks the buffers its protocol calls return (GetCurDir, the map and
 * path lookups that resolving a relative path makes) on a free list it
 * empties only when the current SHELL COMMAND ends -- and this observer
 * samples twice inside one command. Measured: raw LoadImage/UnloadImage lose
 * 0, and every page is back once the command exits. `driver` minus `load`
 * over the same N cancels it, leaving what STARTING the image costs, which
 * is where its heap lives.
 *
 * FREE MEMORY is the sum of EfiConventionalMemory pages in the memory map --
 * the firmware's own count of pages nobody owns -- which is why this reads
 * the map directly rather than through a classified AXL view that reports
 * owned and unowned RAM alike. The test's leak control (a child that takes 64
 * pages and never frees them) proves this sampler sees a leak of known size.
 */

#define AXL_ALLOW_UEFI 1
#include <axl.h>
#include <uefi/axl-uefi.h>

static int
free_pages(
    uint64_t *out
    )
{
    UINTN       size = 0, key = 0, dsize = 0;
    UINT32      dver = 0;
    EFI_STATUS  s;
    uint8_t    *buf;
    uint64_t    conv = 0;

    s = gBS->GetMemoryMap(&size, NULL, &key, &dsize, &dver);
    if (s != EFI_BUFFER_TOO_SMALL || dsize == 0) {
        return AXL_ERR;
    }
    /* Room for the descriptors the allocation below may itself add. */
    size += 8 * dsize;
    buf = axl_malloc(size);
    if (buf == NULL) {
        return AXL_ERR;
    }
    s = gBS->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsize,
                          &dver);
    if (EFI_ERROR(s)) {
        axl_free(buf);
        return AXL_ERR;
    }
    for (UINTN off = 0; off < size; off += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(buf + off);

        if (d->Type == EfiConventionalMemory) {
            conv += d->NumberOfPages;
        }
    }
    axl_free(buf);
    *out = conv;
    return AXL_OK;
}

/* `load`: load -> unload, never started. */
static bool load_only;

/* `fail`: load -> a start that must fail. */
static bool start_fails;

static int
launch(
    bool        driver,
    const char *path
    )
{
    if (driver) {
        AxlDriverHandle h;

        if (axl_driver_load(path, &h) != AXL_OK) {
            return AXL_ERR;
        }
        if (start_fails) {
            if (axl_driver_start(h) == AXL_OK) {
                return AXL_ERR;
            }
            /* The firmware has unloaded the image; this releases what AXL
               still holds for it (see axl_driver_unload's "image gone"). */
            return axl_driver_unload(h);
        }
        if (!load_only && axl_driver_start(h) != AXL_OK) {
            return AXL_ERR;
        }
        return axl_driver_unload(h);
    }

    int exit_code = -1;

    if (axl_image_run(path, NULL, &exit_code) != AXL_OK || exit_code != 0) {
        return AXL_ERR;
    }
    return AXL_OK;
}

/* `hold`: what a resident driver owns, then what its unload gives back. */
static int
hold(
    const char *path
    )
{
    AxlDriverHandle h;
    uint64_t        before = 0, held = 0, after = 0;

    if (free_pages(&before) != AXL_OK
        || axl_driver_load(path, &h) != AXL_OK
        || axl_driver_start(h) != AXL_OK
        || free_pages(&held) != AXL_OK
        || axl_driver_unload(h) != AXL_OK
        || free_pages(&after) != AXL_OK) {
        axl_printf("HEAPHOLD %s FAILED\n", path);
        return 1;
    }
    axl_printf("HEAPHOLD %s held=%ld after_unload=%ld\n", path,
               (long)((int64_t)before - (int64_t)held),
               (long)((int64_t)before - (int64_t)after));
    return 0;
}

int
main(
    int    argc,
    char **argv
    )
{
    uint64_t before = 0, after = 0, n = 0;
    bool     driver;

    if (argc == 3 && axl_strcmp(argv[1], "hold") == 0) {
        return hold(argv[2]);
    }
    if ((argc != 4 && !(argc == 5 && axl_strcmp(argv[1], "fail") == 0))
        || axl_str_to_u64(argv[3], 10, &n, NULL) != AXL_OK || n == 0) {
        axl_printf("usage: heap-release-test app|driver|load|fail <child.efi> <N>\n"
                   "       heap-release-test hold <driver.efi>\n");
        return 1;
    }
    load_only   = axl_strcmp(argv[1], "load") == 0;
    start_fails = axl_strcmp(argv[1], "fail") == 0;
    driver = load_only || start_fails || axl_strcmp(argv[1], "driver") == 0;

    if (launch(driver, argv[2]) != AXL_OK || free_pages(&before) != AXL_OK) {
        axl_printf("HEAPREL %s FAILED to warm up\n", argv[2]);
        return 1;
    }
    for (uint64_t i = 0; i < n; i++) {
        if (launch(driver, argv[2]) != AXL_OK) {
            axl_printf("HEAPREL %s FAILED on launch %lu\n", argv[2],
                       (unsigned long)(i + 1));
            return 1;
        }
    }
    if (free_pages(&after) != AXL_OK) {
        axl_printf("HEAPREL %s FAILED to sample\n", argv[2]);
        return 1;
    }

    /* Signed: memory GAINED across the run would be its own anomaly, and
       must not read as a huge leak. */
    int64_t total = (int64_t)(before - after);

    axl_printf("HEAPREL %s launches=%lu per_launch=%ld total=%ld\n", argv[2],
               (unsigned long)n, (long)(total / (int64_t)n), (long)total);

    if (argc == 5) {
        AxlService probe = { .name = argv[4] };
        AxlGuid    guid;
        void      *iface = NULL;

        if (axl_service_guid(&probe, &guid) != AXL_OK) {
            axl_printf("HEAPSVC %s FAILED to derive its GUID\n", argv[4]);
            return 1;
        }
        axl_printf("HEAPSVC %s installed=%s\n", argv[4],
                   axl_protocol_find_guid(&guid, &iface) == AXL_OK ? "yes" : "no");
    }
    return 0;
}
