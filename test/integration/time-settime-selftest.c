/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * time-settime-selftest.c — round-trip exercise of the RTC-write API
 * axl_time_set_realtime() / axl_time_set_unix() against the firmware
 * real-time clock.
 *
 * There is no unit-test seam for gRT->SetTime (it mutates real
 * firmware state), so the write path is exercised end-to-end under
 * QEMU/OVMF, whose emulated RTC accepts SetTime. The app:
 *
 *   1) saves the current RTC,
 *   2) programs a known calendar time via axl_time_set_realtime,
 *      reads it back via axl_time_realtime, and asserts the fields
 *      match (seconds allowed a small forward tolerance for the
 *      one-second RTC tick between set and read-back),
 *   3) programs a known Unix instant via axl_time_set_unix and
 *      asserts the read-back decodes to the expected UTC calendar,
 *   4) checks that out-of-range / NULL inputs are rejected, and
 *   5) restores the saved RTC so a mutated clock can't perturb later
 *      tests in the same boot.
 *
 * Final line: "TIME-SETTIME-SELFTEST: <N> passed, <M> failed".
 */

#include <axl.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(
    bool         cond,
    const char  *label
    )
{
    if (cond) {
        g_pass++;
        axl_printf("PASS: %s\n", label);
    } else {
        g_fail++;
        axl_printf("FAIL: %s\n", label);
    }
}

/* second read back may be the value we wrote or up to two seconds
   later (the RTC ticks between SetTime and GetTime). Times are chosen
   away from a 59-second boundary so the tolerance can't roll the
   minute over. */
static bool
second_within(
    uint8_t  got,
    uint8_t  expect
    )
{
    return got == expect || got == (uint8_t)(expect + 1)
        || got == (uint8_t)(expect + 2);
}

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    /* 1) Save the current clock so we can restore it at the end. */
    AxlRealtime saved = { 0 };
    bool have_saved = (axl_time_realtime(&saved) == AXL_OK);
    check(have_saved, "read current RTC (baseline)");

    /* 2a) Input validation that happens before any firmware call. */
    check(axl_time_set_realtime(NULL) == AXL_ERR,
          "set_realtime(NULL) rejected");
    /* Pre-epoch and beyond-year-9999 must be rejected up front. */
    check(axl_time_set_unix(-1) == AXL_ERR,
          "set_unix(-1) rejected (pre-epoch)");
    check(axl_time_set_unix((int64_t)253402300800LL) == AXL_ERR,
          "set_unix(year 10000) rejected (out of range)");

    /* 2b) Program a known calendar time and read it back. */
    AxlRealtime want = {
        .year             = 2021,
        .month            = 6,
        .day              = 15,
        .hour             = 12,
        .minute           = 30,
        .second           = 45,
        .flags            = 0,
        .nanosecond       = 0,
        .timezone_minutes = 0,
    };
    check(axl_time_set_realtime(&want) == AXL_OK,
          "set_realtime(known time) succeeds");

    AxlRealtime got = { 0 };
    check(axl_time_realtime(&got) == AXL_OK, "read back after set_realtime");
    axl_printf("RT: %04u-%02u-%02u %02u:%02u:%02u tz=%d\n",
               got.year, got.month, got.day,
               got.hour, got.minute, got.second, got.timezone_minutes);
    check(got.year == 2021, "year round-trips");
    check(got.month == 6, "month round-trips");
    check(got.day == 15, "day round-trips");
    check(got.hour == 12, "hour round-trips");
    check(got.minute == 30, "minute round-trips");
    check(second_within(got.second, 45), "second round-trips (+/- tick)");

    /* 3) Program a known Unix instant (UTC) and read it back.
       1700000000 == 2023-11-14 22:13:20 UTC. */
    check(axl_time_set_unix((int64_t)1700000000LL) == AXL_OK,
          "set_unix(known instant) succeeds");

    AxlRealtime u = { 0 };
    check(axl_time_realtime(&u) == AXL_OK, "read back after set_unix");
    axl_printf("UX: %04u-%02u-%02u %02u:%02u:%02u tz=%d\n",
               u.year, u.month, u.day, u.hour, u.minute, u.second,
               u.timezone_minutes);
    check(u.year == 2023, "unix year decodes");
    check(u.month == 11, "unix month decodes");
    check(u.day == 14, "unix day decodes");
    check(u.hour == 22, "unix hour decodes");
    check(u.minute == 13, "unix minute decodes");
    check(second_within(u.second, 20), "unix second decodes (+/- tick)");

    /* 4) Restore the saved clock. Best-effort — only assert if we had
       a baseline to restore. */
    if (have_saved) {
        check(axl_time_set_realtime(&saved) == AXL_OK,
              "restore saved RTC");
    }

    axl_printf("TIME-SETTIME-SELFTEST: %d passed, %d failed\n",
               g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
