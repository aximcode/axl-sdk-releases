/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-internal.h
    Intra-module private helpers shared between axl-gfx.c and the
    other gfx-module source files (axl-gfx-path.c etc.).

    NOT exported — kept inside src/gfx/.  Consumers go through the
    public <axl/axl-gfx.h> surface.
**/

#ifndef AXL_GFX_INTERNAL_H
#define AXL_GFX_INTERNAL_H

#include <stdbool.h>

#include <axl/axl-math.h>

/// Get the current top of the gfx-module transform stack.  Returns
/// identity if the stack is empty.  Called by path / line primitives
/// to transform incoming vertices.
AxlMat3
axl_gfx_internal_current_transform(void);

#endif /* AXL_GFX_INTERNAL_H */
