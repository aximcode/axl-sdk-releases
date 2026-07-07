/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-version.c
    Runtime AXL SDK version + the shared `--version` / `-V` tool hook.

    The version string is the single source stamped by
    scripts/bump-version.sh (AXL_VERSION_STRING in axl-version.h); reading it
    through this compiled unit lets any binary report the exact SDK build it
    links against, and lets every tool answer `--version` uniformly via
    AXL_TOOL_MAIN — whether or not the tool uses the axl_args_run parser.
**/

#include <axl/axl-version.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>

const char *
axl_version(void)
{
    return AXL_VERSION_STRING;
}

bool
axl_version_handle(
    const char  *prog,
    int          argc,
    char       **argv
    )
{
    if (argv == NULL) {
        return false;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a == NULL) {
            continue;
        }
        /* POSIX end-of-options marker: a bare "--" ends option scanning, so a
           literal "--version" / "-V" positional after it is not a version
           request (mirrors the args framework's -h handling). */
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            break;
        }
        if (axl_strcmp(a, "--version") == 0 || axl_strcmp(a, "-V") == 0) {
            axl_print("%s %s\n", (prog != NULL) ? prog : "axl", axl_version());
            return true;
        }
    }
    return false;
}
