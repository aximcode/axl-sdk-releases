/** @file axl-test-strbuf.c
    Unit tests for AxlString string builder and conversion utilities.
**/

#include "axl-test.h"
#include <float.h>

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
// AxlString tests
// ---------------------------------------------------------------------------

static void
test_strbuf_basic(void)
{
    AxlString    *b;

    b = axl_string_new(NULL);
    test_check(b != NULL, "strbuf: new non-NULL");
    test_check(axl_string_len(b) == 0, "strbuf: new len 0");
    test_check(axl_strcmp(axl_string_str(b), "") == 0, "strbuf: new empty string");

    axl_string_append(b, "hello");
    test_check(axl_string_len(b) == 5, "strbuf: append len 5");
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0, "strbuf: append content");

    axl_string_append(b, " world");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "strbuf: append concatenates");

    axl_string_free(b);

    // NULL free is safe
    axl_string_free(NULL);
    test_survived("strbuf: free(NULL) no crash");
}

static void
test_strbuf_append_n(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append_len(b, "abcdefgh", 4);
    test_check(axl_string_len(b) == 4, "strbuf: append_n len 4");
    test_check(axl_strcmp(axl_string_str(b), "abcd") == 0, "strbuf: append_n content");
    axl_string_free(b);
}

static void
test_strbuf_putc(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append_c(b, 'A');
    axl_string_append_c(b, 'B');
    axl_string_append_c(b, 'C');
    test_check(axl_string_len(b) == 3, "strbuf: putc len 3");
    test_check(axl_strcmp(axl_string_str(b), "ABC") == 0, "strbuf: putc content");
    axl_string_free(b);
}

static void
test_strbuf_printf(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append_printf(b, "count=%d", 42);
    test_check(axl_strcmp(axl_string_str(b), "count=42") == 0,
               "strbuf: printf basic");

    axl_string_append_printf(b, " name=%s", "test");
    test_check(axl_strcmp(axl_string_str(b), "count=42 name=test") == 0,
               "strbuf: printf appends");
    axl_string_free(b);
}

static void
test_strbuf_steal(void)
{
    AxlString  *b;
    char       *s;

    b = axl_string_new(NULL);
    axl_string_append(b, "stolen");
    s = axl_string_steal(b);
    test_check(s != NULL, "strbuf: steal non-NULL");
    test_check(axl_strcmp(s, "stolen") == 0, "strbuf: steal content");
    test_check(axl_string_len(b) == 0, "strbuf: steal resets len");
    axl_free(s);
    axl_string_free(b);
}

static void
test_strbuf_steal_then_append(void)
{
    AxlString  *b;
    char       *s;

    /* REGRESSION: axl_string_steal() hands the buffer away and leaves
       alloc == 0, but grow() sized the new buffer by doubling alloc --
       and 0 * 2 is 0 forever. Every append after a steal spun in that
       loop, which under UEFI is a wedged image, not a failed call. */
    b = axl_string_new(NULL);
    axl_string_append(b, "first");
    s = axl_string_steal(b);
    axl_free(s);

    test_check(axl_string_append(b, "second") == AXL_OK,
               "strbuf: append after steal returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "second") == 0,
               "strbuf: append after steal content");
    test_check(axl_string_len(b) == 6, "strbuf: append after steal len");

    axl_string_clear(b);
    test_check(axl_string_append_c(b, 'x') == AXL_OK,
               "strbuf: append_c after steal returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "x") == 0,
               "strbuf: append_c after steal content");
    axl_string_free(b);

    /* A steal that returns NULL (empty builder) must leave the builder
       just as usable -- the caller cannot tell which branch it took. */
    b = axl_string_new(NULL);
    s = axl_string_steal(b);
    test_check(s == NULL, "strbuf: steal of empty returns NULL");
    test_check(axl_string_append(b, "after-empty-steal") == AXL_OK,
               "strbuf: append after empty steal returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "after-empty-steal") == 0,
               "strbuf: append after empty steal content");
    axl_string_free(b);

    /* A length that overflows len + need + 1 must be REFUSED, not wrapped.
       Unguarded, the sum wraps to a value already <= alloc, grow() reports
       success without resizing, and the append memcpy's SIZE_MAX bytes.
       Nothing is read from `s` -- grow() fails before the copy. */
    b = axl_string_new("seed");
    test_check(axl_string_append_len(b, "x", (size_t)-1) == AXL_ERR,
               "strbuf: append_len refuses a size that overflows");
    test_check(axl_strcmp(axl_string_str(b), "seed") == 0,
               "strbuf: refused overflow leaves content intact");
    axl_string_free(b);
}

static void
test_strbuf_post_steal_paths(void)
{
    AxlString  *b;

    /* After a steal the builder owns NO buffer, so grow() allocates the first
       one -- and nothing wrote the terminator into it. Under AXL_MEM_DEBUG a
       fresh block is 0xDA-filled, so there is no lucky NUL: every reader ran
       off the end of the allocation. Fixing the grow() spin without
       establishing the invariant turned a hang into a heap overread. */
    b = axl_string_new("seed");
    axl_free(axl_string_steal(b));
    test_check(axl_string_reserve(b, 64) == AXL_OK,
               "strbuf: reserve after steal returns AXL_OK");
    test_check(axl_string_len(b) == 0, "strbuf: reserve after steal len 0");
    test_check(axl_strlen(axl_string_str(b)) == 0,
               "strbuf: reserve after steal is NUL-terminated");
    test_check(axl_string_append(b, "x") == AXL_OK,
               "strbuf: append after reserve-after-steal");
    test_check(axl_strcmp(axl_string_str(b), "x") == 0,
               "strbuf: append after reserve-after-steal content");
    axl_string_free(b);

    /* The prepend family reaches the same fresh buffer by a different door:
       its shift loop reads buf[0] before anything has written it. */
    b = axl_string_new("seed");
    axl_free(axl_string_steal(b));
    test_check(axl_string_prepend(b, "abc") == AXL_OK,
               "strbuf: prepend after steal returns AXL_OK");
    test_check(axl_string_len(b) == 3, "strbuf: prepend after steal len");
    test_check(axl_strlen(axl_string_str(b)) == 3,
               "strbuf: prepend after steal is NUL-terminated");
    test_check(axl_strcmp(axl_string_str(b), "abc") == 0,
               "strbuf: prepend after steal content");
    axl_string_free(b);

    b = axl_string_new("seed");
    axl_free(axl_string_steal(b));
    test_check(axl_string_prepend_len(b, "xyz", 2) == AXL_OK,
               "strbuf: prepend_len after steal returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "xy") == 0,
               "strbuf: prepend_len after steal content");
    axl_string_free(b);

    b = axl_string_new("seed");
    axl_free(axl_string_steal(b));
    test_check(axl_string_prepend_c(b, 'q') == AXL_OK,
               "strbuf: prepend_c after steal returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "q") == 0,
               "strbuf: prepend_c after steal content");
    axl_string_free(b);

    b = axl_string_new("seed");
    axl_free(axl_string_steal(b));
    test_check(axl_string_capacity(b) == 0, "strbuf: capacity after steal is 0");
    axl_string_free(b);
}

static void
test_strbuf_self_reference_all_paths(void)
{
    AxlString  *b;
    char       *before;
    size_t      i;

    /* Every mutator taking a caller `const char *` can be handed the
       builder's OWN buffer -- the one grow() is about to realloc. append and
       insert were guarded; these four were not, and each was a
       use-after-free. Fill past the initial 64 bytes so the realloc is
       certain; a short string never moves and the bug stays invisible. */
    #define SELF_FILL(bb)                                        \
        do {                                                     \
            for (i = 0; i < 100; i++) {                          \
                axl_string_append_c((bb), (char)('a' + (int)(i % 26))); \
            }                                                    \
        } while (0)

    b = axl_string_new(NULL);
    SELF_FILL(b);
    before = axl_strdup(axl_string_str(b));

    test_check(axl_string_prepend(b, axl_string_str(b)) == AXL_OK,
               "strbuf: self-prepend returns AXL_OK");
    test_check(axl_string_len(b) == 200, "strbuf: self-prepend doubles len");
    test_check(test_memcmp(axl_string_str(b), before, 100) == 0,
               "strbuf: self-prepend copies the original to the front");
    test_check(test_memcmp(axl_string_str(b) + 100, before, 100) == 0,
               "strbuf: self-prepend keeps the displaced original");
    axl_string_free(b);

    b = axl_string_new(NULL);
    SELF_FILL(b);
    test_check(axl_string_prepend_len(b, axl_string_str(b), 100) == AXL_OK,
               "strbuf: self-prepend_len returns AXL_OK");
    test_check(test_memcmp(axl_string_str(b), before, 100) == 0,
               "strbuf: self-prepend_len content");
    axl_string_free(b);

    b = axl_string_new(NULL);
    SELF_FILL(b);
    test_check(axl_string_overwrite(b, 50, axl_string_str(b)) == AXL_OK,
               "strbuf: self-overwrite returns AXL_OK");
    test_check(axl_string_len(b) == 150, "strbuf: self-overwrite grows to 150");
    test_check(test_memcmp(axl_string_str(b), before, 50) == 0,
               "strbuf: self-overwrite keeps the untouched head");
    test_check(test_memcmp(axl_string_str(b) + 50, before, 100) == 0,
               "strbuf: self-overwrite writes the original at pos 50");
    axl_string_free(b);

    /* axl_vformat hands the caller's %s pointer straight to the writer with
       no intermediate copy, so a self-referencing %s reallocs mid-format. */
    b = axl_string_new(NULL);
    SELF_FILL(b);
    test_check(axl_string_append_printf(b, "%s", axl_string_str(b)) == AXL_OK,
               "strbuf: self-printf %s returns AXL_OK");
    test_check(axl_string_len(b) == 200, "strbuf: self-printf %s doubles len");
    test_check(test_memcmp(axl_string_str(b), before, 100) == 0,
               "strbuf: self-printf %s keeps the original half");
    test_check(test_memcmp(axl_string_str(b) + 100, before, 100) == 0,
               "strbuf: self-printf %s copies the original half");
    axl_string_free(b);

    axl_free(before);
    #undef SELF_FILL
}

static void
test_strbuf_overwrite_bounds(void)
{
    AxlString  *b;

    /* pos + strlen(s) wrapping made `end` land BELOW len, so grow() was never
       consulted and the memcpy wrote at buf + (size_t)-2 -- outside the
       allocation, and a WRITE rather than a read. */
    b = axl_string_new("hello");
    test_check(axl_string_overwrite(b, (size_t)-2, "XY") == AXL_ERR,
               "strbuf: overwrite refuses a wrapping pos + len");
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "strbuf: refused overwrite leaves content intact");

    /* A pos past the end would leave an uninitialized gap that len() counts
       and str() cannot see -- refuse it, as g_string_overwrite does. */
    test_check(axl_string_overwrite(b, 8, "Z") == AXL_ERR,
               "strbuf: overwrite refuses pos past the end");
    test_check(axl_string_len(b) == 5, "strbuf: refused overwrite keeps len");

    /* pos == len is APPEND, and stays legal. */
    test_check(axl_string_overwrite(b, 5, "!") == AXL_OK,
               "strbuf: overwrite at pos == len appends");
    test_check(axl_strcmp(axl_string_str(b), "hello!") == 0,
               "strbuf: overwrite at pos == len content");

    /* In-range overwrite that extends past the end still grows. */
    test_check(axl_string_overwrite(b, 4, "OWORLD") == AXL_OK,
               "strbuf: overwrite extending past the end");
    test_check(axl_strcmp(axl_string_str(b), "hellOWORLD") == 0,
               "strbuf: overwrite extending past the end content");
    axl_string_free(b);
}

static void
test_strbuf_size_overflow_guards(void)
{
    AxlString  *b;

    /* Every public entry point that reaches grow() must REFUSE a size that
       cannot be represented, rather than wrap into a too-small allocation. */
    b = axl_string_new("seed");
    test_check(axl_string_resize(b, (size_t)-1, 'x') == AXL_ERR,
               "strbuf: resize refuses an unrepresentable length");
    test_check(axl_string_reserve(b, (size_t)-1) == AXL_ERR,
               "strbuf: reserve refuses an unrepresentable capacity");
    test_check(axl_string_insert_len(b, 1, "z", (size_t)-1) == AXL_ERR,
               "strbuf: insert_len refuses an unrepresentable length");
    test_check(axl_string_prepend_len(b, "z", (size_t)-1) == AXL_ERR,
               "strbuf: prepend_len refuses an unrepresentable length");
    test_check(axl_strcmp(axl_string_str(b), "seed") == 0,
               "strbuf: refused overflows all leave content intact");
    axl_string_free(b);
}

static void
test_strbuf_capacity_boundary(void)
{
    AxlString   *b;
    const char  *buf_before;
    size_t       cap_before;
    size_t       room;
    size_t       i;

    /* The point of capacity(): appending exactly capacity()-len() bytes is
       the most that can happen WITHOUT a reallocation. Asserted on pointer
       identity, because that is the only observable that distinguishes
       "did not grow" from "grew to the same size". */
    b = axl_string_new("seed");
    axl_string_reserve(b, 200);
    room       = axl_string_capacity(b) - axl_string_len(b);
    buf_before = axl_string_str(b);

    for (i = 0; i < room; i++) {
        axl_string_append_c(b, 'x');
    }
    test_check(axl_string_str(b) == buf_before,
               "strbuf: filling exactly the free capacity does not realloc");
    test_check(axl_string_len(b) == axl_string_capacity(b),
               "strbuf: len reaches capacity exactly");

    cap_before = axl_string_capacity(b);
    axl_string_append_c(b, 'y');
    test_check(axl_string_capacity(b) > cap_before,
               "strbuf: one byte past capacity grows the buffer");
    axl_string_free(b);

    /* new_size(N) and reserve(N) must mean the SAME N. They did not: one
       yielded capacity N-1 and the other >= N, which is two meanings for one
       argument in one API. */
    b = axl_string_new_size(100);
    test_check(axl_string_capacity(b) >= 100,
               "strbuf: new_size(N) gives at least N usable bytes");
    axl_string_free(b);
}

static void
test_strbuf_self_reference(void)
{
    AxlString  *b;
    char       *before;
    size_t      blen;
    size_t      i;

    /* `s += s` hands append the very buffer grow() is about to realloc, so
       the copy read freed memory. Fill well past the initial 64 bytes first
       -- a short string never reallocates and the bug stays invisible. */
    b = axl_string_new(NULL);
    for (i = 0; i < 100; i++) {
        axl_string_append_c(b, (char)('a' + (int)(i % 26)));
    }
    blen   = axl_string_len(b);
    before = axl_strdup(axl_string_str(b));

    test_check(axl_string_append_len(b, axl_string_str(b), blen) == AXL_OK,
               "strbuf: self-append returns AXL_OK");
    test_check(axl_string_len(b) == blen * 2, "strbuf: self-append doubles len");
    test_check(test_memcmp(axl_string_str(b), before, blen) == 0,
               "strbuf: self-append keeps the original half");
    test_check(test_memcmp(axl_string_str(b) + blen, before, blen) == 0,
               "strbuf: self-append copies the original half");
    axl_string_free(b);

    /* Same hazard for insert, plus the shift would scramble the source
       even without a reallocation. */
    b = axl_string_new(NULL);
    for (i = 0; i < 100; i++) {
        axl_string_append_c(b, (char)('a' + (int)(i % 26)));
    }
    /* Recomputed, not carried over from the first builder: the two fill loops
       only happen to match, and an edit to one would silently desynchronize
       every assertion below from the string it is describing. */
    blen = axl_string_len(b);
    test_check(axl_string_insert_len(b, 1, axl_string_str(b), blen) == AXL_OK,
               "strbuf: self-insert returns AXL_OK");
    test_check(axl_string_len(b) == blen * 2, "strbuf: self-insert doubles len");
    test_check(axl_string_str(b)[0] == before[0],
               "strbuf: self-insert keeps the leading byte");
    test_check(test_memcmp(axl_string_str(b) + 1, before, blen) == 0,
               "strbuf: self-insert places the whole original at pos 1");
    test_check(test_memcmp(axl_string_str(b) + 1 + blen, before + 1, blen - 1) == 0,
               "strbuf: self-insert keeps the displaced tail");
    axl_string_free(b);
    axl_free(before);
}

static void
test_strbuf_data(void)
{
    AxlString  *b;
    char       *d;

    b = axl_string_new("hello");
    d = axl_string_data(b);
    test_check(d != NULL, "strbuf: data non-NULL");
    d[0] = 'j';
    test_check(axl_strcmp(axl_string_str(b), "jello") == 0,
               "strbuf: write through data is visible");
    test_check(axl_string_len(b) == 5, "strbuf: data write leaves len alone");

    /* NULL rather than the "" literal axl_string_str() hands back -- a
       writable pointer into .rodata would fault on the first store. */
    test_check(axl_string_data(NULL) == NULL, "strbuf: data(NULL) is NULL");
    axl_string_free(b);

    b = axl_string_new("gone");
    axl_free(axl_string_steal(b));
    test_check(axl_string_data(b) == NULL, "strbuf: data after steal is NULL");
    axl_string_free(b);
}

static void
test_strbuf_capacity(void)
{
    AxlString  *b;
    size_t      cap;

    b = axl_string_new_size(100);
    cap = axl_string_capacity(b);
    test_check(cap >= 99, "strbuf: new_size capacity honoured");
    test_check(axl_string_len(b) == 0, "strbuf: new_size starts empty");
    test_check(axl_string_capacity(NULL) == 0, "strbuf: capacity(NULL) is 0");

    /* The "appending exactly the free capacity does not realloc" property is
       asserted in test_strbuf_capacity_boundary, on pointer identity. */
    test_check(axl_string_reserve(b, 200) == AXL_OK, "strbuf: reserve grows");
    cap = axl_string_capacity(b);
    test_check(cap >= 200, "strbuf: reserve reaches requested capacity");

    test_check(axl_string_reserve(b, 10) == AXL_OK, "strbuf: reserve shrink is OK");
    test_check(axl_string_capacity(b) == cap, "strbuf: reserve never shrinks");

    axl_string_append(b, "content");
    test_check(axl_strcmp(axl_string_str(b), "content") == 0,
               "strbuf: reserve preserves content");

    axl_string_shrink_to_fit(b);
    test_check(axl_strcmp(axl_string_str(b), "content") == 0,
               "strbuf: shrink_to_fit preserves content");
    test_check(axl_string_len(b) == 7, "strbuf: shrink_to_fit preserves len");
    test_check(axl_string_capacity(b) == 7, "strbuf: shrink_to_fit releases slack");

    axl_string_append(b, "-more");
    test_check(axl_strcmp(axl_string_str(b), "content-more") == 0,
               "strbuf: append after shrink_to_fit regrows");
    axl_string_free(b);

    axl_string_shrink_to_fit(NULL);   /* NULL-safe, no crash */
    test_check(axl_string_reserve(NULL, 10) == AXL_ERR,
               "strbuf: reserve(NULL) is AXL_ERR");
}

static void
test_strbuf_resize(void)
{
    AxlString  *b;

    b = axl_string_new("abc");
    test_check(axl_string_resize(b, 6, '.') == AXL_OK, "strbuf: resize grow");
    test_check(axl_strcmp(axl_string_str(b), "abc...") == 0,
               "strbuf: resize pads with fill");
    test_check(axl_string_len(b) == 6, "strbuf: resize grow len");

    test_check(axl_string_resize(b, 2, '.') == AXL_OK, "strbuf: resize shrink");
    test_check(axl_strcmp(axl_string_str(b), "ab") == 0,
               "strbuf: resize shrink truncates");
    test_check(axl_string_len(b) == 2, "strbuf: resize shrink len");

    test_check(axl_string_resize(b, 2, 'x') == AXL_OK, "strbuf: resize to same");
    test_check(axl_strcmp(axl_string_str(b), "ab") == 0,
               "strbuf: resize to same is a no-op");

    test_check(axl_string_resize(b, 0, 'x') == AXL_OK, "strbuf: resize to 0");
    test_check(axl_string_len(b) == 0, "strbuf: resize to 0 empties");
    test_check(axl_strcmp(axl_string_str(b), "") == 0,
               "strbuf: resize to 0 leaves empty string");
    test_check(axl_string_resize(NULL, 4, 'x') == AXL_ERR,
               "strbuf: resize(NULL) is AXL_ERR");
    axl_string_free(b);

    /* Growing from a stolen (buffer-less) builder is the grow()-from-zero
       path again, reached through a different door. */
    b = axl_string_new("seed");
    axl_free(axl_string_steal(b));
    test_check(axl_string_resize(b, 3, 'z') == AXL_OK,
               "strbuf: resize after steal");
    test_check(axl_strcmp(axl_string_str(b), "zzz") == 0,
               "strbuf: resize after steal content");
    axl_string_free(b);
}

static void
test_strbuf_insert_len(void)
{
    AxlString  *b;

    b = axl_string_new("hello world");
    test_check(axl_string_insert_len(b, 5, ",XX", 1) == AXL_OK,
               "strbuf: insert_len returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "hello, world") == 0,
               "strbuf: insert_len honours len over NUL");
    test_check(axl_string_len(b) == 12, "strbuf: insert_len updates len");

    /* pos past the end degrades to append, matching axl_string_insert. */
    test_check(axl_string_insert_len(b, 999, "!?", 1) == AXL_OK,
               "strbuf: insert_len past end returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), "hello, world!") == 0,
               "strbuf: insert_len past end appends");

    test_check(axl_string_insert_len(b, 0, "ab", 0) == AXL_OK,
               "strbuf: insert_len of 0 bytes is OK");
    test_check(axl_strcmp(axl_string_str(b), "hello, world!") == 0,
               "strbuf: insert_len of 0 bytes changes nothing");

    test_check(axl_string_insert_len(b, 0, ">> ", 3) == AXL_OK,
               "strbuf: insert_len at 0 returns AXL_OK");
    test_check(axl_strcmp(axl_string_str(b), ">> hello, world!") == 0,
               "strbuf: insert_len at 0 prepends");

    test_check(axl_string_insert_len(NULL, 0, "x", 1) == AXL_ERR,
               "strbuf: insert_len(NULL) is AXL_ERR");
    axl_string_free(b);

    /* Embedded NUL: len is authoritative, so the byte lands in the buffer
       even though every char* reader stops at it. */
    b = axl_string_new("ac");
    test_check(axl_string_insert_len(b, 1, "\0b", 2) == AXL_OK,
               "strbuf: insert_len accepts an embedded NUL");
    test_check(axl_string_len(b) == 4, "strbuf: insert_len embedded NUL len");
    test_check(test_memcmp(axl_string_str(b), "a\0bc", 4) == 0,
               "strbuf: insert_len embedded NUL bytes");
    axl_string_free(b);
}

static void
test_strbuf_clear(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append(b, "data");
    axl_string_clear(b);
    test_check(axl_string_len(b) == 0, "strbuf: clear resets len");
    test_check(axl_strcmp(axl_string_str(b), "") == 0, "strbuf: clear empty string");

    // Can reuse after clear
    axl_string_append(b, "reused");
    test_check(axl_strcmp(axl_string_str(b), "reused") == 0,
               "strbuf: reuse after clear");
    axl_string_free(b);
}

static void
test_strbuf_grow(void)
{
    AxlString  *b;
    size_t      i;
    bool        ok;

    // Grow past default capacity by appending many chars
    b = axl_string_new(NULL);
    for (i = 0; i < 200; i++) {
        axl_string_append_c(b, 'X');
    }
    test_check(axl_string_len(b) == 200, "strbuf: grow past capacity");

    ok = true;
    for (i = 0; i < 200; i++) {
        if (axl_string_str(b)[i] != 'X') {
            ok = false;
            break;
        }
    }
    test_check(ok, "strbuf: grow content intact");
    axl_string_free(b);
}

// ---------------------------------------------------------------------------
// prepend / insert / erase / truncate / overwrite tests
// ---------------------------------------------------------------------------

static void
test_strbuf_prepend(void)
{
    AxlString  *b;

    b = axl_string_new("world");
    axl_string_prepend(b, "hello ");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "prepend: basic");
    test_check(axl_string_len(b) == 11, "prepend: len 11");
    axl_string_free(b);

    // prepend_len
    b = axl_string_new("world");
    axl_string_prepend_len(b, "hello!!", 6);
    test_check(axl_strcmp(axl_string_str(b), "hello!world") == 0,
               "prepend_len: exact bytes");
    axl_string_free(b);

    // prepend_c
    b = axl_string_new("bc");
    axl_string_prepend_c(b, 'a');
    test_check(axl_strcmp(axl_string_str(b), "abc") == 0,
               "prepend_c: single char");
    test_check(axl_string_len(b) == 3, "prepend_c: len 3");
    axl_string_free(b);
}

static void
test_strbuf_insert(void)
{
    AxlString  *b;

    // Insert at beginning (pos 0)
    b = axl_string_new("world");
    axl_string_insert(b, 0, "hello ");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "insert: at beginning");
    axl_string_free(b);

    // Insert in middle
    b = axl_string_new("helo");
    axl_string_insert(b, 3, "l");
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "insert: middle");
    axl_string_free(b);

    // Insert at end (pos >= len -> append)
    b = axl_string_new("hello");
    axl_string_insert(b, 100, " world");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "insert: past end appends");
    axl_string_free(b);
}

static void
test_strbuf_erase(void)
{
    AxlString  *b;

    // Erase from middle
    b = axl_string_new("hello world");
    axl_string_erase(b, 5, 1);
    test_check(axl_strcmp(axl_string_str(b), "helloworld") == 0,
               "erase: middle");
    test_check(axl_string_len(b) == 10, "erase: middle len");
    axl_string_free(b);

    // Erase with clamp (len exceeds remaining)
    b = axl_string_new("hello");
    axl_string_erase(b, 3, 100);
    test_check(axl_strcmp(axl_string_str(b), "hel") == 0,
               "erase: clamp to end");
    test_check(axl_string_len(b) == 3, "erase: clamp len");
    axl_string_free(b);
}

static void
test_strbuf_truncate(void)
{
    AxlString  *b;

    // Truncate shorter
    b = axl_string_new("hello world");
    axl_string_truncate(b, 5);
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "truncate: shorter");
    test_check(axl_string_len(b) == 5, "truncate: shorter len");
    axl_string_free(b);

    // Truncate at same length (no-op)
    b = axl_string_new("hello");
    axl_string_truncate(b, 5);
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "truncate: same len no-op");
    test_check(axl_string_len(b) == 5, "truncate: same len unchanged");
    axl_string_free(b);
}

static void
test_strbuf_overwrite(void)
{
    AxlString  *b;

    // Overwrite within bounds
    b = axl_string_new("hello world");
    axl_string_overwrite(b, 6, "earth");
    test_check(axl_strcmp(axl_string_str(b), "hello earth") == 0,
               "overwrite: within bounds");
    test_check(axl_string_len(b) == 11, "overwrite: len unchanged");
    axl_string_free(b);

    // Overwrite extending past end
    b = axl_string_new("hello");
    axl_string_overwrite(b, 3, "ping!");
    test_check(axl_strcmp(axl_string_str(b), "helping!") == 0,
               "overwrite: extends");
    test_check(axl_string_len(b) == 8, "overwrite: extended len");
    axl_string_free(b);
}

// ---------------------------------------------------------------------------
// axl_asprintf tests
// ---------------------------------------------------------------------------

static void
test_asprintf(void)
{
    char   *s;

    s = axl_asprintf("hello %s, %d", "world", 42);
    test_check(s != NULL, "asprintf: non-NULL");
    test_check(axl_strcmp(s, "hello world, 42") == 0, "asprintf: content");
    axl_free(s);

    s = axl_asprintf(NULL);
    test_check(s == NULL, "asprintf(NULL): returns NULL");
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UCS-2 tests
// ---------------------------------------------------------------------------

static void
test_utf8_ucs2(void)
{
    unsigned short  *w;
    char            *u;

    // ASCII roundtrip
    w = axl_utf8_to_ucs2("ABC");
    test_check(w != NULL, "utf8_to_ucs2: ASCII non-NULL");
    test_check(w[0] == 'A' && w[1] == 'B' && w[2] == 'C' && w[3] == 0,
               "utf8_to_ucs2: ASCII content");

    u = axl_ucs2_to_utf8(w);
    test_check(u != NULL, "ucs2_to_utf8: roundtrip non-NULL");
    test_check(axl_strcmp(u, "ABC") == 0, "ucs2_to_utf8: roundtrip content");
    axl_free(w);
    axl_free(u);

    // Multibyte: Euro sign U+20AC = 0xE2 0x82 0xAC in UTF-8, 0x20AC in UCS-2
    w = axl_utf8_to_ucs2("\xE2\x82\xAC");
    test_check(w != NULL, "utf8_to_ucs2: multibyte non-NULL");
    test_check(w[0] == 0x20AC && w[1] == 0, "utf8_to_ucs2: euro sign");
    axl_free(w);

    // NULL safety
    test_check(axl_utf8_to_ucs2(NULL) == NULL, "utf8_to_ucs2(NULL): returns NULL");
    test_check(axl_ucs2_to_utf8(NULL) == NULL, "ucs2_to_utf8(NULL): returns NULL");

    /* axl_utf8_to_ucs2_buf — the buffer-backed cousin. Used by the
       fs-provider thunk hot path (no allocation per Open / GetInfo).
       The original implementation cast bytes through Latin-1 and
       silently corrupted any non-ASCII filename; pin the multi-byte
       decode here so future regressions surface immediately. */
    {
        unsigned short buf[16];
        size_t         n;

        /* ASCII fits and round-trips. */
        n = axl_utf8_to_ucs2_buf("ABC", buf, 16);
        test_check(n == 3 &&
                   buf[0] == 'A' && buf[1] == 'B' &&
                   buf[2] == 'C' && buf[3] == 0,
                   "utf8_to_ucs2_buf: ASCII");

        /* 2-byte sequence: U+00E9 'é' = 0xC3 0xA9. */
        n = axl_utf8_to_ucs2_buf("r\xC3\xA9sum\xC3\xA9", buf, 16);
        test_check(n == 6 &&
                   buf[0] == 'r' && buf[1] == 0x00E9 &&
                   buf[2] == 's' && buf[3] == 'u' &&
                   buf[4] == 'm' && buf[5] == 0x00E9 &&
                   buf[6] == 0,
                   "utf8_to_ucs2_buf: résumé");

        /* 3-byte sequence: U+65E5 '日', U+672C '本', U+8A9E '語'. */
        n = axl_utf8_to_ucs2_buf("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                 buf, 16);
        test_check(n == 3 &&
                   buf[0] == 0x65E5 && buf[1] == 0x672C &&
                   buf[2] == 0x8A9E && buf[3] == 0,
                   "utf8_to_ucs2_buf: 日本語");

        /* dst_count truncation: 4 cells means at most 3 chars + NUL. */
        n = axl_utf8_to_ucs2_buf("ABCDEF", buf, 4);
        test_check(n == 3 &&
                   buf[0] == 'A' && buf[1] == 'B' &&
                   buf[2] == 'C' && buf[3] == 0,
                   "utf8_to_ucs2_buf: dst_count truncation");

        /* Round trip through ucs2_to_utf8_buf: "résumé" → CHAR16 →
           UTF-8 byte-for-byte. */
        n = axl_utf8_to_ucs2_buf("r\xC3\xA9sum\xC3\xA9", buf, 16);
        char back[32];
        size_t bn = axl_ucs2_to_utf8_buf(buf, back, sizeof(back));
        test_check(bn == 8 && axl_strcmp(back, "r\xC3\xA9sum\xC3\xA9") == 0,
                   "utf8↔ucs2 round-trip via _buf: résumé");
    }
}

// ---------------------------------------------------------------------------
// axl_utf8_decode — per-codepoint iterator
// ---------------------------------------------------------------------------

static void
test_utf8_decode(void)
{
    uint32_t  cp;
    size_t    n;

    /* End-of-string sentinel: returns 0 on NUL byte. */
    n = axl_utf8_decode("", &cp);
    test_check(n == 0, "utf8_decode: empty string returns 0");

    /* NULL safety: both arguments. */
    n = axl_utf8_decode(NULL, &cp);
    test_check(n == 0, "utf8_decode: NULL s returns 0");
    n = axl_utf8_decode("A", NULL);
    test_check(n == 0, "utf8_decode: NULL out_codepoint returns 0");

    /* 1-byte sequence: ASCII. */
    n = axl_utf8_decode("A", &cp);
    test_check(n == 1 && cp == 0x41, "utf8_decode: 'A' (1-byte) == U+0041");

    /* 2-byte sequence: U+00E9 é = 0xC3 0xA9. */
    n = axl_utf8_decode("\xC3\xA9", &cp);
    test_check(n == 2 && cp == 0x00E9, "utf8_decode: 'é' (2-byte) == U+00E9");

    /* 3-byte sequence: U+2500 ─ (box horizontal) = 0xE2 0x94 0x80. */
    n = axl_utf8_decode("\xE2\x94\x80", &cp);
    test_check(n == 3 && cp == 0x2500,
               "utf8_decode: '─' (3-byte) == U+2500");

    /* 3-byte sequence: U+4E2D 中 = 0xE4 0xB8 0xAD. */
    n = axl_utf8_decode("\xE4\xB8\xAD", &cp);
    test_check(n == 3 && cp == 0x4E2D,
               "utf8_decode: '中' (3-byte) == U+4E2D");

    /* 4-byte sequence (above BMP): U+1F600 😀 = 0xF0 0x9F 0x98 0x80. */
    n = axl_utf8_decode("\xF0\x9F\x98\x80", &cp);
    test_check(n == 4 && cp == 0x1F600,
               "utf8_decode: '😀' (4-byte) == U+1F600 (above BMP)");

    /* Invalid: orphan continuation byte → return 1, cp=0xFFFD. */
    n = axl_utf8_decode("\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: orphan continuation byte → 1 byte + U+FFFD");

    /* Invalid: truncated 2-byte sequence (0xC3 with no continuation). */
    n = axl_utf8_decode("\xC3", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: truncated 2-byte at EOS → 1 byte + U+FFFD");

    /* Invalid: 2-byte lead followed by non-continuation. */
    n = axl_utf8_decode("\xC3X", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: 2-byte lead + non-continuation → 1 byte + U+FFFD");

    /* Invalid: 0xFF (never valid as a UTF-8 lead byte). */
    n = axl_utf8_decode("\xFF", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: 0xFF lead byte → 1 byte + U+FFFD");

    /* Overlong 2-byte encoding of U+0000 (0xC0 0x80, "Modified UTF-8").
       Must be rejected to prevent NUL-smuggling attacks via UTF-8. */
    n = axl_utf8_decode("\xC0\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: overlong 2-byte U+0000 (\\xC0\\x80) → 1 byte + U+FFFD");

    /* Overlong 3-byte encoding of an ASCII codepoint. */
    n = axl_utf8_decode("\xE0\x80\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: overlong 3-byte → 1 byte + U+FFFD");

    /* UTF-16 surrogate U+D800 encoded as 3 bytes (0xED 0xA0 0x80).
       Surrogates are not valid Unicode scalars and must be rejected. */
    n = axl_utf8_decode("\xED\xA0\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: UTF-16 surrogate U+D800 → 1 byte + U+FFFD");

    /* 4-byte encoding above Unicode maximum (U+110000 = 0xF4 0x90 0x80 0x80).
       Anything beyond U+10FFFF is invalid. */
    n = axl_utf8_decode("\xF4\x90\x80\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: out-of-range U+110000 → 1 byte + U+FFFD");

    /* Overlong 4-byte (0xF0 0x80 0x80 0x80 encodes U+0000 in 4 bytes). */
    n = axl_utf8_decode("\xF0\x80\x80\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: overlong 4-byte → 1 byte + U+FFFD");

    /* Iteration over a mixed string: "A中Z" = 1 + 3 + 1 = 5 bytes. */
    {
        const char *p = "A\xE4\xB8\xAD" "Z";
        uint32_t    cps[8] = {0};
        size_t      i = 0;
        size_t      consumed;
        while ((consumed = axl_utf8_decode(p, &cps[i])) > 0) {
            p += consumed;
            i++;
        }
        test_check(i == 3, "utf8_decode iter: 'A中Z' yields 3 codepoints");
        test_check(cps[0] == 0x41 && cps[1] == 0x4E2D && cps[2] == 0x5A,
                   "utf8_decode iter: codepoints are A, 中, Z");
    }
}

// ---------------------------------------------------------------------------
// axl_utf8_encode — per-codepoint encoder (reverse of axl_utf8_decode)
// ---------------------------------------------------------------------------

/* Encode @a cp into an 8-byte buffer whose capacity is CLAIMED to be @a cap,
   and assert the result byte-for-byte.
 *
 * Two things are checked that a bare axl_memcmp would not. Bytes at and past
 * @a expect_len must still hold the fill, so an encoder that writes more than
 * it reports — or writes a partial sequence it then refuses — is caught rather
 * than passing on its return value alone. And the whole check runs twice with
 * two different fills, so "unchanged" cannot be satisfied by an encoder that
 * happens to write the fill byte itself (0x7F is a real encoding of U+007F).
 *
 * (The two fills differ in every position that matters: no single byte value
 * satisfies both passes, so a write of the fill byte cannot masquerade as an
 * untouched one.)
 *
 * @a expect_len == 0 means "must write nothing", and then every byte of the
 * buffer is held to the fill. */
static bool
utf8_enc_eq(uint32_t cp, size_t cap, const char *expect, size_t expect_len)
{
    static const uint8_t fills[2] = { 0x7Fu, 0xA5u };
    char   buf[8];
    size_t f;
    size_t i;

    for (f = 0; f < 2; f++) {
        axl_memset(buf, fills[f], sizeof(buf));
        if (axl_utf8_encode(cp, buf, cap) != expect_len) {
            return false;
        }
        for (i = 0; i < expect_len; i++) {
            if ((uint8_t)buf[i] != (uint8_t)expect[i]) {
                return false;
            }
        }
        for (i = expect_len; i < sizeof(buf); i++) {
            if ((uint8_t)buf[i] != fills[f]) {
                return false;   /* wrote outside what it reported */
            }
        }
    }
    return true;
}

/* The "measure first" loop that the axl_utf8_encode docstring publishes,
   transcribed verbatim.
 *
 * A docstring idiom gets copy-pasted, so it is code whether or not anything
 * executes it — and the first draft of this one was WRONG: it substituted
 * U+FFFD for an unencodable codepoint and then skipped the fit check, so a
 * substitution that did not fit wrote nothing, did not stop the loop, and
 * carried on writing the codepoints AFTER it. The output lost a character out
 * of the MIDDLE while staying well-formed, which is the one failure shape no
 * byte-level assertion downstream would flag. Pinned here so the idiom cannot
 * silently rot back.
 *
 * @return bytes written to @a dst. */
static size_t
utf8_enc_idiom(const uint32_t *cps, size_t count, char *dst, size_t dst_size)
{
    char   *p     = dst;
    size_t  avail = dst_size;
    size_t  i;

    for (i = 0; i < count; i++) {
        uint32_t cp   = cps[i];
        size_t   need = axl_utf8_encode(cp, NULL, 0);

        if (need == 0) {                        /* unencodable — substitute */
            cp   = 0xFFFD;
            need = axl_utf8_encode(cp, NULL, 0);
        }
        if (need > avail) {
            break;                              /* buffer full — stop cleanly */
        }
        p     += axl_utf8_encode(cp, p, avail);
        avail -= need;
    }
    return (size_t)(p - dst);
}

static void
test_utf8_encode(void)
{
    /* 1-byte: U+0000..U+007F. U+0000 is a single 0x00 byte, NOT the overlong
       0xC0 0x80 "Modified UTF-8" form that axl_utf8_decode rejects. */
    test_check(axl_utf8_encode(0x0000, NULL, 0) == 1,
               "utf8_encode: U+0000 measures 1");
    test_check(utf8_enc_eq(0x0000, 8, "\x00", 1),
               "utf8_encode: U+0000 -> 00 (not overlong C0 80)");
    test_check(utf8_enc_eq(0x0041, 8, "A", 1),
               "utf8_encode: U+0041 'A' -> 41");
    test_check(axl_utf8_encode(0x007F, NULL, 0) == 1,
               "utf8_encode: U+007F measures 1 (1-byte max)");
    test_check(utf8_enc_eq(0x007F, 8, "\x7F", 1),
               "utf8_encode: U+007F -> 7F (1-byte max)");

    /* 2-byte: U+0080..U+07FF, both boundaries. */
    test_check(axl_utf8_encode(0x0080, NULL, 0) == 2,
               "utf8_encode: U+0080 measures 2 (2-byte min)");
    test_check(utf8_enc_eq(0x0080, 8, "\xC2\x80", 2),
               "utf8_encode: U+0080 -> C2 80");
    test_check(axl_utf8_encode(0x07FF, NULL, 0) == 2,
               "utf8_encode: U+07FF measures 2 (2-byte max)");
    test_check(utf8_enc_eq(0x07FF, 8, "\xDF\xBF", 2),
               "utf8_encode: U+07FF -> DF BF");

    /* 3-byte: U+0800..U+FFFF, both boundaries plus the two that bracket the
       surrogate hole and U+FFFD (what lenient callers substitute). */
    test_check(axl_utf8_encode(0x0800, NULL, 0) == 3,
               "utf8_encode: U+0800 measures 3 (3-byte min)");
    test_check(utf8_enc_eq(0x0800, 8, "\xE0\xA0\x80", 3),
               "utf8_encode: U+0800 -> E0 A0 80");
    test_check(utf8_enc_eq(0xD7FF, 8, "\xED\x9F\xBF", 3),
               "utf8_encode: U+D7FF -> ED 9F BF (last before surrogates)");
    test_check(utf8_enc_eq(0xE000, 8, "\xEE\x80\x80", 3),
               "utf8_encode: U+E000 -> EE 80 80 (first after surrogates)");
    test_check(utf8_enc_eq(0xFFFD, 8, "\xEF\xBF\xBD", 3),
               "utf8_encode: U+FFFD -> EF BF BD (replacement character)");
    test_check(axl_utf8_encode(0xFFFF, NULL, 0) == 3,
               "utf8_encode: U+FFFF measures 3 (3-byte max)");
    test_check(utf8_enc_eq(0xFFFF, 8, "\xEF\xBF\xBF", 3),
               "utf8_encode: U+FFFF -> EF BF BF");

    /* 4-byte: U+10000..U+10FFFF, both boundaries. */
    test_check(axl_utf8_encode(0x10000, NULL, 0) == 4,
               "utf8_encode: U+10000 measures 4 (4-byte min)");
    test_check(utf8_enc_eq(0x10000, 8, "\xF0\x90\x80\x80", 4),
               "utf8_encode: U+10000 -> F0 90 80 80");
    test_check(utf8_enc_eq(0x1F600, 8, "\xF0\x9F\x98\x80", 4),
               "utf8_encode: U+1F600 -> F0 9F 98 80");
    test_check(axl_utf8_encode(0x10FFFF, NULL, 0) == 4,
               "utf8_encode: U+10FFFF measures 4 (Unicode max)");
    test_check(utf8_enc_eq(0x10FFFF, 8, "\xF4\x8F\xBF\xBF", 4),
               "utf8_encode: U+10FFFF -> F4 8F BF BF");

    /* UTF-16 surrogates are not Unicode scalars: refused, not emitted as the
       3-byte CESU-8 / WTF-8 form a conforming decoder rejects. Refused in the
       sizing pass too, so a measure doubles as the validity query. */
    test_check(axl_utf8_encode(0xD800, NULL, 0) == 0,
               "utf8_encode: U+D800 measures 0 (surrogate refused)");
    test_check(utf8_enc_eq(0xD800, 8, "", 0),
               "utf8_encode: U+D800 writes nothing (first high surrogate)");
    test_check(utf8_enc_eq(0xDBFF, 8, "", 0),
               "utf8_encode: U+DBFF writes nothing (last high surrogate)");
    test_check(utf8_enc_eq(0xDC00, 8, "", 0),
               "utf8_encode: U+DC00 writes nothing (first low surrogate)");
    test_check(axl_utf8_encode(0xDFFF, NULL, 0) == 0,
               "utf8_encode: U+DFFF measures 0 (last low surrogate)");
    test_check(utf8_enc_eq(0xDFFF, 8, "", 0),
               "utf8_encode: U+DFFF writes nothing (last low surrogate)");

    /* Above U+10FFFF: no UTF-8 encoding exists. */
    test_check(axl_utf8_encode(0x110000, NULL, 0) == 0,
               "utf8_encode: U+110000 measures 0 (above Unicode max)");
    test_check(utf8_enc_eq(0x110000, 8, "", 0),
               "utf8_encode: U+110000 writes nothing (above Unicode max)");
    test_check(utf8_enc_eq(0x1FFFFF, 8, "", 0),
               "utf8_encode: U+1FFFFF writes nothing (old 4-byte ceiling)");
    test_check(utf8_enc_eq(0xFFFFFFFFu, 8, "", 0),
               "utf8_encode: 0xFFFFFFFF writes nothing (saturated value)");

    /* Bounded buffer, all-or-nothing at every sequence length: one byte short
       must write NOTHING, never a partial sequence. */
    test_check(utf8_enc_eq(0x0041, 0, "", 0),
               "utf8_encode: 1-byte into 0 bytes writes nothing");
    test_check(utf8_enc_eq(0x0080, 1, "", 0),
               "utf8_encode: 2-byte into 1 byte writes nothing");
    test_check(utf8_enc_eq(0x0800, 2, "", 0),
               "utf8_encode: 3-byte into 2 bytes writes nothing");
    test_check(utf8_enc_eq(0x10000, 3, "", 0),
               "utf8_encode: 4-byte into 3 bytes writes nothing");

    /* ...and an exact fit at every length still succeeds. */
    test_check(utf8_enc_eq(0x0041, 1, "A", 1),
               "utf8_encode: 1-byte into exactly 1 byte");
    test_check(utf8_enc_eq(0x0080, 2, "\xC2\x80", 2),
               "utf8_encode: 2-byte into exactly 2 bytes");
    test_check(utf8_enc_eq(0x0800, 3, "\xE0\xA0\x80", 3),
               "utf8_encode: 3-byte into exactly 3 bytes");
    test_check(utf8_enc_eq(0x10FFFF, 4, "\xF4\x8F\xBF\xBF", 4),
               "utf8_encode: 4-byte into exactly 4 bytes");

    /* A NULL destination measures regardless of dst_size — the capacity is
       not consulted, so a stale/garbage size cannot suppress the answer. */
    test_check(axl_utf8_encode(0x10000, NULL, 1) == 4,
               "utf8_encode: NULL dst measures even with dst_size 1");

    /* Round-trip against axl_utf8_decode across the whole BMP. U+0000 is
       excluded only because the decoder reads NUL as end-of-string; its
       encoding is pinned by the exact-byte assertion above. */
    {
        char     buf[8];
        uint32_t cp;
        uint32_t back = 0;
        size_t   n;
        size_t   covered = 0;
        bool     ok = true;

        for (cp = 1; cp <= 0xFFFF; cp++) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                if (axl_utf8_encode(cp, buf, sizeof(buf)) != 0) {
                    ok = false;
                    break;
                }
                continue;
            }
            n = axl_utf8_encode(cp, buf, sizeof(buf));
            if (n == 0 || n > 3) {
                ok = false;
                break;
            }
            buf[n] = '\0';
            if (axl_utf8_decode(buf, &back) != n || back != cp) {
                ok = false;
                break;
            }
            covered++;
        }
        test_check(ok, "utf8_encode: decode round-trip over the whole BMP");
        test_check(covered == 0xFFFF - 0x800,
                   "utf8_encode: BMP sweep covered every non-surrogate scalar");
    }

    /* ...and above it, where the sequence is 4 bytes. */
    {
        /* Both sides of every byte-rollover in the 4-byte ladder, so a bad
           shift or mask that only shows on a carry cannot hide between the
           two endpoints. */
        static const uint32_t above[] = { 0x10000u,  0x1003Fu, 0x10040u,
                                          0x10FFFu,  0x11000u, 0x1F600u,
                                          0x3FFFFu,  0x40000u, 0xFFFFFu,
                                          0x100000u, 0xE0001u, 0x10FFFFu };
        char     buf[8];
        uint32_t back = 0;
        size_t   n;
        size_t   i;
        bool     ok = true;

        for (i = 0; i < sizeof(above) / sizeof(above[0]); i++) {
            n = axl_utf8_encode(above[i], buf, sizeof(buf));
            if (n != 4) {
                ok = false;
                break;
            }
            buf[n] = '\0';
            if (axl_utf8_decode(buf, &back) != 4 || back != above[i]) {
                ok = false;
                break;
            }
        }
        test_check(ok, "utf8_encode: decode round-trip above the BMP");
    }

    /* The docstring promises @a dst may point into a buffer still being read
       ahead of it (the in-place unescaping shape, where the encoded form is
       never longer than the escape it replaces). Nothing is read back from
       @a dst, so writing at the front must leave the trailing source intact. */
    {
        char   inplace[16] = "\\u4E2Dxyz";
        size_t n           = axl_utf8_encode(0x4E2D, inplace, 3);

        test_check(n == 3 && (uint8_t)inplace[0] == 0xE4
                   && (uint8_t)inplace[1] == 0xB8 && (uint8_t)inplace[2] == 0xAD
                   && axl_strcmp(inplace + 6, "xyz") == 0,
                   "utf8_encode: writing over a source being read ahead of it");
    }

    /* AXL_UTF8_MAX_LEN is the "cannot be too small" size, like the
       AXL_U64_STR_MAX / AXL_DOUBLE_STR_MAX pair above. */
    test_check(AXL_UTF8_MAX_LEN == 4, "utf8_encode: AXL_UTF8_MAX_LEN is 4");
    {
        char max[AXL_UTF8_MAX_LEN];
        test_check(axl_utf8_encode(0x10FFFF, max, sizeof(max)) == 4,
                   "utf8_encode: AXL_UTF8_MAX_LEN holds the longest sequence");
    }

    /* The docstring's "measure first" loop, executed. */
    {
        static const uint32_t mixed[] = { 0x0041, 0xD800, 0x0042 };
        char   out[16];
        size_t n;

        /* Roomy: the unencodable U+D800 becomes U+FFFD between 'A' and 'B'. */
        axl_memset(out, 0, sizeof(out));
        n = utf8_enc_idiom(mixed, 3, out, sizeof(out));
        test_check(n == 5 && (uint8_t)out[0] == 'A' && (uint8_t)out[1] == 0xEF
                   && (uint8_t)out[2] == 0xBF && (uint8_t)out[3] == 0xBD
                   && (uint8_t)out[4] == 'B',
                   "utf8_encode idiom: unencodable -> U+FFFD inline");

        /* Tight: 3 bytes hold 'A', then the 3-byte U+FFFD does not fit. The
           loop must STOP, yielding the prefix "A" — an idiom that skipped the
           fit check after substituting would drop U+FFFD and still append 'B',
           producing "AB" with a character missing from the middle. */
        axl_memset(out, 0, sizeof(out));
        n = utf8_enc_idiom(mixed, 3, out, 3);
        test_check(n == 1 && (uint8_t)out[0] == 'A' && (uint8_t)out[1] == 0,
                   "utf8_encode idiom: substituted U+FFFD that will not fit stops the loop");

        /* Tight on an ordinary codepoint: same clean-prefix rule. */
        {
            static const uint32_t tail[] = { 0x0041, 0x0042, 0x0800 };
            axl_memset(out, 0, sizeof(out));
            n = utf8_enc_idiom(tail, 3, out, 4);
            test_check(n == 2 && (uint8_t)out[0] == 'A' && (uint8_t)out[1] == 'B'
                       && (uint8_t)out[2] == 0,
                       "utf8_encode idiom: a 3-byte tail that will not fit stops the loop");
        }
    }
}

// ---------------------------------------------------------------------------
// Base64 tests
// ---------------------------------------------------------------------------

static void
test_base64(void)
{
    char   *enc;
    void   *dec;
    size_t  dec_len;
    int     ret;

    // Encode empty
    enc = axl_base64_encode("", 0);
    test_check(enc != NULL && enc[0] == '\0', "base64: encode empty");
    axl_free(enc);

    // Encode "Hello"
    enc = axl_base64_encode("Hello", 5);
    test_check(enc != NULL, "base64: encode Hello non-NULL");
    test_check(axl_strcmp(enc, "SGVsbG8=") == 0, "base64: encode Hello = SGVsbG8=");
    axl_free(enc);

    // Decode "SGVsbG8="
    ret = axl_base64_decode("SGVsbG8=", &dec, &dec_len);
    test_check(ret == 0, "base64: decode returns 0");
    test_check(dec_len == 5, "base64: decode len 5");
    test_check(test_memcmp(dec, "Hello", 5) == 0, "base64: decode content Hello");
    axl_free(dec);

    // Roundtrip with binary data
    {
        uint8_t bin[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        enc = axl_base64_encode(bin, 4);
        ret = axl_base64_decode(enc, &dec, &dec_len);
        test_check(ret == 0 && dec_len == 4 && test_memcmp(dec, bin, 4) == 0,
                   "base64: binary roundtrip");
        axl_free(enc);
        axl_free(dec);
    }

    // Decode invalid
    ret = axl_base64_decode("!!!", &dec, &dec_len);
    test_check(ret == -1, "base64: decode invalid returns -1");
}

// ---------------------------------------------------------------------------
// base64url tests
// ---------------------------------------------------------------------------

static void
test_base64url(void)
{
    char   *enc;
    void   *dec;
    size_t  dec_len;
    int     ret;

    // Encode is unpadded.
    enc = axl_base64url_encode("Hello", 5);
    test_check(enc != NULL && axl_strcmp(enc, "SGVsbG8") == 0,
               "base64url: encode Hello = SGVsbG8 (no padding)");
    axl_free(enc);

    // URL-safe alphabet: bytes that map to indices 62/63 yield '-'/'_'.
    {
        uint8_t v[3] = { 0xFB, 0xFF, 0xFF };
        enc = axl_base64url_encode(v, 3);
        test_check(enc != NULL && axl_strcmp(enc, "-___") == 0,
                   "base64url: '-'/'_' replace '+'/'/'");
        axl_free(enc);
    }

    // Decode tolerates missing padding.
    ret = axl_base64url_decode("SGVsbG8", 7, &dec, &dec_len);
    test_check(ret == AXL_OK && dec_len == 5 && test_memcmp(dec, "Hello", 5) == 0,
               "base64url: decode unpadded -> Hello");
    axl_free(dec);

    // Round-trip arbitrary binary (encoder -> decoder).
    {
        uint8_t bin[5] = { 0x14, 0xFB, 0x9C, 0x03, 0xD9 };
        enc = axl_base64url_encode(bin, 5);
        ret = axl_base64url_decode(enc, axl_strlen(enc), &dec, &dec_len);
        test_check(ret == AXL_OK && dec_len == 5 && test_memcmp(dec, bin, 5) == 0,
                   "base64url: binary round-trip");
        axl_free(enc);
        axl_free(dec);
    }

    // Reject standard-base64 '+' and '/' and any '=' padding.
    test_check(axl_base64url_decode("SG+sbG8", 7, &dec, &dec_len) == AXL_ERR,
               "base64url: reject '+'");
    test_check(axl_base64url_decode("SG/sbG8", 7, &dec, &dec_len) == AXL_ERR,
               "base64url: reject '/'");
    test_check(axl_base64url_decode("SGVsbG8=", 8, &dec, &dec_len) == AXL_ERR,
               "base64url: reject '=' padding");

    // A length with remainder 1 is impossible.
    test_check(axl_base64url_decode("SGVsb", 5, &dec, &dec_len) == AXL_ERR,
               "base64url: reject remainder-1 length");

    // Empty.
    ret = axl_base64url_decode("", 0, &dec, &dec_len);
    test_check(ret == AXL_OK && dec_len == 0, "base64url: decode empty");
    axl_free(dec);
}

// ---------------------------------------------------------------------------
// strlcpy / strlcat tests
// ---------------------------------------------------------------------------

static void
test_strlcpy(void)
{
    char    buf[8];
    size_t  n;

    // Normal copy fits
    n = axl_strlcpy(buf, "hello", sizeof(buf));
    test_check(n == 5, "strlcpy: returns src len 5");
    test_check(axl_strcmp(buf, "hello") == 0, "strlcpy: copies string");

    // Truncation
    n = axl_strlcpy(buf, "longstring", sizeof(buf));
    test_check(n == 10, "strlcpy: returns src len 10(truncated)");
    test_check(axl_strcmp(buf, "longstr") == 0, "strlcpy: truncates to 7 chars");
    test_check(buf[7] == '\0', "strlcpy: NUL-terminated after truncation");

    // Zero-size dst
    n = axl_strlcpy(buf, "test", 0);
    test_check(n == 4, "strlcpy: dst_size 0 returns src len");
}

static void
test_strlcat(void)
{
    char    buf[12];
    size_t  n;

    // Normal cat
    axl_strlcpy(buf, "hello", sizeof(buf));
    n = axl_strlcat(buf, " world", sizeof(buf));
    test_check(n == 11, "strlcat: returns total len 11");
    test_check(axl_strcmp(buf, "hello world") == 0, "strlcat: appends");

    // Truncation
    axl_strlcpy(buf, "hello", sizeof(buf));
    n = axl_strlcat(buf, " world!!", sizeof(buf));
    test_check(n == 13, "strlcat: returns 13(truncated)");
    test_check(axl_strcmp(buf, "hello world") == 0, "strlcat: truncates correctly");
}

// ---------------------------------------------------------------------------
// Basic string/memory ops tests
// ---------------------------------------------------------------------------

static void
test_strlen(void)
{
    test_check(axl_strlen("hello") == 5, "strlen: basic");
    test_check(axl_strlen("") == 0, "strlen: empty");
    test_check(axl_strlen(NULL) == 0, "strlen: NULL");
}

static void
test_strcmp(void)
{
    test_check(axl_strcmp("abc", "abc") == 0, "strcmp: equal");
    test_check(axl_strcmp("abc", "abd") < 0, "strcmp: less");
    test_check(axl_strcmp("abd", "abc") > 0, "strcmp: greater");
    test_check(axl_strcmp("ab", "abc") < 0, "strcmp: shorter");
    test_check(axl_strcmp("abc", "ab") > 0, "strcmp: longer");
}

static void
test_strncmp(void)
{
    test_check(axl_strncmp("abc", "abd", 2) == 0, "strncmp: match prefix");
    test_check(axl_strncmp("abc", "abd", 3) < 0, "strncmp: differ at n");
    test_check(axl_strncmp("abc", "abc", 0) == 0, "strncmp: n=0");
}

static void
test_memcpy(void)
{
    char src[] = "hello";
    char dst[8] = {0};
    axl_memcpy(dst, src, 6);
    test_check(axl_strcmp(dst, "hello") == 0, "memcpy: basic");
    test_check(axl_memcpy(NULL, src, 5) == NULL, "memcpy: NULL dst");
}

static void
test_memset(void)
{
    char buf[8];
    axl_memset(buf, 'A', 4);
    buf[4] = '\0';
    test_check(axl_strcmp(buf, "AAAA") == 0, "memset: fill");

    axl_memset(buf, 0, 4);
    test_check(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0,
               "memset: zero fill");
}

static void
test_memchr(void)
{
    const char *s = "a.b.c";
    test_check(axl_memchr(s, '.', 5) == s + 1, "memchr: first '.' at index 1");
    test_check(axl_memchr(s, 'c', 5) == s + 4, "memchr: last byte");
    test_check(axl_memchr(s, 'z', 5) == NULL,  "memchr: absent -> NULL");
    test_check(axl_memchr(s, 'c', 4) == NULL,  "memchr: respects the length bound");
    /* The byte value is taken as unsigned char (>127 is matchable). */
    unsigned char hi[3] = { 0x00, 0xC3, 0x00 };
    test_check(axl_memchr(hi, 0xC3, 3) == (char *)hi + 1, "memchr: matches a high byte");
    test_check(axl_memchr(hi, 0x00, 0) == NULL, "memchr: zero length finds nothing");
}

static void
test_snprintf(void)
{
    char buf[32];
    int n;

    n = axl_snprintf(buf, sizeof(buf), "hello %s", "world");
    test_check(n == 11, "snprintf: returns length");
    test_check(axl_strcmp(buf, "hello world") == 0, "snprintf: formats string");

    n = axl_snprintf(buf, sizeof(buf), "%d + %d = %d", 1, 2, 3);
    test_check(axl_strcmp(buf, "1 + 2 = 3") == 0, "snprintf: formats ints");

    // Truncation
    n = axl_snprintf(buf, 6, "hello world");
    test_check(n == 11, "snprintf: returns full length on truncation");
    test_check(axl_strcmp(buf, "hello") == 0, "snprintf: truncates correctly");
    test_check(buf[5] == '\0', "snprintf: NUL terminates on truncation");
}

// Thin varargs trampolines: axl_vsnprintf takes a va_list, so a test needs a
// variadic caller to build one.
static int
vsn_call(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int     n;

    va_start(args, fmt);
    n = axl_vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

static void
test_vsnprintf(void)
{
    char buf[32];
    int  n;

    n = vsn_call(buf, sizeof(buf), "hello %s", "world");
    test_check(n == 11, "vsnprintf: returns length");
    test_check(axl_strcmp(buf, "hello world") == 0, "vsnprintf: formats string");

    n = vsn_call(buf, sizeof(buf), "%d + %d = %d", 1, 2, 3);
    test_check(n == 9, "vsnprintf: returns length for ints");
    test_check(axl_strcmp(buf, "1 + 2 = 3") == 0, "vsnprintf: formats ints");

    // Truncation: C99 semantics — return the would-be length, NUL-terminate.
    n = vsn_call(buf, 6, "hello world");
    test_check(n == 11, "vsnprintf: returns full length on truncation");
    test_check(axl_strcmp(buf, "hello") == 0, "vsnprintf: truncates correctly");
    test_check(buf[5] == '\0', "vsnprintf: NUL terminates on truncation");

    // Guards: a NULL buffer or zero size writes nothing and reports 0.
    n = vsn_call(NULL, 16, "x");
    test_check(n == 0, "vsnprintf: NULL buf -> 0");

    buf[0] = 'Z';
    n = vsn_call(buf, 0, "x");
    test_check(n == 0, "vsnprintf: zero size -> 0");
    test_check(buf[0] == 'Z', "vsnprintf: zero size does not write");

    // Exact fit: size == len + 1 stores the whole string.
    n = vsn_call(buf, 6, "hello");
    test_check(n == 5, "vsnprintf: exact fit returns length");
    test_check(axl_strcmp(buf, "hello") == 0, "vsnprintf: exact fit stores all bytes");
}

static void
test_snprintf_float(void)
{
    char buf[64];

    /* Default precision is 6 (C standard). */
    axl_snprintf(buf, sizeof(buf), "%f", 3.5);
    test_check(axl_strcmp(buf, "3.500000") == 0,
               "snprintf %f: default precision is 6 decimals");

    /* Explicit precision. */
    axl_snprintf(buf, sizeof(buf), "%.3f", 3.5);
    test_check(axl_strcmp(buf, "3.500") == 0,
               "snprintf %.3f: honors explicit precision");

    /* Zero precision drops the decimal point entirely. */
    axl_snprintf(buf, sizeof(buf), "%.0f", 7.0);
    test_check(axl_strcmp(buf, "7") == 0,
               "snprintf %.0f: no decimal point at precision 0");

    /* Rounding at the precision boundary. Use values whose nearest
       double is unambiguously on one side — a no-libm formatter (no
       arbitrary-precision Grisu/Ryu) cannot promise mathematical-ideal
       behavior on exact ties or representation-boundary values like
       1.005 (stored as 1.00499...), so we don't assert those. */
    axl_snprintf(buf, sizeof(buf), "%.1f", 3.14159);
    test_check(axl_strcmp(buf, "3.1") == 0,
               "snprintf %.1f: rounds down when below the boundary");
    axl_snprintf(buf, sizeof(buf), "%.1f", 3.96);
    test_check(axl_strcmp(buf, "4.0") == 0,
               "snprintf %.1f: rounds up and carries into integer part");
    axl_snprintf(buf, sizeof(buf), "%.0f", 2.7);
    test_check(axl_strcmp(buf, "3") == 0,
               "snprintf %.0f: rounds 2.7 up to 3");
    axl_snprintf(buf, sizeof(buf), "%.0f", 2.3);
    test_check(axl_strcmp(buf, "2") == 0,
               "snprintf %.0f: rounds 2.3 down to 2");

    /* Rounding that carries into the integer part. */
    axl_snprintf(buf, sizeof(buf), "%.1f", 9.99);
    test_check(axl_strcmp(buf, "10.0") == 0,
               "snprintf %.1f: rounding carries into integer part");

    /* Negative values keep the sign. */
    axl_snprintf(buf, sizeof(buf), "%.3f", -7.25);
    test_check(axl_strcmp(buf, "-7.250") == 0,
               "snprintf %f: negative values carry a leading minus");

    /* Zero. */
    axl_snprintf(buf, sizeof(buf), "%.2f", 0.0);
    test_check(axl_strcmp(buf, "0.00") == 0,
               "snprintf %f: zero formats with the requested decimals");

    /* Plus and space flags on a positive value. */
    axl_snprintf(buf, sizeof(buf), "%+.1f", 4.5);
    test_check(axl_strcmp(buf, "+4.5") == 0,
               "snprintf %+f: plus flag prefixes positive values");
    axl_snprintf(buf, sizeof(buf), "% .1f", 4.5);
    test_check(axl_strcmp(buf, " 4.5") == 0,
               "snprintf % f: space flag prefixes positive values");

    /* Width with right- and left-justification. */
    axl_snprintf(buf, sizeof(buf), "%8.2f", 3.14);
    test_check(axl_strcmp(buf, "    3.14") == 0,
               "snprintf %8.2f: right-justifies within width");
    axl_snprintf(buf, sizeof(buf), "%-8.2f", 3.14);
    test_check(axl_strcmp(buf, "3.14    ") == 0,
               "snprintf %-8.2f: left-justifies within width");

    /* Zero-padding to width. */
    axl_snprintf(buf, sizeof(buf), "%08.2f", 3.14);
    test_check(axl_strcmp(buf, "00003.14") == 0,
               "snprintf %08.2f: zero-pads to width");
    axl_snprintf(buf, sizeof(buf), "%08.2f", -3.14);
    test_check(axl_strcmp(buf, "-0003.14") == 0,
               "snprintf %08.2f: zero-pad keeps sign ahead of zeros");

    /* A larger integer part. */
    axl_snprintf(buf, sizeof(buf), "%.2f", 12345.6789);
    test_check(axl_strcmp(buf, "12345.68") == 0,
               "snprintf %f: multi-digit integer part with rounding");

    /* %F is the upper-case alias — same numeric output. */
    axl_snprintf(buf, sizeof(buf), "%.1F", 2.5);
    test_check(axl_strcmp(buf, "2.5") == 0,
               "snprintf %F: upper-case alias formats identically");

    /* Mixed with other conversions in one call (arg consumption stays
       in sync — the bug a missing %f handler would have caused). */
    axl_snprintf(buf, sizeof(buf), "%d:%.2f:%s", 7, 1.5, "x");
    test_check(axl_strcmp(buf, "7:1.50:x") == 0,
               "snprintf: %f consumes exactly one double in a mixed call");

    /* High precision: the Grisu2-backed %f honors the requested
       fractional width exactly (no cap). 1.0 is exact, so beyond its
       single significant digit the fraction is all zeros. */
    axl_snprintf(buf, sizeof(buf), "%.25f", 1.0);
    test_check(axl_strcmp(buf, "1.0000000000000000000000000") == 0,
               "snprintf %.25f: honors full requested precision (25 zeros)");

    /* Negative zero prints WITHOUT a sign (documented divergence from
       glibc, which prints -0.000000). Pin the chosen behavior. */
    double neg_zero = -0.0;
    axl_snprintf(buf, sizeof(buf), "%.1f", neg_zero);
    test_check(axl_strcmp(buf, "0.0") == 0,
               "snprintf %f: negative zero prints without a sign");

    /* NaN and +/-infinity — built without -ffast-math, so the
       dval!=dval and dval>DBL_MAX detections hold. Construct them
       without libm via arithmetic the compiler can't fold away. */
    volatile double zero = 0.0;
    volatile double big  = 1.0e308;
    double nan_val = zero / zero;
    double inf_val = big * 10.0;        /* overflows to +inf */
    axl_snprintf(buf, sizeof(buf), "%f", nan_val);
    test_check(axl_strcmp(buf, "nan") == 0,
               "snprintf %f: NaN prints \"nan\"");
    axl_snprintf(buf, sizeof(buf), "%f", inf_val);
    test_check(axl_strcmp(buf, "inf") == 0,
               "snprintf %f: positive overflow prints \"inf\"");
    axl_snprintf(buf, sizeof(buf), "%f", -inf_val);
    test_check(axl_strcmp(buf, "-inf") == 0,
               "snprintf %f: negative infinity prints \"-inf\"");
    axl_snprintf(buf, sizeof(buf), "%+f", inf_val);
    test_check(axl_strcmp(buf, "+inf") == 0,
               "snprintf %+f: plus flag prefixes +inf");
}

static void
test_snprintf_exp_g(void)
{
    char buf[64];

    /* %e — scientific, default precision 6, exponent >= 2 digits. */
    axl_snprintf(buf, sizeof(buf), "%e", 0.0);
    test_check(axl_strcmp(buf, "0.000000e+00") == 0,
               "snprintf %e: zero -> 0.000000e+00");
    axl_snprintf(buf, sizeof(buf), "%e", 31415.9);
    test_check(axl_strcmp(buf, "3.141590e+04") == 0,
               "snprintf %e: 31415.9 -> 3.141590e+04");
    axl_snprintf(buf, sizeof(buf), "%.2e", 31415.9);
    test_check(axl_strcmp(buf, "3.14e+04") == 0,
               "snprintf %.2e: honors precision");
    axl_snprintf(buf, sizeof(buf), "%e", 0.00042);
    test_check(axl_strcmp(buf, "4.200000e-04") == 0,
               "snprintf %e: small value gets negative exponent");
    axl_snprintf(buf, sizeof(buf), "%.0e", 95.0);
    test_check(axl_strcmp(buf, "1e+02") == 0,
               "snprintf %.0e: rounds 95 up across the decade to 1e+02");
    axl_snprintf(buf, sizeof(buf), "%E", 31415.9);
    test_check(axl_strcmp(buf, "3.141590E+04") == 0,
               "snprintf %E: upper-case exponent marker");
    axl_snprintf(buf, sizeof(buf), "%e", 1e100);
    test_check(axl_strcmp(buf, "1.000000e+100") == 0,
               "snprintf %e: 3-digit exponent");

    /* %g — shortest of %e/%f, trailing zeros trimmed, default P=6. */
    axl_snprintf(buf, sizeof(buf), "%g", 0.1);
    test_check(axl_strcmp(buf, "0.1") == 0,
               "snprintf %g: 0.1 -> 0.1 (shortest, no trailing zeros)");
    axl_snprintf(buf, sizeof(buf), "%g", 100000.0);
    test_check(axl_strcmp(buf, "100000") == 0,
               "snprintf %g: 100000 -> 100000 (fixed, no point)");
    axl_snprintf(buf, sizeof(buf), "%g", 1000000.0);
    test_check(axl_strcmp(buf, "1e+06") == 0,
               "snprintf %g: 1e6 -> 1e+06 (exp >= P switches to scientific)");
    axl_snprintf(buf, sizeof(buf), "%g", 0.0001);
    test_check(axl_strcmp(buf, "0.0001") == 0,
               "snprintf %g: 1e-4 stays fixed");
    axl_snprintf(buf, sizeof(buf), "%g", 0.00001);
    test_check(axl_strcmp(buf, "1e-05") == 0,
               "snprintf %g: 1e-5 (exp < -4 switches to scientific)");
    axl_snprintf(buf, sizeof(buf), "%g", 3.14);
    test_check(axl_strcmp(buf, "3.14") == 0,
               "snprintf %g: 3.14 -> 3.14");
    axl_snprintf(buf, sizeof(buf), "%g", 0.0);
    test_check(axl_strcmp(buf, "0") == 0,
               "snprintf %g: zero -> 0");
    axl_snprintf(buf, sizeof(buf), "%.3g", 3.14159);
    test_check(axl_strcmp(buf, "3.14") == 0,
               "snprintf %.3g: 3 significant digits");
    axl_snprintf(buf, sizeof(buf), "%.2g", 0.00012345);
    test_check(axl_strcmp(buf, "0.00012") == 0,
               "snprintf %.2g: 2 sig digits, fixed form");
    axl_snprintf(buf, sizeof(buf), "%G", 1000000.0);
    test_check(axl_strcmp(buf, "1E+06") == 0,
               "snprintf %G: upper-case exponent marker");
    axl_snprintf(buf, sizeof(buf), "%g", 1.5);
    test_check(axl_strcmp(buf, "1.5") == 0,
               "snprintf %g: 1.5 -> 1.5");

    /* %g width + flags route through the same padding path as %f. */
    axl_snprintf(buf, sizeof(buf), "%10.3g", 3.14159);
    test_check(axl_strcmp(buf, "      3.14") == 0,
               "snprintf %10.3g: right-justified within width");
    axl_snprintf(buf, sizeof(buf), "%-10.3g|", 3.14159);
    test_check(axl_strcmp(buf, "3.14      |") == 0,
               "snprintf %-10.3g: left-justified within width");
}

// ---------------------------------------------------------------------------
// Number parsing tests
// ---------------------------------------------------------------------------

static void
test_strtou64(void)
{
    test_check(axl_strtou64("12345") == 12345, "strtou64: decimal");
    test_check(axl_strtou64("0xFF") == 0xFF, "strtou64: hex 0xFF");
    test_check(axl_strtou64("0x1A") == 0x1A, "strtou64: hex 0x1A");
    test_check(axl_strtou64("0") == 0, "strtou64: zero");
    test_check(axl_strtou64(NULL) == 0, "strtou64: NULL returns 0");
}

static void
test_strtou64_with_offset(void)
{
    uint64_t v;

    /* No offset: just the base. */
    v = 0;
    test_check(axl_strtou64_with_offset("0x100", &v) == AXL_OK && v == 0x100,
               "strtou64_with_offset: no offset, hex");
    v = 0;
    test_check(axl_strtou64_with_offset("256", &v) == AXL_OK && v == 256,
               "strtou64_with_offset: no offset, decimal");
    v = 0;
    test_check(axl_strtou64_with_offset("0", &v) == AXL_OK && v == 0,
               "strtou64_with_offset: zero");

    /* With offset, both forms. */
    v = 0;
    test_check(axl_strtou64_with_offset("0x100+0x10", &v) == AXL_OK && v == 0x110,
               "strtou64_with_offset: hex+hex");
    v = 0;
    test_check(axl_strtou64_with_offset("256+16", &v) == AXL_OK && v == 272,
               "strtou64_with_offset: dec+dec");
    v = 0;
    test_check(axl_strtou64_with_offset("0x1000+0xFF", &v) == AXL_OK && v == 0x10FF,
               "strtou64_with_offset: 0x1000+0xFF");
    v = 0;
    test_check(axl_strtou64_with_offset("100+0x10", &v) == AXL_OK && v == 100 + 0x10,
               "strtou64_with_offset: dec+hex (mixed)");

    /* Errors: NULL, empty offset, garbage. */
    test_check(axl_strtou64_with_offset(NULL, &v) == AXL_ERR,
               "strtou64_with_offset: NULL string");
    test_check(axl_strtou64_with_offset("0x100", NULL) == AXL_ERR,
               "strtou64_with_offset: NULL out");
    test_check(axl_strtou64_with_offset("", &v) == AXL_ERR,
               "strtou64_with_offset: empty string");
    test_check(axl_strtou64_with_offset("0x100+", &v) == AXL_ERR,
               "strtou64_with_offset: dangling +");
    test_check(axl_strtou64_with_offset("0x100+ 0x10", &v) == AXL_ERR,
               "strtou64_with_offset: space after +");
    test_check(axl_strtou64_with_offset("0x100 +0x10", &v) == AXL_ERR,
               "strtou64_with_offset: space before +");
    test_check(axl_strtou64_with_offset("0x100xy", &v) == AXL_ERR,
               "strtou64_with_offset: trailing garbage");
    test_check(axl_strtou64_with_offset("0x100+0x10+0x10", &v) == AXL_ERR,
               "strtou64_with_offset: double offset");
    test_check(axl_strtou64_with_offset("-0x100+0x10", &v) == AXL_ERR,
               "strtou64_with_offset: negative rejected");
    test_check(axl_strtou64_with_offset("0x100++0x10", &v) == AXL_ERR,
               "strtou64_with_offset: ++ rejected");
    test_check(axl_strtou64_with_offset("0x100+-0x10", &v) == AXL_ERR,
               "strtou64_with_offset: +- rejected");
    test_check(axl_strtou64_with_offset("0x100++++0x10", &v) == AXL_ERR,
               "strtou64_with_offset: ++++ rejected");

    /* Overflow on base, on the sum. */
    test_check(axl_strtou64_with_offset("0xFFFFFFFFFFFFFFFF+0x1", &v) == AXL_ERR,
               "strtou64_with_offset: sum overflow");
    test_check(axl_strtou64_with_offset("0x10000000000000000", &v) == AXL_ERR,
               "strtou64_with_offset: base overflow");

    /* Edge: max u64 alone (no offset) is valid. */
    v = 0;
    test_check(axl_strtou64_with_offset("0xFFFFFFFFFFFFFFFF", &v) == AXL_OK
               && v == 0xFFFFFFFFFFFFFFFFULL,
               "strtou64_with_offset: max u64 alone");
    /* Edge: offset==0 still parses fine. */
    v = 0;
    test_check(axl_strtou64_with_offset("0x100+0", &v) == AXL_OK && v == 0x100,
               "strtou64_with_offset: +0 is valid");
}

// ---------------------------------------------------------------------------
// AxlStrReader
// ---------------------------------------------------------------------------

static bool is_alpha_pred(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_digit_pred(char c) { return c >= '0' && c <= '9'; }

static void
test_str_reader(void)
{
    AxlStrReader r;
    uint64_t v;
    const char *out;
    size_t out_len;

    /* init / eof / remaining / peek */
    axl_str_reader_init(&r, "abc");
    test_check(r.ok, "reader: fresh init has ok=true");
    test_check(!axl_str_reader_eof(&r), "reader: not eof on non-empty");
    test_check(axl_str_reader_remaining(&r) == 3, "reader: remaining=3");
    test_check(axl_str_reader_peek(&r) == 'a', "reader: peek 'a'");

    axl_str_reader_init(&r, "");
    test_check(axl_str_reader_eof(&r), "reader: empty string is eof");
    test_check(axl_str_reader_peek(&r) == '\0', "reader: peek at eof = 0");

    axl_str_reader_init(&r, NULL);
    test_check(axl_str_reader_eof(&r), "reader: NULL init is eof");
    test_check(r.ok, "reader: NULL init ok=true (nothing-to-parse, no error)");

    /* init_n with embedded NUL */
    axl_str_reader_init_n(&r, "ab\0cd", 5);
    test_check(axl_str_reader_remaining(&r) == 5, "reader: init_n with embedded NUL");

    /* consume_char */
    axl_str_reader_init(&r, "(x)");
    test_check(axl_str_reader_consume_char(&r, '(') && r.ok, "reader: consume '('");
    test_check(axl_str_reader_peek(&r) == 'x', "reader: cursor advanced past '('");
    test_check(!axl_str_reader_consume_char(&r, '!') && !r.ok,
               "reader: mismatch sets ok=false");
    /* sticky-ok: subsequent ops short-circuit */
    test_check(!axl_str_reader_consume_char(&r, 'x'),
               "reader: ok=false sticks (no further ops succeed)");

    /* consume_str */
    axl_str_reader_init(&r, "hello world");
    test_check(axl_str_reader_consume_str(&r, "hello") && r.ok,
               "reader: consume_str 'hello'");
    test_check(axl_str_reader_peek(&r) == ' ',
               "reader: cursor at space after consume_str");
    test_check(!axl_str_reader_consume_str(&r, "xyz") && !r.ok,
               "reader: consume_str mismatch → ok=false");

    /* consume_str NULL/empty no-op */
    axl_str_reader_init(&r, "abc");
    test_check(axl_str_reader_consume_str(&r, NULL),
               "reader: consume_str NULL is no-op true");
    test_check(axl_str_reader_consume_str(&r, ""),
               "reader: consume_str \"\" is no-op true");

    /* take_until */
    axl_str_reader_init(&r, "host:8080/path");
    out = NULL; out_len = 0;
    test_check(axl_str_reader_take_until(&r, ':', &out, &out_len),
               "reader: take_until ':'");
    test_check(out_len == 4 && axl_strncmp(out, "host", 4) == 0,
               "reader: take_until slice = 'host'");
    test_check(axl_str_reader_peek(&r) == '8',
               "reader: cursor past ':' delim");

    /* take_until at EOF (delim not found) */
    axl_str_reader_init(&r, "no-delim-here");
    test_check(!axl_str_reader_take_until(&r, ':', NULL, NULL),
               "reader: take_until missing delim → ok=false");
    test_check(!r.ok, "reader: take_until missing delim sticks ok=false");

    /* take_while */
    axl_str_reader_init(&r, "abc123!!");
    test_check(axl_str_reader_take_while(&r, is_alpha_pred, &out, &out_len),
               "reader: take_while alpha");
    test_check(out_len == 3 && axl_strncmp(out, "abc", 3) == 0,
               "reader: take_while alpha → 'abc'");
    test_check(axl_str_reader_take_while(&r, is_digit_pred, &out, &out_len),
               "reader: take_while digit");
    test_check(out_len == 3 && axl_strncmp(out, "123", 3) == 0,
               "reader: take_while digit → '123'");
    /* zero-length match is not an error */
    test_check(axl_str_reader_take_while(&r, is_digit_pred, &out, &out_len),
               "reader: take_while zero-length is fine");
    test_check(out_len == 0, "reader: zero-length take returns empty span");

    /* take_u64 — decimal, hex, bare hex with explicit base */
    axl_str_reader_init(&r, "12345abc");
    test_check(axl_str_reader_take_u64(&r, 10, &v) && v == 12345,
               "reader: take_u64 decimal");
    test_check(axl_str_reader_peek(&r) == 'a',
               "reader: cursor past digits");

    axl_str_reader_init(&r, "0xFF rest");
    test_check(axl_str_reader_take_u64(&r, 0, &v) && v == 0xFF,
               "reader: take_u64 auto 0x");

    axl_str_reader_init(&r, "ff rest");
    test_check(axl_str_reader_take_u64(&r, 16, &v) && v == 0xFF,
               "reader: take_u64 explicit base 16");

    /* take_u64 — no digits → ok=false, cursor not advanced */
    axl_str_reader_init(&r, "abc");
    test_check(!axl_str_reader_take_u64(&r, 10, &v),
               "reader: take_u64 no digits → false");
    test_check(!r.ok, "reader: take_u64 no digits sticks ok=false");

    /* take_ident */
    axl_str_reader_init(&r, "_id123 = 42");
    test_check(axl_str_reader_take_ident(&r, &out, &out_len),
               "reader: take_ident '_id123'");
    test_check(out_len == 6 && axl_strncmp(out, "_id123", 6) == 0,
               "reader: take_ident slice");
    axl_str_reader_init(&r, "1foo");
    test_check(!axl_str_reader_take_ident(&r, NULL, NULL),
               "reader: take_ident rejects digit-leading");

    /* skip_ws */
    axl_str_reader_init(&r, "   \t\nfoo");
    test_check(axl_str_reader_skip_ws(&r),
               "reader: skip_ws over mixed whitespace");
    test_check(axl_str_reader_peek(&r) == 'f',
               "reader: cursor at first non-ws");

    /* Composed parse: tagged hex N[xxxx] from a downstream consumer */
    {
        AxlStrReader r2;
        uint64_t val;
        axl_str_reader_init(&r2, "1[03A8]");
        axl_str_reader_consume_char(&r2, '1');
        axl_str_reader_consume_char(&r2, '[');
        axl_str_reader_take_u64(&r2, 16, &val);
        axl_str_reader_consume_char(&r2, ']');
        test_check(r2.ok, "reader: composed tagged-hex parse ok");
        test_check(axl_str_reader_eof(&r2), "reader: composed parse consumes all input");
        test_check(val == 0x03A8, "reader: tagged-hex value = 0x03A8");
    }

    /* Composed parse: malformed input fails cleanly */
    {
        AxlStrReader r2;
        uint64_t val;
        axl_str_reader_init(&r2, "1[XX]");
        axl_str_reader_consume_char(&r2, '1');
        axl_str_reader_consume_char(&r2, '[');
        axl_str_reader_take_u64(&r2, 16, &val);
        test_check(!r2.ok, "reader: composed parse fails on bad hex");
    }
}

// ---------------------------------------------------------------------------
// axl_sscanf
// ---------------------------------------------------------------------------

static void
test_sscanf(void)
{
    /* %d / %u / %x */
    {
        int      d = 0;
        unsigned u = 0;
        unsigned x = 0;
        int n = axl_sscanf("42 100 ff", "%d %u %x", &d, &u, &x);
        test_check(n == 3, "sscanf: %d %u %x — 3 conversions");
        test_check(d == 42, "sscanf: %d=42");
        test_check(u == 100, "sscanf: %u=100");
        test_check(x == 0xFF, "sscanf: %x=0xFF");
    }

    /* %i — auto-detect 0x */
    {
        int v = 0;
        test_check(axl_sscanf("0x10", "%i", &v) == 1, "sscanf: %i 0x10 → 1");
        test_check(v == 0x10, "sscanf: %i decoded as hex");
        test_check(axl_sscanf("16", "%i", &v) == 1, "sscanf: %i 16 → 1");
        test_check(v == 16, "sscanf: %i decoded as decimal");
    }

    /* Length modifiers */
    {
        unsigned char  hh = 0;
        unsigned short h = 0;
        unsigned long long ll = 0;
        size_t z = 0;
        int n = axl_sscanf("255 65535 18446744073709551615 1024",
                           "%hhu %hu %llu %zu", &hh, &h, &ll, &z);
        test_check(n == 4, "sscanf: hh/h/ll/z all parsed");
        test_check(hh == 255, "sscanf: %hhu=255");
        test_check(h == 65535, "sscanf: %hu=65535");
        test_check(ll == 18446744073709551615ULL, "sscanf: %llu=UINT64_MAX");
        test_check(z == 1024, "sscanf: %zu=1024");
    }

    /* Negative + signed */
    {
        int v = 0;
        test_check(axl_sscanf("-42", "%d", &v) == 1, "sscanf: %d -42 → 1");
        test_check(v == -42, "sscanf: %d=-42");
    }

    /* Boundary: INT64_MIN. The unsigned magnitude is INT64_MAX+1 =
     * 0x8000000000000000, and computing -(int64_t)v at that value
     * is signed-int overflow UB if done naively. Verify we pick up
     * INT64_MIN cleanly. */
    {
        long long v = 0;
        test_check(axl_sscanf("-9223372036854775808", "%lld", &v) == 1,
                   "sscanf: INT64_MIN parse succeeds");
        test_check(v == (-9223372036854775807LL - 1LL),
                   "sscanf: INT64_MIN value correct");
    }

    /* Literal char in format must match input */
    {
        int a = 0, b = 0;
        test_check(axl_sscanf("1.2", "%d.%d", &a, &b) == 2,
                   "sscanf: literal '.' separator");
        test_check(a == 1 && b == 2, "sscanf: a=1 b=2");
        /* Mismatch terminates scan, returns count so far */
        test_check(axl_sscanf("1,2", "%d.%d", &a, &b) == 1,
                   "sscanf: literal mismatch returns partial count");
    }

    /* Whitespace in format matches any run of input ws */
    {
        int a = 0, b = 0;
        test_check(axl_sscanf("1   2", "%d %d", &a, &b) == 2,
                   "sscanf: whitespace-flex");
        test_check(axl_sscanf("1  \t  2", "%d %d", &a, &b) == 2,
                   "sscanf: whitespace mixed tabs");
    }

    /* %c — exactly N chars */
    {
        char c1 = 0;
        char buf[4] = {0};
        test_check(axl_sscanf("xyz", "%c", &c1) == 1, "sscanf: %c 1 char");
        test_check(c1 == 'x', "sscanf: %c='x'");
        test_check(axl_sscanf("xyz", "%3c", buf) == 1, "sscanf: %3c");
        test_check(buf[0] == 'x' && buf[1] == 'y' && buf[2] == 'z',
                   "sscanf: %3c contents");
    }

    /* %s — bounded; required width */
    {
        char buf[8] = {0};
        test_check(axl_sscanf("hello world", "%7s", buf) == 1,
                   "sscanf: %7s reads 'hello'");
        test_check(axl_strcmp(buf, "hello") == 0, "sscanf: %s contents");
        /* Width without %s width = format error */
        test_check(axl_sscanf("foo", "%s", buf) == -1,
                   "sscanf: %s without width is malformed");
    }

    /* %[set] / %[^set] */
    {
        char buf[16] = {0};
        test_check(axl_sscanf("abc123 ", "%15[a-z]", buf) == 1,
                   "sscanf: %[a-z] match");
        test_check(axl_strcmp(buf, "abc") == 0, "sscanf: %[a-z] contents");

        char buf2[16] = {0};
        test_check(axl_sscanf("foo:bar", "%15[^:]", buf2) == 1,
                   "sscanf: %[^:] negated set");
        test_check(axl_strcmp(buf2, "foo") == 0, "sscanf: %[^:] contents");
    }

    /* Suppression with '*' */
    {
        int a = 0;
        test_check(axl_sscanf("ignore-me 42", "%*s %d", &a) == 1,
                   "sscanf: %*s suppression");
        test_check(a == 42, "sscanf: suppressed conversion not counted");
    }

    /* %% literal */
    {
        int a = 0;
        test_check(axl_sscanf("100%", "%d%%", &a) == 1, "sscanf: %% literal match");
        test_check(a == 100, "sscanf: a=100");
    }

    /* %n — bytes consumed so far */
    {
        int a = 0, n_out = 0;
        test_check(axl_sscanf("123abc", "%d%n", &a, &n_out) == 1,
                   "sscanf: %n doesn't count as conversion");
        test_check(a == 123, "sscanf: %n preceded by %d works");
        test_check(n_out == 3, "sscanf: %n=3 (bytes consumed)");
    }

    /* IPv4-style — the canonical dogfood case */
    {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        test_check(axl_sscanf("192.168.1.42", "%u.%u.%u.%u", &a, &b, &c, &d) == 4,
                   "sscanf: ipv4 parse");
        test_check(a == 192 && b == 168 && c == 1 && d == 42,
                   "sscanf: ipv4 octets");
    }

    /* Tagged-hex — downstream-consumer parser shape */
    {
        char tag = 0;
        unsigned int v = 0;
        int n = axl_sscanf("1[03A8]", "%c[%x]", &tag, &v);
        test_check(n == 2 && tag == '1' && v == 0x03A8,
                   "sscanf: tagged-hex 'N[xxxx]'");
    }

    /* NULL inputs — return -1 without dereferencing.
     * (Pass a valid &dummy pointer to avoid -Wformat tripping on NULL.) */
    {
        int dummy = 0;
        test_check(axl_sscanf(NULL, "%d", &dummy) == -1,
                   "sscanf: NULL str → -1");
    }
}

// ---------------------------------------------------------------------------
// axl_sscanf — float conversions
// ---------------------------------------------------------------------------

static void
test_sscanf_float(void)
{
    double d = 0.0;
    float  f = 0.0f;
    int    n;

    n = axl_sscanf("3.5", "%lf", &d);
    test_check(n == 1 && d == 3.5, "sscanf: %lf reads a double");

    n = axl_sscanf("2.25", "%f", &f);
    test_check(n == 1 && f == 2.25f, "sscanf: %f reads a float");

    d = 0.0;
    n = axl_sscanf("1e10", "%lf", &d);
    test_check(n == 1 && d == 1e10, "sscanf: %lf reads an exponent");

    d = 0.0;
    n = axl_sscanf("-0.5", "%le", &d);
    test_check(n == 1 && d == -0.5, "sscanf: %le is accepted");
    d = 0.0;
    n = axl_sscanf("7.5", "%lg", &d);
    test_check(n == 1 && d == 7.5, "sscanf: %lg is accepted");

    /* Mixed with other conversions, and the count is right. */
    {
        int    i = 0;
        double t = 0.0;
        n = axl_sscanf("cpu 3 36.5", "cpu %d %lf", &i, &t);
        test_check(n == 2 && i == 3 && t == 36.5,
                   "sscanf: float mixes with %d and literals");
    }

    /* Assignment suppression performs the conversion but stores nothing.
       Spelled %*f rather than the plan's %*lf because gcc warns on a
       length modifier it knows can have no receiving object; %*lf is
       pinned below, through a non-literal format. */
    d = 99.0;
    n = axl_sscanf("1.5 2.5", "%*f %lf", &d);
    test_check(n == 1 && d == 2.5, "sscanf: %*f suppresses assignment");

    /* A non-numeric input assigns nothing. */
    d = 99.0;
    n = axl_sscanf("abc", "%lf", &d);
    test_check(n == 0 && d == 99.0, "sscanf: non-numeric input assigns nothing");

    /* Input already exhausted: no conversion, nothing stored, and the
       count so far is what comes back. */
    d = 99.0;
    n = axl_sscanf("", "%lf", &d);
    test_check(n == 0 && d == 99.0, "sscanf: empty input assigns nothing");
    {
        int i = 0;
        d = 99.0;
        n = axl_sscanf("7", "%d%lf", &i, &d);
        test_check(n == 1 && i == 7 && d == 99.0,
                   "sscanf: input exhausted before the float returns the count so far");
    }

    /* The sign has to reach the conversion. The integer path strips '+'/'-'
       itself because axl_str_to_u64 is unsigned; a float conversion that
       copied that block would answer +0.5 here. */
    d = 0.0;
    n = axl_sscanf("-0.5", "%lf", &d);
    test_check(n == 1 && d == -0.5, "sscanf: %lf keeps the leading minus");
    f = 0.0f;
    n = axl_sscanf("-2.5", "%f", &f);
    test_check(n == 1 && f == -2.5f, "sscanf: %f keeps the leading minus");
    d = 0.0;
    n = axl_sscanf("+4.5", "%lf", &d);
    test_check(n == 1 && d == 4.5, "sscanf: %lf accepts an explicit plus");

    /* %E and %G are the same conversions as %e and %g for scanf. */
    f = 0.0f;
    n = axl_sscanf("1.5E3", "%E", &f);
    test_check(n == 1 && f == 1500.0f, "sscanf: %E is accepted");
    f = 0.0f;
    n = axl_sscanf("2.5", "%G", &f);
    test_check(n == 1 && f == 2.5f, "sscanf: %G is accepted");
    d = 0.0;
    n = axl_sscanf("-1.25e2", "%lE", &d);
    test_check(n == 1 && d == -125.0, "sscanf: %lE reads a double");
    d = 0.0;
    n = axl_sscanf("6.5", "%lG", &d);
    test_check(n == 1 && d == 6.5, "sscanf: %lG reads a double");

    /* Specials are values, not errors — d == nan is always false, so the
       assertions go through axl_isnan / axl_isinf. */
    d = 0.0;
    n = axl_sscanf("nan", "%lf", &d);
    test_check(n == 1 && axl_isnan(d), "sscanf: %lf reads nan");
    d = 0.0;
    n = axl_sscanf("-inf", "%lf", &d);
    test_check(n == 1 && axl_isinf(d) && d < 0.0, "sscanf: %lf reads -inf");
    d = 0.0;
    n = axl_sscanf("INFINITY", "%lf", &d);
    test_check(n == 1 && axl_isinf(d) && d > 0.0,
               "sscanf: %lf reads INFINITY case-insensitively");
    f = 0.0f;
    n = axl_sscanf("nan", "%f", &f);
    test_check(n == 1 && axl_isnan((double)f),
               "sscanf: %f reads nan into a float");

    /* A RANGE error is a successful conversion storing the saturated IEEE
       value — C99 stores +/-HUGE_VAL and sets ERANGE, and this family's own
       rule is that a range error still writes the value. Only a SYNTAX
       error, which consumes nothing, ends the scan. */
    d = 0.0;
    n = axl_sscanf("1e400", "%lf", &d);
    test_check(n == 1 && axl_isinf(d) && d > 0.0,
               "sscanf: %lf overflow counts as a conversion, stores +inf");
    d = 99.0;
    n = axl_sscanf("1e-400", "%lf", &d);
    test_check(n == 1 && d == 0.0,
               "sscanf: %lf underflow counts as a conversion, stores 0.0");
    f = 0.0f;
    n = axl_sscanf("1e300", "%f", &f);
    test_check(n == 1 && axl_isinf((double)f) && f > 0.0f,
               "sscanf: %f saturates a float-range overflow and still counts");
    {
        int i = 0;
        d = 0.0;
        n = axl_sscanf("1e400 7", "%lf %d", &d, &i);
        test_check(n == 2 && axl_isinf(d) && i == 7,
                   "sscanf: a range error does not end the scan");
    }

    /* %Lf and the nonsense length modifiers are rejected with -1 rather than
       quietly taken as %f or %lf — AXL has no long double. The format goes
       through a variable because axl_sscanf carries
       __attribute__((format(scanf, 2, 3))) and gcc would rightly reject the
       literal against a double *. */
    {
        const char *fmt_long_double = "%Lf";
        const char *fmt_bad_len     = "%llf";
        const char *fmt_suppress_l  = "%*lf %lf";
        d = 99.0;
        test_check(axl_sscanf("1.5", fmt_long_double, &d) == -1 && d == 99.0,
                   "sscanf: %Lf is rejected, AXL has no long double");
        d = 99.0;
        test_check(axl_sscanf("1.5", fmt_bad_len, &d) == -1 && d == 99.0,
                   "sscanf: %llf is rejected, not silently taken as %lf");
        /* %*lf IS accepted — the modifier is redundant with nothing to
           store, but it is not an error. Only gcc's -Wformat objects, so
           the format comes through a variable. */
        d = 99.0;
        test_check(axl_sscanf("1.5 2.5", fmt_suppress_l, &d) == 1 && d == 2.5,
                   "sscanf: %*lf suppresses assignment and is not an error");
    }

    /* An explicit field width caps the field at N bytes, the same semantics
       %Nc and %Ns already carry. N comes from the FORMAT STRING, so a width
       that truncates is truncating exactly what the caller asked for. A
       width the field never reaches changes nothing. */
    d = 99.0;
    n = axl_sscanf("3.14159", "%10lf", &d);
    test_check(n == 1 && d == 3.14159,
               "sscanf: a width wider than the float does not bite");

    /* A width that truncates mid-token parses the prefix and consumes
       exactly N bytes -- the trailing %c pins the cursor at byte 3. */
    {
        char c = 0;
        f = 99.0f;
        n = axl_sscanf("3.14159", "%3f%c", &f, &c);
        test_check(n == 2 && f == 3.1f && c == '4',
                   "sscanf: a width truncates the float mid-token");
    }

    /* The cursor advances by what was actually consumed, not by N. */
    {
        char c = 0;
        d = 99.0;
        n = axl_sscanf("3.5xy", "%10lf%c", &d, &c);
        test_check(n == 2 && d == 3.5 && c == 'x',
                   "sscanf: a width advances the cursor by bytes consumed");
    }

    /* Leading whitespace is skipped BEFORE the width applies, so it does not
       count against N: %3lf here still sees three digits of field. */
    d = 99.0;
    n = axl_sscanf("   3.14159", "%3lf", &d);
    test_check(n == 1 && d == 3.1,
               "sscanf: leading whitespace does not count against the width");

    /* Suppression and a width compose. */
    d = 99.0;
    n = axl_sscanf("1.5 2.5", "%*4f %lf", &d);
    test_check(n == 1 && d == 2.5,
               "sscanf: a width on a suppressed float still suppresses");

    /* A width never discards conversions already completed. */
    {
        int i = 0;
        d = 99.0;
        n = axl_sscanf("7 3.5", "%d %4lf", &i, &d);
        test_check(n == 2 && i == 7 && d == 3.5,
                   "sscanf: a float width keeps earlier conversions");
    }

    /* A width above the staging cap is -1. That is a property of the format
       string -- deterministic and checkable up front -- not of the input, so
       rejecting it is loud rather than a truncation risk. */
    d = 99.0;
    n = axl_sscanf("3.14159", "%300lf", &d);
    test_check(n == -1 && d == 99.0,
               "sscanf: a float width above the cap is rejected");

    /* The cursor resumes exactly one byte past the number. */
    {
        char c = 0;
        d = 0.0;
        n = axl_sscanf("36.6C", "%lf%c", &d, &c);
        test_check(n == 2 && d == 36.6 && c == 'C',
                   "sscanf: the cursor resumes just past the float");
        c = 0;
        d = 0.0;
        n = axl_sscanf("1.5e", "%lf%c", &d, &c);
        test_check(n == 2 && d == 1.5 && c == 'e',
                   "sscanf: a bare trailing 'e' is left unconsumed");
    }

    /* Leading whitespace is skipped, like every other numeric conversion. */
    d = 0.0;
    n = axl_sscanf("   4.5", "%lf", &d);
    test_check(n == 1 && d == 4.5, "sscanf: %lf skips leading whitespace");

    /* THE regression guard for the staging-buffer trap. 1 + 2^-53 is the
       exact midpoint between 1.0 and the next double up, and ties-to-even
       sends the midpoint to 1.0; the trailing '1' at byte 102 pushes the
       value just past it, so the correct answer is 1.0 + DBL_EPSILON. An
       implementation that stages the field into a fixed buffer — 80 bytes,
       say, copied from axl_str_reader_take_u64 where that bound IS provable
       — drops that digit and silently answers 1.0. 101 significant digits. */
    {
        const char *long_mantissa =
            "1.00000000000000011102230246251565404236316680908203125"
            "0000000000000000000000000000000000000000000000"
            "1";
        d = 0.0;
        n = axl_sscanf(long_mantissa, "%lf", &d);
        test_check(n == 1 && d == 1.0 + DBL_EPSILON,
                   "sscanf: a 101-digit mantissa is not truncated");
    }
}

// ---------------------------------------------------------------------------
// axl_sscanf — field width that does not fit in size_t
// ---------------------------------------------------------------------------

/* The width accumulator is shared by EVERY conversion that takes one, so a
   missing overflow guard is not one bug but five. A width above SIZE_MAX
   wraps to something SMALLER than the digits say -- 2^64+1 becomes 1 -- and
   every consumer then clamps that harmless-looking value against the input
   and returns a confidently wrong answer with a success count. Nothing here
   is memory-unsafe; that is exactly why it needs pinning, because no
   sanitizer, crash or leak will ever surface it.

   The literals below assume a 64-bit size_t, which both supported targets
   (X64, AARCH64) have. Asserted rather than assumed: on a hypothetical
   32-bit port these become "merely large" widths and would silently stop
   testing the guard. */
static_assert(sizeof(size_t) == 8,
              "the sscanf width-overflow literals below are SIZE_MAX-relative "
              "for a 64-bit size_t");

static void
test_sscanf_width_overflow(void)
{
    /* 18446744073709551615 = SIZE_MAX, the largest width that still fits.
       18446744073709551616 = SIZE_MAX + 1, which wraps to 0.
       18446744073709551617 = SIZE_MAX + 2, which wraps to 1 -- the worst
       case, since a width of 1 looks entirely plausible to every consumer. */

    /* --- Over SIZE_MAX is a malformed format string: -1, one per family. --- */
    {
        char buf[16];
        buf[0] = '!'; buf[1] = '\0';
        test_check(axl_sscanf("xyz", "%18446744073709551617c", buf) == -1
                   && buf[0] == '!',
                   "sscanf: %c width over SIZE_MAX is malformed, writes nothing");

        /* SIZE_MAX + 1 wraps the other way, to width 0: %c would then count a
           conversion having copied no bytes at all.
           Through a const char * rather than a literal, matching
           fmt_bad_len above: clang's own format checker wraps this width to 0
           and emits -Wformat "zero field width ... is unused", so a literal
           here would ship a warning for a form the test exists to reject. */
        {
            const char *fmt_width_wrap0 = "%18446744073709551616c";
            buf[0] = '!'; buf[1] = '\0';
            test_check(axl_sscanf("xyz", fmt_width_wrap0, buf) == -1
                       && buf[0] == '!',
                       "sscanf: %c width of exactly SIZE_MAX+1 is malformed too");
        }

        buf[0] = '!'; buf[1] = '\0';
        test_check(axl_sscanf("hello world", "%18446744073709551617s", buf) == -1
                   && buf[0] == '!',
                   "sscanf: %s width over SIZE_MAX is malformed, writes nothing");

        buf[0] = '!'; buf[1] = '\0';
        test_check(axl_sscanf("abc123", "%18446744073709551617[a-z]", buf) == -1
                   && buf[0] == '!',
                   "sscanf: %[ width over SIZE_MAX is malformed, writes nothing");

        /* A digit run far past the point of wrapping is the same answer, not
           whatever the accumulator happens to hold after 26 digits. */
        buf[0] = '!'; buf[1] = '\0';
        test_check(axl_sscanf("hello", "%99999999999999999999999999s", buf) == -1
                   && buf[0] == '!',
                   "sscanf: an absurdly long width is malformed, not wrapped");
    }
    {
        double d = 99.0;
        test_check(axl_sscanf("3.5", "%18446744073709551617lf", &d) == -1
                   && d == 99.0,
                   "sscanf: %lf width over SIZE_MAX is malformed, stores nothing");
    }
    {
        int i = -7;
        test_check(axl_sscanf("42", "%18446744073709551617d", &i) == -1
                   && i == -7,
                   "sscanf: %d width over SIZE_MAX is malformed, stores nothing");
    }

    /* --- A width at exactly SIZE_MAX still fits, and is honoured. The guard
           has to reject what does not fit, not "anything long". --- */
    {
        char buf[16];
        buf[0] = '!'; buf[1] = '\0';
        /* No input is that long, so this is 0 conversions -- but 0 (input
           ran out) rather than -1 (format is nonsense). */
        test_check(axl_sscanf("xyz", "%18446744073709551615c", buf) == 0
                   && buf[0] == '!',
                   "sscanf: %c width of exactly SIZE_MAX is accepted");

        buf[0] = '\0';
        test_check(axl_sscanf("hello world", "%18446744073709551615s", buf) == 1
                   && axl_strcmp(buf, "hello") == 0,
                   "sscanf: %s width of exactly SIZE_MAX is accepted");

        buf[0] = '\0';
        test_check(axl_sscanf("abc123", "%18446744073709551615[a-z]", buf) == 1
                   && axl_strcmp(buf, "abc") == 0,
                   "sscanf: %[ width of exactly SIZE_MAX is accepted");
    }
    {
        int i = 0;
        test_check(axl_sscanf("42", "%18446744073709551615d", &i) == 1 && i == 42,
                   "sscanf: %d width of exactly SIZE_MAX is accepted");
    }

    /* The float conversions cap the width at 256 (the staging buffer) long
       before SIZE_MAX, so their boundary is that cap. Pinned on both sides so
       the size_t guard cannot be mistaken for the one doing this work. */
    {
        double d = 0.0;
        test_check(axl_sscanf("3.5", "%256lf", &d) == 1 && d == 3.5,
                   "sscanf: a float width at exactly the 256 cap is accepted");
        d = 99.0;
        test_check(axl_sscanf("3.5", "%257lf", &d) == -1 && d == 99.0,
                   "sscanf: a float width one past the cap is rejected");
    }

    /* A malformed width is -1 for the WHOLE call, not the count so far --
       the same answer %s without a width already gives when it appears
       mid-format. The %d before it did store, but the return does not say
       so, and that is the long-standing convention for a bad format here:
       a caller cannot act on a partial count it was never handed. */
    {
        int  i = 0;
        char buf[16] = {0};
        test_check(axl_sscanf("7 hello", "%d %18446744073709551617s", &i, buf) == -1,
                   "sscanf: a malformed width is -1, not the count so far");
    }
}

// ---------------------------------------------------------------------------
// axl_str_to_{u32,s32,u64,s64}
// ---------------------------------------------------------------------------

static void
test_str_to_u64(void)
{
    uint64_t v;
    const char *end;

    /* Happy paths. */
    test_check(axl_str_to_u64("12345", 10, &v, NULL) == AXL_OK && v == 12345,
               "str_to_u64: decimal");
    test_check(axl_str_to_u64("0xFF", 0, &v, NULL) == AXL_OK && v == 0xFF,
               "str_to_u64: base=0 auto-hex");
    test_check(axl_str_to_u64("FF", 16, &v, NULL) == AXL_OK && v == 0xFF,
               "str_to_u64: explicit base=16 no prefix");
    test_check(axl_str_to_u64("0xff", 16, &v, NULL) == AXL_OK && v == 0xFF,
               "str_to_u64: explicit base=16 with prefix");
    test_check(axl_str_to_u64("1010", 2, &v, NULL) == AXL_OK && v == 10,
               "str_to_u64: base=2");
    test_check(axl_str_to_u64("zz", 36, &v, NULL) == AXL_OK && v == 35*36+35,
               "str_to_u64: base=36");
    test_check(axl_str_to_u64("  +42", 10, &v, NULL) == AXL_OK && v == 42,
               "str_to_u64: leading whitespace + sign");

    /* Boundary: UINT64_MAX. */
    test_check(axl_str_to_u64("18446744073709551615", 10, &v, NULL) == AXL_OK
               && v == UINT64_MAX,
               "str_to_u64: UINT64_MAX exact");
    test_check(axl_str_to_u64("18446744073709551616", 10, &v, NULL) == AXL_ERR,
               "str_to_u64: UINT64_MAX + 1 overflows");
    test_check(axl_str_to_u64("99999999999999999999", 10, &v, NULL) == AXL_ERR,
               "str_to_u64: huge value overflows");

    /* Errors. */
    test_check(axl_str_to_u64(NULL, 10, &v, NULL) == AXL_ERR, "str_to_u64: NULL");
    test_check(axl_str_to_u64("", 10, &v, NULL) == AXL_ERR, "str_to_u64: empty");
    test_check(axl_str_to_u64("abc", 10, &v, NULL) == AXL_ERR, "str_to_u64: garbage");
    test_check(axl_str_to_u64("-5", 10, &v, NULL) == AXL_ERR, "str_to_u64: rejects sign");
    test_check(axl_str_to_u64("12", 1, &v, NULL) == AXL_ERR, "str_to_u64: bad base");
    test_check(axl_str_to_u64("12", 37, &v, NULL) == AXL_ERR, "str_to_u64: bad base");
    test_check(axl_str_to_u64("12", 10, NULL, NULL) == AXL_ERR, "str_to_u64: NULL out");
    test_check(axl_str_to_u64("0x", 0, &v, NULL) == AXL_ERR, "str_to_u64: bare 0x");

    /* endptr — partial parses are rejected unless caller takes endptr. */
    test_check(axl_str_to_u64("123abc", 10, &v, NULL) == AXL_ERR,
               "str_to_u64: partial parse without endptr is error");
    test_check(axl_str_to_u64("123abc", 10, &v, &end) == AXL_OK && v == 123
               && *end == 'a',
               "str_to_u64: partial parse via endptr");

    /* endptr on error points back at nptr. */
    const char *src = "abc";
    test_check(axl_str_to_u64(src, 10, &v, &end) == AXL_ERR && end == src,
               "str_to_u64: endptr resets to nptr on error");
}

static void
test_str_to_u32(void)
{
    uint32_t v;
    test_check(axl_str_to_u32("4294967295", 10, &v, NULL) == 0
               && v == UINT32_MAX, "str_to_u32: UINT32_MAX");
    test_check(axl_str_to_u32("4294967296", 10, &v, NULL) == -1,
               "str_to_u32: UINT32_MAX + 1 overflows");
    test_check(axl_str_to_u32("0xffffffff", 0, &v, NULL) == 0
               && v == UINT32_MAX, "str_to_u32: hex max");
    test_check(axl_str_to_u32("0x100000000", 0, &v, NULL) == -1,
               "str_to_u32: hex overflow");
}

static void
test_str_to_s64(void)
{
    int64_t v;

    test_check(axl_str_to_s64("12345", 10, &v, NULL) == 0 && v == 12345,
               "str_to_s64: positive");
    test_check(axl_str_to_s64("-12345", 10, &v, NULL) == 0 && v == -12345,
               "str_to_s64: negative");
    test_check(axl_str_to_s64("+12345", 10, &v, NULL) == 0 && v == 12345,
               "str_to_s64: explicit +");

    /* Boundaries. */
    test_check(axl_str_to_s64("9223372036854775807", 10, &v, NULL) == 0
               && v == INT64_MAX, "str_to_s64: INT64_MAX");
    test_check(axl_str_to_s64("-9223372036854775808", 10, &v, NULL) == 0
               && v == INT64_MIN, "str_to_s64: INT64_MIN");
    test_check(axl_str_to_s64("9223372036854775808", 10, &v, NULL) == -1,
               "str_to_s64: INT64_MAX + 1 overflows");
    test_check(axl_str_to_s64("-9223372036854775809", 10, &v, NULL) == -1,
               "str_to_s64: INT64_MIN - 1 overflows");
}

static void
test_str_to_s32(void)
{
    int32_t v;
    test_check(axl_str_to_s32("2147483647", 10, &v, NULL) == 0
               && v == INT32_MAX, "str_to_s32: INT32_MAX");
    test_check(axl_str_to_s32("-2147483648", 10, &v, NULL) == 0
               && v == INT32_MIN, "str_to_s32: INT32_MIN");
    test_check(axl_str_to_s32("2147483648", 10, &v, NULL) == -1,
               "str_to_s32: INT32_MAX + 1 overflows");
    test_check(axl_str_to_s32("-2147483649", 10, &v, NULL) == -1,
               "str_to_s32: INT32_MIN - 1 overflows");
}

// ---------------------------------------------------------------------------
// Narrow-width variants: u16/u8/s16/s8
// ---------------------------------------------------------------------------

static void
test_str_to_narrow(void)
{
    uint16_t u16;
    uint8_t  u8;
    int16_t  s16;
    int8_t   s8;

    /* u16 */
    test_check(axl_str_to_u16("65535", 10, &u16, NULL) == 0 && u16 == UINT16_MAX,
               "str_to_u16: UINT16_MAX");
    test_check(axl_str_to_u16("65536", 10, &u16, NULL) == -1,
               "str_to_u16: UINT16_MAX + 1 overflows");
    test_check(axl_str_to_u16("0xffff", 0, &u16, NULL) == 0 && u16 == 0xFFFF,
               "str_to_u16: hex max");
    test_check(axl_str_to_u16("0x10000", 0, &u16, NULL) == -1,
               "str_to_u16: hex overflow");
    test_check(axl_str_to_u16("-1", 10, &u16, NULL) == -1,
               "str_to_u16: rejects sign");

    /* u8 */
    test_check(axl_str_to_u8("255", 10, &u8, NULL) == 0 && u8 == UINT8_MAX,
               "str_to_u8: UINT8_MAX");
    test_check(axl_str_to_u8("256", 10, &u8, NULL) == -1,
               "str_to_u8: UINT8_MAX + 1 overflows");
    test_check(axl_str_to_u8("0xff", 0, &u8, NULL) == 0 && u8 == 0xFF,
               "str_to_u8: hex max");

    /* s16 */
    test_check(axl_str_to_s16("32767", 10, &s16, NULL) == 0 && s16 == INT16_MAX,
               "str_to_s16: INT16_MAX");
    test_check(axl_str_to_s16("-32768", 10, &s16, NULL) == 0 && s16 == INT16_MIN,
               "str_to_s16: INT16_MIN");
    test_check(axl_str_to_s16("32768", 10, &s16, NULL) == -1,
               "str_to_s16: INT16_MAX + 1 overflows");
    test_check(axl_str_to_s16("-32769", 10, &s16, NULL) == -1,
               "str_to_s16: INT16_MIN - 1 overflows");

    /* s8 */
    test_check(axl_str_to_s8("127", 10, &s8, NULL) == 0 && s8 == INT8_MAX,
               "str_to_s8: INT8_MAX");
    test_check(axl_str_to_s8("-128", 10, &s8, NULL) == 0 && s8 == INT8_MIN,
               "str_to_s8: INT8_MIN");
    test_check(axl_str_to_s8("128", 10, &s8, NULL) == -1,
               "str_to_s8: INT8_MAX + 1 overflows");
    test_check(axl_str_to_s8("-129", 10, &s8, NULL) == -1,
               "str_to_s8: INT8_MIN - 1 overflows");
}

// ---------------------------------------------------------------------------
// Edge cases the code reviewer flagged
// ---------------------------------------------------------------------------

static void
test_str_to_edge_cases(void)
{
    uint64_t u64;
    int64_t  s64;
    const char *end;

    /* base=0 with leading zero is decimal — NOT octal (deliberately). */
    test_check(axl_str_to_u64("010", 0, &u64, NULL) == AXL_OK && u64 == 10,
               "edge: base=0 leading zero is decimal (not octal)");
    test_check(axl_str_to_u64("077", 0, &u64, NULL) == AXL_OK && u64 == 77,
               "edge: base=0 0xx is decimal");

    /* "0x" followed by non-hex — strict mode rejects, endptr mode
     * succeeds with v=0 and endptr at 'x' (matches strtoul). */
    test_check(axl_str_to_u64("0xZZ", 0, &u64, NULL) == AXL_ERR,
               "edge: 0xZZ rejected in strict mode");
    test_check(axl_str_to_u64("0xZZ", 0, &u64, &end) == AXL_OK
               && u64 == 0 && *end == 'x',
               "edge: 0xZZ in endptr mode parses as 0, leftover 'xZZ'");
    test_check(axl_str_to_u64("0x ", 0, &u64, NULL) == AXL_ERR,
               "edge: 0x<space> rejected in strict mode");

    /* Trailing whitespace — strict mode rejects (consistent with
     * "strict means no trailing content"). */
    test_check(axl_str_to_u64("123 ", 10, &u64, NULL) == AXL_ERR,
               "edge: trailing space rejected in strict mode");
    test_check(axl_str_to_u64("123\t", 10, &u64, NULL) == AXL_ERR,
               "edge: trailing tab rejected in strict mode");
    test_check(axl_str_to_u64("123 ", 10, &u64, &end) == AXL_OK
               && u64 == 123 && *end == ' ',
               "edge: trailing space OK in endptr mode");

    /* s64 negative-overflow must reset endptr to nptr. The earlier
     * version of axl_str_to_s64 left endptr advanced past the digits
     * because the inner u64 succeeded; only the range check failed. */
    const char *src = "-9223372036854775809";
    test_check(axl_str_to_s64(src, 10, &s64, &end) == -1 && end == src,
               "edge: s64 negative overflow resets endptr to nptr");
    src = "9223372036854775808";
    test_check(axl_str_to_s64(src, 10, &s64, &end) == -1 && end == src,
               "edge: s64 positive overflow resets endptr to nptr");
}

// ---------------------------------------------------------------------------
// axl_u64_to_str / axl_s64_to_str -- the reverse of the parse family above
// ---------------------------------------------------------------------------

static void
test_int_to_str(void)
{
    char b[72];

    #define U2S(v, base, want, msg)                                    \
        do {                                                           \
            axl_memset(b, 0, sizeof(b));                               \
            axl_u64_to_str((v), (base), b, sizeof(b));                 \
            test_check(axl_strcmp(b, (want)) == 0, (msg));             \
        } while (0)

    U2S(0,     10, "0",       "u64_to_str: zero");
    U2S(12345, 10, "12345",   "u64_to_str: decimal");
    U2S(255,   16, "ff",      "u64_to_str: hex is lowercase, no 0x prefix");
    U2S(255,    2, "11111111","u64_to_str: binary");
    U2S(8,      8, "10",      "u64_to_str: octal");
    U2S(35,    36, "z",       "u64_to_str: base 36 uses z");
    U2S(18446744073709551615ULL, 10, "18446744073709551615",
        "u64_to_str: UINT64_MAX");

    /* Bases above 16 must index a 36-entry digit table. A 16-entry one
       (the shape used by the private formatter in axl-format.c) reads
       out of bounds here, so pin a value whose digits sit past 'f'. */
    U2S(1295, 36, "zz", "u64_to_str: base 36 two-digit max uses the full table");
    U2S(19,   20, "j",  "u64_to_str: base 20 digit past 'f'");

    axl_memset(b, 0, sizeof(b));
    axl_s64_to_str(-12345, 10, b, sizeof(b));
    test_check(axl_strcmp(b, "-12345") == 0, "s64_to_str: negative");
    axl_memset(b, 0, sizeof(b));
    axl_s64_to_str(INT64_MIN, 10, b, sizeof(b));
    test_check(axl_strcmp(b, "-9223372036854775808") == 0,
               "s64_to_str: INT64_MIN does not overflow on negation");
    axl_memset(b, 0, sizeof(b));
    axl_s64_to_str(42, 10, b, sizeof(b));
    test_check(axl_strcmp(b, "42") == 0, "s64_to_str: positive has no sign");
    axl_memset(b, 0, sizeof(b));
    axl_s64_to_str(0, 10, b, sizeof(b));
    test_check(axl_strcmp(b, "0") == 0, "s64_to_str: zero has no sign");

    /* Sign leads, magnitude digits follow, at a non-decimal base -- and
       the parse side reads that exact spelling back. */
    {
        int64_t back = 0;
        axl_memset(b, 0, sizeof(b));
        axl_s64_to_str(-255, 16, b, sizeof(b));
        test_check(axl_strcmp(b, "-ff") == 0,
                   "s64_to_str: hex negative is '-' then the magnitude");
        test_check(axl_str_to_s64(b, 16, &back, NULL) == AXL_OK && back == -255,
                   "s64_to_str: str_to_s64 reads back the hex negative");
        axl_memset(b, 0, sizeof(b));
        axl_s64_to_str(-35, 36, b, sizeof(b));
        test_check(axl_strcmp(b, "-z") == 0,
                   "s64_to_str: base 36 negative is '-z'");
    }

    test_check(axl_u64_to_str(1, 1, b, sizeof(b)) == 0,
               "u64_to_str: base 1 is rejected");
    test_check(axl_u64_to_str(1, 37, b, sizeof(b)) == 0,
               "u64_to_str: base 37 is rejected");
    test_check(axl_u64_to_str(1, 10, NULL, 8) == 0,
               "u64_to_str: NULL buffer returns 0");
    test_check(axl_u64_to_str(1, 10, b, 0) == 0,
               "u64_to_str: zero size returns 0");

    /* base 0 means auto-detect when PARSING and is meaningless when
       RENDERING, so the renderer rejects it -- a deliberate asymmetry
       with axl_str_to_u64, which accepts it. */
    test_check(axl_u64_to_str(1, 0, b, sizeof(b)) == 0,
               "u64_to_str: base 0 is rejected (no auto-detect when rendering)");
    test_check(axl_s64_to_str(1, 0, b, sizeof(b)) == 0,
               "s64_to_str: base 0 is rejected");
    test_check(axl_s64_to_str(1, 37, b, sizeof(b)) == 0,
               "s64_to_str: base 37 is rejected");
    test_check(axl_s64_to_str(1, 10, NULL, 8) == 0,
               "s64_to_str: NULL buffer returns 0");
    test_check(axl_s64_to_str(1, 10, b, 0) == 0,
               "s64_to_str: zero size returns 0");

    /* Every rejection still leaves a readable string behind, so a caller
       that ignores the return value cannot walk off uninitialized
       memory. Only bufsz == 0 has nowhere to put the NUL. */
    axl_memset(b, 'Q', sizeof(b));
    axl_u64_to_str(12345, 0, b, sizeof(b));
    test_check(axl_strcmp(b, "") == 0,
               "u64_to_str: a rejected base still NUL-terminates");
    axl_memset(b, 'Q', sizeof(b));
    axl_s64_to_str(-12345, 40, b, sizeof(b));
    test_check(axl_strcmp(b, "") == 0,
               "s64_to_str: a rejected base still NUL-terminates");
    axl_memset(b, 'Q', sizeof(b));
    axl_u64_to_str(12345, 10, b, 0);
    test_check(b[0] == 'Q', "u64_to_str: bufsz 0 writes nothing at all");
    axl_memset(b, 'Q', sizeof(b));
    axl_s64_to_str(-12345, 10, b, 0);
    test_check(b[0] == 'Q', "s64_to_str: bufsz 0 writes nothing at all");

    /* axl_snprintf's truncation convention: write what fits, always
       NUL-terminate, and return the length the WHOLE rendering would
       have had -- so `ret >= bufsz` is the caller's truncation test. A
       truncated integer is a different, entirely plausible integer, and
       that return value is the only thing that tells the two apart. */
    {
        size_t n;

        axl_memset(b, 'Q', sizeof(b));
        n = axl_u64_to_str(12345, 10, b, 4);
        test_check(n == 5,
                   "u64_to_str: truncated reports the full length 5, not the 3 written");
        test_check(axl_strcmp(b, "123") == 0,
                   "u64_to_str: truncated keeps the leading digits, NUL-terminated");
        test_check(b[4] == 'Q',
                   "u64_to_str: truncated writes exactly bufsz bytes");

        /* "Call once to size, once to fill": the short call reports the
           same length as the ample one even though the two buffers hold
           different text, so the first return is a usable allocation
           size. */
        {
            char full[AXL_U64_STR_MAX];

            axl_memset(full, 'Q', sizeof(full));
            test_check(axl_u64_to_str(12345, 10, full, sizeof(full)) == n
                       && axl_strcmp(full, "12345") == 0
                       && axl_strcmp(b, "123") == 0,
                       "u64_to_str: sizing pass and filling pass agree on the length");
        }

        /* One byte short: "12345" needs 6 with its NUL. */
        axl_memset(b, 'Q', sizeof(b));
        n = axl_u64_to_str(12345, 10, b, 5);
        test_check(n == 5 && axl_strcmp(b, "1234") == 0,
                   "u64_to_str: no room for the NUL truncates, ret 5 >= bufsz 5");

        axl_memset(b, 'Q', sizeof(b));
        n = axl_u64_to_str(12345, 10, b, 6);
        test_check(n == 5 && axl_strcmp(b, "12345") == 0,
                   "u64_to_str: an exactly-sized buffer renders in full, ret 5 < bufsz 6");

        axl_memset(b, 'Q', sizeof(b));
        n = axl_u64_to_str(12345, 10, b, 1);
        test_check(n == 5 && axl_strcmp(b, "") == 0,
                   "u64_to_str: bufsz 1 holds only the NUL and still reports 5");
        test_check(b[1] == 'Q', "u64_to_str: bufsz 1 writes exactly one byte");

        axl_memset(b, 'Q', sizeof(b));
        n = axl_s64_to_str(-12345, 10, b, 4);
        test_check(n == 6 && axl_strcmp(b, "-12") == 0,
                   "s64_to_str: truncated keeps '-12' and reports the full 6");

        /* The sign counts against the budget: "-12345" needs 7. */
        axl_memset(b, 'Q', sizeof(b));
        n = axl_s64_to_str(-12345, 10, b, 6);
        test_check(n == 6 && axl_strcmp(b, "-1234") == 0,
                   "s64_to_str: the sign counts against the buffer budget");
        axl_memset(b, 'Q', sizeof(b));
        n = axl_s64_to_str(-12345, 10, b, 7);
        test_check(n == 6 && axl_strcmp(b, "-12345") == 0,
                   "s64_to_str: one more byte than the unsigned form renders it");

        axl_memset(b, 'Q', sizeof(b));
        n = axl_s64_to_str(-12345, 10, b, 1);
        test_check(n == 6 && axl_strcmp(b, "") == 0,
                   "s64_to_str: bufsz 1 holds only the NUL and still reports 6");
    }

    /* The two _STR_MAX macros are exactly sufficient -- and differ,
       which is the whole reason both exist. One byte less truncates by
       exactly one character while still reporting the full length, so
       these pin the sizes from both sides. */
    {
        char   umax[AXL_U64_STR_MAX];
        char   smax[AXL_S64_STR_MAX];
        size_t n;

        n = axl_u64_to_str(UINT64_MAX, 2, umax, sizeof(umax));
        test_check(n == 64 && axl_strlen(umax) == 64,
                   "AXL_U64_STR_MAX: exactly fits UINT64_MAX in base 2");
        test_check(axl_u64_to_str(UINT64_MAX, 2, umax, sizeof(umax) - 1) == 64
                   && axl_strlen(umax) == 63,
                   "AXL_U64_STR_MAX: one byte less truncates and reports 64");

        n = axl_s64_to_str(INT64_MIN, 2, smax, sizeof(smax));
        test_check(n == 65 && smax[0] == '-' && axl_strlen(smax) == 65,
                   "AXL_S64_STR_MAX: exactly fits INT64_MIN in base 2");
        test_check(axl_s64_to_str(INT64_MIN, 2, smax, sizeof(smax) - 1) == 65
                   && axl_strlen(smax) == 64,
                   "AXL_S64_STR_MAX: one byte less truncates and reports 65");
    }

    /* Symmetry: the pair round-trips at EVERY base the family accepts,
       not just the round ones -- the docstring claims 2..36, so sweep
       2..36 rather than sampling. */
    {
        static const uint64_t vals[] = {0, 1, 35, 42, 65535, INT64_MAX,
                                        UINT64_MAX};
        /* Both loops break on the first failure, so bad_base / bad_val
           still hold the culprit -- name it, because "survives every
           base" alone tells a future reader nothing about WHICH of the
           245 combinations regressed. */
        bool     all = true;
        int      bad_base = 0;
        uint64_t bad_val = 0;
        char     msg[160];
        for (int base = 2; base <= 36 && all; base++) {
            for (size_t j = 0; j < AXL_ARRAY_SIZE(vals) && all; j++) {
                uint64_t back;
                axl_memset(b, 0, sizeof(b));
                axl_u64_to_str(vals[j], base, b, sizeof(b));
                if (axl_str_to_u64(b, base, &back, NULL) != AXL_OK
                    || back != vals[j]) {
                    all = false;
                    bad_base = base;
                    bad_val = vals[j];
                }
            }
        }
        if (all) {
            axl_strlcpy(msg, "int round-trip: u64 survives every base 2..36",
                        sizeof(msg));
        } else {
            axl_snprintf(msg, sizeof(msg),
                         "int round-trip: u64 base %d value %llu -> \"%s\"",
                         bad_base, (unsigned long long)bad_val, b);
        }
        test_check(all, msg);
    }

    /* Same for the SIGNED pair, including both range endpoints. */
    {
        static const int64_t vals[] = {INT64_MIN, -65535, -42, -1,
                                       0, 1, 42, INT64_MAX};
        bool    all = true;
        int     bad_base = 0;
        int64_t bad_val = 0;
        char    msg[160];
        for (int base = 2; base <= 36 && all; base++) {
            for (size_t j = 0; j < AXL_ARRAY_SIZE(vals) && all; j++) {
                int64_t back;
                axl_memset(b, 0, sizeof(b));
                axl_s64_to_str(vals[j], base, b, sizeof(b));
                if (axl_str_to_s64(b, base, &back, NULL) != AXL_OK
                    || back != vals[j]) {
                    all = false;
                    bad_base = base;
                    bad_val = vals[j];
                }
            }
        }
        if (all) {
            axl_strlcpy(msg, "int round-trip: s64 survives every base 2..36",
                        sizeof(msg));
        } else {
            axl_snprintf(msg, sizeof(msg),
                         "int round-trip: s64 base %d value %lld -> \"%s\"",
                         bad_base, (long long)bad_val, b);
        }
        test_check(all, msg);
    }

    #undef U2S
}

// ---------------------------------------------------------------------------
// axl_strcasestr
// ---------------------------------------------------------------------------

static void
test_strcasestr(void)
{
    test_check(axl_strcasestr("Hello World", "WORLD") != NULL, "strcasestr: case diff");
    test_check(axl_strcasestr("Hello World", "world") != NULL, "strcasestr: lower needle");
    test_check(axl_strcasestr("abcdef", "CDE") != NULL, "strcasestr: middle match");
    test_check(axl_strcasestr("abcdef", "xyz") == NULL, "strcasestr: no match");
    test_check(axl_strcasestr("abc", "") != NULL, "strcasestr: empty needle");
    test_check(axl_strcasestr(NULL, "a") == NULL, "strcasestr: NULL haystack");
    test_check(axl_strcasestr("a", NULL) == NULL, "strcasestr: NULL needle");
    char *p = axl_strcasestr("FooBarBaz", "bar");
    test_check(p != NULL && p[0] == 'B' && p[1] == 'a' && p[2] == 'r',
               "strcasestr: points to match");

    /* Boyer-Moore-Horspool boundary cases. BMH kicks in at needle
       length 4; both short and long needles must produce identical
       results. The "last char repeats earlier" case is the classic
       BMH bug source — skip table for the last byte must reflect
       its later occurrence elsewhere in the needle. */
    test_check(axl_strstr("the quick brown fox", "quick") != NULL,
               "strstr: BMH path matches needle in middle");
    test_check(axl_strstr("aaaab", "aaab") != NULL,
               "strstr: BMH handles repeated-char pattern");
    test_check(axl_strstr("xxabcabcab", "abcab") != NULL,
               "strstr: BMH last-char-repeats-earlier (skip table override)");
    test_check(axl_strstr("abcde", "abcde") != NULL,
               "strstr: BMH whole-string match");
    test_check(axl_strstr("abcde", "edcba") == NULL,
               "strstr: BMH no match");
    test_check(axl_strstr("hello", "hellox") == NULL,
               "strstr: BMH needle-longer-than-haystack");
    test_check(axl_strcasestr("ABCDE FGHIJ KLMNO", "fghij") != NULL,
               "strcasestr: BMH case-insensitive long needle");
    test_check(axl_strcasestr("XXXX", "xxxxx") == NULL,
               "strcasestr: BMH needle-longer-than-haystack");
    test_check(axl_strcasestr("XYZAbcDef", "abcdef") != NULL,
               "strcasestr: BMH case-insensitive at tail");

    /* axl_strcasestr_len — length-bounded variant. Mirrors
       axl_strstr_len for the case-insensitive case so callers with
       non-NUL-terminated slices (AxlLineReader, etc.) skip the
       copy-to-stack-buffer dance. */
    {
        const char buf[] = "the QUICK brown FOX more bytes after";
        char *p = axl_strcasestr_len(buf, 16, "quick");  /* slice ends mid-word */
        test_check(p != NULL && p == buf + 4,
                   "strcasestr_len: matches inside bounded slice");

        /* Match would extend past the slice end — must not be found. */
        char *q = axl_strcasestr_len(buf, 16, "fox");
        test_check(q == NULL,
                   "strcasestr_len: rejects match past slice end");

        /* haystack_len == -1 should fall back to NUL-terminated. */
        char *r = axl_strcasestr_len(buf, -1, "FOX");
        test_check(r != NULL,
                   "strcasestr_len: -1 length defaults to NUL-terminated");

        /* Empty needle — pointer to start. */
        char *e = axl_strcasestr_len(buf, 16, "");
        test_check(e == buf,
                   "strcasestr_len: empty needle returns haystack");

        /* NULL guards. */
        test_check(axl_strcasestr_len(NULL, 4, "x") == NULL,
                   "strcasestr_len: NULL haystack");
        test_check(axl_strcasestr_len(buf, 4, NULL) == NULL,
                   "strcasestr_len: NULL needle");
    }
}

// ---------------------------------------------------------------------------
// axl_strrcasestr / axl_strrcasestr_len — reverse case-insensitive
// ---------------------------------------------------------------------------

static void
test_strrcasestr(void)
{
    /* Finds the LAST (highest-offset) match, case-insensitively. */
    char *p = axl_strrcasestr("ab AB Ab xy", "ab");
    test_check(p != NULL && p == (char *)"ab AB Ab xy" + 6, "strrcasestr: highest match");
    test_check(axl_strrcasestr("Hello World", "WORLD") != NULL, "strrcasestr: case diff");
    test_check(axl_strrcasestr("abcdef", "xyz") == NULL, "strrcasestr: no match");
    test_check(axl_strrcasestr("abc", "") != NULL, "strrcasestr: empty needle");
    test_check(axl_strrcasestr(NULL, "a") == NULL, "strrcasestr: NULL haystack");
    test_check(axl_strrcasestr("a", NULL) == NULL, "strrcasestr: NULL needle");
    test_check(axl_strrcasestr("abc", "abcd") == NULL, "strrcasestr: needle longer than haystack");

    /* Distinct from forward: forward returns the first, reverse the last. */
    const char *hay = "One two ONE two oNe";
    char *fwd = axl_strcasestr(hay, "one");
    char *rev = axl_strrcasestr(hay, "one");
    test_check(fwd == hay + 0 && rev == hay + 16,
               "strrcasestr: returns last where strcasestr returns first");

    /* Length-bounded: must not match past the slice end. */
    {
        const char buf[] = "xx ONE yy one zz";   /* "one" at 3 and 10 */
        char *q = axl_strrcasestr_len(buf, 13, "ONE");   /* slice covers both */
        test_check(q == buf + 10, "strrcasestr_len: last match within slice");

        char *q2 = axl_strrcasestr_len(buf, 9, "one");   /* slice ends before 2nd */
        test_check(q2 == buf + 3, "strrcasestr_len: bounded to first when slice short");

        char *q3 = axl_strrcasestr_len(buf, 12, "one");  /* 2nd 'one' [10,13) exceeds 12 */
        test_check(q3 == buf + 3, "strrcasestr_len: rejects match past slice end");

        test_check(axl_strrcasestr_len(buf, -1, "ZZ") == buf + 14,
                   "strrcasestr_len: -1 defaults to NUL-terminated");
        test_check(axl_strrcasestr_len(buf, 16, "") == buf,
                   "strrcasestr_len: empty needle returns haystack");
        test_check(axl_strrcasestr_len(NULL, 4, "x") == NULL, "strrcasestr_len: NULL haystack");
        test_check(axl_strrcasestr_len(buf, 4, NULL) == NULL, "strrcasestr_len: NULL needle");
    }

    /* BMH-length (>=4) needle works the same reversed. */
    test_check(axl_strrcasestr("zzQUICKzzquickzz", "quick") == (char *)"zzQUICKzzquickzz" + 9,
               "strrcasestr: long needle, highest match, case-insensitive");
}

// ---------------------------------------------------------------------------
// axl_fnmatch
// ---------------------------------------------------------------------------

static void
test_fnmatch(void)
{
    test_check(axl_fnmatch("*.txt", "hello.txt"), "fnmatch: *.txt matches");
    test_check(!axl_fnmatch("*.txt", "hello.c"), "fnmatch: *.txt rejects .c");
    test_check(axl_fnmatch("*", "anything"), "fnmatch: * matches all");
    test_check(axl_fnmatch("*", ""), "fnmatch: * matches empty");
    test_check(axl_fnmatch("?oo", "foo"), "fnmatch: ? single char");
    test_check(!axl_fnmatch("?oo", "oo"), "fnmatch: ? requires char");
    test_check(axl_fnmatch("[abc]x", "bx"), "fnmatch: [abc] class");
    test_check(!axl_fnmatch("[abc]x", "dx"), "fnmatch: [abc] rejects d");
    test_check(axl_fnmatch("[a-z]", "m"), "fnmatch: [a-z] range");
    test_check(!axl_fnmatch("[a-z]", "M"), "fnmatch: [a-z] case sensitive");
    test_check(axl_fnmatch("[!a-z]", "M"), "fnmatch: [!a-z] inverted");
    test_check(axl_fnmatch("hello", "hello"), "fnmatch: exact match");
    test_check(!axl_fnmatch("hello", "world"), "fnmatch: exact mismatch");
    test_check(!axl_fnmatch(NULL, "a"), "fnmatch: NULL pattern");
    test_check(!axl_fnmatch("a", NULL), "fnmatch: NULL string");
    test_check(axl_fnmatch("src/*.c", "src/main.c"), "fnmatch: path glob");
    test_check(!axl_fnmatch("src/*.c", "lib/main.c"), "fnmatch: path mismatch");
}

// ---------------------------------------------------------------------------
// UCS-2 primitive operations
// ---------------------------------------------------------------------------

static void
test_wcs(void)
{
    const unsigned short hello[] = { 'H', 'e', 'l', 'l', 'o', 0 };
    const unsigned short hello2[] = { 'H', 'e', 'l', 'l', 'o', 0 };
    const unsigned short world[] = { 'W', 'o', 'r', 'l', 'd', 0 };
    const unsigned short empty[] = { 0 };

    /* axl_wcslen */
    test_check(axl_wcslen(hello) == 5, "wcslen: basic");
    test_check(axl_wcslen(empty) == 0, "wcslen: empty");
    test_check(axl_wcslen(NULL) == 0, "wcslen: NULL");

    /* axl_wcscmp */
    test_check(axl_wcscmp(hello, hello2) == 0, "wcscmp: equal");
    test_check(axl_wcscmp(hello, world) < 0, "wcscmp: less");
    test_check(axl_wcscmp(world, hello) > 0, "wcscmp: greater");

    /* axl_wcseql */
    test_check(axl_wcseql(hello, hello2), "wcseql: equal");
    test_check(!axl_wcseql(hello, world), "wcseql: not equal");
    test_check(axl_wcseql(NULL, NULL), "wcseql: both NULL");
    test_check(!axl_wcseql(hello, NULL), "wcseql: one NULL");

    /* axl_wcscpy */
    unsigned short buf[8];
    axl_wcscpy(buf, hello, 8);
    test_check(axl_wcseql(buf, hello), "wcscpy: basic copy");

    unsigned short small[3];
    axl_wcscpy(small, hello, 3);
    test_check(small[2] == 0, "wcscpy: NUL on truncation");
    test_check(axl_wcslen(small) == 2, "wcscpy: truncated len");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
// axl_format / axl_vformat tests
// ---------------------------------------------------------------------------

// Capture callback: appends to a char buffer
typedef struct {
    char   buf[256];
    size_t len;
} FormatCapture;

static void
capture_write(const char *data, size_t len, void *ctx)
{
    FormatCapture *cap = (FormatCapture *)ctx;
    for (size_t i = 0; i < len && cap->len + 1 < sizeof(cap->buf); i++) {
        cap->buf[cap->len++] = data[i];
    }
    cap->buf[cap->len] = '\0';
}

static void
test_format_bytes(void)
{
    char buf[40];

    /* Sub-1KiB → bytes. */
    test_check(axl_format_bytes(0, buf, sizeof(buf)) > 0,
               "format_bytes: 0 returns >0");
    test_check(axl_strcmp(buf, "0 B") == 0,
               "format_bytes: 0 -> '0 B'");

    test_check(axl_format_bytes(512, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "512 B") == 0,
               "format_bytes: 512 -> '512 B'");

    /* Even multiples of binary units take the whole-number form. */
    test_check(axl_format_bytes(1024, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "1 KiB") == 0,
               "format_bytes: 1024 -> '1 KiB'");
    test_check(axl_format_bytes(8ULL << 30, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "8 GiB") == 0,
               "format_bytes: 8 GiB exact");

    /* Non-even values fall back to floor + raw byte hint. */
    test_check(axl_format_bytes(1500, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "1 KiB (1500 B)") == 0,
               "format_bytes: 1500 -> '1 KiB (1500 B)'");

    /* NULL / zero-cap rejection. */
    test_check(axl_format_bytes(123, NULL, sizeof(buf)) == -1,
               "format_bytes: NULL buf -> -1");
    test_check(axl_format_bytes(123, buf, 0) == -1,
               "format_bytes: zero capacity -> -1");

    /* Truncation: snprintf-style "would-write" return. */
    char tiny[2];
    int written = axl_format_bytes(8ULL << 30, tiny, sizeof(tiny));
    test_check(written > (int)sizeof(tiny),
               "format_bytes: truncation returns 'would-write' length > buf_size");
    test_check(tiny[sizeof(tiny) - 1] == '\0',
               "format_bytes: buffer NUL-terminated even on truncation");
}

/* Assert axl_dtoa produces exactly @a digits with decimal-point
   position @a decpt and sign @a neg for @a value. */
static void
check_dtoa(double value, const char *digits, int decpt, int neg,
           const char *label)
{
    char buf[AXL_DTOA_BUF_MIN];
    int  dp = -999, ng = -1;
    int  n = axl_dtoa(value, buf, sizeof(buf), &dp, &ng);
    bool ok = n == (int)axl_strlen(digits)
              && axl_strcmp(buf, digits) == 0
              && dp == decpt
              && ng == neg;
    test_check(ok, label);
}

static void
test_dtoa(void)
{
    /* Shortest round-trippable digits + decimal-point position.
       decpt = number of digits left of the point: value magnitude is
       <digits-as-int> x 10^(decpt - ndigits). */
    check_dtoa(0.0,    "0", 1, 0, "dtoa: 0.0 -> \"0\" decpt 1");
    check_dtoa(1.0,    "1", 1, 0, "dtoa: 1.0 -> \"1\" decpt 1");
    check_dtoa(1.5,    "15", 1, 0, "dtoa: 1.5 -> \"15\" decpt 1");
    check_dtoa(100.0,  "1", 3, 0, "dtoa: 100.0 -> \"1\" decpt 3 (no trailing zeros)");
    check_dtoa(0.5,    "5", 0, 0, "dtoa: 0.5 -> \"5\" decpt 0");
    check_dtoa(0.001,  "1", -2, 0, "dtoa: 0.001 -> \"1\" decpt -2");
    check_dtoa(0.1,    "1", 0, 0, "dtoa: 0.1 -> \"1\" decpt 0 (shortest, not 0.1000..)");
    check_dtoa(0.3,    "3", 0, 0, "dtoa: 0.3 -> \"3\" decpt 0");
    check_dtoa(1234.5, "12345", 4, 0, "dtoa: 1234.5 -> \"12345\" decpt 4");

    /* Sign, including negative zero. */
    check_dtoa(-1.5,   "15", 1, 1, "dtoa: -1.5 keeps digits, neg flag set");
    check_dtoa(-0.0,   "0", 1, 1, "dtoa: -0.0 -> \"0\" with neg flag set");

    /* Round-trip-critical values that distinguish shortest from naive.
       1.0/3.0 is the nearest double to 1/3; its shortest form is
       0.3333333333333333 (16 threes). */
    check_dtoa(1.0 / 3.0, "3333333333333333", 0, 0,
               "dtoa: 1/3 -> 16 threes (shortest round-trip)");
    /* 2^53 + 1 is not representable; 2^53 is, exactly. */
    check_dtoa(9007199254740992.0, "9007199254740992", 16, 0,
               "dtoa: 2^53 exact integer");

    /* Powers of ten and two stay short. */
    check_dtoa(1e20, "1", 21, 0, "dtoa: 1e20 -> \"1\" decpt 21");
    check_dtoa(1024.0, "1024", 4, 0, "dtoa: 1024 -> \"1024\" decpt 4");

    /* Magnitude extremes — these exercise the cached-powers table at
       both ends (top/bottom indices), validating the hand-transcribed
       table end-to-end. They are NOT the deepest point of the
       fractional digit loop, a claim this comment used to make and
       axl-dtoa.c used to cite: DBL_MAX and DBL_MIN reach kPow10 index
       8, and the subnormal never enters that loop at all. The block
       below covers the depths these miss. */
    check_dtoa(1.7976931348623157e308, "17976931348623157", 309, 0,
               "dtoa: DBL_MAX shortest digits + decpt 309");
    check_dtoa(2.2250738585072014e-308, "22250738585072014", -307, 0,
               "dtoa: DBL_MIN (smallest normal) shortest digits + decpt -307");
    check_dtoa(5e-324, "5", -323, 0,
               "dtoa: smallest subnormal -> \"5\" decpt -323");

    /* Fractional-loop depth. digit_gen scales grisu_round's target by
       kPow10[-kappa]; that index runs 2..16 over random bit patterns
       (1..16 once subnormals are included), and kPow10 now holds 20
       uint64 entries, so every depth is in range and every conversion
       gets the rounding refinement. Index 9 was the last depth a
       uint32 table could hold, so it is the seam.

       Only FOUR of these discriminate — the three literals below whose
       digits are their own name, and 4.7112871036659575e+180, whose
       pinned string changed from ...579. Those are cases where the old
       clamp-to-0 skipped the refinement AND the unrefined last digit
       was wrong. pi, 1e23 and 1/3 (above) sit at indices 11..13 and
       were equally unrefined, but their unrefined digits were already
       correct, so they are regression guards, not coverage. The round
       trip was exact at every depth both before and after, which is
       why nothing but an exact-digit assertion can tell the two
       versions apart. */
    check_dtoa(2.7797020033791574e+307, "27797020033791574", 308, 0,
               "dtoa: kPow10 index 9, the last uint32-representable entry");
    check_dtoa(3.141592653589793, "3141592653589793", 1, 0,
               "dtoa: pi, kPow10 index 11 (unrefined digits were already right)");
    check_dtoa(8.571428571428571, "8571428571428571", 1, 0,
               "dtoa: kPow10 index 11, refined to the correctly-rounded digits");
    check_dtoa(1.1428571428571428, "11428571428571428", 1, 0,
               "dtoa: kPow10 index 12, refined to the correctly-rounded digits");
    check_dtoa(0.14285714285714285, "14285714285714285", 0, 0,
               "dtoa: kPow10 index 13, refined to the correctly-rounded digits");
    check_dtoa(1e23, "9999999999999999", 23, 0,
               "dtoa: 1e23, kPow10 index 13 (Grisu2's shortest-but-not-optimal case)");
    check_dtoa(4.7112871036659575e+180, "47112871036659575", 181, 0,
               "dtoa: kPow10 index 16, the deepest depth observed");

    /* Argument validation: NULL buf, undersized buf, non-finite. */
    char small[4];
    test_check(axl_dtoa(1.5, NULL, AXL_DTOA_BUF_MIN, NULL, NULL) == 0,
               "dtoa: NULL buf returns 0");
    test_check(axl_dtoa(1.5, small, sizeof(small), NULL, NULL) == 0,
               "dtoa: undersized buf returns 0");
    char buf[AXL_DTOA_BUF_MIN];
    double zero = 0.0;
    double nan_v = zero / zero;       /* not finite */
    double inf_v = 1e308 * 10.0;      /* +inf */
    test_check(axl_dtoa(nan_v, buf, sizeof(buf), NULL, NULL) == 0,
               "dtoa: NaN returns 0 (caller handles non-finite)");
    test_check(axl_dtoa(inf_v, buf, sizeof(buf), NULL, NULL) == 0,
               "dtoa: +inf returns 0");

    /* NULL out params are allowed (just the digit string wanted). */
    int n = axl_dtoa(42.0, buf, sizeof(buf), NULL, NULL);
    test_check(n == 2 && axl_strcmp(buf, "42") == 0,
               "dtoa: NULL out_decpt/out_neg OK; digits still written");
}

static void
test_format(void)
{
    FormatCapture cap;

    // Basic string formatting
    cap.len = 0;
    axl_format(capture_write, &cap, "hello %s", "world");
    test_check(axl_strcmp(cap.buf, "hello world") == 0, "format: string");

    // Integer formatting
    cap.len = 0;
    axl_format(capture_write, &cap, "%d + %d = %d", 2, 3, 5);
    test_check(axl_strcmp(cap.buf, "2 + 3 = 5") == 0, "format: integers");

    // Hex formatting
    cap.len = 0;
    axl_format(capture_write, &cap, "0x%08x", 0xDEAD);
    test_check(axl_strcmp(cap.buf, "0x0000dead") == 0, "format: hex padded");

    // Multiple calls accumulate (callback is stateful)
    cap.len = 0;
    axl_format(capture_write, &cap, "A");
    axl_format(capture_write, &cap, "B");
    axl_format(capture_write, &cap, "C");
    test_check(axl_strcmp(cap.buf, "ABC") == 0, "format: multiple calls accumulate");

    // Empty format (intentional — suppress gcc warning for this edge case test)
    cap.len = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-zero-length"
    axl_format(capture_write, &cap, "");
#pragma GCC diagnostic pop
    test_check(cap.len == 0, "format: empty string");

    // Literal percent
    cap.len = 0;
    axl_format(capture_write, &cap, "100%%");
    test_check(axl_strcmp(cap.buf, "100%") == 0, "format: literal percent");
}

static void
test_utf16(void)
{
    /* U+0041 'A', U+00E9 'é' (2-byte UTF-8), U+20AC '€' (3-byte),
       U+1F600 😀 (4-byte UTF-8 / surrogate pair in UTF-16). */
    static const uint16_t u16[] = { 0x0041, 0x00E9, 0x20AC, 0xD83D, 0xDE00 };
    static const char     u8[]  = { 'A', (char)0xC3, (char)0xA9, (char)0xE2,
                                    (char)0x82, (char)0xAC, (char)0xF0,
                                    (char)0x9F, (char)0x98, (char)0x80 };

    /* UTF-16 -> UTF-8 measure + convert. */
    size_t need = axl_utf16_to_utf8(u16, 5, NULL, 0);
    test_check(need == sizeof(u8), "utf16->utf8: measured length (surrogate pair)");
    char out8[32];
    size_t n = axl_utf16_to_utf8(u16, 5, out8, sizeof(out8));
    test_check(n == sizeof(u8) && axl_memcmp(out8, u8, sizeof(u8)) == 0,
               "utf16->utf8: surrogate pair -> 4-byte UTF-8");

    /* UTF-8 -> UTF-16 measure + convert (round trip). */
    size_t need16 = axl_utf8_to_utf16(u8, sizeof(u8), NULL, 0);
    test_check(need16 == 5, "utf8->utf16: measured unit count");
    uint16_t out16[16];
    size_t u = axl_utf8_to_utf16(u8, sizeof(u8), out16, 16);
    bool eq16 = (u == 5);
    for (size_t i = 0; i < u; i++) {
        if (out16[i] != u16[i]) { eq16 = false; }
    }
    test_check(eq16, "utf8->utf16: 4-byte UTF-8 -> surrogate pair");

    /* Lone high surrogate -> U+FFFD (3-byte EF BF BD). */
    static const uint16_t lone[] = { 0x0041, 0xD83D, 0x0042 };
    n = axl_utf16_to_utf8(lone, 3, out8, sizeof(out8));
    test_check(n == 5 && (uint8_t)out8[0] == 'A' && (uint8_t)out8[1] == 0xEF
               && (uint8_t)out8[2] == 0xBF && (uint8_t)out8[3] == 0xBD
               && (uint8_t)out8[4] == 'B',
               "utf16->utf8: lone surrogate -> U+FFFD");

    /* Clean truncation on a tight buffer (no partial sequence). */
    char tiny[2];
    n = axl_utf16_to_utf8(u16, 5, tiny, sizeof(tiny));   /* only 'A' fits (é needs 2) */
    test_check(n == 1 && tiny[0] == 'A', "utf16->utf8: clean truncation at boundary");
}

static void
test_math_special_values(void)
{
    double inf  = AXL_MATH_INF;
    double nan  = AXL_MATH_NAN;

    test_check(axl_isnan(nan), "isnan: NaN is NaN");
    test_check(!axl_isnan(0.0), "isnan: zero is not NaN");
    test_check(!axl_isnan(inf), "isnan: infinity is not NaN");
    test_check(!axl_isnan(AXL_MATH_DBL_MAX), "isnan: DBL_MAX is not NaN");

    test_check(axl_isinf(inf), "isinf: +inf is infinite");
    test_check(axl_isinf(-inf), "isinf: -inf is infinite");
    test_check(!axl_isinf(nan), "isinf: NaN is not infinite");
    test_check(!axl_isinf(AXL_MATH_DBL_MAX), "isinf: DBL_MAX is finite");

    test_check(axl_isfinite(0.0), "isfinite: zero is finite");
    test_check(axl_isfinite(AXL_MATH_DBL_MAX), "isfinite: DBL_MAX is finite");
    test_check(axl_isfinite(AXL_MATH_DBL_TRUE_MIN),
               "isfinite: smallest subnormal is finite");
    test_check(!axl_isfinite(inf), "isfinite: +inf is not finite");
    test_check(!axl_isfinite(-inf), "isfinite: -inf is not finite");
    test_check(!axl_isfinite(nan), "isfinite: NaN is not finite");

    /* The constants must be the values they claim. DBL_TRUE_MIN is
       subnormal, so halving it reaches zero and doubling recovers it —
       that pins it as the SMALLEST, not merely a small, double. */
    test_check(AXL_MATH_DBL_TRUE_MIN / 2.0 == 0.0,
               "DBL_TRUE_MIN: halving underflows to zero");
    test_check(AXL_MATH_DBL_MAX * 2.0 == inf,
               "DBL_MAX: doubling overflows to infinity");
}

static void
test_str_to_double_basic(void)
{
    double      d;
    const char *end;

    /* Tier 1: mantissa <= 2^53 and |exp| <= 22, exactly representable. */
    test_check(axl_str_to_double("0", &d, NULL) == AXL_OK && d == 0.0,
               "str_to_double: zero");
    test_check(axl_str_to_double("1", &d, NULL) == AXL_OK && d == 1.0,
               "str_to_double: one");
    test_check(axl_str_to_double("-1", &d, NULL) == AXL_OK && d == -1.0,
               "str_to_double: negative");
    test_check(axl_str_to_double("1.5", &d, NULL) == AXL_OK && d == 1.5,
               "str_to_double: 1.5 is exact in binary");
    test_check(axl_str_to_double("36.6", &d, NULL) == AXL_OK && d == 36.6,
               "str_to_double: a sensor-style reading");
    test_check(axl_str_to_double("0.001", &d, NULL) == AXL_OK && d == 0.001,
               "str_to_double: leading zeros after the point");
    test_check(axl_str_to_double("1e10", &d, NULL) == AXL_OK && d == 1e10,
               "str_to_double: positive exponent");
    test_check(axl_str_to_double("1E10", &d, NULL) == AXL_OK && d == 1e10,
               "str_to_double: capital E exponent");
    test_check(axl_str_to_double("1e-10", &d, NULL) == AXL_OK && d == 1e-10,
               "str_to_double: negative exponent");
    test_check(axl_str_to_double("+2.5e+2", &d, NULL) == AXL_OK && d == 250.0,
               "str_to_double: explicit + on both mantissa and exponent");
    test_check(axl_str_to_double("  \t 7.25", &d, NULL) == AXL_OK && d == 7.25,
               "str_to_double: leading whitespace is skipped");

    /* endptr lands just past what was consumed. */
    test_check(axl_str_to_double("3.5abc", &d, &end) == AXL_OK
               && d == 3.5 && axl_strcmp(end, "abc") == 0,
               "str_to_double: endptr stops at the first unconsumed byte");

    /* STRICT MODE: with no endptr the whole input must be consumed, the
       same rule the eight integer parsers enforce (see the
       axl_str_to_u64 cases above). *out stays untouched, because
       trailing content is a SYNTAX failure -- it takes precedence over
       the range-error rule, which would otherwise have written it. */
    d = 99.0;
    test_check(axl_str_to_double("36.6C", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode rejects a trailing unit suffix");
    d = 99.0;
    test_check(axl_str_to_double("3.5.7", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode rejects a second decimal point");
    d = 99.0;
    test_check(axl_str_to_double("1e", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode rejects an unconsumed 'e'");
    d = 99.0;
    test_check(axl_str_to_double("nanq", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode rejects trailing bytes after nan");
    /* Leading whitespace is skipped, TRAILING whitespace is trailing
       content -- exactly what axl_str_to_u64("123 ", ..., NULL) does. */
    d = 99.0;
    test_check(axl_str_to_double(" 5 ", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode rejects trailing space");
    d = 99.0;
    test_check(axl_str_to_double("1.5\t", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode rejects trailing tab");
    /* Strict beats the range error: "1e400" alone writes +inf and
       returns AXL_ERR, but with trailing bytes nothing is written. */
    d = 99.0;
    test_check(axl_str_to_double("1e400xyz", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: strict mode outranks the range-error write");
    /* Passing endptr opts back into partial parsing. */
    d = 99.0;
    test_check(axl_str_to_double("36.6C", &d, &end) == AXL_OK
               && d == 36.6 && axl_strcmp(end, "C") == 0,
               "str_to_double: endptr mode still accepts a trailing suffix");

    /* Syntax errors leave *out untouched and reset endptr to nptr. */
    d = 99.0;
    const char *src = "abc";
    test_check(axl_str_to_double(src, &d, &end) == AXL_ERR
               && d == 99.0 && end == src,
               "str_to_double: syntax error leaves out untouched, endptr = nptr");
    d = 99.0;
    test_check(axl_str_to_double("", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: empty string is a syntax error");
    d = 99.0;
    test_check(axl_str_to_double("e5", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: exponent with no mantissa is a syntax error");
    test_check(axl_str_to_double(NULL, &d, NULL) == AXL_ERR,
               "str_to_double: NULL input is rejected");
    test_check(axl_str_to_double("1.0", NULL, NULL) == AXL_ERR,
               "str_to_double: NULL out is rejected");

    /* Specials are VALUES, so AXL_OK. */
    test_check(axl_str_to_double("nan", &d, NULL) == AXL_OK && axl_isnan(d),
               "str_to_double: nan");
    test_check(axl_str_to_double("NaN", &d, NULL) == AXL_OK && axl_isnan(d),
               "str_to_double: NaN is case-insensitive");
    test_check(axl_str_to_double("inf", &d, NULL) == AXL_OK
               && axl_isinf(d) && d > 0,
               "str_to_double: inf");
    test_check(axl_str_to_double("-INFINITY", &d, NULL) == AXL_OK
               && axl_isinf(d) && d < 0,
               "str_to_double: -INFINITY, long spelling, case-insensitive");

    /* Range errors are TIER 2 behaviour and are asserted in Task 2b, not
       here: they route to the tier-2 stub, so asserting them now would
       commit a knowingly-red suite and trip the pass-count ratchet. */

    /* Hex floats are NOT the hex-float grammar: "0x1.8p3" parses as 0
       and stops at 'x'. Pinning this stops someone "helpfully" adding
       partial hex support later. */
    test_check(axl_str_to_double("0x1.8p3", &d, &end) == AXL_OK
               && d == 0.0 && axl_strcmp(end, "x1.8p3") == 0,
               "str_to_double: hex float is not accepted; stops after the 0");
}

static void
test_str_to_double_exact(void)
{
    double d;

    /* THE case. Naive m * pow(10,23) gives 100000000000000008388608;
       the correctly-rounded answer is 99999999999999991611392. Comparing
       against the literal 1e23 works because the COMPILER rounds
       correctly, so this asserts we agree with it. */
    test_check(axl_str_to_double("1e23", &d, NULL) == AXL_OK && d == 1e23,
               "str_to_double exact: 1e23 (exp > 22, tier 2)");

    /* Mantissa above 2^53: 2^53+1 is not representable, and must round
       to 2^53 (ties-to-even), NOT to 2^53+2. */
    test_check(axl_str_to_double("9007199254740993", &d, NULL) == AXL_OK
               && d == 9007199254740992.0,
               "str_to_double exact: 2^53+1 rounds to even");

    /* The input that hung PHP and Java. Must terminate and be exact. */
    test_check(axl_str_to_double("2.2250738585072011e-308", &d, NULL) == AXL_OK
               && d == 2.2250738585072011e-308,
               "str_to_double exact: the PHP/Java hang case terminates");

    /* Subnormals. */
    test_check(axl_str_to_double("5e-324", &d, NULL) == AXL_OK
               && d == 5e-324 && d > 0.0,
               "str_to_double exact: smallest subnormal");
    test_check(axl_str_to_double("1.7976931348623157e308", &d, NULL) == AXL_OK
               && d == 1.7976931348623157e308,
               "str_to_double exact: DBL_MAX");

    /* Long digit strings must not be truncated into a wrong answer. */
    test_check(axl_str_to_double(
                   "0.30000000000000000000000000000000000000001", &d, NULL)
               == AXL_OK && d == 0.3,
               "str_to_double exact: 41 digits rounds to the nearest double");

    /* A 768-digit input -- the classic slow-path stress case. Built rather
       than pasted so the test stays readable. */
    {
        char big[800];
        size_t i;
        big[0] = '1'; big[1] = '.';
        for (i = 2; i < 770; i++) { big[i] = '0'; }
        big[770] = '1';
        big[771] = '\0';
        /* 1.000...0001 with the 1 far past double precision rounds to 1.0 */
        test_check(axl_str_to_double(big, &d, NULL) == AXL_OK && d == 1.0,
                   "str_to_double exact: 768-digit input rounds to 1.0");
    }

    /* Range errors, which only tier 2 reaches. */
    test_check(axl_str_to_double("1e400", &d, NULL) == AXL_ERR
               && axl_isinf(d) && d > 0,
               "str_to_double exact: overflow -> +inf + AXL_ERR");
    test_check(axl_str_to_double("1e-400", &d, NULL) == AXL_ERR && d == 0.0,
               "str_to_double exact: underflow -> +0.0 + AXL_ERR");
}

static void
test_double_to_str(void)
{
    char b[AXL_DOUBLE_STR_MAX];

    #define D2S(v, want, msg)                                          \
        do {                                                           \
            axl_memset(b, 0, sizeof(b));                                \
            axl_double_to_str((v), b, sizeof(b));                      \
            test_check(axl_strcmp(b, (want)) == 0, (msg));             \
        } while (0)

    D2S(0.0,    "0",     "double_to_str: zero");
    D2S(1.0,    "1",     "double_to_str: one, no trailing .0");
    D2S(-1.0,   "-1",    "double_to_str: negative");
    D2S(1.5,    "1.5",   "double_to_str: fraction");
    D2S(100.0,  "100",   "double_to_str: shortest form, not 100.000");
    D2S(0.001,  "0.001", "double_to_str: small fixed");
    D2S(1e-5,   "1e-05", "double_to_str: exponent < -4 switches to exponential");
    D2S(1e17,   "1e+17", "double_to_str: exponent >= 17 switches to exponential");

    axl_memset(b, 0, sizeof(b));
    axl_double_to_str(AXL_MATH_NAN, b, sizeof(b));
    test_check(axl_strcmp(b, "nan") == 0, "double_to_str: nan");
    axl_memset(b, 0, sizeof(b));
    axl_double_to_str(-AXL_MATH_INF, b, sizeof(b));
    test_check(axl_strcmp(b, "-inf") == 0, "double_to_str: -inf");

    test_check(axl_double_to_str(1.0, NULL, 32) == 0,
               "double_to_str: NULL buffer returns 0");
    test_check(axl_double_to_str(1.0, b, 0) == 0,
               "double_to_str: zero size returns 0");

    /* AXL_DOUBLE_STR_MAX is fixed in the ABI, so prove the worst case
       fits. `n` is a would-be length, so the `n < AXL_DOUBLE_STR_MAX`
       term is load-bearing: it is both the property under test and the
       bounds check that makes indexing tight[n] safe. */
    {
        char tight[AXL_DOUBLE_STR_MAX];
        size_t n = axl_double_to_str(-1.2345678901234567e-308,
                                     tight, sizeof(tight));
        test_check(n > 0 && n < AXL_DOUBLE_STR_MAX
                   && tight[n] == '\0',
                   "double_to_str: worst case fits AXL_DOUBLE_STR_MAX");
    }

    /* axl_snprintf's truncation convention: what fits is written, the
       buffer is always NUL-terminated, and the return is the length the
       WHOLE text would have had -- `ret >= bufsz` is the truncation
       test. This is the motivating case: "1e-300" cut to "1e-3"
       reparses cleanly as 0.001, 297 decades off, with no other signal. */
    {
        char   t[8];
        size_t n;
        double back;

        axl_memset(t, 'Q', sizeof(t));
        n = axl_double_to_str(1e-300, t, 5);
        test_check(n == 6, "double_to_str: 1e-300 in 5 bytes reports the full 6");
        test_check(axl_strcmp(t, "1e-3") == 0,
                   "double_to_str: truncated to '1e-3', NUL-terminated");
        test_check(t[5] == 'Q',
                   "double_to_str: truncated writes exactly bufsz bytes");
        test_check(axl_str_to_double(t, &back, NULL) == AXL_OK && back == 0.001,
                   "double_to_str: the truncated text reparses as a plausible wrong value");

        /* Same buffer, same text, different lengths -- the two cases the
           old bytes-written return could not tell apart. */
        axl_memset(t, 'Q', sizeof(t));
        n = axl_double_to_str(1234567.0, t, 8);
        test_check(n == 7 && axl_strcmp(t, "1234567") == 0,
                   "double_to_str: 1234567 fits 8 bytes exactly, ret 7 < bufsz 8");
        axl_memset(t, 'Q', sizeof(t));
        n = axl_double_to_str(12345678901.0, t, 8);
        test_check(n == 11 && axl_strcmp(t, "1234567") == 0,
                   "double_to_str: 12345678901 gives the SAME text, ret 11 >= bufsz 8");

        axl_memset(t, 'Q', sizeof(t));
        n = axl_double_to_str(1e-300, t, 1);
        test_check(n == 6 && axl_strcmp(t, "") == 0,
                   "double_to_str: bufsz 1 holds only the NUL and still reports 6");
        test_check(t[1] == 'Q', "double_to_str: bufsz 1 writes exactly one byte");

        axl_memset(t, 'Q', sizeof(t));
        test_check(axl_double_to_str(1e-300, t, 0) == 0 && t[0] == 'Q',
                   "double_to_str: bufsz 0 returns 0 and writes nothing");
    }
    #undef D2S
}

static void
test_double_round_trip(void)
{
    /* The headline property: parse(print(x)) == x, bit-exact. */
    static const double kCases[] = {
        0.0, 1.0, -1.0, 0.5, 1.5, 0.1, 0.3, 100.0, 1e10, 1e-10,
        1e23, 9007199254740992.0, 2.2250738585072011e-308,
        5e-324, 1.7976931348623157e308, -0.0,
        3.141592653589793, 2.718281828459045,
        /* Deep in digit_gen's fractional loop: kPow10 indices 9 and 16.
           Round-trippability is the property Grisu2 actually guarantees,
           and it held at every depth both before and after the kPow10
           widening — which is why these two cannot substitute for the
           exact-digit pins in test_dtoa. */
        2.7797020033791574e+307, 4.7112871036659575e+180
    };
    char   b[AXL_DOUBLE_STR_MAX];
    double back;
    size_t i;
    bool   all = true;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        axl_memset(b, 0, sizeof(b));
        axl_double_to_str(kCases[i], b, sizeof(b));
        if (axl_str_to_double(b, &back, NULL) != AXL_OK || back != kCases[i]) {
            all = false;
            break;
        }
    }
    test_check(all, "round-trip: every case parses back bit-identically");

    /* -0.0 keeps its sign through the trip: 1/-0.0 is -inf, 1/+0.0 is +inf. */
    axl_memset(b, 0, sizeof(b));
    axl_double_to_str(-0.0, b, sizeof(b));
    test_check(axl_str_to_double(b, &back, NULL) == AXL_OK
               && back == 0.0 && (1.0 / back) < 0.0,
               "round-trip: negative zero keeps its sign");
}

static void
test_double_round_trip_generated(void)
{
    /* Deterministic pseudo-random doubles (xorshift64, fixed seed) across
       the full exponent range -- cheap, reproducible coverage beyond the
       fixed case list above. Bounded to a few hundred values so this
       doesn't noticeably slow the QEMU suite, which runs it on every
       build across both arches. Bit-exact via memcmp (not ==) so a
       sign-of-zero regression can't hide behind IEEE -0.0 == 0.0. */
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    char     b[AXL_DOUBLE_STR_MAX];
    double   back;
    bool     all = true;
    int      i;

    for (i = 0; i < 300; i++) {
        uint64_t bits;
        double   value;

        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        bits = state;

        /* Force a finite exponent -- exclude 0x7FF (inf/nan), which
           axl_dtoa deliberately does not handle. */
        if (((bits >> 52) & 0x7FFULL) == 0x7FFULL) {
            bits &= ~(0x7FFULL << 52);
        }
        axl_memcpy(&value, &bits, sizeof(value));

        axl_memset(b, 0, sizeof(b));
        axl_double_to_str(value, b, sizeof(b));
        if (axl_str_to_double(b, &back, NULL) != AXL_OK
            || axl_memcmp(&back, &value, sizeof(value)) != 0)
        {
            all = false;
            break;
        }
    }
    test_check(all, "round-trip: 300 generated doubles parse back bit-identically");
}

static void
test_float_to_str(void)
{
    char b[AXL_DOUBLE_STR_MAX];

    #define F2S(v, want, msg)                                          \
        do {                                                           \
            axl_memset(b, 0, sizeof(b));                                \
            axl_float_to_str((v), b, sizeof(b));                       \
            test_check(axl_strcmp(b, (want)) == 0, (msg));             \
        } while (0)

    F2S(0.0f,       "0",           "float_to_str: zero");
    F2S(-0.0f,      "-0",          "float_to_str: negative zero keeps its sign");
    F2S(1.0f,       "1",           "float_to_str: one, no trailing .0");
    F2S(-1.0f,      "-1",          "float_to_str: negative");
    F2S(0.5f,       "0.5",         "float_to_str: fraction");
    F2S(1.5f,       "1.5",         "float_to_str: fraction, 2 sig figs");
    F2S(100.0f,     "100",         "float_to_str: shortest form, not 100.000");
    F2S(0.001f,     "0.001",       "float_to_str: small fixed");
    F2S(0.1f,       "0.1",         "float_to_str: shortest FLOAT form (1 sig fig)");
    F2S(3.14f,      "3.14",        "float_to_str: 3 sig figs suffice for float");
    F2S(123456.0f,  "123456",      "float_to_str: exact integer");
    F2S(9999999.0f, "9999999",     "float_to_str: 7 sig figs, still fixed (exp 6 < 17)");
    F2S(1e10f,      "10000000000", "float_to_str: exponent 10 stays fixed");
    F2S(1e-10f,     "1e-10",       "float_to_str: exponent < -4 switches to exponential");
    F2S(1e30f,      "1e+30",       "float_to_str: exponent >= 17 switches to exponential");
    F2S(FLT_MAX,    "3.4028235e+38", "float_to_str: FLT_MAX, 8 sig figs");
    F2S(FLT_MIN,    "1.1754944e-38", "float_to_str: smallest normal, 8 sig figs");

    axl_memset(b, 0, sizeof(b));
    axl_float_to_str((float)AXL_MATH_NAN, b, sizeof(b));
    test_check(axl_strcmp(b, "nan") == 0, "float_to_str: nan");
    axl_memset(b, 0, sizeof(b));
    axl_float_to_str((float)(-AXL_MATH_INF), b, sizeof(b));
    test_check(axl_strcmp(b, "-inf") == 0, "float_to_str: -inf");

    test_check(axl_float_to_str(1.0f, NULL, 32) == 0,
               "float_to_str: NULL buffer returns 0");
    test_check(axl_float_to_str(1.0f, b, 0) == 0,
               "float_to_str: zero size returns 0");

    /* Smallest subnormal float: bit pattern 0x00000001. */
    {
        union { uint32_t bits; float f; } uf;
        uf.bits = 1u;
        F2S(uf.f, "1e-45", "float_to_str: smallest subnormal float");
    }

    /* The headline reason this function exists: shorter than promoting
       to double and using axl_double_to_str, which would need 17 sig
       figs for this same (ugly, once widened) value. */
    {
        char   fbuf[AXL_DOUBLE_STR_MAX];
        char   dbuf[AXL_DOUBLE_STR_MAX];
        size_t flen, dlen;

        axl_memset(fbuf, 0, sizeof(fbuf));
        axl_memset(dbuf, 0, sizeof(dbuf));
        flen = axl_float_to_str(0.1f, fbuf, sizeof(fbuf));
        dlen = axl_double_to_str((double)0.1f, dbuf, sizeof(dbuf));

        test_check(axl_strcmp(fbuf, "0.1") == 0 && flen == 3,
                   "float_to_str: 0.1f is \"0.1\", not the double-promoted expansion");
        test_check(dlen > flen,
                   "float_to_str: shorter than promoting to double for the same value");
    }

    /* Same axl_snprintf truncation convention as the double form. */
    {
        char   t[8];
        size_t n;

        /* FLT_MAX renders "3.4028235e+38" -- 13 bytes. */
        axl_memset(t, 'Q', sizeof(t));
        n = axl_float_to_str(FLT_MAX, t, 5);
        test_check(n == 13 && axl_strcmp(t, "3.40") == 0,
                   "float_to_str: truncated to '3.40' but reports the full 13");
        test_check(t[5] == 'Q',
                   "float_to_str: truncated writes exactly bufsz bytes");

        axl_memset(t, 'Q', sizeof(t));
        n = axl_float_to_str(1234.5f, t, 7);
        test_check(n == 6 && axl_strcmp(t, "1234.5") == 0,
                   "float_to_str: an exactly-sized buffer renders in full, ret 6 < bufsz 7");

        axl_memset(t, 'Q', sizeof(t));
        n = axl_float_to_str(1234.5f, t, 6);
        test_check(n == 6 && axl_strcmp(t, "1234.") == 0,
                   "float_to_str: one byte short truncates, ret 6 >= bufsz 6");

        axl_memset(t, 'Q', sizeof(t));
        n = axl_float_to_str(1234.5f, t, 1);
        test_check(n == 6 && axl_strcmp(t, "") == 0,
                   "float_to_str: bufsz 1 holds only the NUL and still reports 6");
        test_check(t[1] == 'Q', "float_to_str: bufsz 1 writes exactly one byte");

        axl_memset(t, 'Q', sizeof(t));
        test_check(axl_float_to_str(1234.5f, t, 0) == 0 && t[0] == 'Q',
                   "float_to_str: bufsz 0 returns 0 and writes nothing");

        /* The non-finite path is a separate branch inside the function,
           so it needs its own truncation case: "-inf" is 4 bytes. */
        axl_memset(t, 'Q', sizeof(t));
        n = axl_float_to_str((float)(-AXL_MATH_INF), t, 3);
        test_check(n == 4 && axl_strcmp(t, "-i") == 0,
                   "float_to_str: the nan/inf path truncates by the same rule");
    }

    #undef F2S
}

static void
test_str_to_float(void)
{
    float       f;
    const char *end;

    test_check(axl_str_to_float("1.5", &f, NULL) == AXL_OK && f == 1.5f,
               "str_to_float: exact binary fraction");
    test_check(axl_str_to_float("-2.25", &f, NULL) == AXL_OK && f == -2.25f,
               "str_to_float: negative");
    test_check(axl_str_to_float("0.1", &f, NULL) == AXL_OK && f == 0.1f,
               "str_to_float: 0.1 narrows to the nearest float");
    test_check(axl_str_to_float("3.4028235e38", &f, NULL) == AXL_OK
               && f == 3.4028235e38f,
               "str_to_float: FLT_MAX");
    test_check(axl_str_to_float("1.1754944e-38", &f, NULL) == AXL_OK
               && f == 1.1754944e-38f,
               "str_to_float: FLT_MIN");

    /* Doubles that are FINE as doubles but out of float range. */
    test_check(axl_str_to_float("1e39", &f, NULL) == AXL_ERR
               && axl_isinf((double)f) && f > 0,
               "str_to_float: over float range is +inf AND AXL_ERR");
    test_check(axl_str_to_float("-1e39", &f, NULL) == AXL_ERR
               && axl_isinf((double)f) && f < 0,
               "str_to_float: under -float range is -inf AND AXL_ERR");
    test_check(axl_str_to_float("1e-60", &f, NULL) == AXL_ERR && f == 0.0f,
               "str_to_float: below float range is +0.0 AND AXL_ERR");

    /* Same syntax and endptr contract as the double form. */
    f = 9.0f;
    const char *src = "abc";
    test_check(axl_str_to_float(src, &f, &end) == AXL_ERR
               && f == 9.0f && end == src,
               "str_to_float: syntax error leaves out untouched, endptr = nptr");
    test_check(axl_str_to_float("2.5xyz", &f, &end) == AXL_OK
               && f == 2.5f && axl_strcmp(end, "xyz") == 0,
               "str_to_float: endptr stops at the first unconsumed byte");

    /* STRICT MODE, same rule as axl_str_to_double and the integer
       family: no endptr means the whole input must be consumed, and the
       failure leaves *out untouched. */
    f = 9.0f;
    test_check(axl_str_to_float("2.5xyz", &f, NULL) == AXL_ERR && f == 9.0f,
               "str_to_float: strict mode rejects a trailing suffix");
    f = 9.0f;
    test_check(axl_str_to_float("36.6C", &f, NULL) == AXL_ERR && f == 9.0f,
               "str_to_float: strict mode rejects a trailing unit suffix");
    f = 9.0f;
    test_check(axl_str_to_float(" 5 ", &f, NULL) == AXL_ERR && f == 9.0f,
               "str_to_float: strict mode rejects trailing space");
    f = 9.0f;
    test_check(axl_str_to_float("1e39zz", &f, NULL) == AXL_ERR && f == 9.0f,
               "str_to_float: strict mode outranks the range-error write");

    test_check(axl_str_to_float(NULL, &f, NULL) == AXL_ERR,
               "str_to_float: NULL input is rejected");
    test_check(axl_str_to_float("1.0", NULL, NULL) == AXL_ERR,
               "str_to_float: NULL out is rejected");
    test_check(axl_str_to_float("nan", &f, NULL) == AXL_OK
               && axl_isnan((double)f),
               "str_to_float: nan");

    /* The pair round-trips: print then parse reproduces the same float. */
    {
        static const float kF[] = {
            0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 2.5f, 1e10f, 1e-10f,
            3.4028235e38f, 1.1754944e-38f, 1.4e-45f
        };
        char  b[AXL_DOUBLE_STR_MAX];
        float back;
        bool  all = true;
        for (size_t i = 0; i < sizeof(kF) / sizeof(kF[0]); i++) {
            axl_memset(b, 0, sizeof(b));
            axl_float_to_str(kF[i], b, sizeof(b));
            if (axl_str_to_float(b, &back, NULL) != AXL_OK
                || axl_memcmp(&back, &kF[i], sizeof(float)) != 0) {
                all = false;
                break;
            }
        }
        test_check(all, "float round-trip: str_to_float inverts float_to_str");
    }
}

static void
test_float_round_trip_generated(void)
{
    /* Deterministic pseudo-random floats (xorshift32, fixed seed). Same
       rationale as test_double_round_trip_generated; kept as a separate
       function/loop since the bit width, exponent field, and the
       round-trip path (through axl_str_to_float) all differ from the
       double case. Parsing with axl_str_to_float rather than
       axl_str_to_double + a (float) cast also makes a spurious AXL_ERR
       a failure, which the cast form structurally cannot see. */
    uint32_t state = 0x9E3779B9u;
    char     b[AXL_DOUBLE_STR_MAX];
    float    back;
    bool     all = true;
    int      i;

    for (i = 0; i < 300; i++) {
        uint32_t bits;
        float    value;

        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bits = state;

        /* Force a finite exponent -- exclude 0xFF (inf/nan). */
        if (((bits >> 23) & 0xFFu) == 0xFFu) {
            bits &= ~(0xFFu << 23);
        }
        axl_memcpy(&value, &bits, sizeof(value));

        axl_memset(b, 0, sizeof(b));
        axl_float_to_str(value, b, sizeof(b));
        if (axl_str_to_float(b, &back, NULL) != AXL_OK
            || axl_memcmp(&back, &value, sizeof(value)) != 0) {
            all = false;
            break;
        }
    }
    test_check(all, "round-trip: 300 generated floats parse back bit-identically");
}

// ---------------------------------------------------------------------------

int
test_strbuf_main(
    int    argc,
    char **argv
    )
{
    (void)argc; (void)argv;
    test_print_header("AxlString");

    test_strbuf_basic();
    test_strbuf_append_n();
    test_strbuf_putc();
    test_strbuf_printf();
    test_strbuf_steal();
    test_strbuf_steal_then_append();
    test_strbuf_self_reference();
    test_strbuf_post_steal_paths();
    test_strbuf_self_reference_all_paths();
    test_strbuf_overwrite_bounds();
    test_strbuf_size_overflow_guards();
    test_strbuf_capacity_boundary();
    test_strbuf_data();
    test_strbuf_capacity();
    test_strbuf_resize();
    test_strbuf_insert_len();
    test_strbuf_clear();
    test_strbuf_grow();
    test_strbuf_prepend();
    test_strbuf_insert();
    test_strbuf_erase();
    test_strbuf_truncate();
    test_strbuf_overwrite();
    test_asprintf();
    test_utf8_ucs2();
    test_utf8_decode();
    test_utf8_encode();
    test_base64();
    test_base64url();
    test_strlcpy();
    test_strlcat();
    test_strlen();
    test_strcmp();
    test_strncmp();
    test_memcpy();
    test_memset();
    test_memchr();
    test_snprintf();
    test_vsnprintf();
    test_snprintf_float();
    test_snprintf_exp_g();
    test_dtoa();
    test_format_bytes();
    test_strtou64();
    test_strtou64_with_offset();
    test_str_reader();
    test_sscanf();
    test_sscanf_float();
    test_sscanf_width_overflow();
    test_str_to_u64();
    test_str_to_u32();
    test_str_to_s64();
    test_str_to_s32();
    test_str_to_narrow();
    test_str_to_edge_cases();
    test_int_to_str();
    test_strcasestr();
    test_strrcasestr();
    test_fnmatch();
    test_wcs();
    test_format();
    test_utf16();
    test_math_special_values();
    test_str_to_double_basic();
    test_str_to_double_exact();
    test_double_to_str();
    test_double_round_trip();
    test_double_round_trip_generated();
    test_float_to_str();
    test_str_to_float();
    test_float_round_trip_generated();

    return test_print_results();
}

AXL_APP(test_strbuf_main)
