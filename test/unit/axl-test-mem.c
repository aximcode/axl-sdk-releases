/** @file axl-test-mem.c
    Unit tests for AxlMem — allocation wrappers and debug features.
**/

#include "axl-test.h"
#include <axl/axl-mem.h>

static inline int
test_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// POSIX API tests
// ---------------------------------------------------------------------------

static void
test_malloc(void)
{
    void *p;
    uint8_t *b;
    size_t i;

    // Basic allocation
    p = axl_malloc(64);
    test_check(p != NULL, "malloc: returns non-NULL");

    // Write and read back
    b = (uint8_t *)p;
    for (i = 0; i < 64; i++) {
        b[i] = (uint8_t)(i & 0xFF);
    }
    test_check(b[0] == 0 && b[63] == 63, "malloc: write/read back");

    axl_free(p);

    // NULL free is safe
    axl_free(NULL);
    test_survived("free(NULL): no crash");
}

static void
test_calloc(void)
{
    uint8_t *p;
    size_t i;
    bool all_zero;

    p = (uint8_t *)axl_calloc(16, 8);
    test_check(p != NULL, "calloc: returns non-NULL");

    all_zero = true;
    for (i = 0; i < 128; i++) {
        if (p[i] != 0) {
            all_zero = false;
            break;
        }
    }
    test_check(all_zero, "calloc: zeroed memory");

    axl_free(p);
}

static void
test_realloc(void)
{
    uint8_t *p;
    void *p2;

    // Grow and preserve data
    p = (uint8_t *)axl_malloc(32);
    axl_memset(p, 0xAB, 32);
    p = (uint8_t *)axl_realloc(p, 128);
    test_check(p != NULL, "realloc: grows non-NULL");
    test_check(p[0] == 0xAB && p[31] == 0xAB, "realloc: preserves data");
    axl_free(p);

    // realloc(NULL, n) acts as malloc
    p2 = axl_realloc(NULL, 64);
    test_check(p2 != NULL, "realloc(NULL, n): acts as malloc");
    axl_free(p2);

    // realloc(ptr, 0) acts as free
    p2 = axl_malloc(64);
    p2 = axl_realloc(p2, 0);
    test_check(p2 == NULL, "realloc(ptr, 0): returns NULL");
}

static void
test_strdup(void)
{
    char *s;

    s = axl_strdup("hello");
    test_check(s != NULL, "strdup: non-NULL");
    test_check(axl_strcmp(s, "hello") == 0, "strdup: copies string");
    axl_free(s);

    s = axl_strdup(NULL);
    test_check(s == NULL, "strdup(NULL): returns NULL");
}

static void
test_memdup(void)
{
    uint8_t src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t *dst;

    dst = (uint8_t *)axl_memdup(src, sizeof(src));
    test_check(dst != NULL, "memdup: non-NULL");
    test_check(test_memcmp(dst, src, sizeof(src)) == 0, "memdup: copies bytes");
    axl_free(dst);

    dst = (uint8_t *)axl_memdup(NULL, 16);
    test_check(dst == NULL, "memdup(NULL): returns NULL");
}

// ---------------------------------------------------------------------------
// axl_new / axl_new_array macro tests
// ---------------------------------------------------------------------------

static void
test_new_macros(void)
{
    typedef struct { uint32_t a; uint32_t b; } TestStruct;
    TestStruct *s;
    TestStruct *a;

    s = axl_new(TestStruct);
    test_check(s != NULL, "axl_new: non-NULL");
    test_check(s->a == 0 && s->b == 0, "axl_new: zeroed");
    axl_free(s);

    a = axl_new_array(TestStruct, 4);
    test_check(a != NULL, "axl_new_array: non-NULL");
    test_check(a[3].a == 0 && a[3].b == 0, "axl_new_array: zeroed");
    axl_free(a);
}

// ---------------------------------------------------------------------------
// Debug feature tests
// ---------------------------------------------------------------------------

static void
test_debug_features(void)
{
#ifdef AXL_MEM_DEBUG
    uint8_t *p;
    bool all_da;
    size_t i;

    // Alloc fill: fresh malloc'd memory should be 0xDA. Failure
    // here is almost always a stale RELEASE-mode axl-mem.o being
    // reused in a DEBUG build (the .o cache key doesn't include
    // BUILD mode); see scripts/install.sh's BUILD-segregated
    // prefix for the install-side guard.
    p = (uint8_t *)axl_malloc(32);
    all_da = true;
    size_t bad_index = 0;
    uint8_t bad_value = 0;
    for (i = 0; i < 32; i++) {
        if (p[i] != 0xDA) {
            all_da = false;
            bad_index = i;
            bad_value = p[i];
            break;
        }
    }
    if (!all_da) {
        axl_printf("    (debug-fill failure: byte[%zu]=0x%02x; "
                   "likely stale RELEASE-mode .o in libaxl.a — "
                   "try `make clean && make`)\n",
                   bad_index, bad_value);
    }
    test_check(all_da, "debug: alloc fill 0xDA");
    axl_free(p);

    // Fence check on valid pointer
    p = (uint8_t *)axl_malloc(64);
    test_check(axl_mem_check(p), "debug: fence check valid");
    axl_free(p);

#else
    /* RELEASE: axl_mem_check always returns true, so only one of the DEBUG
       branch's two assertions has an analogue here. The second is DECLARED
       skipped rather than padded with an invented pass, which keeps the
       cross-build-mode count stable the same way without claiming a result. */
    test_check(axl_mem_check(NULL), "release: axl_mem_check returns true");
    test_skip_n(1, "mem release: leak accounting (AXL_MEM_DEBUG not set)");
#endif
}

static void
test_stats(void)
{
    AxlMemStats before;
    AxlMemStats after;
    AxlMemStats final;
    void *p;

    axl_mem_get_stats(&before);

    p = axl_malloc(100);
    axl_mem_get_stats(&after);
    test_check(after.count == before.count + 1, "stats: count increments on alloc");
    test_check(after.bytes == before.bytes + 100, "stats: bytes increments on alloc");

    axl_free(p);
    axl_mem_get_stats(&final);
    test_check(final.count == before.count, "stats: count decrements on free");
    test_check(final.bytes == before.bytes, "stats: bytes decrements on free");
}

/* First WARNING the "mem" domain emits, captured verbatim. The handler
 * runs inside the log dispatcher, which is not re-entrant and forbids
 * allocating — axl_strlcpy into a static buffer is all it does. */
static char g_leak_hdr[192];
static bool g_leak_hdr_seen;

static void
leak_hdr_handler(
    int                level,
    const char        *domain,
    const char        *message,
    const AxlRealtime *stamp,
    void              *data
    )
{
    (void)level; (void)domain; (void)stamp; (void)data;
    if (g_leak_hdr_seen || message == NULL) {
        return;
    }
    g_leak_hdr_seen = true;
    axl_strlcpy(g_leak_hdr, message, sizeof(g_leak_hdr));
}

static void
test_leak_dump(void)
{
    /* Pin the header axl_mem_dump_leaks() prints, EXACTLY.
     *
     * The blocks it lists here are the AXL runtime's own bookkeeping
     * (argv, registry slot array, atexit slot array) — alive, not
     * leaked, released at teardown. That is precisely why this report
     * must stay distinguishable from the teardown one: the QEMU harness
     * (test_check_leaks in test/integration/common-test.sh) fails a run
     * on "=== AxlMem leak report:" and MUST NOT fail on this line. The
     * "(live allocations)" infix is what keeps the two apart, so it is
     * asserted here rather than left to a comment. */
    AxlMemStats  live;
    char         expect[192];

    g_leak_hdr[0]   = '\0';
    g_leak_hdr_seen = false;
    test_check(axl_log_add_domain_handler("mem", AXL_LOG_WARNING,
                                          leak_hdr_handler, NULL) == AXL_OK,
               "leak dump: capture handler installed");

    axl_mem_get_stats(&live);
    axl_mem_dump_leaks();
    axl_log_remove_handler(leak_hdr_handler);

#ifdef AXL_MEM_DEBUG
    axl_snprintf(expect, sizeof(expect),
                 "=== AxlMem leak report (live allocations): "
                 "%zu allocations, %zu bytes ===",
                 live.count, live.bytes);
    test_check(live.count > 0,
               "leak dump: the runtime's own state is alive at the call site");
    test_check(axl_strcmp(g_leak_hdr, expect) == 0,
               "leak dump: header carries the (live allocations) infix");
#else
    /* RELEASE: the whole report compiles out, so nothing reaches the
       handler. Two assertions to balance the DEBUG branch's two (per
       feedback_balancer_count) — and both are real, not padding. */
    (void)expect;
    test_check(!g_leak_hdr_seen,
               "release: dump_leaks emits no report");
    test_check(g_leak_hdr[0] == '\0',
               "release: dump_leaks leaves the capture buffer untouched");
#endif
}

// ---------------------------------------------------------------------------
// OOM fault injection — exercise error paths without a real OOM
// ---------------------------------------------------------------------------

static void
test_oom_allocator_primitives(void)
{
    void *p;
    char *s;

    // axl_malloc: first call after fail_next_alloc(1) returns NULL.
    axl_mem_fail_next_alloc(1);
    p = axl_malloc(16);
    test_check(p == NULL, "oom: axl_malloc returns NULL on injected failure");

    // After failure fires, counter is back to 0 — next alloc succeeds.
    p = axl_malloc(16);
    test_check(p != NULL, "oom: allocation succeeds after injected failure clears");
    axl_free(p);

    // fail_next_alloc(3) lets the first two through, fails the third.
    axl_mem_fail_next_alloc(3);
    void *a = axl_malloc(16);
    void *b = axl_malloc(16);
    void *c = axl_malloc(16);
    test_check(a != NULL && b != NULL, "oom: first 2 of 3 succeed");
    test_check(c == NULL, "oom: 3rd alloc fails as requested");
    axl_free(a);
    axl_free(b);

    // fail_next_alloc(0) is a no-op — allocations still succeed.
    axl_mem_fail_next_alloc(0);
    p = axl_malloc(16);
    test_check(p != NULL, "oom: fail_next_alloc(0) is a no-op");
    axl_free(p);

    // axl_calloc routes through axl_malloc_impl — also affected.
    axl_mem_fail_next_alloc(1);
    p = axl_calloc(4, 8);
    test_check(p == NULL, "oom: axl_calloc returns NULL on injected failure");

    // axl_realloc(NULL, size) is equivalent to malloc — also affected.
    axl_mem_fail_next_alloc(1);
    p = axl_realloc(NULL, 32);
    test_check(p == NULL, "oom: axl_realloc(NULL, n) returns NULL on injected failure");

    // axl_realloc of an existing block: the NEW alloc fails, the
    // ORIGINAL block must be preserved (POSIX-style realloc contract).
    void *orig = axl_malloc(16);
    test_check(orig != NULL, "oom: realloc preserves original — setup");
    axl_mem_fail_next_alloc(1);
    p = axl_realloc(orig, 1024);
    test_check(p == NULL, "oom: axl_realloc returns NULL on injected failure");
    // orig is still valid — write into it without tripping fences.
    ((char *)orig)[0] = 'x';
    test_check(axl_mem_check(orig), "oom: realloc failure leaves original intact");
    axl_free(orig);

    // axl_strdup routes through axl_malloc_impl — also affected.
    axl_mem_fail_next_alloc(1);
    s = axl_strdup("hello");
    test_check(s == NULL, "oom: axl_strdup returns NULL on injected failure");

    // axl_memdup routes through axl_malloc_impl — also affected.
    axl_mem_fail_next_alloc(1);
    p = axl_memdup("ABCD", 4);
    test_check(p == NULL, "oom: axl_memdup returns NULL on injected failure");

    // Confirm counter is cleared — no lingering injection.
    p = axl_malloc(16);
    test_check(p != NULL, "oom: state fully cleared at end");
    axl_free(p);
}

// ---------------------------------------------------------------------------
// Container macros
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t  signature;
    int       value;
    char      name[16];
} TestContainer;

typedef struct {
    int  header;
    int  middle;
    int  trailer;
} TestOffsets;

static void
test_container_macros(void)
{
    /* AXL_SIGNATURE_32 */
    uint32_t sig = AXL_SIGNATURE_32('T', 'E', 'S', 'T');
    test_check(sig == (uint32_t)('T' | ('E' << 8) | ('S' << 16) | ('T' << 24)),
               "sig32: TEST encodes correctly");

    uint32_t sig2 = AXL_SIGNATURE_32('W', 'D', 'F', 'S');
    test_check((sig2 & 0xFF) == 'W', "sig32: first byte is 'W'");
    test_check(((sig2 >> 24) & 0xFF) == 'S', "sig32: last byte is 'S'");

    /* AXL_CONTAINER_OF — member at offset 0 */
    TestContainer tc;
    tc.signature = sig;
    tc.value = 42;
    TestContainer *recovered = AXL_CONTAINER_OF(&tc.signature, TestContainer, signature);
    test_check(recovered == &tc, "container_of: offset-0 member");
    test_check(recovered->value == 42, "container_of: access via recovered ptr");

    /* AXL_CONTAINER_OF — member at non-zero offset */
    TestContainer *from_name = AXL_CONTAINER_OF(&tc.name, TestContainer, name);
    test_check(from_name == &tc, "container_of: non-zero offset member");

    /* AXL_CONTAINER_OF — middle member */
    TestOffsets to;
    to.header = 1;
    to.middle = 2;
    to.trailer = 3;
    TestOffsets *from_mid = AXL_CONTAINER_OF(&to.middle, TestOffsets, middle);
    test_check(from_mid == &to, "container_of: middle member");
    test_check(from_mid->trailer == 3, "container_of: access trailer via middle");
}

// ---------------------------------------------------------------------------
// Free quarantine — catching the defect the leak report cannot see
// ---------------------------------------------------------------------------

static void
test_quarantine(void)
{
#ifdef AXL_MEM_DEBUG
    size_t   before;
    size_t   after;
    uint8_t *p;
    uint8_t *held;
    int      i;

    /* A clean alloc/free cycle through the quarantine must report
       nothing. Without this the "detected a corruption" assertions
       below would pass for a quarantine that flags everything. */
    axl_mem_set_quarantine(8);
    before = axl_mem_corruption_count();
    for (i = 0; i < 24; i++) {
        p = (uint8_t *)axl_malloc(16);
        axl_free(p);
    }
    axl_mem_set_quarantine(0);
    test_check(axl_mem_corruption_count() == before,
               "quarantine: clean alloc/free churn reports no corruption");

    /* Use-after-free WRITE, caught when the block is evicted. The
       block must still be held at the moment of the write, so the
       ring is drained only afterwards. */
    axl_mem_set_quarantine(8);
    before = axl_mem_corruption_count();
    held = (uint8_t *)axl_malloc(32);
    axl_free(held);
    held[7] = 0x5A;                       /* deliberate use-after-free */
    axl_mem_set_quarantine(0);            /* drain -> eviction checks the fill */
    after = axl_mem_corruption_count();
    test_check(after == before + 1,
               "quarantine: use-after-free write detected on eviction");

    /* Double free: the block is still quarantined, so the second free
       is refused rather than releasing the pool twice. */
    axl_mem_set_quarantine(8);
    before = axl_mem_corruption_count();
    p = (uint8_t *)axl_malloc(24);
    axl_free(p);
    axl_free(p);                          /* deliberate double free */
    after = axl_mem_corruption_count();
    test_check(after == before + 1,
               "quarantine: double free refused and counted");
    axl_mem_set_quarantine(0);

    /* Quarantined blocks are NOT leaks: they have left the live list,
       so holding them must not move the allocation count. A quarantine
       that inflated the leak report would fail every run. */
    {
        AxlMemStats qbefore;
        AxlMemStats qafter;
        axl_mem_set_quarantine(8);
        axl_mem_get_stats(&qbefore);
        p = (uint8_t *)axl_malloc(48);
        axl_free(p);
        axl_mem_get_stats(&qafter);
        test_check(qafter.count == qbefore.count,
                   "quarantine: held block does not count as live");
        axl_mem_set_quarantine(0);
    }

    /* No "disabled" sub-test. The only discriminating probe would be
       that a double free goes UNCAUGHT with the quarantine off — which
       means performing a real double free, corrupting the pool to prove
       a negative. The weaker probes that remain (re-reading an unchanged
       counter, or re-checking that the live count balances) pass just as
       well for an implementation that ignores set_quarantine(0)
       entirely, so they are absent rather than present-and-hollow.
       set_quarantine(0)'s drain IS covered — every assertion above
       depends on it to force eviction. */

    /* Restore the build default so anything appended after this test —
       and every later test binary sharing the image — runs with the
       quarantine the library actually ships with. */
    axl_mem_set_quarantine(16);
#else
    /* RELEASE has no fences, no fill and no quarantine, so three of the
       four DEBUG assertions have no analogue. Declared skipped rather
       than padded with invented passes. */
    axl_mem_set_quarantine(8);
    test_check(axl_mem_corruption_count() == 0,
               "release: corruption count is always zero");
    test_skip_n(3, "mem release: quarantine (AXL_MEM_DEBUG not set)");
#endif
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_mem_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlMem");

    test_malloc();
    test_calloc();
    test_realloc();
    test_strdup();
    test_memdup();
    test_new_macros();
    test_container_macros();
    test_debug_features();
    test_stats();
    test_leak_dump();
    test_oom_allocator_primitives();
    test_quarantine();

    return test_print_results();
}

AXL_APP(test_mem_main)
