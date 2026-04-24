Basic graphics output over the UEFI GOP (Graphics Output Protocol).
Fill rectangles, blit pixel buffers, capture screen regions, and render
text with an 8x16 VGA bitmap font. Falls back gracefully on headless
systems where GOP is not available.

Header: `<axl/axl-gfx.h>`

## Overview

Not all UEFI systems have a display. Serial-only servers and headless
BMCs typically lack GOP. Always check `axl_gfx_available()` before
drawing:

```c
#include <axl.h>

if (!axl_gfx_available()) {
    axl_printf("No display — running headless\n");
    return;
}

AxlGfxInfo info;
axl_gfx_get_info(&info);
axl_printf("Display: %ux%u\n", info.width, info.height);

// Fill background
AxlGfxPixel bg = {0, 0, 0, 0};  // BGRX: black
axl_gfx_fill_rect(0, 0, info.width, info.height, bg);

// Draw text (8x16 VGA font, scalable)
AxlGfxPixel white = {255, 255, 255, 0};
axl_gfx_draw_text(20, 20, "Hello from AXL!", white, 2);
```

### Pixel Format

Pixels use BGRX layout (`AxlGfxPixel`): blue, green, red, reserved.
This matches the native GOP pixel format on most hardware, avoiding
conversion overhead.
