/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * libc-c-selftest.c — a plain C program using the C library.
 *
 * P3 of AXL-Libc-Substrate-Design.md §4d. Everything proven so far was proven
 * on the `-fexceptions` C++ link, because that is the ONLY link mode that
 * carried `libc.a` (§1's table). A C consumer -- which is most of them -- still
 * linked `libaxl.a` and nothing else, so `printf` and `malloc` were not
 * available to C at all.
 *
 * This is deliberately a .c built with axl-cc, not a .cpp: the point is that
 * ordinary C gets the C library, with no C++ toolchain and no exceptions
 * anywhere near it.
 *
 * It also pins the interaction §2-DECISION created. `malloc` is newlib's
 * dlmalloc now, and dlmalloc grows through `sbrk` -- which shipped only on the
 * exceptions path. On x64 the miss is an undefined reference; on aa64 it is
 * WORSE, because ARM's newlib supplies its own `sbrk` that calls `_sbrk_r`,
 * which calls `sbrk` -- a mutual recursion, i.e. a stack overflow at the first
 * allocation rather than a link error. So a C program calling malloc is the
 * assertion that keeps that from regressing.
 */
#include <axl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed;
static int failed;

static void
check(bool ok, const char *what)
{
    axl_printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

int
main(void)
{
    int n = printf("cstdio: %d %s\n", 7, "ok");

    /* 13, including the newline. Printed as well as asserted, because I got
       this count wrong here AND in libc-stdio-selftest.cpp -- twice, the same
       off-by-one. An exact assertion catches it; `n > 0` would have shipped
       both my arithmetic and a genuinely broken return value. */
    axl_printf("  printf_ret=%d\n", n);
    check(n == 13, "printf() works from plain C");

    /* dlmalloc through AXL's sbrk, on a link that never mentions C++. */
    char *p = (char *)malloc(64);

    check(p != NULL, "malloc() works from plain C");
    if (p != NULL) {
        strcpy(p, "libc from C");
        check(strcmp(p, "libc from C") == 0, "strcpy/strcmp round-trip");
        free(p);
    }

    /* AXL's own allocator still answers separately -- the namespace split
       holds on a C link too, not just the C++ one. */
    void *a = axl_malloc(32);

    check(a != NULL, "axl_malloc still allocates independently");
    axl_free(a);

    axl_printf("=== %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
