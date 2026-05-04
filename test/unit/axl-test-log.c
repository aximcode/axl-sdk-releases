/** @file axl-test-log.c
    Test application for AxlLog -- exercises levels, domains, handlers,
    ring buffer, and file handler.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("test");

static inline const char *
test_strstr(const char *h, const char *n)
{
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) {
            return h;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Custom Handler State (for handler tests)
// ---------------------------------------------------------------------------

static size_t  custom_handler_calls  = 0;
static size_t  custom_handler_level  = 0;
static char    custom_handler_msg[256];

/* New-style handler signature (no EFIAPI, standard C types) */
static void
custom_handler(int level, const char *domain, const char *message, void *data)
{
    custom_handler_calls++;
    custom_handler_level = (size_t)level;
    if (message != NULL) {
        axl_snprintf(custom_handler_msg, sizeof(custom_handler_msg), "%s", message);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/**
  Test 1: Default level is INFO -- DEBUG should be suppressed.
**/
static void
test_default_level(void)
{
    custom_handler_calls = 0;
    axl_log_add_handler(custom_handler, NULL);

    // INFO should pass (level 2 <= default INFO level 2)
    axl_info("default level info test");
    test_check(custom_handler_calls == 1, "default level: INFO passes");

    // DEBUG should be suppressed (level 3 > INFO level 2)
    axl_debug("default level debug test");
    test_check(custom_handler_calls == 1, "default level: DEBUG suppressed");

    axl_log_remove_handler(custom_handler);
}

/**
  Test 2: SetLevel to DEBUG -- DEBUG should now pass.
**/
static void
test_set_level(void)
{
    custom_handler_calls = 0;
    axl_log_add_handler(custom_handler, NULL);
    axl_log_set_level(AXL_LOG_DEBUG);

    axl_debug("set level debug test");
    test_check(custom_handler_calls == 1, "set level: DEBUG passes at DEBUG level");

    // TRACE should still be suppressed
    axl_trace("set level trace test");
    test_check(custom_handler_calls == 1, "set level: TRACE suppressed at DEBUG level");

    // Restore default
    axl_log_set_level(AXL_LOG_INFO);
    axl_log_remove_handler(custom_handler);
}

/**
  Test 3: Domain override -- set "test" domain to ERROR only.
**/
static void
test_domain_override(void)
{
    custom_handler_calls = 0;
    axl_log_add_handler(custom_handler, NULL);
    axl_log_set_domain_level("test", AXL_LOG_ERROR);

    // INFO should be suppressed for "test" domain
    axl_info("domain override info test");
    test_check(custom_handler_calls == 0, "domain override: INFO suppressed for 'test'");

    // ERROR should pass for "test" domain
    axl_error("domain override error test");
    test_check(custom_handler_calls == 1, "domain override: ERROR passes for 'test'");

    // Clear override
    axl_log_set_domain_level("test", -1);

    // INFO should pass again
    axl_info("domain cleared info test");
    test_check(custom_handler_calls == 2, "domain override: INFO passes after clear");

    axl_log_remove_handler(custom_handler);
}

/**
  Test 4: Custom handler receives correct data.
**/
static void
test_custom_handler(void)
{
    custom_handler_calls = 0;
    custom_handler_level = 99;
    custom_handler_msg[0] = '\0';

    axl_log_add_handler(custom_handler, NULL);
    axl_log(AXL_LOG_WARNING, "test", "hello %d", 42);

    test_check(custom_handler_calls == 1, "custom handler: called once");
    test_check(custom_handler_level == AXL_LOG_WARNING, "custom handler: correct level");
    test_check(test_strstr(custom_handler_msg, "hello 42") != NULL,
               "custom handler: message formatted");

    axl_log_remove_handler(custom_handler);
}

/**
  Test 5: SuppressConsole stops console output but handlers still fire.
**/
static void
test_suppress_console(void)
{
    custom_handler_calls = 0;
    axl_log_add_handler(custom_handler, NULL);
    axl_log_suppress_console();

    axl_info("suppressed console test");
    test_check(custom_handler_calls == 1, "suppress console: handler still fires");

    axl_log_remove_handler(custom_handler);
}

/**
  Test 6: Ring buffer stores entries in newest-first order.
**/
static void
test_ring_buffer(void)
{
    AxlLogRing  *ring;
    AxlLogEntry  entry;
    bool         got;

    ring = axl_log_ring_new(16, 128);
    test_check(ring != NULL, "ring: allocated");
    if (ring == NULL) {
        return;
    }

    axl_log_ring_attach(ring);

    axl_log(AXL_LOG_INFO, "ring", "msg-A");
    axl_log(AXL_LOG_INFO, "ring", "msg-B");
    axl_log(AXL_LOG_INFO, "ring", "msg-C");

    test_check(axl_log_ring_count(ring) == 3, "ring: count is 3");

    // Index 0 = newest = msg-C
    got = axl_log_ring_get(ring, 0, &entry);
    test_check(got && test_strstr(entry.message, "msg-C") != NULL,
               "ring: index 0 is newest(msg-C)");

    // Index 2 = oldest = msg-A
    got = axl_log_ring_get(ring, 2, &entry);
    test_check(got && test_strstr(entry.message, "msg-A") != NULL,
               "ring: index 2 is oldest(msg-A)");

    // Index 3 = out of range
    got = axl_log_ring_get(ring, 3, &entry);
    test_check(!got, "ring: index 3 out of range");

    axl_log_ring_free(ring);
}

/**
  Test 7: Ring buffer overflow evicts oldest entries.
**/
static void
test_ring_overflow(void)
{
    AxlLogRing  *ring;
    AxlLogEntry  entry;
    char         buf[32];
    bool         got;

    ring = axl_log_ring_new(4, 64);
    test_check(ring != NULL, "ring overflow: allocated");
    if (ring == NULL) {
        return;
    }

    axl_log_ring_attach(ring);

    // Write 6 entries into a ring of size 4
    for (size_t i = 0; i < 6; i++) {
        axl_snprintf(buf, sizeof(buf), "overflow-%zu", i);
        axl_log(AXL_LOG_INFO, "ring", "%s", buf);
    }

    // Count should be capped at 4
    test_check(axl_log_ring_count(ring) == 4, "ring overflow: count capped at 4");

    // Newest should be overflow-5
    got = axl_log_ring_get(ring, 0, &entry);
    test_check(got && test_strstr(entry.message, "overflow-5") != NULL,
               "ring overflow: newest is overflow-5");

    // Oldest should be overflow-2 (0 and 1 evicted)
    got = axl_log_ring_get(ring, 3, &entry);
    test_check(got && test_strstr(entry.message, "overflow-2") != NULL,
               "ring overflow: oldest is overflow-2");

    axl_log_ring_free(ring);
}

/**
  Test 8: File handler writes to a file.
  Uses axl_file_get_contents for backend-agnostic verification.
**/
static void
test_file_handler(void)
{
    int     rc;
    void   *buf;
    size_t  len;

    // Write some log messages to a file
    rc = axl_log_file_attach("axl-test-log.log");
    test_check(rc == 0, "file handler: attach succeeded");
    if (rc != 0) {
        return;
    }

    axl_log(AXL_LOG_INFO, "file", "file-handler-test-marker");
    axl_log_flush();

    // Read back and verify the marker is present
    rc = axl_file_get_contents("axl-test-log.log", &buf, &len);
    if (rc != AXL_OK) {
        test_fail("file handler: cannot reopen log file");
        return;
    }

    test_check(len > 0 && test_strstr((const char *)buf, "file-handler-test-marker") != NULL,
               "file handler: marker found in log file");
    axl_free(buf);
}

// ---------------------------------------------------------------------------
// Handler-table overflow — axl_log_add_handler must return -1 when full
// ---------------------------------------------------------------------------

/* Distinct callback per slot so axl_log_remove_handler can pop them
 * one at a time rather than nuking everything at once. */
static void noop_h0(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h1(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h2(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h3(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h4(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h5(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h6(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h7(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }
static void noop_h8(int l, const char *d, const char *m, void *x) { (void)l;(void)d;(void)m;(void)x; }

static void
test_add_handler_overflow(void)
{
    AxlLogHandler slots[] = {
        noop_h0, noop_h1, noop_h2, noop_h3,
        noop_h4, noop_h5, noop_h6, noop_h7,
    };
    /* Fill the table up to MAX_HANDLERS (currently 8). The exact cap
     * is intentionally not asserted — we just have to keep this array
     * at >= MAX_HANDLERS slots if the cap ever rises. */
    int added = 0;
    for (size_t i = 0; i < sizeof(slots)/sizeof(slots[0]); i++) {
        if (axl_log_add_handler(slots[i], NULL) == AXL_OK) {
            added++;
        }
    }
    test_check(added > 0, "add_handler overflow: filled at least one slot");

    /* The next add must be rejected. */
    test_check(axl_log_add_handler(noop_h8, NULL) == AXL_ERR,
               "add_handler overflow: returns -1 when table full");

    /* Same contract for the domain-filtered variant. */
    test_check(axl_log_add_domain_handler("x", AXL_LOG_INFO, noop_h8, NULL) == AXL_ERR,
               "add_domain_handler overflow: returns -1 when table full");

    /* NULL handler always rejected, regardless of fullness. */
    test_check(axl_log_add_handler(NULL, NULL) == AXL_ERR,
               "add_handler: NULL handler rejected");

    /* Clean up so subsequent test runs (if any) start fresh. */
    for (int i = 0; i < added; i++) {
        axl_log_remove_handler(slots[i]);
    }
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int
test_log_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlLog");

    test_default_level();
    test_set_level();
    test_domain_override();
    test_custom_handler();
    test_suppress_console();
    test_ring_buffer();
    test_ring_overflow();
    test_file_handler();
    test_add_handler_overflow();

    return test_print_results();
}

AXL_APP(test_log_main)
