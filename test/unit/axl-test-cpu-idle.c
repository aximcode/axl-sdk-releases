/** @file axl-test-cpu-idle.c
    CPU-idle regression workload. Performs a series of event-driven
    idle waits totaling ~3 seconds of guest wallclock. Paired with
    test/integration/test-cpu-idle.sh, which runs this binary under
    QEMU and asserts the host process's CPU-time / walltime ratio
    stays below threshold.

    Busy-poll regression in the event / wait primitives would push
    host CPU toward 100%; proper event-driven idling keeps it low.
    The ratio is portable across hardware (CI vs laptop vs dev box).

    This binary isn't in test-axl.sh's TEST_APPS -- it's exercised
    only by the dedicated perf script. A normal 'make tests' build
    still produces the .efi, so CI can invoke the script separately.
**/

#include "axl-test.h"

#include <axl/axl-wait.h>

/* ~10 seconds of idle wait, split into naps so the wait helper is
   torn down and re-spun up repeatedly (flushing out any per-wait
   leak of CPU). Each axl_wait_ms internally creates a throwaway
   AxlLoop + timeout source + idle block in axl_backend_event_wait
   -- proper idle, no polling.

   10s (not 3s) because OVMF firmware boot + shell init adds ~5s of
   unavoidable TCG-emulation CPU cost per QEMU run. Longer guest
   waits dilute that constant so the measured ratio reflects library
   behavior, not boot overhead. */
#define NAP_COUNT    10U
#define NAP_MS     1000U

int
test_cpu_idle_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("cpu-idle: start %u x %ums waits\n", NAP_COUNT, NAP_MS);

    for (unsigned int i = 0; i < NAP_COUNT; i++) {
        int rc = axl_wait_ms(NULL, NAP_MS);
        if (rc != 0) {
            axl_printf("cpu-idle: unexpected rc=%d on nap %u\n", rc, i);
            return 1;
        }
    }

    axl_printf("cpu-idle: done\n");
    return 0;
}

AXL_APP(test_cpu_idle_main)
