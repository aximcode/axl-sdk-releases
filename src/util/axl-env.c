/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-env.c
    Shell environment variable access.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-env.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("util");

char *
axl_getenv(const char *name)
{
    unsigned short *wide_name;
    const unsigned short *wide_val;
    char *result;

    if (name == NULL) {
        return NULL;
    }

    wide_name = axl_utf8_to_ucs2(name);
    if (wide_name == NULL) {
        axl_warning("axl_getenv: OOM converting name '%s' to UCS-2", name);
        return NULL;
    }

    wide_val = axl_backend_shell_getenv(wide_name);
    axl_free(wide_name);

    if (wide_val == NULL) {
        return NULL;
    }

    result = axl_ucs2_to_utf8(wide_val);
    return result;
}

int
axl_setenv(const char *name, const char *value, bool overwrite)
{
    unsigned short *wide_name;
    unsigned short *wide_value;
    int rc;

    if (name == NULL || value == NULL) {
        return -1;
    }

    if (!overwrite) {
        /* Check if already exists */
        wide_name = axl_utf8_to_ucs2(name);
        if (wide_name == NULL) {
            axl_warning(
                "axl_setenv: OOM converting name '%s' to UCS-2 (existence check)",
                name
                );
            return -1;
        }
        if (axl_backend_shell_getenv(wide_name) != NULL) {
            axl_free(wide_name);
            return 0;  /* exists, don't overwrite */
        }
        axl_free(wide_name);
    }

    wide_name = axl_utf8_to_ucs2(name);
    if (wide_name == NULL) {
        axl_warning("axl_setenv: OOM converting name '%s' to UCS-2", name);
        return -1;
    }

    wide_value = axl_utf8_to_ucs2(value);
    if (wide_value == NULL) {
        axl_warning("axl_setenv: OOM converting value for '%s' to UCS-2", name);
        axl_free(wide_name);
        return -1;
    }

    rc = axl_backend_shell_setenv(wide_name, wide_value, true);
    axl_free(wide_name);
    axl_free(wide_value);
    return rc;
}

int
axl_unsetenv(const char *name)
{
    unsigned short *wide_name;
    int rc;

    if (name == NULL) {
        return -1;
    }

    wide_name = axl_utf8_to_ucs2(name);
    if (wide_name == NULL) {
        axl_warning("axl_unsetenv: OOM converting name '%s' to UCS-2", name);
        return -1;
    }

    /* Setting to empty string removes the variable in UEFI Shell */
    rc = axl_backend_shell_setenv(wide_name, (const unsigned short *)L"", true);
    axl_free(wide_name);
    return rc;
}
