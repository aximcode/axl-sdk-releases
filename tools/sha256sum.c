/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file sha256sum.c
    Print or check SHA-256 checksums -- GNU coreutils sha256sum(1). The body
    is tools/sum.c, shared with md5sum.

    Usage:
      sha256sum [-b|-t] [--tag] [-z] [FILE...]   print a checksum per file
      sha256sum -c [--quiet|--status|-w] [--strict] [--ignore-missing] [LIST...]
**/

#include <axl.h>

#include "sum.h"

AXL_TOOL_MAIN(sha256sum)
{
    return sum_tool_run(argc, argv, "sha256sum", AXL_CHECKSUM_SHA256);
}
