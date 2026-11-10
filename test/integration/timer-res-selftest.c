/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * timer-res-selftest.c -- the cross-image and exit-path half of the
 * axl_timer_resolution_* contract, one exact line per observation:
 *
 *   period                 PERIOD=<us>
 *   hold <us>              GRANTED=<us>, then exits still holding it
 *   raw-exit <us>          GRANTED=<us>, then leaves through gBS->Exit, so
 *                          AXL's own exit path never runs (a crashed image)
 *   child <us> <self>      holds <us>, runs `<self> hold 1000` as a child,
 *                          then CHILD-DONE PERIOD=<us> and END PERIOD=<us>
 *   prune <us>             begin + end: END PERIOD=<us>
 *   foreign <us>           holds <us>, sets the tick to 2 ms behind AXL's back,
 *                          ends: END PERIOD=<us>; then puts 10 ms back itself
 *
 * Driven by test/integration/test-timer-resolution-qemu.sh. UEFI is used
 * directly for what no AXL API does on purpose: leaving through gBS->Exit,
 * and changing the tick without AXL (AXL_ALLOW_UEFI, as every test in this
 * tree builds with).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

static EFI_TIMER_ARCH_PROTOCOL *
timer_arch(void)
{
    EFI_TIMER_ARCH_PROTOCOL *t = NULL;
    if (axl_protocol_find_guid((const AxlGuid *)&gEfiTimerArchProtocolGuid,
                               (void **)&t) != AXL_OK) {
        return NULL;
    }
    return t;
}

static int
hold(
    const char           *arg,
    AxlTimerResolutionId *id
    )
{
    uint32_t granted = 0;
    if (axl_timer_resolution_begin((uint32_t)axl_strtou64(arg), id, &granted) != AXL_OK) {
        axl_printf("BEGIN-FAILED\n");
        return 1;
    }
    axl_printf("GRANTED=%u\n", granted);
    return 0;
}

int
main(
    int   argc,
    char *argv[]
    )
{
    if (argc < 2) {
        axl_printf("usage: timer-res-selftest.efi <mode> [us] [self]\n");
        return 2;
    }
    const char *mode = argv[1];
    if (axl_strcmp(mode, "period") == 0) {
        axl_printf("PERIOD=%u\n", axl_timer_period_us());
        return 0;
    }
    if (argc < 3) {
        axl_printf("timer-res-selftest: %s needs <us>\n", mode);
        return 2;
    }
    AxlTimerResolutionId id = 0;
    if (hold(argv[2], &id) != 0) {
        return 1;
    }
    if (axl_strcmp(mode, "hold") == 0) {
        return 0;   /* still holding: AXL's exit path must end it */
    }
    if (axl_strcmp(mode, "raw-exit") == 0) {
        gBS->Exit(gImageHandle, EFI_SUCCESS, 0, NULL);
        return 1;   /* not reached */
    }
    if (axl_strcmp(mode, "child") == 0 && argc >= 4) {
        int child_rc = 0;
        if (axl_image_run(argv[3], "timer-res-selftest.efi hold 1000", &child_rc) != AXL_OK) {
            axl_printf("CHILD-FAILED\n");
            return 1;
        }
        axl_printf("CHILD-DONE PERIOD=%u\n", axl_timer_period_us());
    } else if (axl_strcmp(mode, "foreign") == 0) {
        EFI_TIMER_ARCH_PROTOCOL *t = timer_arch();
        if (t == NULL || EFI_ERROR(t->SetTimerPeriod(t, 20000))) {   /* 2 ms */
            axl_printf("FOREIGN-FAILED\n");
            return 1;
        }
    } else if (axl_strcmp(mode, "prune") != 0) {
        axl_printf("timer-res-selftest: unknown mode %s\n", mode);
        return 2;
    }
    int rc = axl_timer_resolution_end(id);
    axl_printf("END rc=%d PERIOD=%u\n", rc, axl_timer_period_us());
    if (axl_strcmp(mode, "foreign") == 0) {
        EFI_TIMER_ARCH_PROTOCOL *t = timer_arch();
        if (t != NULL) {
            t->SetTimerPeriod(t, 100000);   /* 10 ms: undo our own meddling */
        }
    }
    return 0;
}
