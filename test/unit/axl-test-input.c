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
       The same helper backs attach_mouse and attach_touch. */
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
    uint32_t id = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id != 0, "attach_mouse: returns non-zero source ID");
    uint32_t id2 = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id2 == 0, "attach_mouse: second attach returns 0 "
                         "(only one mouse source per process)");

    /* Detach frees the single-mouse slot (and removes the caller-owned
       WaitForInput source) so later tests can re-attach. */
    axl_input_detach_mouse(loop);
    uint32_t id3 = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id3 != 0, "attach_mouse: re-attach succeeds after detach");
    axl_input_detach_mouse(loop);

    axl_loop_free(loop);
    gBS->UninstallProtocolInterface(physh, &guid, &phys);
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
    uint32_t id = axl_input_attach_key(loop, unused_cb, NULL);
    test_check(id != 0,
               "attach_key: first attach returns non-zero source ID");

    uint32_t id2 = axl_input_attach_key(loop, unused_cb, NULL);
    test_check(id2 == 0,
               "attach_key: second attach returns 0 (single-source limit)");

    /* Detach frees the per-process slot so a re-attach succeeds — the
       mirror of the mouse case, and what the compositor seat handoff to a
       modal dialog's compositor relies on (C7). */
    axl_input_detach_key(loop);
    uint32_t id3 = axl_input_attach_key(loop, unused_cb, NULL);
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
    uint32_t id = axl_input_attach_touch(loop, unused_cb, NULL);
    test_check(id != 0,
               "attach_touch: returns non-zero source ID when "
               "EFI_ABSOLUTE_POINTER_PROTOCOL available (QEMU baseline)");

    uint32_t id2 = axl_input_attach_touch(loop, unused_cb, NULL);
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
    uint32_t id3 = axl_input_attach_touch(loop, unused_cb, NULL);
    test_check(id3 != 0,
               "attach_touch: re-attach after detach succeeds "
               "(handle/notify re-bind lifecycle)");
    axl_input_detach_touch(loop);

    axl_loop_free(loop);
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
    uint32_t id = axl_input_attach_mouse(loop, btn_repeat_cb, NULL);
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
    uint32_t id = axl_input_attach_mouse(loop, modtest_cb, NULL);
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
    uint32_t id = axl_input_attach_mouse(loop, press_release_cb, NULL);
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

    test_attach_key_null_loop_returns_zero();
    test_attach_key_null_cb_returns_zero();
    test_attach_key_success_and_already_attached();

    test_attach_touch_null_loop_returns_zero();
    test_attach_touch_null_cb_returns_zero();
    test_attach_touch_protocol_available();

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
    test_button_repeat();

    test_modifier_tracking();
    test_pointer_carries_modifiers();
    test_button_edge_carries_full_mask();

    return test_print_results();
}

AXL_APP(test_input_main)
