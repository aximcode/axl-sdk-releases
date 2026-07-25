/**
 * reload-svc-fail-dxe.c — poisoned replacement image for the
 * axl_service_reload start-failure test.
 *
 * Publishes the SAME service name as reload-svc-dxe.c, so it is a genuine
 * replacement candidate as far as the framework is concerned — but its setup
 * fails on purpose. The framework declines to attach and DriverEntry returns
 * an EFI error, so axl_service_reload must report a start failure (the caller
 * is then told, per the reload contract, that the service is DOWN).
 *
 * Driven by test-service-reload-fail-qemu.sh: staging this image beside
 * reload-svc-dxe.efi is what selects the start-failure scenario.
 */
#include <axl.h>

static int
rsvcf_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    axl_printf("RSVCF: poisoned setup failing on purpose\n");
    return AXL_ERR;
}

static const AxlService reload_fail_svc = {
    .name           = "reload-svc",
    .setup          = rsvcf_setup,
    .driver_tick_ms = 20,
};

AXL_SERVICE_DRIVER(reload_fail_svc)
