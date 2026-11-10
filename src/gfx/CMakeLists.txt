# src/gfx -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-gfx.c
    axl-gfx-output.c
    axl-font.c
    axl-cursor.c
    axl-compositor.c
    axl-gfx-region.c
    axl-truetype.c
    axl-pixmap.c
    axl-gfx-path.c
    axl-gfx-stroke.c
    axl-gfx-rasterize.c
    axl-gfx-gradient.c
    axl-gfx-effects.c
    axl-gfx-display-list.c
    axl-edid.c)
