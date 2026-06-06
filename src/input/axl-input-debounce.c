/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-input-debounce.c
    Keyboard debounce / repeat suppression for axl-input.

    Over a high-latency remote console (iDRAC Virtual Console, IPMI SOL,
    AMT) a single intended keypress can register as held long enough that
    the firmware's typematic fires, so one character arrives several
    times. UEFI exposes no way to turn firmware typematic off, so the fix
    is a software filter above SimpleTextIn: drop a same-key KEY_DOWN that
    repeats faster than a human would.

    Pure timing logic over the key stream — axl_input_attach_key runs one
    of these internally when enabled; the core is exported
    (axl_input_key_accept) for reuse and deterministic unit testing.

    Off by default (preserves today's behavior). Char-aware: by default
    the filter applies only to printable characters, leaving navigation /
    editing keys (whose unicode is 0 or a control code) free to repeat so
    held-key navigation still works.
**/

#include <axl/axl-input.h>

// Global tuning (see axl_input_set_key_debounce). 0 = disabled.
static uint32_t g_key_debounce_ms     = 0;
static bool     g_debounce_printable  = true;

void
axl_input_set_key_debounce(
    uint32_t  min_repeat_ms,
    bool      printable_only
    )
{
    g_key_debounce_ms    = min_repeat_ms;
    g_debounce_printable = printable_only;
}

bool
axl_input_key_accept(
    AxlKeyDebounce  *d,
    AxlInputEvent   *ev
    )
{
    // Never silently eat input on a bad call, and only debounce key
    // presses — mouse / wheel / touch and key-up-less protocols pass.
    if (d == NULL || ev == NULL || ev->type != AXL_INPUT_KEY_DOWN) {
        return true;
    }

    bool drop = false;
    if (g_key_debounce_ms != 0) {
        // Printable = a visible character; control codes (Backspace 0x08,
        // Tab, Enter) and pure scancodes (arrows: unicode 0) are
        // navigation/editing keys that should keep their firmware repeat.
        bool printable = ev->unicode >= 0x20 && ev->unicode != 0x7F;
        if (!g_debounce_printable || printable) {
            bool same = ev->keycode == d->last_keycode
                     && ev->unicode == d->last_unicode;
            uint64_t window_us = (uint64_t)g_key_debounce_ms * 1000u;
            if (same && (ev->timestamp_us - d->last_us) < window_us) {
                drop = true;
            }
        }
    }

    // Track the last key seen (accepted or dropped) so a sustained repeat
    // stream keeps being suppressed, not just its second event.
    d->last_keycode = ev->keycode;
    d->last_unicode = ev->unicode;
    d->last_us      = ev->timestamp_us;

    return !drop;
}
