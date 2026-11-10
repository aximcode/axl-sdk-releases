/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * upgrade-svc-test-hook.c -- the staged upgrade's test seam, linked into the
 * TEST launcher (upgrade-svc-t.efi) only.
 *
 * The library defines _axl_service_stage_test_point weakly as "never"; this
 * strong definition replaces it in the one image the stage suites boot, so
 * neither libaxl.a nor the example as shipped carries the Shell variables
 * below (test-service-stage-resume-qemu.sh checks that, with this file's
 * presence in upgrade-svc-t.efi as the control).
 *
 *   AXL_SERVICE_STAGE_TEST_CUT  = <phase>   reset right after <phase> is
 *                                           recorded: a power loss there
 *   AXL_SERVICE_STAGE_TEST_FAIL = <what>    the NV write <what> fails, as on
 *                                           a full or write-protected store
 *   AXL_SERVICE_STAGE_TEST_CLOBBER = <phase> right after <phase> is
 *                                           recorded, fs0:\restage.efi is
 *                                           written over P.new: a writer
 *                                           racing the promotion
 *
 * The suites set them with `set -v` (volatile) so they do not outlive a boot.
 */

#include <axl.h>

#include "../../src/service/axl-service-internal.h"

#define STAGE_TEST_CUT_VAR   "AXL_SERVICE_STAGE_TEST_CUT"
#define STAGE_TEST_FAIL_VAR  "AXL_SERVICE_STAGE_TEST_FAIL"
#define STAGE_TEST_CLOBBER_VAR "AXL_SERVICE_STAGE_TEST_CLOBBER"
#define STAGE_TEST_NV_PREFIX "nv:"
#define STAGE_TEST_CLOBBER_FROM "fs0:\\restage.efi"
#define STAGE_TEST_CLOBBER_TO   "fs0:\\upgrade-svc-dxe.efi.new"

static bool
var_is(
    const char *var,
    const char *value
    )
{
    char *v   = axl_getenv(var);
    bool  hit = v != NULL && axl_strcmp(v, value) == 0;
    axl_free(v);
    return hit;
}

bool
_axl_service_stage_test_point(
    const char *point
    )
{
    size_t np = sizeof(STAGE_TEST_NV_PREFIX) - 1;
    if (axl_strncmp(point, STAGE_TEST_NV_PREFIX, np) == 0) {
        return var_is(STAGE_TEST_FAIL_VAR, point + np);
    }
    if (var_is(STAGE_TEST_CLOBBER_VAR, point)) {
        void  *buf = NULL;
        size_t len = 0;
        if (axl_file_get_contents(STAGE_TEST_CLOBBER_FROM, &buf, &len) == AXL_OK) {
            int rc = axl_file_write_atomic(STAGE_TEST_CLOBBER_TO, buf, len);
            axl_printf("stage test: %s written over P.new at %s (rc=%d)\n",
                       STAGE_TEST_CLOBBER_FROM, point, rc);
            axl_free(buf);
        }
        return false;
    }
    return var_is(STAGE_TEST_CUT_VAR, point);
}
