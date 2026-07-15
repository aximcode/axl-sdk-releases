/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-vt.h
    Internal: shared VT wire-format encoders for the two console serializers.

    `axl-console-mirror` (structured ops -> VT, live) and `axl-console-screen`
    (grid snapshot -> VT, repaint) both turn an @ref AxlConsolePen into the same
    SGR escape. This is that one encoder, so the byte format cannot drift between
    them. Not a public header — internal to the console modules.
**/

#ifndef AXL_CONSOLE_VT_H
#define AXL_CONSOLE_VT_H

#include <stddef.h>
#include <axl/axl-console-ops.h>

/**
 * @brief Encode a pen snapshot as a full SGR escape "ESC[0;<fg>;<bg>;<styles>m".
 *
 * Standard SGR vocabulary only: 39/49 default, 38;5;N/48;5;N indexed,
 * 38;2;R;G;B / 48;2;R;G;B truecolour, plus 1/3/4/5/7/8/9 for the style bits. The
 * leading 0 resets, so the escape is absolute — it fully replaces any prior pen.
 *
 * @param buf receives the escape (needs ~64 bytes for the widest pen).
 * @param cap size of @p buf.
 * @param pen the rendition to encode.
 * @return the number of bytes written (never more than @p cap - 1), or 0 if
 *     @p buf is NULL or @p cap is 0.
 */
size_t
axl_console_pen_to_sgr(char *buf, size_t cap, const AxlConsolePen *pen);

#endif /* AXL_CONSOLE_VT_H */
