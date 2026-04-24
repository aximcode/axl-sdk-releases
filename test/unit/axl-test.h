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
    axl_printf("\n=== Results: %u passed, %u failed ===\n\n",
               test_pass_count, test_fail_count);
    return (test_fail_count > 0) ? 1 : 0;
}

#endif /* AXL_TEST_H */
