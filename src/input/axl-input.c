/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-input.c
    axl-input — source registration for UEFI input protocols.

    Mouse via `EFI_SIMPLE_POINTER_PROTOCOL` (Phase 0h, this file).
    Keyboard via thin wrapper over `axl_loop_add_key_press` (Phase 0i).
    Touch via `EFI_ABSOLUTE_POINTER_PROTOCOL` (deferred until consumer).

    Each `axl_input_attach_*` wrapper registers the underlying UEFI
    protocol's WaitForInput event with the existing
    `axl_loop_add_event` / `axl_loop_add_key_press` primitive.  No
    parallel queue — axl-loop already buffers events at the UEFI
    layer via WaitForInput.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-format.h>
#include <axl/axl-fs.h>
#include <axl/axl-input.h>
#include <axl/axl-log.h>
#include <axl/axl-loop.h>
#include <axl/axl-stream.h>
#include <axl/axl-str.h>
#include <axl/axl-time.h>
#include <axl/axl-wait.h>

AXL_LOG_DOMAIN("input");

// ===================================================================
// Keyboard chord helpers (pure — no protocol I/O)
// ===================================================================

char
axl_input_ctrl_letter(uint32_t unicode, uint32_t modifiers)
{
    // Serial console (TerminalDxe): Ctrl+letter arrives folded to a C0
    // control code (Ctrl+A = 0x01 … Ctrl+Z = 0x1A) with modifiers == 0.
    // Exclude the four C0 codes that are real editing keys in their own
    // right — Ctrl+H / I / J / M == Backspace / Tab / LF / CR.
    if (unicode >= 1 && unicode <= 26
        && unicode != 0x08 && unicode != 0x09
        && unicode != 0x0A && unicode != 0x0D) {
        return (char)('a' + (int)unicode - 1);
    }
    // Physical keyboard (Simple Text Input Ex): Ctrl+letter arrives as
    // the printable letter + AXL_INPUT_MOD_CTRL (not folded).  Case-fold
    // so Ctrl+A and Ctrl+Shift+A both resolve to 'a'.
    if ((modifiers & AXL_INPUT_MOD_CTRL) && unicode != 0) {
        uint32_t u = unicode;
        if (u >= 'A' && u <= 'Z') u += 0x20;
        if (u >= 'a' && u <= 'z') return (char)u;
    }
    return 0;
}

// ===================================================================
// Live keyboard modifier state — stamped onto pointer events
// ===================================================================
//
// UEFI delivers keyboard modifier state only WITH a keystroke, so to make
// Shift+wheel / Ctrl+click work the substrate keeps a live modifier state,
// updated from every keystroke — including the modifier-only "partial"
// keystrokes that EFI_KEY_STATE_EXPOSED delivers on shift/ctrl down+up
// (enabled in the backend), which keep it current between character keys.
// Pointer events (mouse + touch) are stamped with this state. It is 0 until
// a keyboard source is attached AND the firmware reports modifiers (no
// SimpleTextInputEx — e.g. a serial console — leaves it 0).

static uint32_t g_kbd_modifiers = 0;

// Track the live modifier state from a keystroke @p key. Returns true if the
// keystroke carries a character / scan code to deliver, false for a
// modifier-only "partial" keystroke (the state was updated; nothing to
// deliver). Pure aside from the module-global update; non-static (no public
// header) so the unit test can drive it.
bool
axl_input_track_modifiers(const AxlInputKey *key)
{
    if (key == NULL) {
        return true;   // never silently swallow input
    }
    g_kbd_modifiers = key->modifiers;
    return !(key->scan_code == 0 && key->unicode_char == 0);
}

// The current live keyboard modifier state (for pointer-event stamping and
// the unit test). Non-static (no public header).
uint32_t
axl_input_live_modifiers(void)
{
    return g_kbd_modifiers;
}

// ===================================================================
// Mouse — EFI_SIMPLE_POINTER_PROTOCOL
// ===================================================================
//
// State tracked per attach:
//   - accumulated cursor position (X, Y) starting at (0, 0)
//   - previous button state (so we can debounce DOWN/UP transitions)
//
// One mouse source per process for v0.1.  Multiple mice / hotplug
// could be added later by allocating MouseSource per attach call
// and supporting axl_loop_remove_source notification.
//
// Spec-typo note: `EFI_SIMPLE_POINTER_PROTOCOL.Mode` is documented
// in UEFI spec text as `EFI_SIMPLE_POINTER_MODE *`, but the struct
// definition in the spec HTML mistakenly uses `EFI_SIMPLE_INPUT_MODE`
// — a name that doesn't exist.  Our generated header falls back to
// `void *` because of that typo.  We cast at use site below.

// Max pointer interfaces a source binds (ConsoleInHandle + a few physical).
#define AXL_MAX_POINTER_IFACES 8

typedef struct {
    AxlInputCallback              cb;
    void                         *data;
    // Bound by HANDLE, ConsoleInHandle FIRST, and re-resolved to the current
    // interface every dispatch — the same model as TouchSource.  A virtual
    // pointer (and a BMC remote-console mouse) publishes its SimplePointer on
    // gST->ConsoleInHandle; binding ConsoleInHandle-first is what delivers it.
    // (The old single-interface bind via locate_physical_pointer SKIPPED
    // ConsoleInHandle in favour of a physical device, so a virtual scroll /
    // move never reached the consumer.)  Resolve-per-dispatch keeps a driver
    // Stop()/FreePool() from leaving us calling through freed memory.
    EFI_HANDLE                    handles[AXL_MAX_POINTER_IFACES]; ///< ConIn-first
    // Cached mode (axl_input_attach_mouse_ifaces): the caller took the pointer OUT
    // of the handle database (e.g. axl_console_device take_pointer), so bind the
    // supplied interfaces DIRECTLY and skip the per-dispatch HandleProtocol
    // re-resolve (which would now fail).  Valid because the caller guarantees the
    // interfaces outlive the attachment (their producers are not Stop()'d).
    EFI_SIMPLE_POINTER_PROTOCOL  *ifaces[AXL_MAX_POINTER_IFACES];
    bool                          cached;
    int                           nproto;
    int32_t                       cursor_x;
    int32_t                       cursor_y;
    bool                          prev_left;
    bool                          prev_right;
    AxlGesture                    gesture;       ///< click-count / drag recognizer
    AxlLoop                      *loop;          ///< loop (for repeat timers + detach)
    AxlSourceId                   source_ids[AXL_MAX_POINTER_IFACES]; ///< per-handle WaitForInput sources
    int                           nsrc;
    AxlSourceId                   repeat_src;    ///< active held-button repeat timer (0 = none)
    uint32_t                      repeat_button; ///< AXL_INPUT_BUTTON_* being repeated
} MouseSource;

// Run the gesture recognizer over @p ev (annotating click_count /
// dragging), then deliver it to the consumer callback.
static bool
mouse_emit(MouseSource *ms, AxlInputEvent *ev)
{
    ev->modifiers = g_kbd_modifiers;   // live keyboard modifiers (Shift+wheel, Ctrl+click)
    axl_input_gesture_feed(&ms->gesture, ev);
    return ms->cb(ev, ms->data);
}

// Held-pointer-button auto-repeat. Firmware never repeats a held mouse
// button (UsbMouseDxe has no repeat), so the substrate synthesizes it on
// a loop timer while the button stays down — for scrollbar arrows,
// spinners, press-and-hold.
static uint32_t g_repeat_delay_ms    = 0;    // 0 = disabled (default)
static uint32_t g_repeat_interval_ms = 50;

void
axl_input_set_button_repeat(uint32_t delay_ms, uint32_t interval_ms)
{
    g_repeat_delay_ms    = delay_ms;
    g_repeat_interval_ms = (interval_ms == 0) ? 50 : interval_ms;
}

// Deliver one synthetic held-button repeat. Bypasses mouse_emit (and so
// the gesture recognizer) on purpose — a repeat must never be counted as
// a click. Returns the consumer's keep/remove decision.
static bool
emit_repeat(MouseSource *ms)
{
    AxlInputEvent ev = {0};
    ev.type         = AXL_INPUT_MOUSE_BUTTON_DOWN;
    ev.timestamp_us = axl_time_get_us();
    ev.x            = ms->cursor_x;
    ev.y            = ms->cursor_y;
    ev.buttons      = ms->repeat_button;
    ev.modifiers    = g_kbd_modifiers;
    ev.dragging     = ms->gesture.dragging;
    ev.repeat       = true;
    return ms->cb(&ev, ms->data);
}

static bool
repeat_tick_cb(void *data)   // periodic, interval phase
{
    MouseSource *ms = (MouseSource *)data;
    if (!emit_repeat(ms)) {
        ms->repeat_src = 0;
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static bool
repeat_initial_cb(void *data)   // one-shot, fires after the initial delay
{
    MouseSource *ms = (MouseSource *)data;
    bool keep = emit_repeat(ms);
    // This one-shot auto-removes; hand off to the periodic interval phase.
    ms->repeat_src = keep
        ? axl_loop_add_timer(ms->loop, g_repeat_interval_ms, repeat_tick_cb, ms)
        : 0;
    return AXL_SOURCE_REMOVE;
}

static void
arm_repeat(MouseSource *ms, uint32_t button)
{
    if (g_repeat_delay_ms != 0 && ms->repeat_src == 0 && ms->loop != NULL) {
        ms->repeat_button = button;
        ms->repeat_src    = axl_loop_add_timeout(ms->loop, g_repeat_delay_ms,
                                                 repeat_initial_cb, ms);
    }
}

static void
disarm_repeat(MouseSource *ms)
{
    if (ms->repeat_src != 0) {
        axl_loop_remove_source(ms->loop, ms->repeat_src);
        ms->repeat_src = 0;
    }
}

static MouseSource mouse_state;
static bool        mouse_state_used = false;

// Resolve a bound handle to its CURRENT simple-pointer interface, or NULL if
// the protocol is gone (the providing driver was Stop()'d).  HandleProtocol
// validates the handle against the firmware database, so a stale handle fails
// safely rather than faulting — the simple-pointer twin of touch_resolve.
static EFI_SIMPLE_POINTER_PROTOCOL *
mouse_resolve(EFI_HANDLE handle)
{
    void *iface = NULL;
    if (handle != NULL
        && axl_bs()->HandleProtocol(handle, &EFI_SIMPLE_POINTER_PROTOCOL_GUID,
                                    &iface) == 0) {
        return (EFI_SIMPLE_POINTER_PROTOCOL *)iface;
    }
    return NULL;
}

// The i-th bound pointer interface: a cached pointer for the ifaces-attach path
// (the handle database no longer lists it), else re-resolved from its handle.
static EFI_SIMPLE_POINTER_PROTOCOL *
mouse_iface_at(MouseSource *ms, int i)
{
    return ms->cached ? ms->ifaces[i] : mouse_resolve(ms->handles[i]);
}

static bool
mouse_dispatch_cb(
    void  *data
    )
{
    MouseSource              *ms = (MouseSource *)data;
    EFI_SIMPLE_POINTER_STATE  state;

    /* Read whichever bound handle has data FIRST, re-resolving the current
       interface each dispatch.  GetState consumes one queued state per call.
       ms->handles is ordered physical devices first, ConsoleInHandle LAST
       (see attach_mouse): reading a physical EFI_SIMPLE_POINTER before the
       ConsoleInHandle aggregator is REQUIRED for the scroll wheel.  On
       firmware that routes the pointer through the ConSplitter aggregator on
       ConsoleInHandle, that aggregator's GetState CONSUMES the physical
       child's queued state but drops RelativeMovementZ (the wheel) — so
       reading ConsoleInHandle first silently eats every wheel notch while
       still delivering buttons/motion.  Reading the physical handle first
       captures the wheel; a virtual / BMC pointer published directly on
       ConsoleInHandle is still read (it comes last, and the physical handles
       are idle/NOT_READY when only the virtual pointer moved). */
    bool got = false;
    for (int i = 0; i < ms->nproto; i++) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = mouse_iface_at(ms, i);
        if (sp != NULL && sp->GetState(sp, &state) == 0) {
            got = true;
            break;
        }
    }
    if (!got) {
        /* No new data on any handle — keep sources alive for next dispatch. */
        return AXL_SOURCE_CONTINUE;
    }

    /* Accumulate cursor position from relative motion.  GetState
       returns deltas in protocol counts; for v0.1 we treat 1 count =
       1 pixel (no Mode scaling — the spec-typo workaround would cost
       a cast and most real input drivers report ~1 count per pixel
       at default sensitivity anyway).  Callers can clamp to screen
       bounds in their callback. */
    ms->cursor_x += state.RelativeMovementX;
    ms->cursor_y += state.RelativeMovementY;

    uint64_t      ts      = axl_time_get_us();
    uint32_t      buttons = (state.LeftButton  ? AXL_INPUT_BUTTON_LEFT  : 0u)
                          | (state.RightButton ? AXL_INPUT_BUTTON_RIGHT : 0u);

    /* MOUSE_MOVE on non-zero relative motion. */
    if (state.RelativeMovementX != 0 || state.RelativeMovementY != 0) {
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_MOUSE_MOVE;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.buttons      = buttons;
        if (!mouse_emit(ms, &ev)) {
            return AXL_SOURCE_REMOVE;
        }
    }

    /* BUTTON_DOWN / BUTTON_UP on state transitions (debounced). */
    if (state.LeftButton != ms->prev_left) {
        AxlInputEvent ev = {0};
        ev.type         = state.LeftButton
                          ? AXL_INPUT_MOUSE_BUTTON_DOWN
                          : AXL_INPUT_MOUSE_BUTTON_UP;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.buttons      = buttons;   // CURRENT button state (post-edge), per the
                                     // AxlInputEvent contract — NOT just the
                                     // changed bit.  A consumer that derives the
                                     // change (the compositor seat: ptr_buttons ^
                                     // buttons) needs the full mask or a release
                                     // (LEFT^LEFT==0) dispatches nothing.
        ms->prev_left   = state.LeftButton;
        if (!mouse_emit(ms, &ev)) {
            disarm_repeat(ms);
            return AXL_SOURCE_REMOVE;
        }
        if (state.LeftButton) {
            arm_repeat(ms, AXL_INPUT_BUTTON_LEFT);
        } else {
            disarm_repeat(ms);
        }
    }
    if (state.RightButton != ms->prev_right) {
        AxlInputEvent ev = {0};
        ev.type         = state.RightButton
                          ? AXL_INPUT_MOUSE_BUTTON_DOWN
                          : AXL_INPUT_MOUSE_BUTTON_UP;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.buttons      = buttons;   // current state (post-edge) — see the LEFT note
        ms->prev_right  = state.RightButton;
        if (!mouse_emit(ms, &ev)) {
            disarm_repeat(ms);
            return AXL_SOURCE_REMOVE;
        }
        if (state.RightButton) {
            arm_repeat(ms, AXL_INPUT_BUTTON_RIGHT);
        } else {
            disarm_repeat(ms);
        }
    }

    /* MOUSE_WHEEL on non-zero Z motion. */
    if (state.RelativeMovementZ != 0) {
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_MOUSE_WHEEL;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.wheel_dy     = state.RelativeMovementZ;
        if (!mouse_emit(ms, &ev)) {
            return AXL_SOURCE_REMOVE;
        }
    }

    return AXL_SOURCE_CONTINUE;
}

// Prefer a PHYSICAL pointer device over the ConSplitter console
// aggregator. LocateProtocol returns the FIRST handle publishing @p guid,
// which is ConSplitterDxe's aggregator installed early on
// gST->ConsoleInHandle (alongside SimpleTextIn/InputEx) — and that
// aggregator carries no backing device when the firmware never wired the
// pointer into ConIn (common on OVMF and real platforms). The physical
// device's own protocol instance is installed later, during BDS
// ConnectAll, so it is shadowed; WaitForInput on the aggregator then
// never signals and no pointer input reaches the consumer.
//
// Enumerate every handle publishing @p guid and take the first that is
// NOT the aggregator (the physical device). Fall back to the aggregator,
// then to LocateProtocol, when no separate physical handle exists.
//
// If several physical pointers exist (USB mouse + PS/2) this takes the first
// enumerated.
//
// NOTE: attach_mouse no longer binds via this helper — it now binds every
// SimplePointer handle ConsoleInHandle-first (collect_pointers), like
// attach_touch, so a virtual / BMC remote-console pointer published on
// ConsoleInHandle is delivered (this helper deliberately SKIPS ConsoleInHandle
// and so missed it). Retained as a utility — and called directly by the input
// regression test, which asserts the prefer-physical / skip-aggregator
// contract a single-located bind would need. Non-static (no public header) so
// the test can reach it.
void *
axl_input_locate_physical_pointer(EFI_GUID *guid)
{
    EFI_HANDLE *handles = NULL;
    uint64_t    count   = 0;
    void       *iface   = NULL;

    if (axl_bs()->LocateHandleBuffer(ByProtocol, guid, NULL, &count, &handles) == 0
        && handles != NULL) {
        for (uint64_t i = 0; i < count; i++) {
            if (handles[i] == axl_st()->ConsoleInHandle) {
                continue;   /* skip the console aggregator */
            }
            void *cand = NULL;
            if (axl_bs()->HandleProtocol(handles[i], guid, &cand) == 0 && cand != NULL) {
                iface = cand;   /* first physical device wins */
                break;
            }
        }
        if (iface == NULL && count > 0) {
            /* No separate physical handle — keep the old behaviour and
               use the first (the aggregator). */
            axl_bs()->HandleProtocol(handles[0], guid, &iface);
        }
        axl_bs()->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    }
    if (iface == NULL) {
        /* LocateHandleBuffer failed outright — last resort. */
        axl_bs()->LocateProtocol(guid, NULL, &iface);
    }
    return iface;
}

// Collect every interface publishing @p guid into @p out (cap @p max),
// **ConsoleInHandle FIRST**.  On modern firmware (UEFI >= 2.30 — all current
// servers) the BIOS multiplexes pointer devices, INCLUDING a BMC remote-console virtual
// mouse, through gST->ConsoleInHandle, and that is where live events arrive
// (verified on real hardware: event-driven WaitForInput delivers through ConIn while the
// separate physical handle stays idle).  Binding every handle + reading
// first-with-data means an idle/empty aggregator simply stays silent, so this is
// robust across firmware that routes via ConIn (a BMC remote console) AND firmware that
// exposes only a physical handle (some QEMU OVMF).  Returns the handle count.
//
// Collects HANDLES (not interface pointers): the interface is re-resolved per
// dispatch (touch_resolve), so a driver Stop()/FreePool() that invalidates the
// interface can't leave us calling through freed memory.  A handle is included
// only if it currently publishes @p guid (HandleProtocol succeeds).
static int
collect_pointers(EFI_GUID *guid, EFI_HANDLE *out, int max)
{
    int        n      = 0;
    EFI_HANDLE con_in = (axl_st() != NULL) ? axl_st()->ConsoleInHandle : NULL;
    void      *iface  = NULL;
    if (con_in != NULL
        && axl_bs()->HandleProtocol(con_in, guid, &iface) == 0 && iface != NULL) {
        out[n++] = con_in;
    }
    EFI_HANDLE *handles = NULL;
    uint64_t    count   = 0;
    if (axl_bs()->LocateHandleBuffer(ByProtocol, guid, NULL, &count, &handles) == 0
        && handles != NULL) {
        for (uint64_t i = 0; i < count && n < max; i++) {
            if (handles[i] == con_in) continue;   // ConIn already added first
            void *cand = NULL;
            if (axl_bs()->HandleProtocol(handles[i], guid, &cand) == 0
                && cand != NULL) {
                out[n++] = handles[i];
            }
        }
        axl_bs()->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    }
    return n;
}

// ---------------------------------------------------------------------------
// Diagnostic probe — see what pointer protocols a platform actually exposes.
// ---------------------------------------------------------------------------

// A bounded line sink for axl_vformat.
typedef struct { char *buf; size_t cap; size_t n; } ProbeLine;
static void
probe_line_writer(const char *data, size_t len, void *ctx)
{
    ProbeLine *s = (ProbeLine *)ctx;
    for (size_t i = 0; i < len && s->n < s->cap - 1; i++) s->buf[s->n++] = data[i];
    s->buf[s->n] = '\0';
}

// Append a formatted line to @p buf (bounded by @p cap, tracked by *@p len) AND
// print it to the console, so the report shows live and is captured to a file.
static void __attribute__((format(printf, 4, 5)))
probe_emit(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    char      line[256];
    ProbeLine ls = { line, sizeof line, 0 };
    va_list   ap;
    va_start(ap, fmt);
    axl_vformat(probe_line_writer, &ls, fmt, ap);
    va_end(ap);

    axl_print("%s", line);
    if (*len < cap - 1) {
        size_t room = cap - *len - 1;
        size_t copy = (ls.n < room) ? ls.n : room;
        for (size_t i = 0; i < copy; i++) buf[*len + i] = line[i];
        *len += copy;
        buf[*len] = '\0';
    }
}

// Report every handle publishing @p guid: which is the ConsoleInHandle
// aggregator, whether HandleProtocol succeeds, the mode, and a live GetState.
static void
probe_one_protocol(EFI_GUID *guid, const char *name, bool absolute,
                   char *buf, size_t cap, size_t *len)
{
    EFI_HANDLE *handles = NULL;
    uint64_t    count   = 0;
    EFI_HANDLE  con_in  = axl_st()->ConsoleInHandle;

    EFI_STATUS st = axl_bs()->LocateHandleBuffer(ByProtocol, guid, NULL,
                                                 &count, &handles);
    if (st != 0 || handles == NULL) {
        probe_emit(buf, cap, len, "%s: none (LocateHandleBuffer=0x%llx)\n",
                   name, (unsigned long long)st);
        return;
    }
    probe_emit(buf, cap, len, "%s: %llu handle(s)\n", name,
               (unsigned long long)count);
    for (uint64_t i = 0; i < count; i++) {
        bool  is_con_in = (handles[i] == con_in);
        void *iface     = NULL;
        EFI_STATUS hp   = axl_bs()->HandleProtocol(handles[i], guid, &iface);
        probe_emit(buf, cap, len, "  [%llu] handle=0x%llx%s HandleProtocol=%s\n",
                   (unsigned long long)i,
                   (unsigned long long)(uintptr_t)handles[i],
                   is_con_in ? " (ConsoleInHandle)" : "",
                   (hp == 0 && iface) ? "OK" : "FAIL");
        if (hp != 0 || !iface) continue;

        if (absolute) {
            EFI_ABSOLUTE_POINTER_PROTOCOL *ap =
                (EFI_ABSOLUTE_POINTER_PROTOCOL *)iface;
            EFI_ABSOLUTE_POINTER_MODE *m = ap->Mode;
            if (m) {
                probe_emit(buf, cap, len,
                           "        mode: maxX=%llu maxY=%llu attr=0x%llx\n",
                           (unsigned long long)m->AbsoluteMaxX,
                           (unsigned long long)m->AbsoluteMaxY,
                           (unsigned long long)m->Attributes);
            }
            EFI_ABSOLUTE_POINTER_STATE state;
            EFI_STATUS gs = ap->GetState(ap, &state);
            if (gs == 0) {
                probe_emit(buf, cap, len,
                           "        GetState=OK x=%llu y=%llu buttons=0x%x\n",
                           (unsigned long long)state.CurrentX,
                           (unsigned long long)state.CurrentY,
                           state.ActiveButtons);
            } else {
                probe_emit(buf, cap, len, "        GetState=0x%llx (no data)\n",
                           (unsigned long long)gs);
            }
        } else {
            EFI_SIMPLE_POINTER_PROTOCOL *sp =
                (EFI_SIMPLE_POINTER_PROTOCOL *)iface;
            EFI_SIMPLE_POINTER_MODE *m = (EFI_SIMPLE_POINTER_MODE *)sp->Mode;
            if (m) {
                probe_emit(buf, cap, len,
                           "        mode: resX=%llu resY=%llu hasL=%d hasR=%d\n",
                           (unsigned long long)m->ResolutionX,
                           (unsigned long long)m->ResolutionY,
                           (int)m->LeftButton, (int)m->RightButton);
            }
            EFI_SIMPLE_POINTER_STATE state;
            EFI_STATUS gs = sp->GetState(sp, &state);
            if (gs == 0) {
                probe_emit(buf, cap, len,
                           "        GetState=OK dx=%d dy=%d L=%d R=%d\n",
                           state.RelativeMovementX, state.RelativeMovementY,
                           (int)state.LeftButton, (int)state.RightButton);
            } else {
                probe_emit(buf, cap, len, "        GetState=0x%llx (no data)\n",
                           (unsigned long long)gs);
            }
        }
    }
    // Explicit ConsoleInHandle check (the multiplex path a BMC console uses).
    void *cin = NULL;
    EFI_STATUS chp = axl_bs()->HandleProtocol(con_in, guid, &cin);
    probe_emit(buf, cap, len, "  ConsoleInHandle %s: %s\n", name,
               (chp == 0 && cin) ? "PRESENT (multiplexed here)" : "absent");
    axl_bs()->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
}

void
axl_input_probe_pointers(const char *log_path)
{
    if (axl_bs() == NULL || axl_st() == NULL) return;

    static char buf[8192];
    size_t len = 0;
    buf[0] = '\0';

    uint32_t rev = axl_st()->Hdr.Revision;
    probe_emit(buf, sizeof buf, &len, "=== axl-input pointer probe ===\n");
    probe_emit(buf, sizeof buf, &len,
               "UEFI revision: %u.%u (0x%08x)  [>= 2.30: %s]\n",
               rev >> 16, rev & 0xFFFF, rev,
               rev >= 0x0002001E ? "yes" : "no");

    EFI_GUID sp_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_GUID ap_guid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    probe_one_protocol(&sp_guid, "EFI_SIMPLE_POINTER_PROTOCOL",   false,
                       buf, sizeof buf, &len);
    probe_one_protocol(&ap_guid, "EFI_ABSOLUTE_POINTER_PROTOCOL", true,
                       buf, sizeof buf, &len);

    // Method comparison on the ConsoleInHandle pointer(s): does the efficient
    // EVENT-DRIVEN path (CheckEvent on WaitForInput, then GetState — what
    // axl_loop_add_event relies on) actually deliver on this device, or must we
    // POLL (GetState on a timer (a poll timer))?  Two separate windows so the
    // two readers don't consume each other's data; the user moves the device in
    // both.  This tells us empirically which mechanism the fix should use.
    EFI_HANDLE con_in = axl_st()->ConsoleInHandle;
    EFI_SIMPLE_POINTER_PROTOCOL   *sp = NULL;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
    axl_bs()->HandleProtocol(con_in, &sp_guid, (void **)&sp);
    axl_bs()->HandleProtocol(con_in, &ap_guid, (void **)&ap);

    int sp_evt = 0, ap_evt = 0, sp_poll = 0, ap_poll = 0;
    const int PRINT_CAP = 6;   // lines per kind (the counts still tally all)

    // [1/2] Event-driven: wait on WaitForInput via CheckEvent, then GetState.
    probe_emit(buf, sizeof buf, &len,
               "-- [1/2] EVENT-DRIVEN test: MOVE the mouse now (3s) --\n");
    for (int t = 0; t < 60; t++) {
        if (ap && ap->WaitForInput
            && axl_bs()->CheckEvent(ap->WaitForInput) == 0) {
            EFI_ABSOLUTE_POINTER_STATE s;
            if (ap->GetState(ap, &s) == 0) {
                if (ap_evt < PRINT_CAP)
                    probe_emit(buf, sizeof buf, &len,
                               "  evt abs: x=%llu y=%llu buttons=0x%x\n",
                               (unsigned long long)s.CurrentX,
                               (unsigned long long)s.CurrentY, s.ActiveButtons);
                ap_evt++;
            }
        }
        if (sp && sp->WaitForInput
            && axl_bs()->CheckEvent(sp->WaitForInput) == 0) {
            EFI_SIMPLE_POINTER_STATE s;
            if (sp->GetState(sp, &s) == 0) {
                if (sp_evt < PRINT_CAP)
                    probe_emit(buf, sizeof buf, &len,
                               "  evt simple: dx=%d dy=%d L=%d R=%d\n",
                               s.RelativeMovementX, s.RelativeMovementY,
                               (int)s.LeftButton, (int)s.RightButton);
                sp_evt++;
            }
        }
        axl_msleep(50);
    }

    // [2/2] Polling: GetState every tick (the polling method).
    probe_emit(buf, sizeof buf, &len,
               "-- [2/2] POLLING test: MOVE the mouse now (3s) --\n");
    for (int t = 0; t < 60; t++) {
        if (ap) {
            EFI_ABSOLUTE_POINTER_STATE s;
            if (ap->GetState(ap, &s) == 0) {
                if (ap_poll < PRINT_CAP)
                    probe_emit(buf, sizeof buf, &len,
                               "  poll abs: x=%llu y=%llu buttons=0x%x\n",
                               (unsigned long long)s.CurrentX,
                               (unsigned long long)s.CurrentY, s.ActiveButtons);
                ap_poll++;
            }
        }
        if (sp) {
            EFI_SIMPLE_POINTER_STATE s;
            if (sp->GetState(sp, &s) == 0
                && (s.RelativeMovementX || s.RelativeMovementY
                    || s.LeftButton || s.RightButton)) {
                if (sp_poll < PRINT_CAP)
                    probe_emit(buf, sizeof buf, &len,
                               "  poll simple: dx=%d dy=%d L=%d R=%d\n",
                               s.RelativeMovementX, s.RelativeMovementY,
                               (int)s.LeftButton, (int)s.RightButton);
                sp_poll++;
            }
        }
        axl_msleep(50);
    }

    probe_emit(buf, sizeof buf, &len,
               "RESULT absolute: event-driven=%d polling=%d\n", ap_evt, ap_poll);
    probe_emit(buf, sizeof buf, &len,
               "RESULT simple:   event-driven=%d polling=%d\n", sp_evt, sp_poll);
    bool any_evt  = (ap_evt > 0 || sp_evt > 0);
    bool any_poll = (ap_poll > 0 || sp_poll > 0);
    probe_emit(buf, sizeof buf, &len, "  -> %s\n",
               any_evt  ? "EVENT-DRIVEN works (WaitForInput) - the efficient path"
             : any_poll ? "event-driven DEAD; POLLING works - use a poll timer"
                        : "no events seen (move during BOTH windows, then re-run)");
    probe_emit(buf, sizeof buf, &len, "=== end probe ===\n");

    if (log_path != NULL && len > 0) {
        /* The probe's findings are already on screen; the file is a copy the
           caller asked for. Nothing here can retry or roll back, and this
           routine reports through the console rather than a return value --
           so the status is CHECKED and turned into a notice, not discarded.
           An operator who asked for a log and is about to go read it needs
           to know it is not there (a failed flush now surfaces here, where
           axl_file_set_contents used to report success for a file that
           never reached the volume). */
        if (axl_file_set_contents(log_path, buf, len) != AXL_OK) {
            /* Console, not probe_emit: appending to the buffer whose write
               just failed would report the failure only into the file that
               does not exist. */
            axl_print("NOTE: could not write the probe log to '%s'\n",
                      log_path);
        }
    }
}

// Shared tail of both attach paths: mouse_state.nproto + the bound handles/ifaces
// (+ .cached) must already be set. Inits per-session state, resets each device, and
// registers a WaitForInput loop source per interface. Returns the first source id
// (non-zero == success), 0 if none exposed a usable WaitForInput.
static AxlSourceId
mouse_attach_finish(AxlLoop *loop, AxlInputCallback cb, void *data)
{
    mouse_state.cb            = cb;
    mouse_state.data          = data;
    mouse_state.cursor_x      = 0;
    mouse_state.cursor_y      = 0;
    mouse_state.prev_left     = false;
    mouse_state.prev_right    = false;
    mouse_state.gesture       = (AxlGesture){0};
    mouse_state.loop          = loop;
    mouse_state.nsrc          = 0;
    mouse_state.repeat_src    = 0;
    mouse_state.repeat_button = 0;
    mouse_state_used          = true;

    /* Reset each device + register a WaitForInput source per interface. */
    for (int i = 0; i < mouse_state.nproto
                    && mouse_state.nsrc < AXL_MAX_POINTER_IFACES; i++) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = mouse_iface_at(&mouse_state, i);
        if (sp == NULL) {
            continue;
        }
        sp->Reset(sp, false);   /* best-effort; first GetState may be NOT_READY */
        if (sp->WaitForInput != NULL) {
            AxlSourceId sid = axl_loop_add_event(loop, sp->WaitForInput,
                                                 mouse_dispatch_cb, &mouse_state);
            if (sid != 0) {
                mouse_state.source_ids[mouse_state.nsrc++] = sid;
            }
        }
    }
    if (mouse_state.nsrc == 0) {
        /* No interface exposed a usable WaitForInput — nothing to dispatch on. */
        mouse_state_used = false;
        return 0;
    }
    /* Return the first source id as the attach handle (non-zero == success);
       detach removes every registered source. */
    return mouse_state.source_ids[0];
}

AxlSourceId
axl_input_attach_mouse(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    )
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }
    if (mouse_state_used) {
        axl_warning("axl_input_attach_mouse: already attached "
                    "(only one mouse source per process for v0.1)");
        return 0;
    }

    /* Bind every SimplePointer handle, ConsoleInHandle FIRST — where a virtual
       pointer and a BMC remote-console mouse publish, and where the live events
       arrive on modern firmware that multiplexes pointers through ConIn.  A
       separate physical handle (some QEMU OVMF) is bound too, so an idle/empty
       aggregator simply stays silent and the physical device still delivers.
       This is the same model attach_touch uses (collect_pointers); the old
       single-interface locate_physical_pointer bind SKIPPED ConsoleInHandle and
       so never saw a virtual pointer. */
    mouse_state.nproto = collect_pointers(&EFI_SIMPLE_POINTER_PROTOCOL_GUID,
                                          mouse_state.handles,
                                          AXL_MAX_POINTER_IFACES);
    if (mouse_state.nproto == 0) {
        axl_debug("EFI_SIMPLE_POINTER_PROTOCOL not available "
                  "(headless / no mouse hardware)");
        return 0;
    }

    /* collect_pointers lists ConsoleInHandle FIRST (a virtual / BMC pointer
       publishes there).  For the mouse we must READ it LAST: when the pointer
       is routed through the ConSplitter aggregator on ConsoleInHandle, its
       GetState consumes the physical child's state but drops the wheel
       (RelativeMovementZ) — so reading it first eats every scroll notch.  Move
       ConsoleInHandle to the end so mouse_dispatch_cb reads physical devices
       (which carry the wheel) first and the aggregator/virtual pointer last.
       attach_touch keeps ConsoleInHandle first — the AbsolutePointer aggregator
       does not have this wheel-dropping issue. */
    EFI_HANDLE con_in = (axl_st() != NULL) ? axl_st()->ConsoleInHandle : NULL;
    if (con_in != NULL && mouse_state.nproto > 1
        && mouse_state.handles[0] == con_in) {
        for (int i = 0; i < mouse_state.nproto - 1; i++) {
            mouse_state.handles[i] = mouse_state.handles[i + 1];
        }
        mouse_state.handles[mouse_state.nproto - 1] = con_in;
    }

    mouse_state.cached = false;
    return mouse_attach_finish(loop, cb, data);
}

AxlSourceId
axl_input_attach_mouse_ifaces(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data,
    void *const       *ifaces,
    int                n
    )
{
    if (loop == NULL || cb == NULL || ifaces == NULL || n <= 0) {
        return 0;
    }
    if (mouse_state_used) {
        axl_warning("axl_input_attach_mouse_ifaces: already attached "
                    "(only one mouse source per process for v0.1)");
        return 0;
    }

    /* Bind the caller-supplied interfaces directly (they are no longer in the
       handle database, so mouse_resolve cannot find them). No ConsoleInHandle
       reorder: the caller (e.g. axl_console_device) supplies the pointers it
       evicted, already in producer order. */
    mouse_state.nproto = 0;
    for (int i = 0; i < n && mouse_state.nproto < AXL_MAX_POINTER_IFACES; i++) {
        if (ifaces[i] != NULL) {
            mouse_state.ifaces[mouse_state.nproto++] =
                (EFI_SIMPLE_POINTER_PROTOCOL *)ifaces[i];
        }
    }
    if (mouse_state.nproto == 0) {
        return 0;   /* all interfaces NULL — nothing to bind */
    }
    mouse_state.cached = true;
    return mouse_attach_finish(loop, cb, data);
}

void
axl_input_detach_mouse(AxlLoop *loop)
{
    if (!mouse_state_used) {
        return;
    }
    disarm_repeat(&mouse_state);
    for (int i = 0; i < mouse_state.nsrc; i++) {
        axl_loop_remove_source(loop, mouse_state.source_ids[i]);
    }
    mouse_state.nsrc = 0;
    mouse_state_used = false;
}

// ===================================================================
// Keyboard — thin wrapper over axl_loop_add_key_press
// ===================================================================
//
// axl_loop_add_key_press reads ConIn via SimpleTextInputEx when the
// firmware publishes it (falling back to the basic ReadKeyStroke), and
// hands us an AxlInputKey with scan_code + unicode_char + normalized
// modifiers.  Our wrapper translates that into the unified
// AxlInputEvent shape so callers register one AxlInputCallback for
// all input kinds.
//
// modifiers (Shift/Ctrl/Alt/Meta + caps/num/scroll lock) arrive
// populated from the Ex protocol's KeyShiftState/KeyToggleState, and
// are 0 when there is no Ex protocol (e.g. a serial console). KEY_UP
// is not emitted — UEFI delivers no key-up events. See the Ctrl+letter
// encoding note in <axl/axl-input.h> (keyboard: letter + MOD_CTRL;
// serial: folded C0 control code).

typedef struct {
    AxlInputCallback  cb;
    void             *data;
    AxlKeyDebounce    debounce;   ///< repeat-suppression state (opt-in)
    AxlSourceId       source_id;  ///< loop source id, for axl_input_detach_key
} KeySource;

static KeySource key_state;
static bool      key_state_used = false;

static bool
key_dispatch_cb(
    AxlInputKey  key,
    void        *data
    )
{
    KeySource     *ks = (KeySource *)data;
    /* Update the live modifier state from every keystroke. A modifier-only
       "partial" keystroke (EFI_KEY_STATE_EXPOSED) has both scan code and
       unicode == 0, so it only refreshes the state — don't deliver a
       KEY_DOWN for it (it would look like a phantom no-char key). */
    if (!axl_input_track_modifiers(&key)) {
        return AXL_SOURCE_CONTINUE;
    }
    AxlInputEvent  ev = {0};
    ev.type         = AXL_INPUT_KEY_DOWN;
    ev.timestamp_us = axl_time_get_us();
    ev.keycode      = key.scan_code;
    ev.unicode      = key.unicode_char;
    ev.modifiers    = key.modifiers;  /* AXL_INPUT_MOD_* (0 if no ConIn-Ex) */
    /* Drop too-fast same-key repeats when debounce is enabled (remote
       consoles); keep the source alive. No-op when disabled (default). */
    if (!axl_input_key_accept(&ks->debounce, &ev)) {
        return AXL_SOURCE_CONTINUE;
    }
    return ks->cb(&ev, ks->data);
}

AxlSourceId
axl_input_attach_key(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    )
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }
    if (key_state_used) {
        axl_warning("axl_input_attach_key: already attached "
                    "(only one keyboard source per process for v0.1)");
        return 0;
    }
    key_state.cb       = cb;
    key_state.data     = data;
    key_state.debounce = (AxlKeyDebounce){0};
    key_state_used     = true;
    g_kbd_modifiers    = 0;   /* fresh keyboard session — no held modifiers yet */
    /* Ask the firmware for modifier-only partial keystrokes so the live
       modifier state stays current between character keys (Shift+wheel,
       Ctrl+click). Best-effort; no-op without an Ex protocol. */
    axl_backend_console_expose_modifiers();
    key_state.source_id = axl_loop_add_key_press(loop, key_dispatch_cb,
                                                 &key_state);
    return key_state.source_id;
}

void
axl_input_detach_key(AxlLoop *loop)
{
    if (!key_state_used) {
        return;
    }
    if (key_state.source_id != 0) {
        axl_loop_remove_source(loop, key_state.source_id);
        key_state.source_id = 0;
    }
    key_state_used = false;
}

// ===================================================================
// Touch — EFI_ABSOLUTE_POINTER_PROTOCOL
// ===================================================================
//
// Absolute pointer reports (CurrentX, CurrentY) in the device's native
// EFI_ABSOLUTE_POINTER_MODE AbsoluteMin/Max range; we normalize each event
// to [0, AXL_INPUT_ABS_RANGE) (axl_input_abs_normalize) so the value is
// display-independent — the consumer maps it onto its own surface (axl-input
// stays a sibling of axl-gfx, never learning the screen resolution).
// ActiveButtons is a bitfield: bit 0 = touch contact, higher bits
// device-specific (alt-button on stylus, etc.).
//
// Emit semantics:
//   TOUCH_DOWN — first dispatch where ActiveButtons != 0 after being 0.
//   TOUCH_UP   — transition from non-zero ActiveButtons back to 0.
//   TOUCH_MOVE — whenever (CurrentX, CurrentY) changes, INCLUDING with no
//                button held (hover): a pen / tablet / VNC-tablet reports
//                position continuously, so it drives a pointer.  `buttons`
//                carries the live ActiveButtons (0 = hover, non-zero = drag).

// Fallback poll interval (ms) — used ONLY on firmware whose WaitForInput never
// signals; when the event path works (the common case, e.g. a BMC remote console) the
// poll just finds "no data" cheaply (GetState consumes, so no double-emit).
#define TOUCH_POLL_MS 30

typedef struct {
    AxlInputCallback                cb;
    void                           *data;
    // Bind by HANDLE, not by a cached interface pointer: the providing driver
    // (e.g. DellUsbMouseAbsolutePointerDxe) can be Stop()'d on USB re-enum /
    // console reconnect — common over a BMC remote console — which uninstalls
    // AND FreePool()s its EFI_ABSOLUTE_POINTER_PROTOCOL.  A cached interface
    // would then dangle and the next GetState() would call through freed memory
    // (a #GP).  We re-resolve the handle to its CURRENT interface every dispatch
    // (HandleProtocol fails safely if the protocol is gone), and re-bind on
    // (re)install via a protocol-notify source.
    EFI_HANDLE                      handles[AXL_MAX_POINTER_IFACES]; ///< ConIn-first
    int                             nproto;
    int32_t                         last_x;         ///< native coords for change-detect
    int32_t                         last_y;
    bool                            contact_active;
    AxlLoop                        *loop;
    AxlSourceId                     source_ids[AXL_MAX_POINTER_IFACES]; ///< per-handle WaitForInput sources
    int                             nsrc;
    AxlSourceId                     poll_src;       ///< fallback poll timer (0 = none)
    AxlSourceId                     notify_src;     ///< protocol-notify source: re-bind on (re)install (0 = none)
} TouchSource;

static TouchSource touch_state;
static bool        touch_state_used = false;

// Tunable touch-read config (axl_input_set_touch_config). Default: ConsoleIn-
// only, event sources + poll fallback, 30 ms. ConsoleIn-only is the default
// because on the firmware this matters for — UEFI >= 2.30 that multiplexes
// pointers (a BMC remote-console virtual mouse) through gST->ConsoleInHandle —
// that one handle is where the live events arrive, and binding ONLY it avoids
// double events from a separate physical handle that mirrors the same device.
// A platform whose absolute pointer is published ONLY on a separate physical
// handle (not multiplexed through ConIn) must opt back into all-handles via
// axl_input_set_touch_config(.., console_only=false, ..).
static AxlInputTouchMethod g_touch_method       = AXL_INPUT_TOUCH_EVENT_AND_POLL;
static bool                g_touch_console_only = true;
static uint32_t            g_touch_poll_ms      = TOUCH_POLL_MS;
// Max queued states a single read drains, coalescing to the latest position.
// 1 = legacy single-read (default).  Higher collapses a firmware backlog so a
// slow poll catches up (see axl_input_set_touch_drain).
static uint32_t            g_touch_max_drain    = 1;

void
axl_input_set_touch_config(AxlInputTouchMethod method, bool console_only,
                           uint32_t poll_ms)
{
    g_touch_method       = method;
    g_touch_console_only = console_only;
    g_touch_poll_ms      = (poll_ms == 0) ? TOUCH_POLL_MS : poll_ms;
}

void
axl_input_set_touch_drain(uint32_t max_states)
{
    g_touch_max_drain = (max_states == 0) ? 1 : max_states;
}

// Map a native absolute coord into [0, AXL_INPUT_ABS_RANGE) using the
// device's reported min/max.  Keeps axl-input display-agnostic (no GOP /
// axl-gfx dependency) — the consumer maps the normalized value onto its
// own surface.  A degenerate range (max <= min) yields 0.
int32_t
axl_input_abs_normalize(int64_t v, uint64_t lo, uint64_t hi)
{
    if (hi <= lo) {
        return 0;
    }
    if (v < (int64_t)lo) v = (int64_t)lo;
    if (v > (int64_t)hi) v = (int64_t)hi;
    return (int32_t)(((uint64_t)(v - (int64_t)lo) * (AXL_INPUT_ABS_RANGE - 1u))
                     / (hi - lo));
}

// Resolve a bound handle to its CURRENT absolute-pointer interface, or NULL if
// the protocol is gone (the providing driver was Stop()'d, the handle was
// destroyed, etc.).  HandleProtocol validates the handle against the firmware
// handle database, so a stale handle returns an error rather than faulting —
// this is what makes the per-dispatch call safe across driver teardown.
// (EFI_ABSOLUTE_POINTER_PROTOCOL_GUID is a file-scope variable from the
// generated GUID header; HandleProtocol takes a mutable EFI_GUID *.)
static EFI_ABSOLUTE_POINTER_PROTOCOL *
touch_resolve(EFI_HANDLE handle)
{
    void *iface = NULL;
    if (handle != NULL
        && axl_bs()->HandleProtocol(handle, &EFI_ABSOLUTE_POINTER_PROTOCOL_GUID,
                                    &iface) == 0) {
        return (EFI_ABSOLUTE_POINTER_PROTOCOL *)iface;
    }
    return NULL;
}

// Pulls the next queued absolute-pointer state into *out, returning true if
// one was available (false = every bound handle's queue is empty / gone).
// Decoupled from the firmware so the coalescing policy can be unit-tested
// with a scripted state sequence (see axl_input_touch_coalesce).
typedef bool (*AxlAbsReader)(void *ctx, EFI_ABSOLUTE_POINTER_STATE *out);

// Drain up to @max_drain queued states via @read, coalescing a pure-motion
// backlog to the latest position so a FIFO-queuing firmware (a BMC virtual
// mouse) can't lag seconds behind — BUT stopping at the first state whose
// contact-active differs from @baseline_active.  That edge state is kept as
// the chosen one and processed this dispatch; the rest of the backlog drains
// on later dispatches.  This is what keeps a press/release from being
// coalesced away: motion compresses, button transitions never do.  With
// @max_drain == 1 this reads exactly one state (legacy behavior).
//
// Returns the number of states read; 0 leaves *out untouched.  On a non-zero
// return *out is the chosen state.
uint32_t
axl_input_touch_coalesce(
    AxlAbsReader                read,
    void                       *ctx,
    bool                        baseline_active,
    uint32_t                    max_drain,
    EFI_ABSOLUTE_POINTER_STATE *out
    )
{
    uint32_t n = 0;
    for (uint32_t rd = 0; rd < max_drain; rd++) {
        EFI_ABSOLUTE_POINTER_STATE s;
        if (!read(ctx, &s)) {
            break;   // every handle's queue is empty (or gone) — nothing to drain
        }
        *out = s;
        n++;
        if ((s.ActiveButtons != 0) != baseline_active) {
            break;   // contact transition — keep it as the chosen state and stop,
                     // so a press/release is never coalesced away (the rest of the
                     // backlog drains on later dispatches)
        }
    }
    return n;
}

// Reader ctx for the live firmware path: the bound handles plus the protocol
// + mode of the most recent successful read (needed to normalize the chosen
// state's coordinates).
typedef struct {
    TouchSource                   *tch;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap;
    EFI_ABSOLUTE_POINTER_MODE     *mode;
} TouchReadCtx;

static bool
touch_read_next(void *vctx, EFI_ABSOLUTE_POINTER_STATE *out)
{
    TouchReadCtx *ctx = (TouchReadCtx *)vctx;
    for (int i = 0; i < ctx->tch->nproto; i++) {
        // Re-resolve to the CURRENT interface every read — never call through
        // a cached pointer the providing driver may have freed.
        EFI_ABSOLUTE_POINTER_PROTOCOL *p = touch_resolve(ctx->tch->handles[i]);
        if (p == NULL) {
            continue;   // protocol gone (driver Stop()'d) — skip, no stale call
        }
        if (p->GetState(p, out) == 0) {
            ctx->ap   = p;
            ctx->mode = p->Mode;
            return true;
        }
    }
    return false;
}

static bool
touch_dispatch_cb(
    void  *data
    )
{
    TouchSource                *tch = (TouchSource *)data;
    /* Zero-init: the `ap != NULL` guard below proves `state` was assigned
       before use, but -O2 can't see the correlation and warns
       -Wmaybe-uninitialized; the initializer silences the false positive. */
    EFI_ABSOLUTE_POINTER_STATE  state = {0};

    // Read whichever bound handle has data FIRST (ConsoleInHandle first).
    // GetState consumes one queued state, so by default we read exactly one
    // per dispatch.  When drain coalescing is enabled (g_touch_max_drain > 1)
    // axl_input_touch_coalesce collapses a motion backlog to the live position
    // while stopping at any button transition so clicks survive.
    TouchReadCtx rctx = { .tch = tch, .ap = NULL, .mode = NULL };
    uint32_t got = axl_input_touch_coalesce(touch_read_next, &rctx,
                                            tch->contact_active,
                                            g_touch_max_drain, &state);
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap   = rctx.ap;
    EFI_ABSOLUTE_POINTER_MODE     *mode = rctx.mode;
    if (got == 0 || ap == NULL || mode == NULL) {
        return AXL_SOURCE_CONTINUE;   /* no data on any handle (Mode: be defensive) */
    }

    uint64_t now_us = axl_time_get_us();
    int32_t  cx     = (int32_t)state.CurrentX;   /* native device coords */
    int32_t  cy     = (int32_t)state.CurrentY;
    bool     active = (state.ActiveButtons != 0);
    /* Normalized [0, AXL_INPUT_ABS_RANGE) coords for the event payload;
       last_x/last_y stay NATIVE for change detection. */
    int32_t  nx = axl_input_abs_normalize(cx, mode->AbsoluteMinX, mode->AbsoluteMaxX);
    int32_t  ny = axl_input_abs_normalize(cy, mode->AbsoluteMinY, mode->AbsoluteMaxY);

    if (active && !tch->contact_active) {
        /* Transition 0 → contact: emit TOUCH_DOWN. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_DOWN;
        ev.timestamp_us = now_us;
        ev.x            = nx;
        ev.y            = ny;
        ev.buttons      = state.ActiveButtons;
        ev.modifiers    = g_kbd_modifiers;
        tch->contact_active = true;
        tch->last_x = cx;
        tch->last_y = cy;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    if (!active && tch->contact_active) {
        /* Transition contact → 0: emit TOUCH_UP at the last position. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_UP;
        ev.timestamp_us = now_us;
        ev.x            = axl_input_abs_normalize(tch->last_x, mode->AbsoluteMinX, mode->AbsoluteMaxX);
        ev.y            = axl_input_abs_normalize(tch->last_y, mode->AbsoluteMinY, mode->AbsoluteMaxY);
        ev.buttons      = 0;
        ev.modifiers    = g_kbd_modifiers;
        tch->contact_active = false;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    if (cx != tch->last_x || cy != tch->last_y) {
        /* Position changed — a contact drag OR a no-button hover (pen /
           tablet / VNC tablet reports position continuously).  Emit
           TOUCH_MOVE either way so an absolute pointer drives a cursor;
           buttons carries the live ActiveButtons (0 = hover). */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_MOVE;
        ev.timestamp_us = now_us;
        ev.x            = nx;
        ev.y            = ny;
        ev.buttons      = state.ActiveButtons;
        ev.modifiers    = g_kbd_modifiers;
        tch->last_x = cx;
        tch->last_y = cy;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    /* No state change worth reporting (position unchanged, no contact
       transition). */
    return AXL_SOURCE_CONTINUE;
}

// (Re)bind the absolute-pointer handle set into touch_state: drop any existing
// WaitForInput event sources (the firmware closes those events when a driver
// stops, so they must not linger), re-enumerate the current handles
// (ConsoleInHandle first, or only ConsoleInHandle in console-only mode), Reset
// each device, and — outside POLL_ONLY mode — register a fresh WaitForInput
// source per handle.  Used for the initial bind (attach) AND to re-bind after a
// device (re)install (touch_notify_cb), so the cursor survives the USB re-enum /
// console reconnect that frees the old interface.
static void
touch_rebind_(TouchSource *tch)
{
    for (int i = 0; i < tch->nsrc; i++) {
        axl_loop_remove_source(tch->loop, tch->source_ids[i]);
    }
    tch->nsrc = 0;

    if (g_touch_console_only) {
        EFI_HANDLE con_in = (axl_st() != NULL) ? axl_st()->ConsoleInHandle : NULL;
        tch->nproto = (con_in != NULL && touch_resolve(con_in) != NULL)
                      ? (tch->handles[0] = con_in, 1) : 0;
    } else {
        tch->nproto = collect_pointers(&EFI_ABSOLUTE_POINTER_PROTOCOL_GUID,
                                       tch->handles, AXL_MAX_POINTER_IFACES);
    }

    if (g_touch_method == AXL_INPUT_TOUCH_POLL_ONLY) {
        // Poll-only still Resets the devices, but registers no event sources.
        for (int i = 0; i < tch->nproto; i++) {
            EFI_ABSOLUTE_POINTER_PROTOCOL *ap = touch_resolve(tch->handles[i]);
            if (ap != NULL) { ap->Reset(ap, false); }
        }
        return;
    }
    for (int i = 0; i < tch->nproto && tch->nsrc < AXL_MAX_POINTER_IFACES; i++) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = touch_resolve(tch->handles[i]);
        if (ap == NULL) {
            continue;
        }
        ap->Reset(ap, false);   // best-effort
        if (ap->WaitForInput != NULL) {
            AxlSourceId sid = axl_loop_add_event(tch->loop, ap->WaitForInput,
                                              touch_dispatch_cb, tch);
            if (sid != 0) {
                tch->source_ids[tch->nsrc++] = sid;
            }
        }
    }
}

// Protocol-notify: an absolute pointer was (re)installed — e.g. the USB mouse
// re-enumerated and its absolute-pointer driver re-attached after a Stop().
// Re-bind to the fresh handle/interface set so the cursor keeps working.
static bool
touch_notify_cb(void *data)
{
    touch_rebind_((TouchSource *)data);
    return AXL_SOURCE_CONTINUE;   // keep watching for further (re)installs
}

AxlSourceId
axl_input_attach_touch(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    )
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }
    if (touch_state_used) {
        axl_warning("axl_input_attach_touch: already attached "
                    "(only one touch source per process for v0.1)");
        return 0;
    }

    touch_state.cb             = cb;
    touch_state.data           = data;
    touch_state.last_x         = 0;
    touch_state.last_y         = 0;
    touch_state.contact_active = false;
    touch_state.loop           = loop;
    touch_state.nproto         = 0;
    touch_state.nsrc           = 0;
    touch_state.poll_src       = 0;
    touch_state.notify_src     = 0;
    touch_state_used           = true;

    // Bind the absolute-pointer handle(s), ConsoleInHandle first (where a BMC
    // remote-console virtual mouse's events arrive on modern firmware): Reset
    // each + (event mode) register a WaitForInput source.  Tunable via
    // axl_input_set_touch_config (all handles vs ConsoleInHandle only, method).
    touch_rebind_(&touch_state);
    if (touch_state.nproto == 0) {
        axl_debug("EFI_ABSOLUTE_POINTER_PROTOCOL not available "
                  "(no touch / digitizer / remote-console pointer)");
        touch_state_used = false;
        return 0;
    }

    // Fallback poll for firmware whose WaitForInput never signals.  Skipped in
    // EVENT_ONLY mode; never double-counts (GetState consumes — see
    // touch_dispatch_cb).
    if (g_touch_method != AXL_INPUT_TOUCH_EVENT_ONLY) {
        touch_state.poll_src = axl_loop_add_timer(loop, g_touch_poll_ms,
                                                  touch_dispatch_cb, &touch_state);
    }

    // Re-bind when an absolute pointer is (re)installed — USB re-enumeration /
    // console reconnect over a BMC console Stop()s the providing driver, which
    // frees its interface; without this the cursor would silently die after the
    // device re-appears (and the per-dispatch re-resolve would only ever find a
    // dead handle).  touch_notify_cb re-runs touch_rebind_ on each (re)install.
    touch_state.notify_src = axl_loop_add_protocol_notify(
        loop, &EFI_ABSOLUTE_POINTER_PROTOCOL_GUID, touch_notify_cb, &touch_state);

    // Non-zero on success (first event source, else the poll timer, else the
    // notify source) for the caller's "did it attach" check;
    // axl_input_detach_touch() tears down all.
    if (touch_state.nsrc > 0)       return touch_state.source_ids[0];
    if (touch_state.poll_src != 0)  return touch_state.poll_src;
    return touch_state.notify_src;
}

void
axl_input_detach_touch(AxlLoop *loop)
{
    if (!touch_state_used) {
        return;
    }
    for (int i = 0; i < touch_state.nsrc; i++) {
        axl_loop_remove_source(loop, touch_state.source_ids[i]);
    }
    touch_state.nsrc = 0;
    if (touch_state.poll_src != 0) {
        axl_loop_remove_source(loop, touch_state.poll_src);
        touch_state.poll_src = 0;
    }
    if (touch_state.notify_src != 0) {
        // Owns its EFI_EVENT — remove_source closes it (axl_loop_add_protocol_notify).
        axl_loop_remove_source(loop, touch_state.notify_src);
        touch_state.notify_src = 0;
    }
    touch_state_used = false;
}
