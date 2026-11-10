/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-lines.h
    Backend-internal: console text written a line at a time, so a line
    exactly the console's width is followed by the next on the very next
    row (axl-backend-native-lines.c).

    Internal header -- not installed, not part of the public API.
**/

#ifndef AXL_BACKEND_NATIVE_LINES_H
#define AXL_BACKEND_NATIVE_LINES_H

#include <stdbool.h>
#include <uefi/axl-uefi.h>

/**
 * @brief Write @p str to the text output @p out: its text as it is, and each
 *        line break (CR LF, or a bare LF) that follows a wrap of @p out's
 *        cursor as what leaves each device it reaches where the wrap put it
 *        (axl-backend-native-lines.c), so no empty row is left after a line
 *        that filled its row. An output whose cursor does not move -- the
 *        Shell's `>`/`2>` file -- gets the string's characters unchanged.
 *
 *        @p out reaches the devices @p fanout tags only when it verifiably IS
 *        that console's splitter (or shares its Mode, as the Shell's console
 *        logger does); any other wrapper, a console tap included, is treated
 *        as one device. Uses only @p st, so a freestanding image (the pack
 *        stub) links it.
 */
void
axl_text_out_lines(
    EFI_SYSTEM_TABLE                 *st,      ///< the system table
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *out,     ///< text output to write to
    EFI_GUID                         *fanout,  ///< the device tag @p out fans out to
                                               ///< (a console splitter's: ConOut or
                                               ///< StdErr device), or NULL for one device
    EFI_HANDLE                        device,  ///< with @p fanout NULL: the handle
                                               ///< @p out is on, or NULL when unknown
    const CHAR16                     *str      ///< NUL-terminated UCS-2 text
    );

#endif /* AXL_BACKEND_NATIVE_LINES_H */
