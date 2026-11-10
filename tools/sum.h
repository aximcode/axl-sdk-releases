/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file sum.h
    The shared body of md5sum and sha256sum (tools/sum.c). Each tool is a
    one-line entry point (tools/md5sum.c, tools/sha256sum.c) naming its
    program and algorithm, the BusyBox shape: one implementation, one image
    per command.
**/

#ifndef AXL_TOOLS_SUM_H
#define AXL_TOOLS_SUM_H

#include <axl.h>

/**
 * @brief Run a GNU-coreutils-style checksum command.
 *
 * Hashes each FILE operand (`-` or none: stdin) and prints
 * `<hex>  <name>`, or with `-c` reads checksum lists and verifies them.
 *
 * @return 0 when every file was read (and, with -c, matched); 1 otherwise.
 */
int
sum_tool_run(
    int             argc,  ///< argument count
    char          **argv,  ///< argument vector
    const char     *prog,  ///< program name for messages ("md5sum")
    AxlChecksumType type   ///< algorithm
);

#endif /* AXL_TOOLS_SUM_H */
