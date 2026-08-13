/** @file axl-test-log.c
    Test application for AxlLog -- exercises levels, domains, handlers,
    ring buffer, and file handler.
**/

#include "axl-test.h"
#include <axl/axl-env.h>
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
custom_handler(int level, const char *domain, const char *message,
               const AxlRealtime *stamp, void *data)
{
    (void)stamp;
    (void)domain;
    (void)data;
    custom_handler_calls++;
    custom_handler_level = (size_t)level;
    if (message != NULL) {
        axl_snprintf(custom_handler_msg, sizeof(custom_handler_msg), "%s", message);
    }
}

/* Two sinks that record the stamp they were handed, so a single record can be
   compared across them. */
static AxlRealtime  stamp_a, stamp_b;
static bool         stamp_a_seen, stamp_b_seen, stamp_a_null, stamp_b_null;

static bool stamp_second_rolled;

static void
stamp_handler_a(int level, const char *domain, const char *message,
                const AxlRealtime *stamp, void *data)
{
    (void)level; (void)domain; (void)message; (void)data;
    stamp_a_seen = true;
    if (stamp == NULL) { stamp_a_null = true; return; }
    stamp_a = *stamp;

    /* Cross a wallclock second boundary before returning, so the NEXT sink
       runs in a different second than this one.
       This is what makes the test discriminate at RUNTIME rather than only
       at compile time: under the old read-it-yourself behaviour handler B
       would take its own reading here and land on the later second, so the
       "same instant" assertion below would fail. Under stamp-once it cannot,
       because B is handed the value the dispatcher already captured.
       Waits on the MONOTONIC counter and reads the RTC exactly once at the
       end, rather than polling GetTime in a tight loop -- that hammered CMOS
       with thousands of reads from inside a log handler, which is a lot of
       firmware pressure to introduce into a shared test boot for a signal one
       read can give. Any window longer than a second must contain a
       boundary. */
    const uint64_t deadline = axl_time_get_us() + 1100000ull;
    while (axl_time_get_us() < deadline) {
        /* spin on the monotonic counter only */
    }
    AxlRealtime after;
    if (axl_time_realtime(&after) == AXL_OK && after.second != stamp->second) {
        stamp_second_rolled = true;
    }
}

static void
stamp_handler_b(int level, const char *domain, const char *message,
                const AxlRealtime *stamp, void *data)
{
    (void)level; (void)domain; (void)message; (void)data;
    stamp_b_seen = true;
    if (stamp == NULL) { stamp_b_null = true; return; }
    stamp_b = *stamp;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/* The dispatcher must read the clock ONCE per record and hand the same value
   to every sink.
   Before this, each sink stamped itself: N sinks meant N firmware GetTime
   calls per record, and -- the part that actually corrupts a transcript --
   two sinks could land on opposite sides of a second boundary and disagree
   about when one record happened. Correlating a serial transcript against a
   file log then silently misaligns.
   Comparing two handlers is what makes this discriminate: asserting a single
   handler merely "got a plausible stamp" would pass just as well against the
   old read-it-yourself behaviour. */
static void
test_dispatch_stamps_once(void)
{
    stamp_a_seen = stamp_b_seen = false;
    stamp_a_null = stamp_b_null = false;
    stamp_second_rolled = false;

    /* Checked: the handler table is global and capped (MAX_HANDLERS 8). If a
       previous test leaked a slot these adds fail silently, neither handler
       runs, and the comparisons below then pass against zeroed statics --
       three of four assertions green while testing nothing. */
    bool added = (axl_log_add_handler(stamp_handler_a, NULL) == AXL_OK) &&
                 (axl_log_add_handler(stamp_handler_b, NULL) == AXL_OK);

    axl_info("one record, two sinks");

    axl_log_remove_handler(stamp_handler_a);
    axl_log_remove_handler(stamp_handler_b);

    test_check(added, "stamp: both handlers registered");
    test_check(stamp_a_seen && stamp_b_seen,
               "stamp: both sinks received the record");
    if (!added || !stamp_a_seen || !stamp_b_seen) {
        return;   /* comparing zeroed statics proves nothing */
    }
    test_check(!stamp_a_null && !stamp_b_null,
               "stamp: the dispatcher supplied a timestamp");
    test_check(stamp_a.year   == stamp_b.year   &&
               stamp_a.month  == stamp_b.month  &&
               stamp_a.day    == stamp_b.day    &&
               stamp_a.hour   == stamp_b.hour   &&
               stamp_a.minute == stamp_b.minute &&
               stamp_a.second == stamp_b.second &&
               stamp_a.nanosecond == stamp_b.nanosecond,
               "stamp: every sink sees the SAME instant for one record");
    test_check(stamp_a.year >= 2000 && stamp_a.month >= 1 && stamp_a.month <= 12,
               "stamp: the supplied time is a real reading");
    /* Without this the "same instant" check above is vacuous: if the second
       never rolled, both sinks would agree even under per-sink reads. */
    test_check(stamp_second_rolled,
               "stamp: the wallclock second rolled while the first sink ran");
}

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

    /* Restore. Suppression used to be one-way, which silenced this binary's
       console for the rest of the run — including AXL's teardown memory
       verdict, leaving the harness leak gate structurally blind to
       AxlTestLog alone. Nothing in-guest can observe a console write, so
       the assertion that this line took effect is NOT here: it is the
       per-binary verdict requirement in test_check_leaks
       (test/integration/common-test.sh), which fails the run if AxlTestLog
       reaches teardown without printing one. Delete this line and that
       gate goes red. */
    axl_log_set_console_enabled(true);

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
  Test 7b: axl_log_ring_clear empties an attached ring in place, and the
  ring keeps receiving new messages afterward (no detach/re-attach).
**/
static void
test_ring_clear(void)
{
    AxlLogRing  *ring;
    AxlLogEntry  entry;

    ring = axl_log_ring_new(16, 128);
    test_check(ring != NULL, "ring clear: allocated");
    if (ring == NULL) {
        return;
    }
    axl_log_ring_attach(ring);

    axl_log(AXL_LOG_INFO, "rc", "before-1");
    axl_log(AXL_LOG_INFO, "rc", "before-2");
    test_check(axl_log_ring_count(ring) == 2, "ring clear: 2 before clear");

    axl_log_ring_clear(ring);
    test_check(axl_log_ring_count(ring) == 0, "ring clear: empty after clear");
    test_check(!axl_log_ring_get(ring, 0, &entry),
               "ring clear: get(0) fails on the emptied ring");

    /* Still attached: new messages land in the ring. */
    axl_log(AXL_LOG_INFO, "rc", "after-1");
    test_check(axl_log_ring_count(ring) == 1,
               "ring clear: ring still receives after clear");
    test_check(axl_log_ring_get(ring, 0, &entry)
               && test_strstr(entry.message, "after-1") != NULL,
               "ring clear: newest entry is the post-clear message");

    /* clear(NULL) must not crash, and must not disturb a real ring. */
    axl_log_ring_clear(NULL);
    test_check(axl_log_ring_count(ring) == 1,
               "ring clear: clear(NULL) left the real ring intact");

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

    /* Detach: explicit teardown that pairs with attach. After detach,
       subsequent log calls must NOT make it into the file (handler is
       gone). Re-attach with the same path then read back to confirm
       only the second-attach-era marker is present beyond the first.
       This is the AxlService driver-teardown pattern: attach in setup,
       detach in teardown, image unload doesn't have to clean up. */
    axl_log_file_detach();
    axl_log(AXL_LOG_INFO, "file", "after-detach-marker");
    /* No flush needed — the buffer's empty after detach (it flushed)
       and the handler is gone so this log went only to the console. */

    /* Re-attach to a fresh path so the new write isn't conflated with
       seek-to-0 overwriting prior content (UEFI Open with READ|WRITE|
       CREATE preserves bytes but starts position at 0; a re-attach to
       the same path overwrites from byte 0 — that's the SDK's expected
       behavior, not what this test is here to assert). The test goal is
       "detach + re-attach round-trip works AND the post-detach
       no-handler window really blocks writes." */
    rc = axl_log_file_attach("axl-test-log-2.log");
    test_check(rc == 0, "file handler: re-attach after detach succeeds");
    if (rc != 0) {
        return;
    }
    axl_log(AXL_LOG_INFO, "file", "second-attach-marker");
    axl_log_flush();

    rc = axl_file_get_contents("axl-test-log-2.log", &buf, &len);
    test_check(rc == AXL_OK, "file handler: reopen after re-attach");
    if (rc == AXL_OK) {
        const char *s = (const char *)buf;
        test_check(test_strstr(s, "second-attach-marker") != NULL,
                   "file handler: second-attach marker present in new file");
        test_check(test_strstr(s, "after-detach-marker") == NULL,
                   "file handler: detach really stopped the handler from writing");
        axl_free(buf);
    }

    /* Final detach + double-detach safety: after two detaches the
       internal state must still be clean enough for a fresh attach to
       succeed. Reaching the third attach proves the second detach
       neither corrupted state nor double-freed anything. */
    axl_log_file_detach();
    axl_log_file_detach();
    rc = axl_log_file_attach("axl-test-log.log");
    test_check(rc == 0, "file handler: attach after double-detach succeeds");
    axl_log_file_detach();
}

// ---------------------------------------------------------------------------
// Handler-table overflow — axl_log_add_handler must return -1 when full
// ---------------------------------------------------------------------------

/* Distinct callback per slot so axl_log_remove_handler can pop them
 * one at a time rather than nuking everything at once. */
static void noop_h0(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h1(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h2(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h3(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h4(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h5(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h6(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h7(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }
static void noop_h8(int l, const char *d, const char *m, const AxlRealtime *s, void *x)
{ (void)l;(void)d;(void)m;(void)s;(void)x; }

/* The serial sink shares its line builder with the file sink, so the FORMAT
   is pinned here through the file sink (which a unit test can read back).
   Driving the serial sink itself is deliberately NOT attempted: the unit
   runner boots -nographic with the guest's UART redirected to the harness's
   own transcript, so a sink writing to it would corrupt the very output the
   results are parsed from. What is left for the serial API are the safe
   negatives -- the errors its own validation produces before any firmware
   call. See feedback_uefi_firmware_test_hazards. */
static void
test_serial_sink_contract(void)
{
    void   *buf = NULL;
    size_t  len = 0;

    /* Exact line format, so the shared-builder refactor cannot drift it:
           2026-03-27T14:05:32.123456 [INFO ] dom: msg\n
       Offsets are fixed because the timestamp is fixed-width. */
    bool have_fixture = axl_log_file_attach("axl-test-logfmt.log") == 0;
    test_check(have_fixture, "log line: fmt fixture attached");
    if (have_fixture) {
    axl_log(AXL_LOG_INFO, "dom", "msg");
    /* A message longer than the line buffer pins the TRUNCATION policy --
       the half of the builder the extraction actually changed, and the one
       an off-by-one in the domain gate shows up in. */
    char big[900];
    for (size_t i = 0; i < sizeof(big) - 1; i++) { big[i] = 'x'; }
    big[sizeof(big) - 1] = '\0';
    axl_log(AXL_LOG_INFO, "dom", "%s", big);
    /* A long DOMAIN is the case that actually reaches the line buffer's
       limit -- the dispatcher caps the MESSAGE long before 640, but not the
       domain. At the exact boundary the domain must be dropped rather than
       emitted, because emitting it would leave zero room and the record
       would degenerate into a domain with no message. */
    char dom[602];
    for (size_t i = 0; i < sizeof(dom) - 1; i++) { dom[i] = 'd'; }
    dom[sizeof(dom) - 1] = '\0';
    axl_log(AXL_LOG_INFO, dom, "ZZZ");
    axl_log_flush();
    axl_log_file_detach();
    }

    if (axl_file_get_contents("axl-test-logfmt.log", &buf, &len) != AXL_OK) {
        test_check(false, "log line: fmt fixture readable");
        buf = NULL;
    }
    if (buf != NULL) {
    const char *ln = (const char *)buf;
    /* Measure the FIRST line, not the file: robust if the fixture path
       survives from an earlier run and gets appended to. */
    size_t line_len = 0;
    while (line_len < len && ln[line_len] != '\n') {
        line_len++;
    }
    if (line_len < len) {
        line_len++;                       /* include the LF */
    }
    test_check(line_len == 26 + 1 + 8 + 5 + 3 + 1,
               "log line: exact length (ts + space + tag + domain + msg + LF)");
    bool shape = len > 40
              && ln[4] == '-' && ln[7] == '-' && ln[10] == 'T'
              && ln[13] == ':' && ln[16] == ':' && ln[19] == '.'
              && ln[26] == ' ';
    test_check(shape, "log line: timestamp is fixed-width with a trailing space");
    test_check(shape && axl_strncmp(ln + 27, "[INFO ] dom: msg\n", 17) == 0,
               "log line: [LEVEL] domain: message + LF, exactly");
    /* Second record: the oversized message must be truncated to exactly fill
       the line, never dropped and never overrun. */
    const char *l2 = ln + line_len;
    size_t l2_len = 0;
    while ((size_t)(l2 - ln) + l2_len < len && l2[l2_len] != '\n') {
        l2_len++;
    }
    if ((size_t)(l2 - ln) + l2_len < len) {
        l2_len++;
    }
    test_check(l2_len > 27 + 8 + 5 && l2_len <= 640 - 1
               && l2[l2_len - 1] == '\n'
               && axl_strncmp(l2 + 27, "[INFO ] dom: xxx", 16) == 0,
               "log line: an oversized message truncates, keeping tag + domain");

    /* Third record: the domain-gate boundary. Whatever the builder decides
       about the domain, the record must still END with the message -- an
       off-by-one that emits the domain with no room left produces a
       "<domain>: \n" line instead, which this catches. */
    const char *l3 = l2 + l2_len;
    size_t l3_len = 0;
    while ((size_t)(l3 - ln) + l3_len < len && l3[l3_len] != '\n') {
        l3_len++;
    }
    test_check(l3_len >= 4 && axl_strncmp(l3 + l3_len - 3, "ZZZ", 3) == 0,
               "log line: a domain that would fill the line never displaces the message");
    axl_free(buf);
    }

    /* Safe negatives for the serial sink. */
    test_check(axl_log_serial_attach(NULL, AXL_LOG_INFO) == AXL_ERR,
               "serial sink: attach(NULL) returns AXL_ERR");
    /* Detaching when nothing is attached must be a genuine no-op -- not
       merely crash-free. A detach that removed a handler unconditionally
       would evict somebody ELSE's, which is invisible unless you check that
       a bystander still receives records. */
    custom_handler_calls = 0;
    axl_log_add_handler(custom_handler, NULL);
    axl_log_serial_detach();      /* nothing attached */
    axl_log_serial_detach();      /* and again */
    axl_log(AXL_LOG_WARNING, "test", "bystander");
    test_check(custom_handler_calls == 1,
               "serial sink: detach with nothing attached leaves other handlers registered");
    axl_log_remove_handler(custom_handler);
}

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

// ---------------------------------------------------------------------------
// AXL_LOG_LEVEL parser
// ---------------------------------------------------------------------------

static void
test_axl_log_level_env(void)
{
    /* Helper pattern for each case: reset state, set env, init, fire
     * a probe message via the custom handler, count what came through. */
    axl_log_add_handler(custom_handler, NULL);

    /* 1. Bare level keyword — global default */
    axl_log_set_level(AXL_LOG_INFO);
    axl_log_set_domain_level("evttest", -1);
    axl_setenv("AXL_LOG_LEVEL", "debug", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "evttest", "should pass");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL=debug: DEBUG passes globally");

    /* 2. Wildcard syntax (*:warn) — explicit default */
    axl_setenv("AXL_LOG_LEVEL", "*:warn", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_INFO, "evttest", "info filtered");
    test_check(custom_handler_calls == 0,
               "AXL_LOG_LEVEL=*:warn: INFO filtered");
    axl_log(AXL_LOG_WARNING, "evttest", "warning passes");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL=*:warn: WARNING passes");

    /* 3. Per-domain — domain loud, default quiet */
    axl_setenv("AXL_LOG_LEVEL", "*:warn,evttest:debug", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "evttest", "domain-loud passes");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL multi-domain: domain DEBUG passes");
    axl_log(AXL_LOG_INFO, "other", "default-quiet filtered");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL multi-domain: default INFO filtered");

    /* 4. "all" alias — every domain to DEBUG */
    axl_log_set_domain_level("evttest", -1);
    axl_setenv("AXL_LOG_LEVEL", "all", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "anywhere", "all-alias passes");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL=all: DEBUG passes everywhere");

    /* 5. "off" alias — even ERROR is filtered */
    axl_setenv("AXL_LOG_LEVEL", "off", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_ERROR, "test", "should be filtered");
    test_check(custom_handler_calls == 0,
               "AXL_LOG_LEVEL=off: ERROR filtered");

    /* 6. Unknown level keyword — ignored, prior config kept */
    axl_log_set_level(AXL_LOG_INFO);
    axl_setenv("AXL_LOG_LEVEL", "garbage", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_INFO, "test", "still works");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL=garbage: ignored, prior config kept");

    /* 7. Case-insensitive level keywords */
    axl_setenv("AXL_LOG_LEVEL", "DEBUG", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "test", "uppercase debug accepted");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL=DEBUG (uppercase): parsed as debug");

    axl_setenv("AXL_LOG_LEVEL", "Warn", true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_INFO, "test", "info filtered");
    test_check(custom_handler_calls == 0,
               "AXL_LOG_LEVEL=Warn (mixed case): parsed as warning");

    /* 8. Env precedence — env applied lazily, programmatic call wins */
    axl_unsetenv("AXL_LOG_LEVEL");
    axl_setenv("AXL_LOG_LEVEL", "warn", true);
    /* Programmatic set BEFORE first emission. axl_log_set_level()
     * itself runs ensure_env_init_once() (so env baseline is applied)
     * and then writes the user's level on top — code wins. */
    axl_log_set_level(AXL_LOG_DEBUG);
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "test", "code-wins precedence");
    test_check(custom_handler_calls == 1,
               "axl_log_set_level after env: programmatic call wins");

    /* 9. Length cap: an over-long value is ignored WHOLE, not applied up to
     * the cut. The two halves differ by one padding entry, so the only
     * variable is length — and the trailing "debug" is the probe: applied
     * under the cap, absent over it. A prefix-truncating implementation
     * would pass the first half and ALSO pass the second (the tail entry
     * silently vanishing is indistinguishable from a config that took), so
     * the pair is what makes this discriminating. */
    char envbuf[320];
    size_t env_n = 0;
    for (int i = 0; i < 35; i++) {                 /* 35 * 7 = 245 bytes */
        axl_memcpy(envbuf + env_n, "x:info,", 7);
        env_n += 7;
    }
    axl_memcpy(envbuf + env_n, "debug", 6);        /* + NUL -> 250 bytes */
    axl_log_set_level(AXL_LOG_INFO);
    axl_setenv("AXL_LOG_LEVEL", envbuf, true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "test", "under the cap");
    test_check(custom_handler_calls == 1,
               "AXL_LOG_LEVEL: a 250-byte value is applied");

    axl_memcpy(envbuf + 245, "x:info,debug", 13);  /* + NUL -> 257 bytes */
    axl_log_set_level(AXL_LOG_INFO);
    axl_setenv("AXL_LOG_LEVEL", envbuf, true);
    axl_log_init_from_env();
    custom_handler_calls = 0;
    axl_log(AXL_LOG_DEBUG, "test", "over the cap");
    test_check(custom_handler_calls == 0,
               "AXL_LOG_LEVEL: a 257-byte value is ignored whole, not truncated");
    axl_log_set_domain_level("x", -1);

    /* 10. Reading the variable must not allocate.
     *
     * axl_getenv hands back an OWNED heap copy, and AxlLog cannot free one
     * (axl-mem.c logs through this module, so calling axl_free from here
     * closes the circular dependency the module layout exists to avoid).
     * Reading AXL_LOG_LEVEL through it therefore leaked one string per
     * image, every image, for as long as the variable was set — the
     * teardown leak report showed it. The fix reads the shell's own
     * storage via the backend and decodes into a stack buffer; this pins
     * "allocates nothing", which is the property that makes it correct
     * rather than merely tidy. The value is "info" so that whatever it
     * does apply lands on the level this function's cleanup restores
     * anyway. */
    axl_setenv("AXL_LOG_LEVEL", "info", true);
    AxlMemStats env_before, env_after;
    axl_mem_get_stats(&env_before);
    axl_log_init_from_env();
    axl_mem_get_stats(&env_after);
    test_check(env_after.count == env_before.count,
               "AXL_LOG_LEVEL: init_from_env allocates nothing");
    test_check(env_after.total_count == env_before.total_count,
               "AXL_LOG_LEVEL: init_from_env does not even transiently allocate");

    /* Cleanup so other tests start fresh */
    axl_log_remove_handler(custom_handler);
    axl_unsetenv("AXL_LOG_LEVEL");
    axl_log_set_level(AXL_LOG_INFO);
    axl_log_set_domain_level("evttest", -1);
    axl_log_set_domain_level("anywhere", -1);
    axl_log_set_domain_level("other", -1);
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
    test_ring_clear();
    test_file_handler();
    test_serial_sink_contract();
    test_add_handler_overflow();
    test_dispatch_stamps_once();
    test_axl_log_level_env();

    return test_print_results();
}

AXL_APP(test_log_main)
