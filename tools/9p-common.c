/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-common.c - helpers shared by the `9p` tool's translation units.
 */

#include <axl.h>

#include "9p-common.h"

bool
axl9p_split_host_port(
    const char *spec,
    char       *host,
    size_t      host_cap,
    uint16_t   *port
)
{
    const char *colon;
    size_t      host_len;

    if (spec == NULL || spec[0] == '\0') {
        return false;
    }
    colon = axl_strchr(spec, ':');
    if (colon == NULL) {
        /* No inline port: *port keeps whatever the caller seeded it with. */
        return axl_strlcpy(host, spec, host_cap) < host_cap;
    }
    if (axl_str_to_u16(colon + 1, 10, port, NULL) != AXL_OK || *port == 0) {
        return false;
    }
    host_len = (size_t)(colon - spec);
    if (host_len == 0 || host_len >= host_cap) {
        return false;
    }
    axl_memcpy(host, spec, host_len);
    host[host_len] = '\0';
    return true;
}

bool
axl9p_report_service(
    const char       *label,
    const AxlService *svc
)
{
    const AxlServiceDeploy deploy = { .service = svc };
    bool                   running;

    /* axl_service_is_running only reads deploy->service - the identity it
       looks up is the GUID derived from svc->name - so the blob fields a
       launch would need are correctly absent here. */
    running = axl_service_is_running(&deploy);
    axl_printf("%s: %s\n", label, running ? "running" : "stopped");
    return running;
}

int
axl9p_stop_service(
    const AxlService    *svc,
    const Axl9pStopMsgs *msgs
)
{
    /* As above: axl_service_stop reads only deploy->service, so the blob
       fields a launch would need are correctly absent here. */
    const AxlServiceDeploy deploy = { .service = svc };

    if (!axl_service_is_running(&deploy)) {
        axl_printf("%s", msgs->idle);
        return 0;
    }
    if (axl_service_stop(&deploy) != AXL_OK) {
        axl_printerr("%s", msgs->fail);
        return 1;
    }
    axl_printf("%s", msgs->done);
    return 0;
}
