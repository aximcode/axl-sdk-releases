/** @file axl-test-input.c
    Sanity tests for the axl-input event-type surface.

    Phase 0g ships only types (AxlInputType, AxlInputEvent, button +
    modifier bitfields, AxlInputCallback).  Source-registration tests
    land with Phase 0h (mouse) / 0i (keyboard wrapper) where the
    actual UEFI protocol integration happens.
**/

#include "axl-test.h"

#include <axl/axl-input.h>
#include <axl/axl-loop.h>
#include <axl/axl-event.h>   /* AxlEvent for the mock's WaitForInput */
#include <uefi/axl-uefi.h>   /* gBS / gST / EFI_SIMPLE_POINTER_* for mock install */

// ---------------------------------------------------------------------------
// Event type discriminator sanity
// ---------------------------------------------------------------------------

static void
test_input_type_values_distinct(void)
{
    /* AXL_INPUT_NONE is the sentinel — never used as a real event. */
    test_check(AXL_INPUT_NONE == 0,
               "AXL_INPUT_NONE == 0 (sentinel)");
    /* Real event kinds are all non-zero and distinct. */
    test_check(AXL_INPUT_MOUSE_MOVE        != AXL_INPUT_NONE &&
               AXL_INPUT_MOUSE_BUTTON_DOWN != AXL_INPUT_MOUSE_MOVE &&
               AXL_INPUT_KEY_DOWN          != AXL_INPUT_KEY_UP    &&
               AXL_INPUT_TOUCH_MOVE        != AXL_INPUT_MOUSE_MOVE,
               "event type values are distinct");
}

// ---------------------------------------------------------------------------
// Button + modifier bitfields don't overlap within their own set
// ---------------------------------------------------------------------------

static void
test_button_bits_distinct(void)
{
    test_check((AXL_INPUT_BUTTON_LEFT   & AXL_INPUT_BUTTON_RIGHT)  == 0 &&
               (AXL_INPUT_BUTTON_LEFT   & AXL_INPUT_BUTTON_MIDDLE) == 0 &&
               (AXL_INPUT_BUTTON_RIGHT  & AXL_INPUT_BUTTON_MIDDLE) == 0,
               "button bitfield: left/right/middle are pairwise disjoint");
    /* Combining is associative bitwise OR — sanity check a combined state. */
    uint32_t combined = AXL_INPUT_BUTTON_LEFT | AXL_INPUT_BUTTON_RIGHT;
    test_check((combined & AXL_INPUT_BUTTON_LEFT)  != 0 &&
               (combined & AXL_INPUT_BUTTON_RIGHT) != 0 &&
               (combined & AXL_INPUT_BUTTON_MIDDLE) == 0,
               "button bitfield: combined state masks correctly");
}

static void
test_modifier_bits_distinct(void)
{
    /* Side-agnostic masks remain pairwise disjoint. */
    test_check((AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_CTRL) == 0 &&
               (AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_ALT)  == 0 &&
               (AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_META) == 0 &&
               (AXL_INPUT_MOD_CTRL  & AXL_INPUT_MOD_ALT)  == 0 &&
               (AXL_INPUT_MOD_CTRL  & AXL_INPUT_MOD_META) == 0 &&
               (AXL_INPUT_MOD_ALT   & AXL_INPUT_MOD_META) == 0,
               "modifier bitfield: shift/ctrl/alt/meta pairwise disjoint");
}

static void
test_modifier_lr_and_masks(void)
{
    /* Each L/R bit is a distinct single bit. */
    uint32_t lr[] = {
        AXL_INPUT_MOD_LSHIFT, AXL_INPUT_MOD_RSHIFT,
        AXL_INPUT_MOD_LCTRL,  AXL_INPUT_MOD_RCTRL,
        AXL_INPUT_MOD_LALT,   AXL_INPUT_MOD_RALT,
        AXL_INPUT_MOD_LMETA,  AXL_INPUT_MOD_RMETA,
        AXL_INPUT_MOD_CAPS_LOCK, AXL_INPUT_MOD_NUM_LOCK,
        AXL_INPUT_MOD_SCROLL_LOCK,
    };
    uint32_t seen = 0;
    bool all_single = true, all_distinct = true;
    for (size_t i = 0; i < sizeof lr / sizeof lr[0]; i++) {
        if (lr[i] == 0 || (lr[i] & (lr[i] - 1)) != 0) {
            all_single = false;   /* not a single power-of-two bit */
        }
        if (seen & lr[i]) {
            all_distinct = false;
        }
        seen |= lr[i];
    }
    test_check(all_single, "modifier: every L/R + lock bit is a single bit");
    test_check(all_distinct, "modifier: L/R + lock bits mutually disjoint");

    /* Side-agnostic masks are exactly the OR of their two sides. */
    test_check(AXL_INPUT_MOD_SHIFT ==
                   (AXL_INPUT_MOD_LSHIFT | AXL_INPUT_MOD_RSHIFT) &&
               AXL_INPUT_MOD_CTRL ==
                   (AXL_INPUT_MOD_LCTRL | AXL_INPUT_MOD_RCTRL) &&
               AXL_INPUT_MOD_ALT ==
                   (AXL_INPUT_MOD_LALT | AXL_INPUT_MOD_RALT) &&
               AXL_INPUT_MOD_META ==
                   (AXL_INPUT_MOD_LMETA | AXL_INPUT_MOD_RMETA),
               "modifier: SHIFT/CTRL/ALT/META masks = OR of L/R sides");

    /* A mask matches its own side but not a different modifier. */
    test_check((AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_LSHIFT) != 0 &&
               (AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_RSHIFT) != 0 &&
               (AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_LCTRL)  == 0,
               "modifier: SHIFT mask matches either shift, not ctrl");

    /* Locks are disjoint from the held-modifier masks. */
    uint32_t held = AXL_INPUT_MOD_SHIFT | AXL_INPUT_MOD_CTRL |
                    AXL_INPUT_MOD_ALT | AXL_INPUT_MOD_META;
    uint32_t locks = AXL_INPUT_MOD_CAPS_LOCK | AXL_INPUT_MOD_NUM_LOCK |
                     AXL_INPUT_MOD_SCROLL_LOCK;
    test_check((held & locks) == 0,
               "modifier: lock bits disjoint from held-modifier bits");
}

// ---------------------------------------------------------------------------
// AxlInputEvent struct layout sanity
// ---------------------------------------------------------------------------

static void
test_event_struct_field_init(void)
{
    /* Zero-init must leave .type == AXL_INPUT_NONE (sentinel). */
    AxlInputEvent ev = {0};
    test_check(ev.type == AXL_INPUT_NONE,
               "AxlInputEvent: zero-init yields AXL_INPUT_NONE type");
    /* All scalar fields zero. */
    test_check(ev.timestamp_us == 0 && ev.x == 0 && ev.y == 0 &&
               ev.buttons == 0 && ev.wheel_dx == 0 && ev.wheel_dy == 0 &&
               ev.keycode == 0 && ev.unicode == 0 && ev.modifiers == 0,
               "AxlInputEvent: zero-init yields all-zero fields");
}

static void
test_event_designated_init(void)
{
    /* Designated initializer round-trip — pin field names so a
       struct-rename regression surfaces. */
    AxlInputEvent ev = {
        .type = AXL_INPUT_MOUSE_BUTTON_DOWN,
        .timestamp_us = 12345,
        .x = 100,
        .y = 200,
        .buttons = AXL_INPUT_BUTTON_LEFT | AXL_INPUT_BUTTON_MIDDLE,
        .modifiers = AXL_INPUT_MOD_SHIFT,
    };
    test_check(ev.type == AXL_INPUT_MOUSE_BUTTON_DOWN,
               "AxlInputEvent designated init: type field");
    test_check(ev.timestamp_us == 12345,
               "AxlInputEvent designated init: timestamp_us field");
    test_check(ev.x == 100 && ev.y == 200,
               "AxlInputEvent designated init: x/y fields");
    test_check(ev.buttons == 0x05,
               "AxlInputEvent designated init: buttons LEFT|MIDDLE = 0x05");
    test_check(ev.modifiers == AXL_INPUT_MOD_SHIFT,
               "AxlInputEvent designated init: modifiers field");
}

// ---------------------------------------------------------------------------
// axl_input_attach_mouse — registration error paths
// ---------------------------------------------------------------------------

static bool
unused_cb(const AxlInputEvent *ev, void *data)
{
    (void)ev; (void)data;
    return AXL_SOURCE_CONTINUE;
}

static void
test_attach_mouse_null_loop_returns_zero(void)
{
    test_check(axl_input_attach_mouse(NULL, unused_cb, NULL) == 0,
               "attach_mouse: NULL loop returns 0");
}

static void
test_attach_mouse_null_cb_returns_zero(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "attach_mouse fixture: axl_loop_new succeeds");
    test_check(axl_input_attach_mouse(loop, NULL, NULL) == 0,
               "attach_mouse: NULL cb returns 0");
    axl_loop_free(loop);
}

// --- Mock SIMPLE_POINTER for the discriminating physical-vs-aggregator test ---

// Internal helper from src/input/axl-input.c, exposed (non-static, no
// public header) for this regression test. Calling it directly keeps the
// check deterministic and correct even when the QEMU test platform also
// exposes a real SimplePointer (the runner's usb-mouse, when the firmware
// binds it): the helper then returns the real device, which is still NOT
// the aggregator. A dispatch-through-the-loop check couldn't tell which
// device was bound in that case.
extern void *axl_input_locate_physical_pointer(EFI_GUID *guid);

/* Live keyboard-modifier tracking (non-static, no public header) — see the
   modifier-stamping tests below. */
extern bool     axl_input_track_modifiers(const AxlInputKey *key);
extern uint32_t axl_input_live_modifiers(void);

static EFI_STATUS EFIAPI
mock_sp_reset(EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN ext)
{
    (void)This; (void)ext;
    return 0;   /* EFI_SUCCESS */
}

static EFI_STATUS EFIAPI
mock_sp_getstate(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *st)
{
    (void)This; (void)st;
    return 6;   /* EFI_NOT_READY — never called here, just a valid vtable slot */
}

static void
test_attach_mouse_protocol_available(void)
{
    /* ConSplitterDxe publishes EFI_SIMPLE_POINTER_PROTOCOL on
       gST->ConsoleInHandle (the console aggregator) even with no backing
       device — that is exactly the interface the old LocateProtocol
       returned first, and the dead handle locate_physical_pointer exists
       to skip. Verify the helper prefers a PHYSICAL device (a non-
       ConsoleIn handle) over that aggregator.

       Call the helper directly rather than dispatching a mock event
       through the loop: it stays deterministic and still discriminates
       when the QEMU platform exposes a real SimplePointer (the runner's
       usb-mouse, once the firmware binds it). In that case the helper
       returns the real device — still != the aggregator — whereas the
       buggy LocateProtocol path would return the aggregator interface.
       NOTE: attach_mouse no longer binds via this helper — it binds every
       SimplePointer handle ConsoleInHandle-first (collect_pointers), like
       attach_touch, so a virtual / remote-console pointer on ConsoleInHandle
       is delivered. This still exercises the helper's prefer-physical
       contract (retained as a utility) and attach_mouse's registration. */
    EFI_GUID guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;

    /* The aggregator interface — what the buggy LocateProtocol returns. */
    void      *aggregator = NULL;
    EFI_STATUS ls = gBS->LocateProtocol(&guid, NULL, &aggregator);
    test_check(!EFI_ERROR(ls) && aggregator != NULL,
               "attach_mouse: ConSplitter aggregator SimplePointer present "
               "(LocateProtocol baseline)");

    /* Install a physical mock on its own (non-ConsoleIn) handle. Give it a
       real WaitForInput so axl_input_attach_mouse can register it on
       platforms where the mock is the chosen device (no real SimplePointer
       — e.g. AAVMF, which doesn't bind the runner's usb-mouse). */
    AxlEvent *wfi = axl_event_new();
    test_check(wfi != NULL, "attach_mouse: fixture WaitForInput event created");
    static EFI_SIMPLE_POINTER_PROTOCOL phys;
    phys.Reset        = mock_sp_reset;
    phys.GetState     = mock_sp_getstate;
    phys.WaitForInput = axl_event_handle(wfi);
    phys.Mode         = NULL;
    EFI_HANDLE physh = NULL;   /* fresh handle — NOT ConsoleInHandle */
    EFI_STATUS s = gBS->InstallProtocolInterface(&physh, &guid,
                                                 EFI_NATIVE_INTERFACE, &phys);
    test_check(!EFI_ERROR(s) && physh != gST->ConsoleInHandle,
               "attach_mouse: physical mock installed on a non-ConsoleIn handle");

    /* The fix: the helper returns a physical device, never the boot-time
       aggregator. */
    void *got = axl_input_locate_physical_pointer(&guid);
    test_check(got != NULL,
               "attach_mouse: locate_physical_pointer returns a device");
    test_check(got != aggregator,
               "attach_mouse: locate_physical_pointer prefers a PHYSICAL device, "
               "not the ConSplitter console aggregator");

    /* Public-API registration: attach succeeds and is single-source. */
    AxlLoop *loop = axl_loop_new();
    AxlSourceId id = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id != 0, "attach_mouse: returns non-zero source ID");
    AxlSourceId id2 = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id2 == 0, "attach_mouse: second attach returns 0 "
                         "(only one mouse source per process)");

    /* Detach frees the single-mouse slot (and removes the caller-owned
       WaitForInput source) so later tests can re-attach. */
    axl_input_detach_mouse(loop);
    AxlSourceId id3 = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id3 != 0, "attach_mouse: re-attach succeeds after detach");
    axl_input_detach_mouse(loop);

    axl_loop_free(loop);
    gBS->UninstallProtocolInterface(physh, &guid, &phys);
    axl_event_free(wfi);
}

// axl_input_attach_mouse_ifaces — bind caller-supplied interfaces directly (the
// pointer-take-over path, where the interfaces are no longer in the handle DB).
static void
test_attach_mouse_ifaces(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "attach_mouse_ifaces fixture: loop created");

    /* A cached interface: never installed on a handle (that's the point). */
    AxlEvent *wfi = axl_event_new();
    test_check(wfi != NULL, "attach_mouse_ifaces: fixture WaitForInput created");
    static EFI_SIMPLE_POINTER_PROTOCOL sp;
    sp.Reset        = mock_sp_reset;
    sp.GetState     = mock_sp_getstate;
    sp.WaitForInput = axl_event_handle(wfi);
    sp.Mode         = NULL;
    void *ifaces[1] = { &sp };

    /* Guards. */
    test_check(axl_input_attach_mouse_ifaces(NULL, unused_cb, NULL, ifaces, 1) == 0,
               "attach_mouse_ifaces: NULL loop returns 0");
    test_check(axl_input_attach_mouse_ifaces(loop, NULL, NULL, ifaces, 1) == 0,
               "attach_mouse_ifaces: NULL cb returns 0");
    test_check(axl_input_attach_mouse_ifaces(loop, unused_cb, NULL, NULL, 1) == 0,
               "attach_mouse_ifaces: NULL ifaces returns 0");
    test_check(axl_input_attach_mouse_ifaces(loop, unused_cb, NULL, ifaces, 0) == 0,
               "attach_mouse_ifaces: n<=0 returns 0");
    void *all_null[2] = { NULL, NULL };
    test_check(axl_input_attach_mouse_ifaces(loop, unused_cb, NULL, all_null, 2) == 0,
               "attach_mouse_ifaces: all-NULL interfaces returns 0");

    /* Positive: binds the cached interface and is single-source. */
    AxlSourceId id = axl_input_attach_mouse_ifaces(loop, unused_cb, NULL, ifaces, 1);
    test_check(id != 0, "attach_mouse_ifaces: returns non-zero source ID");
    test_check(axl_input_attach_mouse_ifaces(loop, unused_cb, NULL, ifaces, 1) == 0,
               "attach_mouse_ifaces: second attach returns 0 (single mouse source)");

    /* Detach frees the slot so a later attach (either flavor) succeeds. */
    axl_input_detach_mouse(loop);
    AxlSourceId id2 = axl_input_attach_mouse_ifaces(loop, unused_cb, NULL, ifaces, 1);
    test_check(id2 != 0, "attach_mouse_ifaces: re-attach succeeds after detach");
    axl_input_detach_mouse(loop);

    axl_loop_free(loop);
    axl_event_free(wfi);
}

// ---------------------------------------------------------------------------
// axl_input_attach_key — registration error paths + success
// ---------------------------------------------------------------------------

static void
test_attach_key_null_loop_returns_zero(void)
{
    test_check(axl_input_attach_key(NULL, unused_cb, NULL) == 0,
               "attach_key: NULL loop returns 0");
}

static void
test_attach_key_null_cb_returns_zero(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "attach_key fixture: axl_loop_new succeeds");
    test_check(axl_input_attach_key(loop, NULL, NULL) == 0,
               "attach_key: NULL cb returns 0");
    axl_loop_free(loop);
}

static void
test_attach_key_success_and_already_attached(void)
{
    /* axl_loop_add_key_press uses ConIn's WaitForKey event, which the
       firmware always provides — so attach should always succeed.
       Second attach must fail per the single-source-per-process
       limit (mirrors the mouse case). */
    AxlLoop *loop = axl_loop_new();
    AxlSourceId id = axl_input_attach_key(loop, unused_cb, NULL);
    test_check(id != 0,
               "attach_key: first attach returns non-zero source ID");

    AxlSourceId id2 = axl_input_attach_key(loop, unused_cb, NULL);
    test_check(id2 == 0,
               "attach_key: second attach returns 0 (single-source limit)");

    /* Detach frees the per-process slot so a re-attach succeeds — the
       mirror of the mouse case, and what the compositor seat handoff to a
       modal dialog's compositor relies on (C7). */
    axl_input_detach_key(loop);
    AxlSourceId id3 = axl_input_attach_key(loop, unused_cb, NULL);
    test_check(id3 != 0, "attach_key: re-attach succeeds after detach");
    axl_input_detach_key(loop);

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// axl_input_attach_touch — registration error paths
// ---------------------------------------------------------------------------

static void
test_attach_touch_null_loop_returns_zero(void)
{
    test_check(axl_input_attach_touch(NULL, unused_cb, NULL) == 0,
               "attach_touch: NULL loop returns 0");
}

static void
test_attach_touch_null_cb_returns_zero(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(axl_input_attach_touch(loop, NULL, NULL) == 0,
               "attach_touch: NULL cb returns 0");
    axl_loop_free(loop);
}

static void
test_attach_touch_protocol_available(void)
{
    /* QEMU's OVMF firmware publishes EFI_ABSOLUTE_POINTER_PROTOCOL
       on the virtual digitizer (same as it does for the simple
       pointer).  Verify attach succeeds and the single-source
       limit rejects a second attach. */
    AxlLoop *loop = axl_loop_new();
    AxlSourceId id = axl_input_attach_touch(loop, unused_cb, NULL);
    test_check(id != 0,
               "attach_touch: returns non-zero source ID when "
               "EFI_ABSOLUTE_POINTER_PROTOCOL available (QEMU baseline)");

    AxlSourceId id2 = axl_input_attach_touch(loop, unused_cb, NULL);
    test_check(id2 == 0,
               "attach_touch: second attach returns 0 "
               "(only one touch source per process)");

    /* Detach must free the single-source slot AND the protocol-notify source
       (and the WaitForInput sources) so a fresh attach re-binds cleanly — a
       regression guard for the handle-based re-resolve + protocol-notify rework
       (the fix for the stale-interface #GP when the pointer driver is Stop()'d).
       A botched detach (leaked notify source / unreset state) makes re-attach
       fail here. */
    axl_input_detach_touch(loop);
    AxlSourceId id3 = axl_input_attach_touch(loop, unused_cb, NULL);
    test_check(id3 != 0,
               "attach_touch: re-attach after detach succeeds "
               "(handle/notify re-bind lifecycle)");
    axl_input_detach_touch(loop);

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// axl_input_touch_coalesce — absolute-pointer drain policy
//
// Internal helper (no public header), exposed for this test. The drain
// collapses a FIFO firmware backlog (iDRAC / BMC virtual mouse) to the live
// position to kill seconds-long lag, but must STOP at a button transition so a
// press/release is never coalesced away. Tested here with a scripted state
// sequence — deterministic and firmware-free (QEMU/OVMF doesn't queue
// absolute-pointer states, so a live demo can't exercise this).
// ---------------------------------------------------------------------------

typedef bool (*AxlAbsReader)(void *ctx, EFI_ABSOLUTE_POINTER_STATE *out);
extern uint32_t axl_input_touch_coalesce(AxlAbsReader read, void *ctx,
                                         bool baseline_active, uint32_t max_drain,
                                         EFI_ABSOLUTE_POINTER_STATE *out);

typedef struct {
    const EFI_ABSOLUTE_POINTER_STATE *seq;
    uint32_t                          n;
    uint32_t                          pos;
} ScriptedAbs;

static bool
scripted_abs_read(void *vctx, EFI_ABSOLUTE_POINTER_STATE *out)
{
    ScriptedAbs *s = (ScriptedAbs *)vctx;
    if (s->pos >= s->n) {
        return false;
    }
    *out = s->seq[s->pos++];
    return true;
}

static EFI_ABSOLUTE_POINTER_STATE
abs_state(uint64_t x, uint64_t y, uint32_t buttons)
{
    EFI_ABSOLUTE_POINTER_STATE s = {0};
    s.CurrentX      = x;
    s.CurrentY      = y;
    s.ActiveButtons = buttons;
    return s;
}

static void
test_touch_coalesce(void)
{
    EFI_ABSOLUTE_POINTER_STATE out = {0};

    /* Empty queue: reads nothing, leaves out untouched (per contract). */
    {
        out = abs_state(0xDEAD, 0xBEEF, 0);   // sentinel
        ScriptedAbs s = { .seq = NULL, .n = 0, .pos = 0 };
        uint32_t n = axl_input_touch_coalesce(scripted_abs_read, &s, false, 8, &out);
        test_check(n == 0, "coalesce: empty queue reads 0");
        test_check(out.CurrentX == 0xDEAD && out.CurrentY == 0xBEEF,
                   "coalesce: empty queue leaves out untouched");
    }

    /* Pure-motion backlog coalesces to the LAST position. */
    {
        const EFI_ABSOLUTE_POINTER_STATE seq[] = {
            abs_state(10, 10, 0), abs_state(20, 20, 0), abs_state(30, 30, 0),
        };
        ScriptedAbs s = { .seq = seq, .n = 3, .pos = 0 };
        uint32_t n = axl_input_touch_coalesce(scripted_abs_read, &s, false, 8, &out);
        test_check(n == 3, "coalesce: drains the whole motion backlog");
        test_check(out.CurrentX == 30 && out.CurrentY == 30,
                   "coalesce: motion coalesces to the latest position");
    }

    /* max_drain caps the read count (out is the last drained, not the newest). */
    {
        const EFI_ABSOLUTE_POINTER_STATE seq[] = {
            abs_state(10, 10, 0), abs_state(20, 20, 0), abs_state(30, 30, 0),
        };
        ScriptedAbs s = { .seq = seq, .n = 3, .pos = 0 };
        uint32_t n = axl_input_touch_coalesce(scripted_abs_read, &s, false, 2, &out);
        test_check(n == 2, "coalesce: max_drain caps the read count");
        test_check(out.CurrentX == 20, "coalesce: stops after max_drain states");
    }

    /* max_drain == 1: exactly one read (legacy single-read behavior). */
    {
        const EFI_ABSOLUTE_POINTER_STATE seq[] = {
            abs_state(10, 10, 0), abs_state(20, 20, 0),
        };
        ScriptedAbs s = { .seq = seq, .n = 2, .pos = 0 };
        uint32_t n = axl_input_touch_coalesce(scripted_abs_read, &s, false, 1, &out);
        test_check(n == 1 && out.CurrentX == 10,
                   "coalesce: max_drain==1 reads exactly one state");
    }

    /* Edge stop: a press transition is NOT coalesced past. With a held-motion
       state queued AFTER the press, the drain must stop AT the press (the
       chosen state is the DOWN, not the later held move). */
    {
        const EFI_ABSOLUTE_POINTER_STATE seq[] = {
            abs_state(10, 10, 0),   // motion, no button (baseline)
            abs_state(20, 20, 1),   // PRESS — the edge
            abs_state(30, 30, 1),   // held drag after the press
        };
        ScriptedAbs s = { .seq = seq, .n = 3, .pos = 0 };
        uint32_t n = axl_input_touch_coalesce(scripted_abs_read, &s, false, 8, &out);
        test_check(n == 2, "coalesce: stops at the press transition (does not drain past)");
        test_check(out.CurrentX == 20 && out.ActiveButtons != 0,
                   "coalesce: chosen state IS the press, not the later held move");
    }

    /* Click survives: a DOWN then UP queued together must NOT collapse into a
       single buttons==0 state (which would swallow the whole click). The first
       drain stops at the DOWN; a second drain (now contact-active baseline)
       stops at the UP — both edges observable across two dispatches. */
    {
        const EFI_ABSOLUTE_POINTER_STATE seq[] = {
            abs_state(15, 15, 1),   // DOWN
            abs_state(15, 15, 0),   // UP (same spot — a click)
        };
        ScriptedAbs s = { .seq = seq, .n = 2, .pos = 0 };

        uint32_t n1 = axl_input_touch_coalesce(scripted_abs_read, &s, false, 8, &out);
        test_check(n1 == 1 && out.ActiveButtons != 0,
                   "coalesce: click DOWN is not swallowed (stops at the press)");

        uint32_t n2 = axl_input_touch_coalesce(scripted_abs_read, &s, true, 8, &out);
        test_check(n2 == 1 && out.ActiveButtons == 0,
                   "coalesce: the following release is delivered on the next dispatch");
    }
}

// ---------------------------------------------------------------------------
// axl_input_ctrl_letter — dual-encoding Ctrl+<letter> decode
// ---------------------------------------------------------------------------

static void
test_ctrl_letter_serial_folded(void)
{
    /* Serial console: folded C0 code, modifiers == 0. */
    test_check(axl_input_ctrl_letter(0x01, 0) == 'a',
               "ctrl_letter: serial 0x01 -> 'a'");
    test_check(axl_input_ctrl_letter(0x1A, 0) == 'z',
               "ctrl_letter: serial 0x1A -> 'z'");
    test_check(axl_input_ctrl_letter(0x0E, 0) == 'n',
               "ctrl_letter: serial 0x0E (Ctrl+N) -> 'n'");
}

static void
test_ctrl_letter_keyboard_letter_plus_mod(void)
{
    /* Physical keyboard: printable letter + MOD_CTRL, not folded. */
    test_check(axl_input_ctrl_letter('a', AXL_INPUT_MOD_CTRL) == 'a',
               "ctrl_letter: keyboard 'a'+CTRL -> 'a'");
    test_check(axl_input_ctrl_letter('n', AXL_INPUT_MOD_LCTRL) == 'n',
               "ctrl_letter: keyboard 'n'+LCTRL -> 'n'");
    /* Case-folded: Ctrl+Shift+A (uppercase) still resolves to 'a'. */
    test_check(axl_input_ctrl_letter('A', AXL_INPUT_MOD_CTRL) == 'a',
               "ctrl_letter: keyboard 'A'+CTRL case-folds -> 'a'");
}

static void
test_ctrl_letter_excludes_edit_keys(void)
{
    /* The four C0 codes that are dedicated editing keys are NOT chords. */
    test_check(axl_input_ctrl_letter(0x08, 0) == 0,
               "ctrl_letter: 0x08 (Backspace / Ctrl+H) -> 0");
    test_check(axl_input_ctrl_letter(0x09, 0) == 0,
               "ctrl_letter: 0x09 (Tab / Ctrl+I) -> 0");
    test_check(axl_input_ctrl_letter(0x0A, 0) == 0,
               "ctrl_letter: 0x0A (LF / Ctrl+J) -> 0");
    test_check(axl_input_ctrl_letter(0x0D, 0) == 0,
               "ctrl_letter: 0x0D (CR / Ctrl+M) -> 0");
    /* Asymmetry: the exclusion is folded-branch only.  On the keyboard
     * path Ctrl+H/I/J/M arrive as the letter + MOD_CTRL (never the C0
     * code), so they ARE chords there — pin that so a future refactor
     * can't fold the exclusion into a shared check and kill them. */
    test_check(axl_input_ctrl_letter('h', AXL_INPUT_MOD_CTRL) == 'h',
               "ctrl_letter: keyboard 'h'+CTRL -> 'h' (not folded/excluded)");
    test_check(axl_input_ctrl_letter('m', AXL_INPUT_MOD_CTRL) == 'm',
               "ctrl_letter: keyboard 'm'+CTRL -> 'm' (not folded/excluded)");
}

static void
test_ctrl_letter_non_chord(void)
{
    /* Plain printable letter with no Ctrl held is not a chord. */
    test_check(axl_input_ctrl_letter('a', 0) == 0,
               "ctrl_letter: 'a' with no modifiers -> 0");
    /* A non-letter with Ctrl held (e.g. Ctrl+1) is not a letter chord. */
    test_check(axl_input_ctrl_letter('1', AXL_INPUT_MOD_CTRL) == 0,
               "ctrl_letter: '1'+CTRL -> 0 (not a letter)");
    /* Shift alone does not make a chord. */
    test_check(axl_input_ctrl_letter('a', AXL_INPUT_MOD_SHIFT) == 0,
               "ctrl_letter: 'a'+SHIFT -> 0");
    test_check(axl_input_ctrl_letter(0, 0) == 0,
               "ctrl_letter: empty event -> 0");
}

// ---------------------------------------------------------------------------
// axl_input_abs_normalize — native absolute coord -> [0, AXL_INPUT_ABS_RANGE)
// ---------------------------------------------------------------------------

static void
test_abs_normalize_endpoints(void)
{
    /* QEMU usb-tablet range 0..32767: min->0, max->RANGE-1. */
    test_check(axl_input_abs_normalize(0, 0, 32767) == 0,
               "abs_normalize: low endpoint -> 0");
    test_check(axl_input_abs_normalize(32767, 0, 32767)
                   == (int32_t)(AXL_INPUT_ABS_RANGE - 1),
               "abs_normalize: high endpoint -> RANGE-1");
    /* Midpoint lands near RANGE/2. */
    int32_t mid = axl_input_abs_normalize(16383, 0, 32767);
    test_check(mid > 32000 && mid < 33500, "abs_normalize: midpoint ~ RANGE/2");
}

static void
test_abs_normalize_offset_range(void)
{
    /* A device whose range doesn't start at 0 still maps to [0, RANGE). */
    test_check(axl_input_abs_normalize(100, 100, 1124) == 0,
               "abs_normalize: offset low endpoint -> 0");
    test_check(axl_input_abs_normalize(1124, 100, 1124)
                   == (int32_t)(AXL_INPUT_ABS_RANGE - 1),
               "abs_normalize: offset high endpoint -> RANGE-1");
}

static void
test_abs_normalize_clamps_and_degenerate(void)
{
    /* Out-of-range values clamp to the endpoints. */
    test_check(axl_input_abs_normalize(-5, 0, 100) == 0,
               "abs_normalize: below low clamps to 0");
    test_check(axl_input_abs_normalize(200, 0, 100)
                   == (int32_t)(AXL_INPUT_ABS_RANGE - 1),
               "abs_normalize: above high clamps to RANGE-1");
    /* Degenerate range (max <= min) yields 0 rather than dividing by 0. */
    test_check(axl_input_abs_normalize(50, 100, 100) == 0,
               "abs_normalize: degenerate range (max==min) -> 0");
    test_check(axl_input_abs_normalize(50, 200, 100) == 0,
               "abs_normalize: inverted range (max<min) -> 0");
}

// ---------------------------------------------------------------------------
// Click-gesture recognizer (pure — synthetic events, deterministic clock)
// ---------------------------------------------------------------------------

static AxlInputEvent
gest_ev(AxlInputType type, uint64_t ts, int32_t x, int32_t y, uint32_t buttons)
{
    AxlInputEvent ev = {0};
    ev.type = type;
    ev.timestamp_us = ts;
    ev.x = x;
    ev.y = y;
    ev.buttons = buttons;
    return ev;
}

static void
test_gesture_click_count(void)
{
    axl_input_set_click_tuning(400, 4);   /* 400 ms window, 4 px */
    AxlGesture g = {0};
    AxlInputEvent e;

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 0, 10, 10, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(e.click_count == 1, "gesture: first click -> count 1");

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_UP, 0, 10, 10, 0);
    axl_input_gesture_feed(&g, &e);
    test_check(e.click_count == 0, "gesture: button-up is not a click");

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 200000, 11, 11, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(e.click_count == 2, "gesture: 2nd click within window+threshold -> count 2");

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 350000, 12, 12, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(e.click_count == 3, "gesture: 3rd click -> count 3 (triple)");

    /* Too slow: 500 ms after the last down (> 400 ms window) -> resets. */
    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 850000, 12, 12, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(e.click_count == 1, "gesture: click after window expires -> resets to 1");

    /* Too far: second click 100 px away -> resets. */
    AxlGesture g2 = {0};
    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 0, 100, 100, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g2, &e);
    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 100000, 200, 100, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g2, &e);
    test_check(e.click_count == 1, "gesture: 2nd click beyond threshold -> resets to 1");
}

static void
test_gesture_drag(void)
{
    axl_input_set_click_tuning(400, 4);
    AxlGesture g = {0};
    AxlInputEvent e;

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 0, 50, 50, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(!e.dragging, "gesture: a press alone is not a drag");

    e = gest_ev(AXL_INPUT_MOUSE_MOVE, 1000, 52, 51, AXL_INPUT_BUTTON_LEFT);   /* ~2 px */
    axl_input_gesture_feed(&g, &e);
    test_check(!e.dragging, "gesture: held move under threshold is not a drag");

    e = gest_ev(AXL_INPUT_MOUSE_MOVE, 2000, 60, 50, AXL_INPUT_BUTTON_LEFT);   /* 10 px */
    axl_input_gesture_feed(&g, &e);
    test_check(e.dragging, "gesture: held move past threshold latches dragging");

    e = gest_ev(AXL_INPUT_MOUSE_MOVE, 3000, 40, 50, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(e.dragging, "gesture: dragging stays latched while held");

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_UP, 4000, 40, 50, 0);
    axl_input_gesture_feed(&g, &e);
    test_check(e.dragging, "gesture: the release ending a drag reports dragging");

    e = gest_ev(AXL_INPUT_MOUSE_BUTTON_DOWN, 5000, 40, 50, AXL_INPUT_BUTTON_LEFT);
    axl_input_gesture_feed(&g, &e);
    test_check(!e.dragging, "gesture: a fresh press after a drag is not dragging");

    /* Motion with no button held is never a drag. */
    AxlGesture g2 = {0};
    e = gest_ev(AXL_INPUT_MOUSE_MOVE, 0, 80, 80, 0);
    axl_input_gesture_feed(&g2, &e);
    test_check(!e.dragging, "gesture: move with no button held is not a drag");

    /* NULL-safety: feed(NULL, ev) is a no-op (event untouched); feed(g,
       NULL) just must not crash (no event to inspect). */
    uint32_t cc = e.click_count;
    axl_input_gesture_feed(NULL, &e);
    test_check(e.click_count == cc, "gesture: NULL state pointer is a safe no-op");
    axl_input_gesture_feed(&g2, NULL);
}

// ---------------------------------------------------------------------------
// Key debounce / repeat suppression (pure — synthetic events)
// ---------------------------------------------------------------------------

static AxlInputEvent
key_ev(uint64_t ts, uint32_t keycode, uint32_t unicode)
{
    AxlInputEvent ev = {0};
    ev.type = AXL_INPUT_KEY_DOWN;
    ev.timestamp_us = ts;
    ev.keycode = keycode;
    ev.unicode = unicode;
    return ev;
}

static void
test_key_debounce(void)
{
    AxlInputEvent e;

    /* Disabled (default): never drops, even a 1 ms same-key repeat. */
    axl_input_set_key_debounce(0, true);
    AxlKeyDebounce d0 = {0};
    e = key_ev(0, 0, 'a');
    test_check(axl_input_key_accept(&d0, &e), "debounce off: accept first 'a'");
    e = key_ev(1000, 0, 'a');
    test_check(axl_input_key_accept(&d0, &e), "debounce off: accept 1 ms repeat");

    /* Enabled 40 ms, printable-only. */
    axl_input_set_key_debounce(40, true);

    AxlKeyDebounce d = {0};
    e = key_ev(0, 0, 'a');
    test_check(axl_input_key_accept(&d, &e), "debounce: accept first 'a'");
    e = key_ev(20000, 0, 'a');     /* 20 ms typematic-rate repeat */
    test_check(!axl_input_key_accept(&d, &e), "debounce: drop 20 ms repeat of 'a'");
    e = key_ev(100000, 0, 'a');    /* 80 ms after the last-seen -> accept */
    test_check(axl_input_key_accept(&d, &e), "debounce: accept 'a' after the window");

    /* Different keys never interfere. */
    AxlKeyDebounce d2 = {0};
    e = key_ev(0, 0, 'a');
    (void)axl_input_key_accept(&d2, &e);
    e = key_ev(5000, 0, 'b');
    test_check(axl_input_key_accept(&d2, &e), "debounce: a different key is never dropped");

    /* Navigation key (scancode, unicode 0) is exempt when printable-only. */
    AxlKeyDebounce d3 = {0};
    e = key_ev(0, 0x01, 0);
    (void)axl_input_key_accept(&d3, &e);
    e = key_ev(20000, 0x01, 0);
    test_check(axl_input_key_accept(&d3, &e),
               "debounce: nav key exempt under printable-only");

    /* Backspace (control code 0x08) is exempt when printable-only. */
    AxlKeyDebounce d4 = {0};
    e = key_ev(0, 0, 0x08);
    (void)axl_input_key_accept(&d4, &e);
    e = key_ev(20000, 0, 0x08);
    test_check(axl_input_key_accept(&d4, &e),
               "debounce: backspace exempt under printable-only");

    /* With printable_only == false, even nav keys are debounced. */
    axl_input_set_key_debounce(40, false);
    AxlKeyDebounce d5 = {0};
    e = key_ev(0, 0x01, 0);
    (void)axl_input_key_accept(&d5, &e);
    e = key_ev(20000, 0x01, 0);
    test_check(!axl_input_key_accept(&d5, &e),
               "debounce: !printable_only debounces nav keys too");

    /* Non-key events pass through untouched. */
    axl_input_set_key_debounce(40, true);
    AxlKeyDebounce d6 = {0};
    AxlInputEvent m = {0};
    m.type = AXL_INPUT_MOUSE_MOVE;
    m.timestamp_us = 1000;
    test_check(axl_input_key_accept(&d6, &m), "debounce: non-key event accepted");

    /* NULL-safety: no state / no event -> accept (never silently eat input). */
    e = key_ev(0, 0, 'a');
    test_check(axl_input_key_accept(NULL, &e), "debounce: NULL state accepts");
    test_check(axl_input_key_accept(&d6, NULL), "debounce: NULL event accepts");

    axl_input_set_key_debounce(0, true);   /* reset for the rest of the suite */
}

// ---------------------------------------------------------------------------
// Min-gap delivery gate (pure timing — deliver-now vs hold-until)
// ---------------------------------------------------------------------------

static void
test_key_gate(void)
{
    /* Fresh gate: the first key of a stream is never held, whatever the gap. */
    AxlKeyGate g = {0};
    test_check(axl_input_key_gate_ready_at(&g, 20) == 0,
               "gate: first key ready immediately (ready_at 0)");

    /* Disabled (min_gap 0): always ready, even after a delivery. */
    axl_input_key_gate_mark(&g, 100000);
    test_check(axl_input_key_gate_ready_at(&g, 0) == 0,
               "gate: min_gap 0 disables (ready_at 0)");

    /* Primed with a 20 ms gap: next ready at last + 20 ms. */
    AxlKeyGate g2 = {0};
    axl_input_key_gate_mark(&g2, 100000);   /* delivered at t=100 ms */
    test_check(axl_input_key_gate_ready_at(&g2, 20) == 120000,
               "gate: ready_at = last_delivered + min_gap");
    test_check(g2.primed, "gate: mark primes the gate");
    test_check(g2.last_delivered_us == 100000, "gate: mark records timestamp");

    /* Deliver/hold sequence: a key 5 ms later is held; one at exactly the gap
       is ready (>= boundary is inclusive). */
    uint64_t ready = axl_input_key_gate_ready_at(&g2, 20);
    test_check(105000 < ready, "gate: a 5 ms-later key is held (now < ready_at)");
    test_check(120000 >= ready, "gate: a key at exactly the gap is ready");

    /* Advancing on the released key re-bases the next gap. */
    axl_input_key_gate_mark(&g2, 120000);
    test_check(axl_input_key_gate_ready_at(&g2, 20) == 140000,
               "gate: mark re-bases the next window");

    /* NULL-safe: never hold (never silently eat input); mark is a no-op. */
    test_check(axl_input_key_gate_ready_at(NULL, 20) == 0, "gate: NULL state ready");
    axl_input_key_gate_mark(NULL, 1000);   /* must not crash */
}

// ---------------------------------------------------------------------------
// Held-button auto-repeat (timer-driven; bounded-window count, no clock race)
// ---------------------------------------------------------------------------

static int g_btn_repeat_count;
static int g_btn_total_down;

static bool
btn_repeat_cb(const AxlInputEvent *ev, void *data)
{
    (void)data;
    if (ev->type == AXL_INPUT_MOUSE_BUTTON_DOWN) {
        g_btn_total_down++;
        if (ev->repeat) {
            g_btn_repeat_count++;
        }
    }
    return AXL_SOURCE_CONTINUE;
}

static EFI_STATUS EFIAPI
mock_sp_getstate_held(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *st)
{
    (void)This;
    st->RelativeMovementX = 0;
    st->RelativeMovementY = 0;
    st->RelativeMovementZ = 0;
    st->LeftButton  = true;     /* button stays held */
    st->RightButton = false;
    return 0;
}

static void
test_button_repeat(void)
{
    /* Physical mock whose button is held down, on a fresh handle. */
    AxlEvent *wfi = axl_event_new();
    static EFI_SIMPLE_POINTER_PROTOCOL phys;
    phys.Reset        = mock_sp_reset;
    phys.GetState     = mock_sp_getstate_held;
    phys.WaitForInput = axl_event_handle(wfi);
    phys.Mode         = NULL;
    EFI_GUID   guid  = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_HANDLE physh = NULL;
    gBS->InstallProtocolInterface(&physh, &guid, EFI_NATIVE_INTERFACE, &phys);

    /* On firmware that publishes a real physical SimplePointer (x64 OVMF
       binds the runner's usb-mouse), locate_physical_pointer would choose
       it over our mock — and we can't drive a real device. Temporarily
       remove any other non-console SimplePointer so attach binds OUR
       mock, then restore them after. */
    EFI_HANDLE *hb = NULL;
    UINTN       hc = 0;
    EFI_HANDLE  saved_h[8];
    void       *saved_i[8];
    int         nsaved = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &guid, NULL, &hc, &hb) == 0) {
        for (UINTN i = 0; i < hc && nsaved < 8; i++) {
            if (hb[i] == gST->ConsoleInHandle || hb[i] == physh) {
                continue;
            }
            void *iface = NULL;
            if (gBS->HandleProtocol(hb[i], &guid, &iface) == 0 && iface != NULL
                && gBS->UninstallProtocolInterface(hb[i], &guid, iface) == 0) {
                saved_h[nsaved] = hb[i];
                saved_i[nsaved] = iface;
                nsaved++;
            }
        }
        if (hb != NULL) {
            gBS->FreePool(hb);
        }
    }
    test_check(axl_input_locate_physical_pointer(&guid) == &phys,
               "button_repeat: mock is the bound pointer (competitors removed)");

    axl_input_set_button_repeat(10, 10);   /* 10 ms delay, 10 ms interval */
    AxlLoop *loop = axl_loop_new();
    g_btn_repeat_count = 0;
    g_btn_total_down   = 0;
    AxlSourceId id = axl_input_attach_mouse(loop, btn_repeat_cb, NULL);
    test_check(id != 0, "button_repeat: attach succeeds");

    /* Signal the press (Left transition -> arms repeat), then pump the
       loop for a generous bounded window. At 10 ms interval ~48 repeats
       fit in 500 ms; assert >= 3 so the check is robust under load and
       can't hang (fixed window, not a clock-race wait). */
    axl_event_signal(wfi);
    (void)axl_loop_iterate_until(loop, NULL, 500000);   /* 500 ms */
    test_check(g_btn_total_down >= 1, "button_repeat: the press fired");
    test_check(g_btn_repeat_count >= 3,
               "button_repeat: held button auto-repeats while down");

    axl_input_set_button_repeat(0, 0);     /* disable for the rest of the suite */
    axl_input_detach_mouse(loop);          /* removes source + repeat timer */
    axl_loop_free(loop);
    gBS->UninstallProtocolInterface(physh, &guid, &phys);
    axl_event_free(wfi);

    /* Restore any competitor SimplePointer interfaces we removed. */
    for (int k = 0; k < nsaved; k++) {
        (void)gBS->InstallProtocolInterface(&saved_h[k], &guid,
                                            EFI_NATIVE_INTERFACE, saved_i[k]);
    }
}

// ---------------------------------------------------------------------------
// Live keyboard-modifier tracking + stamping onto pointer events
//
// Keyboard modifiers are delivered only with a keystroke, so the substrate
// tracks a live modifier state (kept current between character keystrokes by
// EFI_KEY_STATE_EXPOSED modifier-only "partial" keystrokes) and stamps it
// onto pointer events — so a consumer sees Shift+wheel / Ctrl+click. The
// pure tracking + partial-filter is unit-testable here; the EXPOSED partial
// delivery itself is firmware-gated (real hardware), not exercisable in the
// serial QEMU harness.
// ---------------------------------------------------------------------------

static void
test_modifier_tracking(void)
{
    /* A modifier-only partial keystroke (no scan/char) updates the live
       state but is NOT delivered as a key. */
    AxlInputKey shift = { .scan_code = 0, .unicode_char = 0,
                          .modifiers = AXL_INPUT_MOD_LSHIFT };
    test_check(!axl_input_track_modifiers(&shift),
               "modtrack: a modifier-only partial keystroke is not delivered");
    test_check(axl_input_live_modifiers() == AXL_INPUT_MOD_LSHIFT,
               "modtrack: the partial updates the live modifier state");

    /* Releasing (partial with no modifiers) clears the state. */
    AxlInputKey release = { .scan_code = 0, .unicode_char = 0, .modifiers = 0 };
    test_check(!axl_input_track_modifiers(&release),
               "modtrack: a modifier-release partial is not delivered");
    test_check(axl_input_live_modifiers() == 0,
               "modtrack: releasing clears the live modifier state");

    /* A character keystroke IS delivered and also updates the live state. */
    AxlInputKey ch = { .scan_code = 0, .unicode_char = 'a',
                       .modifiers = AXL_INPUT_MOD_LCTRL };
    test_check(axl_input_track_modifiers(&ch),
               "modtrack: a character keystroke is delivered");
    test_check(axl_input_live_modifiers() == AXL_INPUT_MOD_LCTRL,
               "modtrack: a full keystroke updates the live modifier state");

    /* A special key (scan code, no char) is delivered too. */
    AxlInputKey arrow = { .scan_code = 0x01, .unicode_char = 0, .modifiers = 0 };
    test_check(axl_input_track_modifiers(&arrow),
               "modtrack: a scan-code key is delivered");

    /* NULL is treated as deliverable (never silently eat input). */
    test_check(axl_input_track_modifiers(NULL), "modtrack: NULL key is a no-op pass");

    AxlInputKey clr = {0};   /* reset the global for later tests */
    (void)axl_input_track_modifiers(&clr);
}

static int g_modtest_calls;
static EFI_STATUS EFIAPI
mock_sp_getstate_modtest(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *st)
{
    (void)This;
    if (g_modtest_calls++ > 0) {
        return 6;   /* EFI_NOT_READY after the first call (one dispatch) */
    }
    st->RelativeMovementX = 5;     /* -> MOUSE_MOVE */
    st->RelativeMovementY = 0;
    st->RelativeMovementZ = 1;     /* -> MOUSE_WHEEL */
    st->LeftButton  = true;        /* transition -> BUTTON_DOWN */
    st->RightButton = false;
    return 0;
}

static uint32_t g_mod_move, g_mod_button, g_mod_wheel;
static bool     g_saw_move, g_saw_button, g_saw_wheel;
static bool
modtest_cb(const AxlInputEvent *ev, void *data)
{
    (void)data;
    switch (ev->type) {
    case AXL_INPUT_MOUSE_MOVE:        g_mod_move   = ev->modifiers; g_saw_move   = true; break;
    case AXL_INPUT_MOUSE_BUTTON_DOWN: g_mod_button = ev->modifiers; g_saw_button = true; break;
    case AXL_INPUT_MOUSE_WHEEL:       g_mod_wheel  = ev->modifiers; g_saw_wheel  = true; break;
    default: break;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_pointer_carries_modifiers(void)
{
    /* Simulate Shift held via a partial keystroke. */
    AxlInputKey shift = { .scan_code = 0, .unicode_char = 0,
                          .modifiers = AXL_INPUT_MOD_LSHIFT };
    (void)axl_input_track_modifiers(&shift);

    AxlEvent *wfi = axl_event_new();
    static EFI_SIMPLE_POINTER_PROTOCOL phys;
    phys.Reset        = mock_sp_reset;
    phys.GetState     = mock_sp_getstate_modtest;
    phys.WaitForInput = axl_event_handle(wfi);
    phys.Mode         = NULL;
    EFI_GUID   guid  = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_HANDLE physh = NULL;
    gBS->InstallProtocolInterface(&physh, &guid, EFI_NATIVE_INTERFACE, &phys);

    /* Remove competitor physical pointers so attach binds OUR mock (x64 OVMF
       binds a real usb-mouse that would otherwise win), then restore them. */
    EFI_HANDLE *hb = NULL; UINTN hc = 0;
    EFI_HANDLE  saved_h[8]; void *saved_i[8]; int nsaved = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &guid, NULL, &hc, &hb) == 0) {
        for (UINTN i = 0; i < hc && nsaved < 8; i++) {
            if (hb[i] == gST->ConsoleInHandle || hb[i] == physh) {
                continue;
            }
            void *iface = NULL;
            if (gBS->HandleProtocol(hb[i], &guid, &iface) == 0 && iface != NULL
                && gBS->UninstallProtocolInterface(hb[i], &guid, iface) == 0) {
                saved_h[nsaved] = hb[i]; saved_i[nsaved] = iface; nsaved++;
            }
        }
        if (hb != NULL) { gBS->FreePool(hb); }
    }

    g_modtest_calls = 0;
    g_saw_move = g_saw_button = g_saw_wheel = false;
    AxlLoop *loop = axl_loop_new();
    AxlSourceId id = axl_input_attach_mouse(loop, modtest_cb, NULL);
    test_check(id != 0, "modtrack: mouse attach succeeds");

    axl_event_signal(wfi);
    (void)axl_loop_iterate_until(loop, NULL, 50000);   /* drain the one dispatch */

    test_check(g_saw_move && g_mod_move == AXL_INPUT_MOD_LSHIFT,
               "modtrack: MOVE carries the live keyboard modifiers");
    test_check(g_saw_button && g_mod_button == AXL_INPUT_MOD_LSHIFT,
               "modtrack: BUTTON carries the live keyboard modifiers");
    test_check(g_saw_wheel && g_mod_wheel == AXL_INPUT_MOD_LSHIFT,
               "modtrack: WHEEL carries the live keyboard modifiers (Shift+wheel)");

    axl_input_detach_mouse(loop);
    axl_loop_free(loop);
    gBS->UninstallProtocolInterface(physh, &guid, &phys);
    axl_event_free(wfi);
    for (int k = 0; k < nsaved; k++) {
        (void)gBS->InstallProtocolInterface(&saved_h[k], &guid,
                                            EFI_NATIVE_INTERFACE, saved_i[k]);
    }

    AxlInputKey clr = {0};   /* clear the global so it doesn't leak forward */
    (void)axl_input_track_modifiers(&clr);
}

// ---------------------------------------------------------------------------
// Button events carry the FULL current button mask, not the changed bit
//
// Regression for the C7-seat stuck-button bug: axl-input used to emit button
// edges with ev.buttons = the single changed bit (e.g. always LEFT), but the
// AxlInputEvent contract documents .buttons as the CURRENT state (full mask)
// and MOUSE_MOVE already obeyed it. A consumer that derives the change by XOR
// (the compositor seat: ptr_buttons ^ ev->buttons) needs the full mask, or a
// release (LEFT ^ LEFT == 0) dispatches nothing and the button sticks down.
// Assert a press then release emits DOWN(buttons=LEFT) then UP(buttons=0).
// ---------------------------------------------------------------------------

static int g_press_calls;
static EFI_STATUS EFIAPI
mock_sp_getstate_press_release(EFI_SIMPLE_POINTER_PROTOCOL *This,
                               EFI_SIMPLE_POINTER_STATE *st)
{
    (void)This;
    st->RelativeMovementX = 0;
    st->RelativeMovementY = 0;
    st->RelativeMovementZ = 0;
    st->RightButton       = false;
    int c = g_press_calls++;
    if (c == 0) { st->LeftButton = true;  return 0; }   /* press   -> DOWN */
    if (c == 1) { st->LeftButton = false; return 0; }   /* release -> UP   */
    return 6;   /* EFI_NOT_READY thereafter */
}

static uint32_t g_down_mask, g_up_mask;
static int      g_down_seen, g_up_seen;
static bool
press_release_cb(const AxlInputEvent *ev, void *data)
{
    (void)data;
    if (ev->type == AXL_INPUT_MOUSE_BUTTON_DOWN) { g_down_mask = ev->buttons; g_down_seen++; }
    if (ev->type == AXL_INPUT_MOUSE_BUTTON_UP)   { g_up_mask   = ev->buttons; g_up_seen++; }
    return AXL_SOURCE_CONTINUE;
}

static void
test_button_edge_carries_full_mask(void)
{
    AxlEvent *wfi = axl_event_new();
    static EFI_SIMPLE_POINTER_PROTOCOL phys;
    phys.Reset        = mock_sp_reset;
    phys.GetState     = mock_sp_getstate_press_release;
    phys.WaitForInput = axl_event_handle(wfi);
    phys.Mode         = NULL;
    EFI_GUID   guid  = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_HANDLE physh = NULL;
    gBS->InstallProtocolInterface(&physh, &guid, EFI_NATIVE_INTERFACE, &phys);

    /* Remove competitor physical pointers so attach binds OUR mock (x64 OVMF
       binds a real usb-mouse that would otherwise win), then restore them. */
    EFI_HANDLE *hb = NULL; UINTN hc = 0;
    EFI_HANDLE  saved_h[8]; void *saved_i[8]; int nsaved = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &guid, NULL, &hc, &hb) == 0) {
        for (UINTN i = 0; i < hc && nsaved < 8; i++) {
            if (hb[i] == gST->ConsoleInHandle || hb[i] == physh) {
                continue;
            }
            void *iface = NULL;
            if (gBS->HandleProtocol(hb[i], &guid, &iface) == 0 && iface != NULL
                && gBS->UninstallProtocolInterface(hb[i], &guid, iface) == 0) {
                saved_h[nsaved] = hb[i]; saved_i[nsaved] = iface; nsaved++;
            }
        }
        if (hb != NULL) { gBS->FreePool(hb); }
    }

    g_press_calls = 0;
    g_down_mask = g_up_mask = 0;
    g_down_seen = g_up_seen = 0;
    AxlLoop *loop = axl_loop_new();
    AxlSourceId id = axl_input_attach_mouse(loop, press_release_cb, NULL);
    test_check(id != 0, "button_mask: mouse attach succeeds");

    /* Dispatch the press, then (re-signal) the release — one GetState per
       signal, since a consumed event source auto-clears. */
    axl_event_signal(wfi);
    (void)axl_loop_iterate_until(loop, NULL, 50000);
    axl_event_signal(wfi);
    (void)axl_loop_iterate_until(loop, NULL, 50000);

    test_check(g_down_seen == 1 && g_down_mask == AXL_INPUT_BUTTON_LEFT,
               "button_mask: DOWN carries the full current mask (LEFT held)");
    test_check(g_up_seen == 1 && g_up_mask == 0,
               "button_mask: UP carries the full current mask (no buttons), "
               "not the changed bit");

    axl_input_detach_mouse(loop);
    axl_loop_free(loop);
    gBS->UninstallProtocolInterface(physh, &guid, &phys);
    axl_event_free(wfi);
    for (int k = 0; k < nsaved; k++) {
        (void)gBS->InstallProtocolInterface(&saved_h[k], &guid,
                                            EFI_NATIVE_INTERFACE, saved_i[k]);
    }
}

// ---------------------------------------------------------------------------
// axl_virtual_pointer_* — install + drive a synthetic EFI_ABSOLUTE_POINTER on
// gST->ConsoleInHandle (where the firmware Setup browser reads it), inject a
// state, and consume it exactly as the browser would (HandleProtocol +
// GetState + WaitForInput). The browser's visual response is real-HW; this
// pins the substrate round-trip under QEMU.
// ---------------------------------------------------------------------------

static void
test_virtual_pointer(void)
{
    EFI_GUID apg = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;

    /* NULL-safety. */
    test_check(axl_virtual_pointer_install(NULL, NULL) == AXL_ERR,
               "vptr: install(NULL out) -> AXL_ERR");
    test_check(axl_virtual_pointer_inject(NULL, 0, 0, 0) == AXL_ERR,
               "vptr: inject(NULL) -> AXL_ERR");
    axl_virtual_pointer_uninstall(NULL);   /* must not crash */

    /* Baseline: whatever AbsolutePointer (if any) ConSplitter aggregates on
       ConsoleInHandle right now — the interface our install replaces. */
    void *orig = NULL;
    gBS->HandleProtocol(gST->ConsoleInHandle, &apg, &orig);

    AxlVirtualPointer      *vp  = NULL;
    AxlVirtualPointerConfig cfg = { .width = 800, .height = 600,
                                    .also_simple = false };
    test_check(axl_virtual_pointer_install(&vp, &cfg) == AXL_OK && vp != NULL,
               "vptr: install returns AXL_OK");

    /* Singleton: a second install is rejected. */
    AxlVirtualPointer *vp2 = NULL;
    test_check(axl_virtual_pointer_install(&vp2, NULL) == AXL_ERR && vp2 == NULL,
               "vptr: second install rejected (singleton)");

    /* Scroll requires also_simple (this vp was installed without it). */
    test_check(axl_virtual_pointer_scroll(NULL, 1) == AXL_ERR,
               "vptr: scroll(NULL) -> AXL_ERR");
    test_check(axl_virtual_pointer_scroll(vp, 1) == AXL_ERR,
               "vptr: scroll without also_simple -> AXL_ERR");

    /* ConsoleInHandle's AbsolutePointer is now OURS (and != the original). */
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
    test_check(!EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle, &apg,
                                              (void **)&ap)) && ap != NULL,
               "vptr: AbsolutePointer present on ConsoleInHandle after install");
    test_check((void *)ap != orig,
               "vptr: ConsoleInHandle AbsolutePointer replaced by ours");

    /* Mode range matches cfg (1:1 pixel mapping -> max = dim - 1). */
    test_check(ap->Mode != NULL
                   && ap->Mode->AbsoluteMinX == 0 && ap->Mode->AbsoluteMinY == 0
                   && ap->Mode->AbsoluteMaxX == 799
                   && ap->Mode->AbsoluteMaxY == 599,
               "vptr: Mode absolute range matches cfg (0..799 x 0..599)");

    /* Nothing injected yet: GetState NOT_READY, WaitForInput not signaled. */
    EFI_ABSOLUTE_POINTER_STATE st;
    test_check(ap->GetState(ap, &st) == EFI_NOT_READY,
               "vptr: GetState NOT_READY before inject");
    test_check(ap->WaitForInput != NULL
                   && gBS->CheckEvent(ap->WaitForInput) == EFI_NOT_READY,
               "vptr: WaitForInput not signaled before inject");

    /* Inject -> WaitForInput wakes, GetState returns the injected state. */
    test_check(axl_virtual_pointer_inject(vp, 123, 45, 0x1) == AXL_OK,
               "vptr: inject returns AXL_OK");
    test_check(gBS->CheckEvent(ap->WaitForInput) == EFI_SUCCESS,
               "vptr: WaitForInput signaled after inject");
    EFI_STATUS gs = ap->GetState(ap, &st);
    test_check(gs == EFI_SUCCESS && st.CurrentX == 123 && st.CurrentY == 45
                   && (st.ActiveButtons & 0x1) != 0,
               "vptr: GetState returns injected x=123 y=45 touch-active");

    /* GetState is one-shot per inject. */
    test_check(ap->GetState(ap, &st) == EFI_NOT_READY,
               "vptr: GetState NOT_READY after consuming the state");

    /* Out-of-range injection clamps to [0,w) x [0,h). */
    test_check(axl_virtual_pointer_inject(vp, 9999, 9999, 0) == AXL_OK,
               "vptr: inject out-of-range returns AXL_OK");
    ap->GetState(ap, &st);
    test_check(st.CurrentX == 799 && st.CurrentY == 599,
               "vptr: out-of-range injection clamps to the Mode max");

    /* Reset clears the pending state. */
    test_check(ap->Reset(ap, FALSE) == EFI_SUCCESS, "vptr: Reset returns SUCCESS");
    test_check(ap->GetState(ap, &st) == EFI_NOT_READY,
               "vptr: GetState NOT_READY after Reset");

    /* Uninstall restores the original ConsoleInHandle AbsolutePointer. */
    axl_virtual_pointer_uninstall(vp);
    void *after = NULL;
    gBS->HandleProtocol(gST->ConsoleInHandle, &apg, &after);
    test_check(after == orig,
               "vptr: uninstall restores the original ConsoleInHandle AbsolutePointer");

    /* Singleton slot freed -> a fresh install (default range) succeeds. */
    AxlVirtualPointer *vp3 = NULL;
    test_check(axl_virtual_pointer_install(&vp3, NULL) == AXL_OK,
               "vptr: re-install after uninstall succeeds");
    axl_virtual_pointer_uninstall(vp3);
}

// also_simple: publishes a relative EFI_SIMPLE_POINTER too; deltas come from
// successive absolute injects.
static void
test_virtual_pointer_simple(void)
{
    EFI_GUID spg = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    void    *orig_sp = NULL;
    gBS->HandleProtocol(gST->ConsoleInHandle, &spg, &orig_sp);

    AxlVirtualPointer      *vp  = NULL;
    AxlVirtualPointerConfig cfg = { .width = 640, .height = 480,
                                    .also_simple = true };
    test_check(axl_virtual_pointer_install(&vp, &cfg) == AXL_OK && vp != NULL,
               "vptr-sp: install with also_simple");

    EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
    test_check(!EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle, &spg,
                                              (void **)&sp)) && sp != NULL,
               "vptr-sp: SimplePointer present on ConsoleInHandle");
    test_check((void *)sp != orig_sp,
               "vptr-sp: ConsoleInHandle SimplePointer replaced by ours");

    EFI_SIMPLE_POINTER_STATE st;
    test_check(sp->GetState(sp, &st) == EFI_NOT_READY,
               "vptr-sp: GetState NOT_READY before inject");

    /* First inject sets the baseline position + button level (no delta yet). */
    axl_virtual_pointer_inject(vp, 100, 100, 0x1);
    test_check(sp->GetState(sp, &st) == EFI_SUCCESS && st.LeftButton,
               "vptr-sp: first inject reports the button level");

    /* Second inject -> relative delta = new - previous. */
    axl_virtual_pointer_inject(vp, 110, 130, 0x0);
    EFI_STATUS gs = sp->GetState(sp, &st);
    test_check(gs == EFI_SUCCESS
                   && st.RelativeMovementX == 10 && st.RelativeMovementY == 30,
               "vptr-sp: relative delta = +10, +30");
    test_check(!st.LeftButton, "vptr-sp: button released in second state");
    test_check(sp->GetState(sp, &st) == EFI_NOT_READY,
               "vptr-sp: delta consumed (NOT_READY until next inject)");

    /* Scroll wheel -> RelativeMovementZ (what attach_mouse decodes to
       MOUSE_WHEEL / wheel_dy). */
    test_check(axl_virtual_pointer_scroll(vp, 3) == AXL_OK,
               "vptr-sp: scroll returns AXL_OK");
    test_check(sp->GetState(sp, &st) == EFI_SUCCESS && st.RelativeMovementZ == 3,
               "vptr-sp: scroll -> RelativeMovementZ = 3");
    /* Notches accumulate until consumed. */
    axl_virtual_pointer_scroll(vp, 2);
    axl_virtual_pointer_scroll(vp, -1);
    sp->GetState(sp, &st);
    test_check(st.RelativeMovementZ == 1,
               "vptr-sp: scroll deltas accumulate (2 + -1 = 1)");

    axl_virtual_pointer_uninstall(vp);
    void *after = NULL;
    gBS->HandleProtocol(gST->ConsoleInHandle, &spg, &after);
    test_check(after == orig_sp,
               "vptr-sp: uninstall restores the original SimplePointer");
}

// End-to-end: install a virtual pointer, attach the REAL touch consumer to it,
// inject a press/move/release, pump the loop, and assert the events arrive at
// the callback through the full path (WaitForInput source -> GetState -> emit
// -> recognizer). This is the pointer-delivery coverage QMP injection can't
// provide on a headless runner — the virtual pointer is firmware-internal.
typedef struct {
    AxlInputType type[32];
    int32_t      x[32];
    int32_t      y[32];
    uint32_t     click[32];
    size_t       n;
} EvLog;

static bool
ev_collect_cb(const AxlInputEvent *ev, void *data)
{
    EvLog *l = (EvLog *)data;
    if (l->n < 32) {
        l->type[l->n]  = ev->type;
        l->x[l->n]     = ev->x;
        l->y[l->n]     = ev->y;
        l->click[l->n] = ev->click_count;
        l->n++;
    }
    return true;   /* AXL_SOURCE_CONTINUE */
}

/* Drain every ready loop source (non-blocking) — process all events an inject
   produced. */
static void
ev_pump(AxlLoop *loop)
{
    for (int i = 0; i < 64; i++) {
        if (axl_loop_dispatch(loop, false) != 0) {
            break;   /* nothing ready */
        }
    }
}

static bool
ev_log_has(const EvLog *l, AxlInputType t)
{
    for (size_t i = 0; i < l->n; i++) {
        if (l->type[i] == t) {
            return true;
        }
    }
    return false;
}

static void
test_virtual_pointer_e2e_touch(void)
{
    AxlVirtualPointer      *vp  = NULL;
    AxlVirtualPointerConfig cfg = { .width = 1000, .height = 1000,
                                    .also_simple = false };
    test_check(axl_virtual_pointer_install(&vp, &cfg) == AXL_OK && vp != NULL,
               "vptr-e2e: install virtual abs pointer");

    AxlLoop *loop = axl_loop_new();
    EvLog    log  = { .n = 0 };
    /* attach_touch binds gST->ConsoleInHandle first (console_only default) —
       exactly where the virtual pointer lives. */
    AxlSourceId src = axl_input_attach_touch(loop, ev_collect_cb, &log);
    test_check(src != 0,
               "vptr-e2e: attach_touch binds the virtual AbsolutePointer");

    /* Press at (250,250), move to (750,500) held, release. */
    axl_virtual_pointer_inject(vp, 250, 250, 0x1);
    ev_pump(loop);
    axl_virtual_pointer_inject(vp, 750, 500, 0x1);
    ev_pump(loop);
    axl_virtual_pointer_inject(vp, 750, 500, 0x0);
    ev_pump(loop);

    test_check(ev_log_has(&log, AXL_INPUT_TOUCH_DOWN),
               "vptr-e2e: injected press -> TOUCH_DOWN delivered through the loop");
    test_check(ev_log_has(&log, AXL_INPUT_TOUCH_MOVE),
               "vptr-e2e: injected move -> TOUCH_MOVE delivered");
    test_check(ev_log_has(&log, AXL_INPUT_TOUCH_UP),
               "vptr-e2e: injected release -> TOUCH_UP delivered");

    /* The injected position is normalized [0,AXL_INPUT_ABS_RANGE) from the Mode
       range: x=750 of 1000 lands in the right half (> 0x8000). */
    int32_t last_move_x = -1;
    for (size_t i = 0; i < log.n; i++) {
        if (log.type[i] == AXL_INPUT_TOUCH_MOVE) {
            last_move_x = log.x[i];
        }
    }
    test_check(last_move_x > (int32_t)(AXL_INPUT_ABS_RANGE / 2),
               "vptr-e2e: TOUCH_MOVE x normalized to the right half (injected 750/1000)");

    axl_input_detach_touch(loop);
    axl_loop_free(loop);
    axl_virtual_pointer_uninstall(vp);
}

// Substrate-level coverage of the inject -> WaitForInput loop-wake -> GetState
// delivery of relative movement, buttons, and the scroll wheel: drive OUR
// virtual SimplePointer directly as a loop event source. The full
// attach_mouse path (which now binds ConsoleInHandle-first and so DOES bind the
// virtual pointer) is covered by test_virtual_pointer_e2e_mouse_wheel below;
// this isolates the GetState->delta decode from the binding.
typedef struct {
    EFI_SIMPLE_POINTER_PROTOCOL *sp;
    int32_t                      dx, dy, dz;
    bool                         left;
    int                          reads;
} SpSink;

static bool
sp_sink_cb(void *data)
{
    SpSink *s = (SpSink *)data;
    EFI_SIMPLE_POINTER_STATE st;
    if (s->sp->GetState(s->sp, &st) == EFI_SUCCESS) {
        s->dx  += st.RelativeMovementX;
        s->dy  += st.RelativeMovementY;
        s->dz  += st.RelativeMovementZ;
        s->left = st.LeftButton;
        s->reads++;
    }
    return true;   /* AXL_SOURCE_CONTINUE */
}

static void
test_virtual_pointer_e2e_mouse(void)
{
    AxlVirtualPointer      *vp  = NULL;
    AxlVirtualPointerConfig cfg = { .width = 800, .height = 600,
                                    .also_simple = true };
    test_check(axl_virtual_pointer_install(&vp, &cfg) == AXL_OK && vp != NULL,
               "vptr-mouse: install virtual pointer with also_simple");

    EFI_GUID spg = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
    test_check(!EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle, &spg,
                                              (void **)&sp)) && sp != NULL,
               "vptr-mouse: virtual SimplePointer on ConsoleInHandle");

    AxlLoop *loop = axl_loop_new();
    SpSink   sink = { .sp = sp };
    AxlSourceId src  = axl_loop_add_event(loop, sp->WaitForInput, sp_sink_cb, &sink);
    test_check(src != 0, "vptr-mouse: SimplePointer WaitForInput bound as a loop source");

    axl_virtual_pointer_inject(vp, 100, 100, 0x1);   /* baseline + left down */
    ev_pump(loop);
    axl_virtual_pointer_inject(vp, 150, 130, 0x1);   /* relative move +50,+30 */
    ev_pump(loop);
    axl_virtual_pointer_scroll(vp, 4);               /* wheel +4 */
    ev_pump(loop);

    test_check(sink.reads >= 1,
               "vptr-mouse: injects woke the loop via WaitForInput (GetState delivered)");
    test_check(sink.dx == 50 && sink.dy == 30,
               "vptr-mouse: relative movement delivered through the loop (+50,+30)");
    test_check(sink.dz == 4,
               "vptr-mouse: scroll wheel delivered through the loop (dz=4)");
    test_check(sink.left,
               "vptr-mouse: button level delivered through the loop");

    axl_loop_remove_source(loop, src);
    axl_loop_free(loop);
    axl_virtual_pointer_uninstall(vp);
}

// ---------------------------------------------------------------------------
// Virtual-pointer SCROLL surfaces as MOUSE_WHEEL through the REAL attach_mouse
// consumer (not a direct WaitForInput sink). Regression for the
// axl-sdk-vpointer-wheel-handoff: the virtual pointer publishes its
// EFI_SIMPLE_POINTER on gST->ConsoleInHandle (where a BMC remote-console
// mouse's events arrive), but attach_mouse used to bind via
// locate_physical_pointer, which SKIPS ConsoleInHandle in favour of a physical
// device — so a virtual scroll()/move never reached the consumer. attach_mouse
// now binds ConsoleInHandle-first (like attach_touch), delivering both.
//
// An idle physical mock SimplePointer on a non-ConsoleIn handle makes the OLD
// binding deterministically pick a non-virtual device on BOTH arches (x64 also
// has the runner's usb-mouse; AAVMF would otherwise fall back to the virtual
// one), so this fails RED on the unfixed path rather than passing by luck.
// ---------------------------------------------------------------------------

static int32_t g_vpw_wheel_dy;
static int     g_vpw_wheel, g_vpw_move;
static bool
vpw_cb(const AxlInputEvent *ev, void *data)
{
    (void)data;
    if (ev->type == AXL_INPUT_MOUSE_WHEEL) {
        g_vpw_wheel_dy = ev->wheel_dy;
        g_vpw_wheel++;
    } else if (ev->type == AXL_INPUT_MOUSE_MOVE) {
        g_vpw_move++;
    }
    return true;   /* AXL_SOURCE_CONTINUE */
}

static EFI_STATUS EFIAPI
mock_sp_getstate_idle(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *st)
{
    (void)This;
    (void)st;
    return 6;   /* EFI_NOT_READY: present but never has data — a competitor that
                   must not absorb the bind away from the virtual pointer. */
}

static void
test_virtual_pointer_e2e_mouse_wheel(void)
{
    /* Idle physical mock on a non-ConsoleIn handle: the OLD attach_mouse
       binding (locate_physical_pointer) picks this over the virtual pointer on
       every arch, so without the fix the scroll/move is never delivered. */
    EFI_GUID  guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    AxlEvent *wfi  = axl_event_new();
    test_check(wfi != NULL, "vptr-wheel: fixture WaitForInput created");
    static EFI_SIMPLE_POINTER_PROTOCOL phys;
    phys.Reset        = mock_sp_reset;
    phys.GetState     = mock_sp_getstate_idle;
    phys.WaitForInput = axl_event_handle(wfi);
    phys.Mode         = NULL;
    EFI_HANDLE physh = NULL;
    gBS->InstallProtocolInterface(&physh, &guid, EFI_NATIVE_INTERFACE, &phys);
    test_check(physh != gST->ConsoleInHandle,
               "vptr-wheel: idle physical mock on a non-ConsoleIn handle");

    AxlVirtualPointer      *vp  = NULL;
    AxlVirtualPointerConfig cfg = { .width = 800, .height = 600,
                                    .also_simple = true };
    test_check(axl_virtual_pointer_install(&vp, &cfg) == AXL_OK && vp != NULL,
               "vptr-wheel: install virtual pointer with also_simple");

    g_vpw_wheel = g_vpw_move = 0;
    g_vpw_wheel_dy = 0;
    AxlLoop    *loop = axl_loop_new();
    AxlSourceId id   = axl_input_attach_mouse(loop, vpw_cb, NULL);
    test_check(id != 0, "vptr-wheel: attach_mouse returns a valid source");

    /* Two injects produce a relative move; then a scroll notch. Pump after
       each so the WaitForInput source dispatches. */
    axl_virtual_pointer_inject(vp, 100, 100, 0x0);   /* baseline */
    ev_pump(loop);
    axl_virtual_pointer_inject(vp, 140, 100, 0x0);   /* +40 in X -> MOUSE_MOVE */
    ev_pump(loop);
    test_check(axl_virtual_pointer_scroll(vp, +1) == AXL_OK,
               "vptr-wheel: scroll(+1) accepted");
    ev_pump(loop);

    test_check(g_vpw_move >= 1,
               "vptr-wheel: relative inject -> MOUSE_MOVE delivered to attach_mouse");
    test_check(g_vpw_wheel >= 1 && g_vpw_wheel_dy == +1,
               "vptr-wheel: scroll(+1) -> MOUSE_WHEEL wheel_dy==+1 to attach_mouse");

    axl_input_detach_mouse(loop);
    axl_loop_free(loop);
    axl_virtual_pointer_uninstall(vp);
    gBS->UninstallProtocolInterface(physh, &guid, &phys);
    axl_event_free(wfi);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_input_main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

    test_print_header("AxlTestInput");

    test_input_type_values_distinct();
    test_button_bits_distinct();
    test_modifier_bits_distinct();
    test_modifier_lr_and_masks();
    test_event_struct_field_init();
    test_event_designated_init();

    test_attach_mouse_null_loop_returns_zero();
    test_attach_mouse_null_cb_returns_zero();
    test_attach_mouse_protocol_available();
    test_attach_mouse_ifaces();

    test_attach_key_null_loop_returns_zero();
    test_attach_key_null_cb_returns_zero();
    test_attach_key_success_and_already_attached();

    test_attach_touch_null_loop_returns_zero();
    test_attach_touch_null_cb_returns_zero();
    test_attach_touch_protocol_available();
    test_touch_coalesce();

    test_virtual_pointer();
    test_virtual_pointer_simple();
    test_virtual_pointer_e2e_touch();
    test_virtual_pointer_e2e_mouse();
    test_virtual_pointer_e2e_mouse_wheel();

    test_ctrl_letter_serial_folded();
    test_ctrl_letter_keyboard_letter_plus_mod();
    test_ctrl_letter_excludes_edit_keys();
    test_ctrl_letter_non_chord();

    test_abs_normalize_endpoints();
    test_abs_normalize_offset_range();
    test_abs_normalize_clamps_and_degenerate();

    test_gesture_click_count();
    test_gesture_drag();
    test_key_debounce();
    test_key_gate();
    test_button_repeat();

    test_modifier_tracking();
    test_pointer_carries_modifiers();
    test_button_edge_carries_full_mask();

    return test_print_results();
}

AXL_APP(test_input_main)
