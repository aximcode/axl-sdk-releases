/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-version.h
    Compile-time AXL SDK version.

    Keep the macros below in sync with the VERSION file at the repo
    root — the Makefile runs a `check-version` target before every
    build that hard-errors on drift. Use `scripts/bump-version.sh`
    to update both files atomically at release time.

    Consumers can gate features on a specific AXL version at
    compile time:

    @code
    #include <axl/axl-version.h>

    #if AXL_VERSION_AT_LEAST(0, 1, 1)
        // use the cache policy fixes from 0.1.1
    #endif
    @endcode
**/

#ifndef AXL_VERSION_H
#define AXL_VERSION_H

#include <stdbool.h>

#define AXL_VERSION_MAJOR   2
#define AXL_VERSION_MINOR   8
#define AXL_VERSION_PATCH   5
#define AXL_VERSION_STRING  "2.8.5"

/* Encoded as 0xMMmmpp (major, minor, patch) for simple comparisons. */
#define AXL_VERSION_NUMBER \
    ((AXL_VERSION_MAJOR << 16) | (AXL_VERSION_MINOR << 8) | AXL_VERSION_PATCH)

#define AXL_VERSION_AT_LEAST(major, minor, patch) \
    (AXL_VERSION_NUMBER >= (((major) << 16) | ((minor) << 8) | (patch)))

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The AXL SDK release version of the linked library, at runtime.
 *
 * Returns the same string as the compile-time @c AXL_VERSION_STRING macro,
 * but read from the linked @c libaxl.a — so a program (a tool, or a
 * consumer app) can report the exact SDK build it is running against without
 * recompiling. This is the version @c bump-version.sh stamps at each release.
 *
 * @return NUL-terminated version string, e.g. "2.8.4". Never NULL.
 */
const char *
axl_version(void);

/**
 * @brief Framework hook: handle a `--version` / `-V` request for a tool.
 *
 * Scans @p argv for a leading @c --version or @c -V option (stopping at a
 * @c -- end-of-options marker, so a positional literal after @c -- is not
 * mistaken for the flag). If found, prints `"<prog> <version>"` to stdout and
 * returns true; the caller should then exit 0 without doing any other work.
 * Returns false otherwise. Used by @c AXL_TOOL_MAIN so every tool reports its
 * version uniformly, whether or not it uses the @c axl_args_run parser.
 *
 * @param prog  program name to print (the tool stem, e.g. "mkrd").
 * @param argc  argument count.
 * @param argv  argument vector.
 * @return true if a version request was handled (caller should return 0).
 */
bool
axl_version_handle(
    const char  *prog,
    int          argc,
    char       **argv
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_VERSION_H */
