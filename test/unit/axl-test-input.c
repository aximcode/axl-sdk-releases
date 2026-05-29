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
    test_check((AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_CTRL) == 0 &&
               (AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_ALT)  == 0 &&
               (AXL_INPUT_MOD_SHIFT & AXL_INPUT_MOD_META) == 0 &&
               (AXL_INPUT_MOD_CTRL  & AXL_INPUT_MOD_ALT)  == 0 &&
               (AXL_INPUT_MOD_CTRL  & AXL_INPUT_MOD_META) == 0 &&
               (AXL_INPUT_MOD_ALT   & AXL_INPUT_MOD_META) == 0,
               "modifier bitfield: shift/ctrl/alt/meta pairwise disjoint");
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

static void
test_attach_mouse_protocol_available(void)
{
    /* QEMU's OVMF firmware publishes EFI_SIMPLE_POINTER_PROTOCOL via
       the virtual usb-mouse / ps2-mouse device.  Verify the attach
       succeeds (returns non-zero source ID) when the protocol is
       available, the loop callback is registered, and a follow-up
       attach is rejected (single-mouse-per-process limit).

       On hardware without a mouse this test would need to be guarded
       — for now QEMU + OVMF is the baseline. */
    AxlLoop *loop = axl_loop_new();
    uint32_t id = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id != 0,
               "attach_mouse: returns non-zero source ID when "
               "EFI_SIMPLE_POINTER_PROTOCOL available (QEMU baseline)");

    /* Second attach must fail per the documented single-source limit. */
    uint32_t id2 = axl_input_attach_mouse(loop, unused_cb, NULL);
    test_check(id2 == 0,
               "attach_mouse: second attach returns 0 "
               "(only one mouse source per process)");

    axl_loop_free(loop);
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

    axl_loop_free(loop);
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

    return test_print_results();
}

AXL_APP(test_input_main)
