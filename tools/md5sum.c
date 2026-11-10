/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file md5sum.c
    Print or check MD5 checksums -- GNU coreutils md5sum(1). The body is
    tools/sum.c, shared with sha256sum.

    Usage:
      md5sum [-b|-t] [--tag] [-z] [FILE...]   print a checksum per file
      md5sum -c [--quiet|--status|-w] [--strict] [--ignore-missing] [LIST...]
**/

#include <axl.h>

#include "sum.h"

AXL_TOOL_MAIN(md5sum)
{
    return sum_tool_run(argc, argv, "md5sum", AXL_CHECKSUM_MD5);
}
