/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-virtual-pointer.c
    Install + drive a synthetic pointer — the pointer twin of
    axl_console_mirror_inject_key. Publishes an EFI_ABSOLUTE_POINTER_PROTOCOL
    (and optionally EFI_SIMPLE_POINTER) that a remote / synthetic source (a VNC
    server's RFB PointerEvent, an automated UI test) drives, so the firmware
    Setup browser / HII FrontPage responds on a box with no physical mouse.

    Console routing (the subtle part): the firmware Setup browser reads the
    pointer the console aggregator (ConSplitter) publishes on
    gST->ConsoleInHandle via LocateProtocol / HandleProtocol, not a blind
    handle. So install REPLACES the AbsolutePointer on ConsoleInHandle
    (ReinstallProtocolInterface, saving the original), exactly as
    AxlConsoleMirror replaces SimpleTextInputEx there for `edit` — see
    docs and axl-console-mirror.c.
**/

#include "../backend/axl-backend.h"   /* axl_bs(), axl_st() */
#include <axl/axl-input.h>
#include <axl/axl-gfx.h>              /* axl_gfx_get_info (default range) */
#include <axl/axl-mem.h>
#include <axl/axl-str.h>             /* axl_memset */
#include <axl/axl-log.h>
#include <axl/axl-atexit.h>
#include <uefi/axl-uefi.h>

#include <stddef.h>                   /* offsetof */

AXL_LOG_DOMAIN("vptr");

/* EFI_ABSOLUTE_POINTER_STATE.ActiveButtons bits (UEFI 2.x §12.7). */
#define VP_ABSP_TOUCH_ACTIVE  0x00000001u   /* primary / left  */
#define VP_ABSP_ALT_ACTIVE    0x00000002u   /* secondary / right */

/* Fallback range when no GOP resolution is available. */
#define VP_DEFAULT_WIDTH   1024u
#define VP_DEFAULT_HEIGHT   768u

struct AxlVirtualPointer {
    uint32_t  width;
    uint32_t  height;

    /* Absolute pointer (always published). The vtable callbacks recover the
       owner from the embedded protocol via offsetof, so field order is free. */
    EFI_ABSOLUTE_POINTER_PROTOCOL  abs;
    EFI_ABSOLUTE_POINTER_MODE      abs_mode;
    EFI_ABSOLUTE_POINTER_STATE     abs_state;   /* pending state */
    bool                           abs_avail;   /* new state since last GetState */
    EFI_EVENT                      abs_wait;
    void                          *orig_abs;    /* original on ConsoleInHandle */
    bool                           reinstalled_abs;  /* replaced vs fresh-installed */

    /* Optional simple (relative) pointer. */
    bool                           has_simple;
    EFI_SIMPLE_POINTER_PROTOCOL    sp;
    EFI_SIMPLE_POINTER_MODE        sp_mode;
    EFI_SIMPLE_POINTER_STATE       sp_state;    /* accumulated relative delta */
    bool                           sp_avail;
    EFI_EVENT                      sp_wait;
    void                          *orig_sp;
    bool                           sp_published;     /* did we install/reinstall sp? */
    bool                           reinstalled_sp;
    uint32_t                       last_x, last_y;
    bool                           have_last;

    uint32_t                       atexit_handle;
};

/* Singleton: there is one console pointer to publish on. */
static AxlVirtualPointer *g_vp;

// ---------------------------------------------------------------------------
// EFI_ABSOLUTE_POINTER vtable
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
vp_abs_reset(EFI_ABSOLUTE_POINTER_PROTOCOL *This, BOOLEAN ext)
{
    (void)ext;
    /* Resets only the absolute-pointer state — the two published protocols are
       independent, so a consumer resetting one doesn't disturb the other's
       (the simple pointer has its own vp_sp_reset). */
    AxlVirtualPointer *vp =
        (AxlVirtualPointer *)((char *)This - offsetof(AxlVirtualPointer, abs));
    EFI_TPL old = axl_bs()->RaiseTPL(TPL_NOTIFY);
    axl_memset(&vp->abs_state, 0, sizeof(vp->abs_state));
    vp->abs_avail = false;
    axl_bs()->RestoreTPL(old);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
vp_abs_getstate(EFI_ABSOLUTE_POINTER_PROTOCOL *This, EFI_ABSOLUTE_POINTER_STATE *State)
{
    if (State == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    AxlVirtualPointer *vp =
        (AxlVirtualPointer *)((char *)This - offsetof(AxlVirtualPointer, abs));
    EFI_TPL old = axl_bs()->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS rc;
    if (!vp->abs_avail) {
        rc = EFI_NOT_READY;
    } else {
        *State = vp->abs_state;
        vp->abs_avail = false;   /* one state per inject */
        rc = EFI_SUCCESS;
    }
    axl_bs()->RestoreTPL(old);
    return rc;
}

static void EFIAPI
vp_abs_wait_notify(EFI_EVENT ev, void *ctx)
{
    /* WAIT-type notify: signal iff a new state is pending, so a consumer
       polling via CheckEvent / WaitForEvent sees exactly the injected states. */
    AxlVirtualPointer *vp = (AxlVirtualPointer *)ctx;
    if (vp->abs_avail) {
        axl_bs()->SignalEvent(ev);
    }
}

// ---------------------------------------------------------------------------
// EFI_SIMPLE_POINTER vtable (optional; relative deltas from successive injects)
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
vp_sp_reset(EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN ext)
{
    (void)ext;
    AxlVirtualPointer *vp =
        (AxlVirtualPointer *)((char *)This - offsetof(AxlVirtualPointer, sp));
    EFI_TPL old = axl_bs()->RaiseTPL(TPL_NOTIFY);
    axl_memset(&vp->sp_state, 0, sizeof(vp->sp_state));
    vp->sp_avail  = false;
    vp->have_last = false;
    axl_bs()->RestoreTPL(old);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
vp_sp_getstate(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *State)
{
    if (State == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    AxlVirtualPointer *vp =
        (AxlVirtualPointer *)((char *)This - offsetof(AxlVirtualPointer, sp));
    EFI_TPL old = axl_bs()->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS rc;
    if (!vp->sp_avail) {
        rc = EFI_NOT_READY;
    } else {
        *State = vp->sp_state;
        /* Relative deltas are consumed; zero them so the next read reports
           only new movement (buttons persist as level state). */
        vp->sp_state.RelativeMovementX = 0;
        vp->sp_state.RelativeMovementY = 0;
        vp->sp_state.RelativeMovementZ = 0;
        vp->sp_avail = false;
        rc = EFI_SUCCESS;
    }
    axl_bs()->RestoreTPL(old);
    return rc;
}

static void EFIAPI
vp_sp_wait_notify(EFI_EVENT ev, void *ctx)
{
    AxlVirtualPointer *vp = (AxlVirtualPointer *)ctx;
    if (vp->sp_avail) {
        axl_bs()->SignalEvent(ev);
    }
}

// ---------------------------------------------------------------------------
// Internal: tear down a (possibly partially-built) instance.
// ---------------------------------------------------------------------------

static void
vp_free(AxlVirtualPointer *vp)
{
    if (vp->abs_wait != NULL) {
        axl_bs()->CloseEvent(vp->abs_wait);
    }
    if (vp->sp_wait != NULL) {
        axl_bs()->CloseEvent(vp->sp_wait);
    }
    axl_free(vp);
}

static void vp_atexit(void *ctx);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_virtual_pointer_install(AxlVirtualPointer **out,
                            const AxlVirtualPointerConfig *cfg)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    if (g_vp != NULL) {
        axl_warning("virtual pointer already installed (singleton)");
        return AXL_ERR;
    }

    /* Resolve the absolute range: cfg, else the active GOP resolution, else a
       sensible default. */
    uint32_t w = (cfg != NULL) ? cfg->width  : 0;
    uint32_t h = (cfg != NULL) ? cfg->height : 0;
    if (w == 0 || h == 0) {
        AxlGfxInfo gi;
        if (axl_gfx_get_info(&gi) == AXL_OK && gi.width > 0 && gi.height > 0) {
            if (w == 0) { w = gi.width;  }
            if (h == 0) { h = gi.height; }
        } else {
            if (w == 0) { w = VP_DEFAULT_WIDTH;  }
            if (h == 0) { h = VP_DEFAULT_HEIGHT; }
        }
    }

    AxlVirtualPointer *vp = axl_calloc(1, sizeof(*vp));
    if (vp == NULL) {
        return AXL_ERR;
    }
    vp->width  = w;
    vp->height = h;

    /* Absolute pointer: 1:1 pixel mapping -> max coordinate = dimension - 1. */
    vp->abs_mode.AbsoluteMaxX = w - 1;
    vp->abs_mode.AbsoluteMaxY = h - 1;
    vp->abs.Reset    = vp_abs_reset;
    vp->abs.GetState = vp_abs_getstate;
    vp->abs.Mode     = &vp->abs_mode;
    if (EFI_ERROR(axl_bs()->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY,
                                        vp_abs_wait_notify, vp, &vp->abs_wait))) {
        axl_error("virtual pointer: CreateEvent (abs WaitForInput) failed");
        vp_free(vp);
        return AXL_ERR;
    }
    vp->abs.WaitForInput = vp->abs_wait;

    /* Optional relative pointer. */
    if (cfg != NULL && cfg->also_simple) {
        /* Resolution is nominally counts/mm; our relative deltas are already in
           framebuffer pixels, so report 1 (1 count == 1 pixel) rather than a
           misleading per-mm figure. */
        vp->sp_mode.ResolutionX = 1;
        vp->sp_mode.ResolutionY = 1;
        vp->sp_mode.LeftButton  = TRUE;
        vp->sp_mode.RightButton = TRUE;
        vp->sp.Reset    = vp_sp_reset;
        vp->sp.GetState = vp_sp_getstate;
        vp->sp.Mode     = &vp->sp_mode;
        if (EFI_ERROR(axl_bs()->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY,
                                            vp_sp_wait_notify, vp, &vp->sp_wait))) {
            axl_error("virtual pointer: CreateEvent (simple WaitForInput) failed");
            vp_free(vp);
            return AXL_ERR;
        }
        vp->sp.WaitForInput = vp->sp_wait;
        vp->has_simple = true;
    }

    /* Publish the singleton BEFORE touching the console so a ConSplitter
       reinstall notify (which can fire synchronously) sees a live instance. */
    g_vp = vp;

    /* Console routing — replace (or install) the AbsolutePointer on
       gST->ConsoleInHandle, where the Setup browser locates it. */
    EFI_GUID   apg = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    EFI_HANDLE ci  = (EFI_HANDLE)axl_st()->ConsoleInHandle;
    EFI_STATUS st;
    if (!EFI_ERROR(axl_bs()->HandleProtocol(ci, &apg, &vp->orig_abs))
        && vp->orig_abs != NULL) {
        st = axl_bs()->ReinstallProtocolInterface(ci, &apg, vp->orig_abs, &vp->abs);
        if (EFI_ERROR(st)) {
            axl_error("virtual pointer: AbsolutePointer reinstall failed: %llx",
                      (unsigned long long)st);
            g_vp = NULL;
            vp_free(vp);
            return AXL_ERR;
        }
        vp->reinstalled_abs = true;
    } else {
        vp->orig_abs = NULL;
        st = axl_bs()->InstallProtocolInterface(&ci, &apg,
                                                EFI_NATIVE_INTERFACE, &vp->abs);
        if (EFI_ERROR(st)) {
            axl_error("virtual pointer: AbsolutePointer install failed: %llx",
                      (unsigned long long)st);
            g_vp = NULL;
            vp_free(vp);
            return AXL_ERR;
        }
    }

    /* Same dance for SimplePointer (best-effort: the AbsolutePointer is the
       primary path; a SimplePointer routing failure is non-fatal). */
    if (vp->has_simple) {
        EFI_GUID spg = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        if (!EFI_ERROR(axl_bs()->HandleProtocol(ci, &spg, &vp->orig_sp))
            && vp->orig_sp != NULL) {
            if (!EFI_ERROR(axl_bs()->ReinstallProtocolInterface(
                    ci, &spg, vp->orig_sp, &vp->sp))) {
                vp->reinstalled_sp = true;
                vp->sp_published   = true;
            }
        } else {
            vp->orig_sp = NULL;
            EFI_HANDLE ci2 = ci;
            if (!EFI_ERROR(axl_bs()->InstallProtocolInterface(
                    &ci2, &spg, EFI_NATIVE_INTERFACE, &vp->sp))) {
                vp->sp_published = true;
            }
        }
        if (!vp->sp_published) {
            axl_warning("virtual pointer: SimplePointer routing failed "
                        "(absolute pointer still active)");
        }
    }

    vp->atexit_handle = axl_atexit(vp_atexit, vp);

    *out = vp;
    axl_info("virtual pointer installed (%ux%u%s)", w, h,
             vp->has_simple ? " +simple" : "");
    return AXL_OK;
}

void
axl_virtual_pointer_uninstall(AxlVirtualPointer *vp)
{
    if (vp == NULL || g_vp != vp) {
        return;
    }

    EFI_HANDLE ci  = (EFI_HANDLE)axl_st()->ConsoleInHandle;
    EFI_GUID   apg = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    if (vp->reinstalled_abs && vp->orig_abs != NULL) {
        axl_bs()->ReinstallProtocolInterface(ci, &apg, &vp->abs, vp->orig_abs);
    } else {
        axl_bs()->UninstallProtocolInterface(ci, &apg, &vp->abs);
    }

    if (vp->sp_published) {
        EFI_GUID spg = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        if (vp->reinstalled_sp && vp->orig_sp != NULL) {
            axl_bs()->ReinstallProtocolInterface(ci, &spg, &vp->sp, vp->orig_sp);
        } else {
            axl_bs()->UninstallProtocolInterface(ci, &spg, &vp->sp);
        }
    }

    if (vp->atexit_handle != 0) {
        axl_atexit_remove(vp->atexit_handle);
    }

    g_vp = NULL;
    vp_free(vp);
    axl_info("virtual pointer uninstalled");
}

static void
vp_atexit(void *ctx)
{
    /* Safety net for a consumer that forgot to uninstall. uninstall clears the
       atexit handle on the normal path, so this only runs on a leak. */
    axl_virtual_pointer_uninstall((AxlVirtualPointer *)ctx);
}

int
axl_virtual_pointer_inject(AxlVirtualPointer *vp, uint32_t x, uint32_t y,
                           uint32_t buttons)
{
    if (vp == NULL) {
        return AXL_ERR;
    }
    if (x >= vp->width)  { x = vp->width  - 1; }
    if (y >= vp->height) { y = vp->height - 1; }

    uint32_t active = 0;
    if (buttons & 0x1u) { active |= VP_ABSP_TOUCH_ACTIVE; }
    if (buttons & 0x2u) { active |= VP_ABSP_ALT_ACTIVE;   }

    EFI_TPL old = axl_bs()->RaiseTPL(TPL_NOTIFY);
    vp->abs_state.CurrentX      = x;
    vp->abs_state.CurrentY      = y;
    vp->abs_state.CurrentZ      = 0;
    vp->abs_state.ActiveButtons = active;
    vp->abs_avail = true;

    if (vp->has_simple) {
        if (vp->have_last) {
            vp->sp_state.RelativeMovementX += (INT32)((int64_t)x - (int64_t)vp->last_x);
            vp->sp_state.RelativeMovementY += (INT32)((int64_t)y - (int64_t)vp->last_y);
        }
        vp->sp_state.LeftButton  = (buttons & 0x1u) ? TRUE : FALSE;
        vp->sp_state.RightButton = (buttons & 0x2u) ? TRUE : FALSE;
        vp->last_x    = x;
        vp->last_y    = y;
        vp->have_last = true;
        vp->sp_avail  = true;
    }
    axl_bs()->RestoreTPL(old);

    /* Wake a consumer already blocked in WaitForEvent (the wait-notify only
       re-arms on a fresh check). */
    axl_bs()->SignalEvent(vp->abs_wait);
    if (vp->has_simple) {
        axl_bs()->SignalEvent(vp->sp_wait);
    }
    return AXL_OK;
}

int
axl_virtual_pointer_scroll(AxlVirtualPointer *vp, int32_t dy)
{
    if (vp == NULL || !vp->has_simple) {
        return AXL_ERR;   /* the wheel only exists on the SimplePointer */
    }

    EFI_TPL old = axl_bs()->RaiseTPL(TPL_NOTIFY);
    /* The wheel rides on RelativeMovementZ; accumulate until consumed so
       back-to-back notches add up (like the X/Y deltas). Buttons stay at their
       current level; no position change. */
    vp->sp_state.RelativeMovementZ += dy;
    vp->sp_avail = true;
    axl_bs()->RestoreTPL(old);

    axl_bs()->SignalEvent(vp->sp_wait);
    return AXL_OK;
}
