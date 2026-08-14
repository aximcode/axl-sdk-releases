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

/* --------------------------------------------------------------------------
   Green-path log-quiet detector.

   A consumer running at AXL_LOG_INFO must see nothing from AXL on a healthy
   run. The library reports a condition to its caller through the return
   value, and only the caller has the context to judge whether a failed open
   is a fault or an expected probe -- so anything AXL emits above debug for a
   condition the caller can already test is a duplicate, and anything it emits
   at all on a success path is noise.

   The dispatcher applies the level filter BEFORE handler dispatch
   (axl_log_full returns early when level > the effective level), and the
   handler carries its own INFO cap on top, so the window fires for exactly
   the lines a consumer at INFO would have seen on its console.

   Usage -- the argument names the domain whose silence is being asserted:

       test_log_quiet_begin("fs");
       (void)axl_file_get_contents("fs0:\\nope.bin", &buf, &len);
       test_log_quiet_end("fs: failed open is quiet at INFO");

   Two things this deliberately does NOT do.

   It never touches the global level: the cap lives on the handler, so a run
   started at debug still measures ERROR/WARN/INFO and nothing downstream of
   the window is left reconfigured.

   It never trusts its own silence unverified. The handler table holds 8, and
   a level can be pinned per domain (AXL_LOG_LEVEL="fs:error"); either would
   make an unarmed window report a confident PASS over the exact regression it
   guards. So begin() emits one probe line in @a probe_domain and requires to
   see it, and a window that failed to arm FAILS -- a gate that cannot see is
   worse than no gate.
   -------------------------------------------------------------------------- */

static unsigned int test_log_quiet_hits         = 0;
static bool         test_log_quiet_armed        = false;
static int          test_log_quiet_first_level  = -1;
static char         test_log_quiet_first_domain[32];
static char         test_log_quiet_first_msg[160];

static inline void
test_log_quiet_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (src != NULL) {
        for (; src[i] != '\0' && i + 1 < cap; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/* Records only; the dispatcher is not re-entrant, so this must not allocate
   or call anything that can itself log. A hand-rolled copy keeps it to
   stores. */
static inline void
test_log_quiet_handler(
    int                level,
    const char        *domain,
    const char        *message,
    const AxlRealtime *stamp,
    void              *data
    ) AXL_CB_NOEXCEPT
{
    (void)stamp; (void)data;

    if (test_log_quiet_hits == 0) {
        test_log_quiet_first_level = level;
        test_log_quiet_copy(test_log_quiet_first_domain,
                            sizeof(test_log_quiet_first_domain), domain);
        test_log_quiet_copy(test_log_quiet_first_msg,
                            sizeof(test_log_quiet_first_msg), message);
    }
    test_log_quiet_hits++;
}

static inline void
test_log_quiet_reset(void)
{
    test_log_quiet_hits            = 0;
    test_log_quiet_first_level     = -1;
    test_log_quiet_first_domain[0] = '\0';
    test_log_quiet_first_msg[0]    = '\0';
}

static inline void
test_log_quiet_begin(const char *probe_domain)
{
    test_log_quiet_reset();
    test_log_quiet_armed = false;

    /* The INFO cap rides on the HANDLER, not on the global level: there is no
       axl_log_get_level to restore from, so a window that set the global one
       could only "restore" it by guessing, and would silently reconfigure
       everything downstream of it in a run started at debug. */
    if (axl_log_add_domain_handler(NULL, AXL_LOG_INFO,
                                   test_log_quiet_handler, NULL) != AXL_OK) {
        return;   /* table full -- end() reports it rather than passing */
    }

    /* Arm check: emit one line the window MUST see. Without this, a full
       handler table or a domain pinned below INFO (AXL_LOG_LEVEL="fs:error")
       reports a confident PASS over the very regression being guarded. */
    axl_log_full(AXL_LOG_INFO, probe_domain, NULL, 0,
                 "log-quiet detector probe (expected)");
    test_log_quiet_armed = (test_log_quiet_hits == 1);
    test_log_quiet_reset();
}

static inline void
test_log_quiet_end(const char *name)
{
    axl_log_remove_handler(test_log_quiet_handler);

    if (!test_log_quiet_armed) {
        axl_printf("      detector never saw its own probe -- handler table "
                   "full, or this domain is pinned below INFO\n");
        test_fail(name);
        return;
    }

    /* Name the offender. "expected 0, got 1" tells a future reader nothing
       about WHICH line regrew, and the console copy scrolls past in a run
       with 2400 assertions. */
    if (test_log_quiet_hits != 0) {
        axl_printf("      offender: level=%d domain=%s msg=%s\n",
                   test_log_quiet_first_level,
                   test_log_quiet_first_domain,
                   test_log_quiet_first_msg);
    }
    test_check(test_log_quiet_hits == 0, name);
}

/* Unwind a window WITHOUT asserting -- for a topology-gated caller that
   discovered mid-window it is on the path this release does not cover, and
   must balance with test_skip_n() instead. */
static inline void
test_log_quiet_abort(void)
{
    axl_log_remove_handler(test_log_quiet_handler);
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
