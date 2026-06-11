/*
 * exit-status-selftest.c -- prove axl_set_exit_status reaches %lasterror%.
 *
 * Mirrors the consumer's `do err <N>` parity case: parse argv[1] as a HEX
 * number, arm it as this image's exact EFI_STATUS, and return a NONZERO rc.
 * Without axl_set_exit_status the runtime would collapse that nonzero rc to
 * EFI_ABORTED (0x15); with it, the UEFI shell's %lasterror% must reflect the
 * verbatim value (e.g. `err 34` -> 0x34, a non-error-class code).
 *
 * Driven by test/integration/test-exit-status-qemu.sh, which echoes
 * %lasterror% from a custom startup.nsh and asserts it.
 *
 * Build with: make exit-status-selftest
 */

#include <axl.h>

int
main(int argc, char *argv[])
{
    AxlEfiStatus status = 0x34;   /* default mirrors `do err 34` */
    if (argc > 1) {
        uint64_t v = 0;
        /* Hex parse, like the legacy `do err <N>` (N is hex). */
        if (axl_hex_parse_u64(argv[1], axl_strlen(argv[1]), &v) > 0) {
            status = (AxlEfiStatus)v;
        }
    }

    /* Printed BEFORE the exit so the serial log shows intent even if the
       shell formats %lasterror% unexpectedly. */
    axl_printf("exit-status-selftest: arming 0x%llx, returning rc=1\n",
               (unsigned long long)status);

    axl_set_exit_status(status);
    return 1;   /* nonzero on purpose: the armed status must override it */
}
