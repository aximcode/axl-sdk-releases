/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-version-internal.h
    Internal: the single source of truth for the tool-identity header line.

    Both the AxlArgs `-h` renderer (src/util/axl-args.c `print_help_for`) and
    the non-framework help hook (`axl_help_handle`, src/util/axl-version.c) must
    print the SAME first line so `<tool> -h` self-identifies the build
    identically no matter which mechanism a tool uses. This declares the one
    function that owns that format, so the two callers can never drift.
**/

#ifndef AXL_VERSION_INTERNAL_H
#define AXL_VERSION_INTERNAL_H

/**
 * @brief Print the tool-identity header: "<prog> <axl_version()> - <tagline>\n\n".
 *
 * ASCII '-' separator, not a Unicode em-dash: a UEFI text console has no UTF-8,
 * so U+2014 would render as a white block. Emits the trailing blank line so the
 * next block (Usage:, prolog) is separated.
 *
 * @param prog     program name / breadcrumb (e.g. "sed", "mytool bios").
 * @param tagline  one-line description printed after the " - ".
 */
void
axl_tool_header_line(
    const char  *prog,
    const char  *tagline
    );

#endif /* AXL_VERSION_INTERNAL_H */
