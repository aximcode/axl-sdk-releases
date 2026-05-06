/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file timetest.c
    Verify the monotonic-microsecond counter (axl_backend_get_monotonic_us)
    on real firmware.

    Calls the counter at known sleep intervals and reports the deltas, then
    fires a few axl_info logs so the operator can eyeball the timestamp
    fractional field.

    Usage: TimeTest.efi
**/

#include <axl.h>

AXL_LOG_DOMAIN("timetest");

/* Forward declaration so we don't need to expose the backend header
   to tools — backend functions are linked in via libaxl. */
extern uint64_t axl_backend_get_monotonic_us(void);

static int
run_timetest(AxlArgs *a)
{
    (void)a;

    /* Warm the calibration. The first call is the calibration itself
       (uses ~10ms gBS->Stall on x86) and returns 0; the second call
       gives a real value. */
    uint64_t t0 = axl_backend_get_monotonic_us();
    uint64_t t1 = axl_backend_get_monotonic_us();
    axl_printf("Calibration: first call = %llu us, second call = %llu us\n",
               (unsigned long long)t0, (unsigned long long)t1);

    /* Capture deltas across known sleeps. axl_msleep takes ms and
       calls gBS->Stall(ms*1000) under the hood, so the measured
       delta should be very close to the requested interval. */
    struct {
        unsigned ms;
        const char *label;
    } steps[] = {
        {  1,  "1ms" },
        {  5,  "5ms" },
        { 25, "25ms" },
        {100, "100ms" },
        {500, "500ms" },
    };
    axl_printf("\nMeasured intervals (axl_msleep N, then sample):\n");
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        uint64_t before = axl_backend_get_monotonic_us();
        axl_msleep(steps[i].ms);
        uint64_t after  = axl_backend_get_monotonic_us();
        uint64_t delta  = after - before;
        long long error_us = (long long)delta
                           - (long long)((unsigned long long)steps[i].ms * 1000);
        axl_printf("  axl_msleep(%-5s): delta=%llu us, error=%+lld us\n",
                   steps[i].label,
                   (unsigned long long)delta, error_us);
    }

    /* Show 8 back-to-back log lines so the operator sees the
       timestamp's microsecond field actually advancing. */
    axl_printf("\nEight consecutive axl_info calls — watch the .uuuuuu field:\n");
    for (int i = 0; i < 8; i++) {
        axl_info("tick %d (mono=%llu us)",
                 i, (unsigned long long)axl_backend_get_monotonic_us());
    }

    /* Final read so the operator can compute boot age = (final - 0). */
    uint64_t mono_us = axl_backend_get_monotonic_us();
    axl_printf("\nMonotonic: %llu.%06llu seconds since axl init\n",
               (unsigned long long)(mono_us / 1000000),
               (unsigned long long)(mono_us % 1000000));

    return 0;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name    = "timetest",
        .help    = "Verify high-resolution monotonic-us counter from firmware",
        .handler = run_timetest,
    });
}
