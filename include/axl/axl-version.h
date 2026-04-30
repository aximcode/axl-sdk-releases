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

#define AXL_VERSION_MAJOR   0
#define AXL_VERSION_MINOR   6
#define AXL_VERSION_PATCH   0
#define AXL_VERSION_STRING  "0.6.0"

/* Encoded as 0xMMmmpp (major, minor, patch) for simple comparisons. */
#define AXL_VERSION_NUMBER \
    ((AXL_VERSION_MAJOR << 16) | (AXL_VERSION_MINOR << 8) | AXL_VERSION_PATCH)

#define AXL_VERSION_AT_LEAST(major, minor, patch) \
    (AXL_VERSION_NUMBER >= (((major) << 16) | ((minor) << 8) | (patch)))

#endif /* AXL_VERSION_H */
