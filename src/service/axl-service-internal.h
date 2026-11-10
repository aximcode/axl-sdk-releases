/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-service-internal.h -- shared between axl-service.c and
 * axl-service-stage.c. Not part of the public API.
 */

#ifndef AXL_SERVICE_INTERNAL_H
#define AXL_SERVICE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include <axl/axl-service.h>

/* LoadOptions buffer cap. axl_config_to_string serializes set-only values
   (defaults are re-applied on the parse side from the descriptor table), so a
   typical service's payload is well under 1 KiB; 4 KiB is headroom for
   MULTI-heavy configs. Hitting the cap is an explicit error, never truncated
   load options. */
#define AXL_SERVICE_LOAD_OPTIONS_MAX  4096

/*
 * Build the LoadOptions a service image is started with from @p svc's option
 * struct: the AXLSVC1 magic and the URL-encoded options. @p buf holds
 * AXL_SERVICE_LOAD_OPTIONS_MAX bytes; @p out_len receives the payload length.
 * AXL_OK, or AXL_ERR (logged) when the options do not fit.
 */
int
_axl_service_build_load_options(
    const AxlService *svc,
    char             *buf,
    size_t           *out_len);

/*
 * axl_service_start_staged, with @p apply false for the `start --no-staged`
 * verb: what the last boot left is still resolved; no trial or promotion is
 * made.
 */
AxlStatus
_axl_service_start_staged_opt(
    const AxlServiceDeploy *deploy,
    bool                    apply,
    AxlServiceStagedResult *result);

/*
 * The staged upgrade's test seam: true makes the library act at @p point as
 * a test asks. A bare phase name ("trial", "promote:bak", ...) is reached
 * right after that phase is recorded -- true resets the box there, a power
 * loss at exactly that point. "nv:<what>" is reached before an NV write
 * ("nv:trial", "nv:promote:bak", "nv:promote:swap", "nv:quarantine") -- true
 * fails that write as a full or write-protected store would.
 *
 * The library's definition is WEAK and always false, so production carries no
 * trace of a test control; the stage suites' launcher links a strong one
 * (test/integration/upgrade-svc-test-hook.c).
 *
 * Keep the `*_test_point` name: test-efi-pack-release-qemu.sh finds the
 * seams in a shipped image by that suffix alone, and fails one that is not
 * the weak stub's size.
 */
bool
_axl_service_stage_test_point(
    const char *point);

/*
 * The GUID of @p svc's @p which state tag ("stopping" or "degraded"),
 * derived from the service name in the tags' own namespace. Internal so a
 * test fixture can publish a tag of another layout by hand, as a driver built
 * against another SDK version would.
 */
int
_axl_service_tag_guid(
    const AxlService *svc,
    const char       *which,
    AxlGuid          *out);

/*
 * The service lifecycle's test seam: true makes the library act at @p point
 * as if the firmware had refused it. The one point is "attach": true fails
 * axl_service_attach_driver's axl_loop_attach_driver after setup succeeded,
 * which needs a full per-image timer table to happen for real, so a test can
 * reach the rollback teardown behind it.
 *
 * The library's definition is WEAK and always false, so production carries no
 * trace of a test control; a fixture's driver image links a strong one
 * (test/integration/svc-tdfail.c). Same pattern as
 * _axl_service_stage_test_point below.
 *
 * Keep the `*_test_point` name: test-efi-pack-release-qemu.sh finds the
 * seams in a shipped image by that suffix alone, and fails one that is not
 * the weak stub's size.
 */
bool
_axl_service_test_point(
    const char *point);

#endif /* AXL_SERVICE_INTERNAL_H */
