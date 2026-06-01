/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file compat/setjmp.h
    Minimal freestanding <setjmp.h> shim.

    The vendored FreeType rasterizer (deps/freetype/ftgrays.c) includes
    <setjmp.h> in its STANDALONE_ build, but modern ftgrays does NOT
    call setjmp/longjmp at runtime — cell-buffer overflow is recovered
    via a return code + re-banding loop, not a longjmp.  This shim
    satisfies the include with declarations only; if anything ever
    actually called these, the link would fail loudly (by design).
**/

#ifndef AXL_COMPAT_SETJMP_H
#define AXL_COMPAT_SETJMP_H

typedef long  jmp_buf[8];

int   setjmp(jmp_buf env);
void  longjmp(jmp_buf env, int val);

#endif /* AXL_COMPAT_SETJMP_H */
