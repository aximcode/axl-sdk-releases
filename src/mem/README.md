# AxlMem — Memory Allocation

Memory allocation with dmalloc-inspired debug features. Size-tracking headers
enable `axl_realloc` without passing the old size. Debug builds add
fence-post guards, alloc/free fill patterns, file/line tracking, and leak
reporting.

Header: `<axl/axl-mem.h>`

## Overview

AXL provides its own allocator on top of UEFI's pool memory. All allocated
memory is tracked with a small header that stores the block size, enabling
`axl_realloc` and debug features without requiring the caller to pass the
old size.

**Do not mix** `axl_malloc`/`axl_free` with UEFI's
`AllocatePool`/`FreePool` — they use different headers.

### Debug vs. Release

In debug builds (`-DAXL_MEM_DEBUG`, the default for `make`):

- **Fill patterns**: newly allocated memory is filled with `0xDA`;
  freed memory is filled with `0xDF`. Use-after-free often manifests
  as reads of `0xDF`.
- **Fence-post guards**: three `size_t` sentinels bracket the user
  region — `0xC0C0AB1B` at the head and again just before the body,
  `0xFACADE69` after it. `axl_mem_check(ptr)` verifies all three; a
  tail mismatch specifically means an overflow.
- **File/line tracking**: each allocation records `__FILE__` and
  `__LINE__` for leak reports.
- **Leak reporting**: `axl_mem_dump_leaks()` prints all outstanding
  allocations with their sizes and source locations, under the header
  `=== AxlMem leak report (live allocations): ... ===`. AXL prints the
  same list from its own teardown path (`_axl_cleanup`, or the minimal
  CRT0), and there the header drops the infix — by then atexit
  callbacks and the tier-1 sweep have run, so what is still live IS
  leaked. That one token is the difference between a diagnostic and a
  verdict, and the QEMU harness fails a test run on the verdict form
  (`test_check_leaks` in `test/integration/common-test.sh`). Call
  `axl_mem_dump_leaks()` freely; it will not fail anybody's build.
- **OOM fault injection**: `axl_mem_fail_next_alloc(N)` arms the Nth
  subsequent allocation to return NULL without touching the backend.
  Used by unit tests to exercise caller-side error paths (rollback,
  cleanup, error logging) that would otherwise be unreachable.
- **Free quarantine**: `axl_free` holds the block briefly instead of
  returning it to the firmware immediately, then re-checks it on the
  way out. On by default in debug (16 blocks, capped at 64 KiB total
  and at one block per 64 KiB so a large allocation cannot pin the
  heap); `axl_mem_set_quarantine(n)` resizes it, and `0` disables it
  *and drains what is held* — which is how a test forces the pending
  checks to run now. `axl_mem_corruption_count()` returns the running
  total for tests to assert on; the log is what a human reads.

  This exists because the leak report answers only one of the two
  questions. It sees memory that was never freed, and is blind **by
  construction** to memory freed too early — a block released while
  still referenced produces no leak at all. The quarantine makes that
  half observable:

  - **use-after-free write** — the `0xDF` fill no longer reads back
    when the block is evicted, reported with the original allocation
    site.
  - **double free** — the block is still held, so the second free is
    refused rather than releasing the firmware pool twice. Checked
    *before* the fences, because once a block has gone back to the
    firmware, re-reading its header to validate anything is itself
    undefined.

  A quarantined block has already left the live list, so holding it
  never inflates the leak report, and the teardown path drains the
  ring before printing its verdict.

In release builds (`make BUILD=RELEASE`), these features are disabled
and the allocator has minimal overhead.

### OOM Fault Injection

AXL's error-handling paths — allocator OOM, hash-table insert
rollback, radix-tree split fallbacks, HTTP cache eviction, etc. —
are only ever taken under memory pressure, which is hard to trigger
naturally in tests. `axl_mem_fail_next_alloc` lets a test
deterministically force a specific allocation to fail:

```c
// Exercise the OOM path of a constructor
axl_mem_fail_next_alloc(1);
AxlHashTable *h = axl_hash_table_new_str();
assert(h == NULL);

// Let the first N-1 allocations through, fail the Nth
axl_mem_fail_next_alloc(3);
void *a = axl_malloc(16);  // succeeds
void *b = axl_malloc(16);  // succeeds
void *c = axl_malloc(16);  // returns NULL

// Passing 0 disables injection (the default state)
axl_mem_fail_next_alloc(0);
```

After the failure fires, the counter clears itself and subsequent
allocations succeed normally. Because every allocation path
(`axl_calloc`, `axl_realloc`, `axl_strdup`, `axl_memdup`) routes
through the same `axl_malloc_impl`, one hook point catches them all.
The injection logs at `axl_debug` level so it doesn't pollute the
real-OOM error signal in test output.

The hook is active in both DEBUG and RELEASE — the counter check
is one well-predicted branch on the malloc path, cheap enough that
production builds keep the contract too. The fence/leak-tracking
machinery (alloc-fill, fences, alloc-list) stays DEBUG-only; only
the fail-next-alloc counter is universal.

### RAII Auto-Cleanup

AXL provides GLib-style auto-cleanup macros using
`__attribute__((cleanup))`:

```c
// Automatically freed when 's' goes out of scope
AXL_AUTO_FREE char *s = axl_strdup("hello");

// Automatically freed when 'h' goes out of scope
AXL_AUTOPTR(AxlHashTable) h = axl_hash_table_new_str();

if (error_condition) {
    return -1;  // both 's' and 'h' are freed automatically
}
```

**Important**: Always initialize at declaration. Never use with `goto`
that jumps over the declaration.

### Quick Reference

```c
#include <axl.h>

// Basic allocation
char *buf = axl_malloc(256);
void *copy = axl_memdup(original, size);
char *str = axl_strdup("hello");

// Typed allocation (zero-initialized)
MyStruct *s = axl_new(MyStruct);
int *arr = axl_new_array(int, 100);

// Free (NULL-safe)
axl_free(buf);

// Debug: check for leaks before exit
axl_mem_dump_leaks();
```

See also: **AxlString** for auto-growing string builders,
**AxlStream** / **AxlFs** for I/O.
