/** @file axl-test.h
    Shared test helpers for AXL unit tests.
    Header-only — include in each test .c file.
**/

#ifndef AXL_TEST_H
#define AXL_TEST_H

#include <axl.h>
#include <stdbool.h>

static unsigned int test_pass_count = 0;
static unsigned int test_fail_count = 0;
/* Two counters, because a skip has two sizes and conflating them was confusing
 * in the footer: how many GROUPS declined to run, and how many assertions that
 * accounts for. Only test_skip_n() knows the second number; a build-config
 * test_skip() has a separate ratchet baseline and so does not need it. */
static unsigned int test_skip_groups  = 0;
static unsigned int test_skip_asserts = 0;

static inline void
test_pass(const char *name)
{
    axl_printf("PASS: %s\n", name);
    test_pass_count++;
}

static inline void
test_fail(const char *name)
{
    axl_printf("FAIL: %s\n", name);
    test_fail_count++;
}

/**
 * Record that execution REACHED here without faulting.
 *
 * For a call whose whole contract is "does not crash" -- axl_free(NULL),
 * close(NULL), a print routine with no return value. There is nothing to
 * evaluate: arriving at the next line IS the result, and in a freestanding
 * UEFI build the alternative outcome is a #GP that takes the binary with it,
 * not a false.
 *
 * Exists so those stop being spelled `test_check(true, "free(NULL): no
 * crash")`. That form is indistinguishable, to a reader and to a grep, from
 * the genuine `test_check(true, ...)` anti-pattern the project bans -- an
 * assertion that claims a result while evaluating nothing. Same PASS line and
 * same count either way; the difference is that this one is honest about what
 * it checked, so the ban can be enforced without exceptions.
 */
static inline void
test_survived(const char *name)
{
    test_pass(name);
}

/**
 * Record that a group of assertions could NOT run in this build.
 *
 * Exists because a `#ifdef`-gated block that quietly compiles to nothing is
 * indistinguishable, in the output, from one that ran and passed. The default
 * build has no TLS, so AxlTestJose and AxlTestCrypto drop ~190 assertions --
 * and a run reporting "13 passed, 0 failed" looks exactly like success. It cost
 * a real mistake: new JOSE assertions were declared green having never
 * executed.
 *
 * A skip is neither a pass nor a fail. The harness counts `^PASS:` and `^FAIL:`
 * lines, so `SKIP:` stays out of both totals while still being greppable, and
 * test_print_results reports the count so it cannot be read past.
 *
 * For a BUILD-CONFIGURATION gate (AXL_TLS and friends), where the harness keeps
 * a separate ratchet baseline per configuration so a lower count is expected.
 * For a TOPOLOGY gate -- a device the QEMU image may or may not expose, real
 * hardware -- both outcomes share ONE baseline, so use test_skip_n() and tell
 * it how many assertions did not run.
 *
 * @param name what was skipped, and the flag that would enable it -- the
 *     harness matches "AXL_TLS" here for TEST_REQUIRE_TLS=1.
 */
static inline void
test_skip(const char *name)
{
    axl_printf("SKIP: %s\n", name);
    test_skip_groups++;
}

/**
 * Record that exactly @a n assertions did not run, and why.
 *
 * The topology-gate form: a device absent from this QEMU image or this machine,
 * where the SAME baseline covers both outcomes, so the count has to be made up
 * somewhere or the ratchet drifts between images.
 *
 * It used to be made up with padding -- a run of `test_check(true, "... SKIP
 * balance")` calls, one per assertion the populated path would have run. There
 * were about 170 of those. They are indistinguishable from the genuine
 * assert-nothing anti-pattern the project bans, they inflate the pass count
 * with results nobody produced, and keeping the run length in sync with the
 * populated branch by hand is a standing invitation to drift.
 *
 * So the count is declared instead of faked: this prints `SKIP[n]:` and the
 * harness ratchets on passes PLUS declared skips, which is exactly the total
 * the populated path produces. Same drift protection, no invented passes.
 *
 * @param n     assertions the populated path would have run -- keep this equal
 *              to that branch's count, which is the one thing to check when
 *              adding an assertion to it.
 * @param name  what was skipped and why the path was not taken.
 */
static inline void
test_skip_n(unsigned int n, const char *name)
{
    axl_printf("SKIP[%u]: %s\n", n, name);
    test_skip_groups++;
    test_skip_asserts += n;
}

static inline void
test_check(bool cond, const char *name)
{
    if (cond) {
        test_pass(name);
    } else {
        test_fail(name);
    }
}

static inline void
test_print_header(const char *suite_name)
{
    axl_printf("\n=== %s Tests ===\n\n", suite_name);
}

static inline int
test_print_results(void)
{
    /* The skipped count is only printed when non-zero, so the footer every
       other binary emits is byte-identical to before -- the harness greps for
       "=== Results:" as a completion marker and the ratchet counts PASS lines,
       neither of which this disturbs. */
    if (test_skip_groups > 0) {
        axl_printf("\n=== Results: %u passed, %u failed, "
                   "%u group(s) SKIPPED (%u assertions) ===\n\n",
                   test_pass_count, test_fail_count,
                   test_skip_groups, test_skip_asserts);
    } else {
        axl_printf("\n=== Results: %u passed, %u failed ===\n\n",
                   test_pass_count, test_fail_count);
    }
    return (test_fail_count > 0) ? 1 : 0;
}

#endif /* AXL_TEST_H */
