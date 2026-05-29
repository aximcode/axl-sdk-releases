/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx.h
    AxlGfx 2D graphics — umbrella header.

    Includes the full 2D library: shared pixel/color types, the
    drawing surface (buffers, clip, transform), immediate-mode
    primitives + bitmap text, retained-mode paths, bitmap fonts,
    vector text (TrueType), and image decode.  `#include <axl.h>`
    or `#include <axl/axl-gfx.h>` pulls all of it.

    Individual headers can be included separately:
      - `#include <axl/axl-gfx-types.h>`   — pixel, blend, color palette
      - `#include <axl/axl-gfx-surface.h>` — availability, buffers, clip, transform
      - `#include <axl/axl-gfx-draw.h>`    — primitives + bitmap text
      - `#include <axl/axl-gfx-path.h>`    — retained paths + fill/stroke
      - `#include <axl/axl-gfx-gradient.h>`— linear / radial gradients
      - `#include <axl/axl-font.h>`        — bitmap font atlas
      - `#include <axl/axl-truetype.h>`    — vector text (TTF/OTF)
      - `#include <axl/axl-pixmap.h>`      — image decode (PNG/JPG/GIF/BMP)

    @code
    if (axl_gfx_available()) {
        AxlGfxInfo info;
        axl_gfx_get_info(&info);
        printf("Display: %ux%u\n", info.width, info.height);

        axl_gfx_fill_rect(100, 100, 200, 150, AXL_GFX_RED);
    }
    @endcode
**/

#ifndef AXL_GFX_H
#define AXL_GFX_H

#include <axl/axl-gfx-types.h>
#include <axl/axl-gfx-surface.h>
#include <axl/axl-gfx-draw.h>
#include <axl/axl-gfx-path.h>
#include <axl/axl-gfx-gradient.h>
#include <axl/axl-font.h>
#include <axl/axl-truetype.h>
#include <axl/axl-pixmap.h>

#endif /* AXL_GFX_H */
