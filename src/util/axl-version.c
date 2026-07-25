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

#include "axl-version-internal.h"

const char *
axl_version(void)
{
    return AXL_VERSION_STRING;
}

void
axl_tool_header_line(
    const char  *prog,
    const char  *tagline
    )
{
    /* ASCII '-' separator, not a Unicode em-dash: a UEFI text console has no
       UTF-8, so U+2014 would render as a white block. This is the single owner
       of the header format; the AxlArgs `-h` renderer calls it too. */
    axl_print("%s %s - %s\n\n", (prog != NULL) ? prog : "axl",
              axl_version(), (tagline != NULL) ? tagline : "");
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

bool
axl_help_handle(
    const char  *prog,
    const char  *tagline,
    const char  *usage,
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
           literal "--help" / "-h" positional after it is not a help request
           (mirrors axl_version_handle and the args framework's -h handling). */
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            break;
        }
        /* Standalone help token only — same set the args framework's
           is_help_flag() recognizes. A bundled "-h" (e.g. sed's "-nh") is not a
           help request. */
        if (axl_strcmp(a, "-h") == 0 || axl_strcmp(a, "--help") == 0
            || axl_strcmp(a, "?") == 0) {
            axl_tool_header_line(prog, tagline);
            axl_print("Usage: %s\n", (usage != NULL) ? usage : "");
            return true;
        }
    }
    return false;
}
