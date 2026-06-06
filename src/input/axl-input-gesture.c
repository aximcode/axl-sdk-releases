/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-input-gesture.c
    Click-gesture recognizer for axl-input.

    Pure timing/distance logic over pointer-button events: annotates each
    event with a click_count (single / double / triple) and a dragging
    latch. axl_input_attach_mouse runs one of these internally so every
    mouse consumer gets the same notion of "double-click" and "drag"; the
    recognizer is also exported (axl_input_gesture_feed) for consumers
    with their own event stream and for deterministic unit testing.

    Tunables are global (one pointer per process for v0.1).
**/

#include <axl/axl-input.h>

// Global tuning (see axl_input_set_click_tuning). Defaults: a 400 ms
// multi-click window and a 4 px drag threshold — conventional desktop
// values, and the same threshold doubles as the double-click position
// tolerance.
static uint32_t g_multi_click_ms     = 400;
static int32_t  g_drag_threshold_px  = 4;

void
axl_input_set_click_tuning(
    uint32_t  multi_click_ms,
    int32_t   drag_threshold_px
    )
{
    g_multi_click_ms    = (multi_click_ms == 0)     ? 400 : multi_click_ms;
    g_drag_threshold_px = (drag_threshold_px == 0)  ? 4   : drag_threshold_px;
}

// True if (ax,ay) is within g_drag_threshold_px of (bx,by). Squared
// compare keeps it integer-only (no sqrt) and overflow-safe in int64.
static bool
within_threshold(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    int64_t dx = (int64_t)ax - bx;
    int64_t dy = (int64_t)ay - by;
    int64_t thr = g_drag_threshold_px;
    return dx * dx + dy * dy <= thr * thr;
}

void
axl_input_gesture_feed(
    AxlGesture     *g,
    AxlInputEvent  *ev
    )
{
    if (g == NULL || ev == NULL) {
        return;
    }

    uint64_t window_us = (uint64_t)g_multi_click_ms * 1000u;

    switch (ev->type) {
    case AXL_INPUT_MOUSE_BUTTON_DOWN: {
        // Extend the click streak only if this press is close enough in
        // both time and space to the previous one; otherwise restart.
        bool continues = g->streak > 0
            && (ev->timestamp_us - g->last_down_us) <= window_us
            && within_threshold(ev->x, ev->y, g->last_down_x, g->last_down_y);
        g->streak       = continues ? g->streak + 1 : 1;
        g->last_down_us = ev->timestamp_us;
        g->last_down_x  = ev->x;
        g->last_down_y  = ev->y;
        g->held         = ev->buttons;
        g->press_x      = ev->x;
        g->press_y      = ev->y;
        g->dragging     = false;       // a fresh press is never mid-drag
        ev->click_count = g->streak;
        ev->dragging    = false;
        break;
    }

    case AXL_INPUT_MOUSE_BUTTON_UP:
        // Report the drag state on the release that ends it (so a
        // consumer can tell "drag finished" from "plain click"), then
        // clear the latch once nothing is held.
        ev->click_count = 0;
        ev->dragging    = g->dragging;
        g->held         = ev->buttons;
        if (g->held == 0) {
            g->dragging = false;
        }
        break;

    case AXL_INPUT_MOUSE_MOVE:
        ev->click_count = 0;
        // Latch a drag once a held press moves past the threshold.
        if (g->held != 0 && !g->dragging
            && !within_threshold(ev->x, ev->y, g->press_x, g->press_y)) {
            g->dragging = true;
        }
        ev->dragging = g->dragging;
        break;

    default:
        // Wheel / keyboard / touch events carry no click or drag meaning.
        ev->click_count = 0;
        ev->dragging    = false;
        break;
    }
}
