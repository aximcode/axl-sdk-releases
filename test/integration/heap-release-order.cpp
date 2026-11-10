/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-order.cpp — the C heap is released AFTER everything that can
 * still use it, for test-heap-release-qemu.sh.
 *
 * A global destructor runs during teardown (it registers through
 * __cxa_atexit, so it runs in the atexit drain) and allocates 3 MiB. That
 * does not fit the arena's 1 MiB initial commit, so it must GROW the heap,
 * and _sbrk refuses to grow a heap that has already been released. So the
 * line it prints is the ordering verdict:
 *
 *   HEAPORDER ok       the heap was still ours when the destructor ran
 *   HEAPORDER refused  it had already gone back to the firmware
 *
 * The growth also makes the arena more than one allocation (the initial
 * commit plus an extension), so the observer's zero-leak figure covers the
 * release of a multi-piece arena, not only the 1 MiB first commit.
 */

#include <cstdlib>

#include <axl.h>

namespace {

struct LateAllocator {
    ~LateAllocator()
    {
        void *p = std::malloc(3u << 20);

        axl_printf("HEAPORDER %s\n", p != nullptr ? "ok" : "refused");
        std::free(p);
    }
};

LateAllocator g_late;

} // namespace

int
main()
{
    return 0;
}
