/**
 * cpu-spin-fixture.c — CPU busy-wait DETECTOR FIXTURE.
 *
 * *** THIS APP BUSY-WAITS ON PURPOSE. DO NOT "FIX" IT. ***
 *
 * It is the positive/negative control for every CPU-spike detector in
 * this repo (run-qemu.sh's `--cpu-report` sampler, test-cpu-idle.sh,
 * and any host-side KVM exit-reason profiling). A detector that has
 * never been shown to fire on a KNOWN spin is not a detector; this
 * fixture is what proves it fires, and proves the negative control
 * stays quiet.
 *
 * Modes (argv[1], duration in ms as argv[2], default 4000):
 *
 *   compute  — POSITIVE control, pure-compute spin. A tight loop over
 *              memory + the monotonic clock. No firmware calls, no
 *              device access. Under KVM this produces ~1.0 host core
 *              with almost NO vmexits: the "guest is computing" shape.
 *
 *   port     — POSITIVE control, I/O-port poll (x86 only). Reads the
 *              ACPI PM timer port in a tight loop — the same shape as
 *              a firmware `gBS->Stall`, EDK2's UEFI Shell `stall`
 *              command, and any register-polling delay loop. Under KVM
 *              this produces ~1.0 host core AND a very high
 *              IO_INSTRUCTION vmexit rate: the "guest is polling a
 *              port" shape. This is the reference signature for
 *              telling a firmware/stall spin apart from a compute spin.
 *
 *   idle     — NEGATIVE control. Same wall-clock duration spent in
 *              axl_msleep, which idles on a firmware timer event.
 *              Under KVM this produces ~0.0 host cores and HLT exits:
 *              the shape a correctly-waiting AXL app must have.
 *
 * Every mode brackets its phase with `CPUSPIN: begin <mode>` /
 * `CPUSPIN: end <mode> elapsed=<ms>ms` markers so a host-side sampler
 * can place the phase on the serial timeline exactly.
 *
 * Example (host side):
 *   ./scripts/run-qemu.sh --cpu-report --timeout 30 \
 *       out/native-x64/cpu-spin-fixture.efi port 6000
 *   -> expect CPU-REPORT mean ~1.0 cores and exit code 8 (spike).
 *   ./scripts/run-qemu.sh --cpu-report --timeout 30 \
 *       out/native-x64/cpu-spin-fixture.efi idle 6000
 *   -> expect CPU-REPORT mean ~0.0 cores and exit code 0 (no spike).
 */

#include <axl.h>

#if defined(__x86_64__) || defined(__i386__)
#include <axl/axl-io-port.h>
#endif

/* ACPI PM timer I/O port on QEMU's q35/piix4 ACPI block. Reading it is
   side-effect-free (it is a free-running 24/32-bit counter), which is
   exactly why firmware delay loops poll it — and why it is safe to use
   as a fixture. */
#define ACPI_PM_TIMER_PORT  0x608

#define DEFAULT_DURATION_MS  4000ULL

static void
spin_compute(
    uint64_t  duration_ms
    )
{
    /* Volatile accumulator so the compiler cannot fold the loop away —
       the whole point of this fixture is that the loop actually runs. */
    volatile uint64_t acc = 0;
    uint64_t deadline = axl_time_get_ms() + duration_ms;

    while (axl_time_get_ms() < deadline) {
        for (uint32_t i = 0; i < 100000U; i++) {
            acc += i;
        }
    }
}

static void
spin_port(
    uint64_t  duration_ms
    )
{
#if defined(__x86_64__) || defined(__i386__)
    volatile uint32_t last = 0;
    uint64_t deadline = axl_time_get_ms() + duration_ms;

    while (axl_time_get_ms() < deadline) {
        for (uint32_t i = 0; i < 256U; i++) {
            last = axl_io_port_read32(ACPI_PM_TIMER_PORT);
        }
    }
    (void)last;
#else
    /* No architectural port space on AARCH64 — fall back to the compute
       spin so the mode still produces a POSITIVE control there, and say
       so, rather than silently measuring nothing. */
    axl_printf("CPUSPIN: port mode unavailable on this arch, using compute\n");
    spin_compute(duration_ms);
#endif
}

static void
spin_idle(
    uint64_t  duration_ms
    )
{
    uint64_t deadline = axl_time_get_ms() + duration_ms;

    while (axl_time_get_ms() < deadline) {
        axl_msleep(200);
    }
}

static void
usage(void)
{
    axl_printf("usage: cpu-spin-fixture <compute|port|idle> [duration_ms]\n");
    axl_printf("  compute  positive control: pure-compute spin (few vmexits)\n");
    axl_printf("  port     positive control: I/O-port poll (many io vmexits)\n");
    axl_printf("  idle     negative control: event-driven sleep (HLT, ~0 CPU)\n");
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *mode = argv[1];
    uint64_t duration_ms = DEFAULT_DURATION_MS;

    if (argc >= 3) {
        uint64_t parsed = 0;
        if (axl_str_to_u64(argv[2], 10, &parsed, NULL) != AXL_OK || parsed == 0) {
            axl_printf("cpu-spin-fixture: bad duration '%s'\n", argv[2]);
            return 1;
        }
        duration_ms = parsed;
    }

    axl_printf("CPUSPIN: begin %s duration=%lums\n", mode,
               (unsigned long)duration_ms);

    uint64_t start = axl_time_get_ms();

    if (axl_strcmp(mode, "compute") == 0) {
        spin_compute(duration_ms);
    } else if (axl_strcmp(mode, "port") == 0) {
        spin_port(duration_ms);
    } else if (axl_strcmp(mode, "idle") == 0) {
        spin_idle(duration_ms);
    } else {
        axl_printf("cpu-spin-fixture: unknown mode '%s'\n", mode);
        usage();
        return 1;
    }

    axl_printf("CPUSPIN: end %s elapsed=%lums\n", mode,
               (unsigned long)(axl_time_get_ms() - start));
    return 0;
}
