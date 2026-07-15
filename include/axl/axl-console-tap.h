/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-tap.h
    Tap the firmware console as STRUCTURED operations, and inject input.

    The console tap is the firmware surgery that lets an AXL application host the
    **real UEFI Shell** (or any console app, via @ref axl_shell_launch) — including
    full-screen apps like the Shell's `edit` — while a consumer observes and drives
    its console. It wraps `gST->ConOut`/`ConIn`(/`StdErr`) and the `ConsoleInHandle`'s
    SimpleTextInputEx, and reports every console *output* call to a consumer-supplied
    @ref AxlConsoleOps vtable; *input* injected by the consumer is pushed into the
    console's key queue and the Shell's blocked reader is woken.

    **Ops, not bytes.** `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` has no wire format: a
    full-screen app just calls `SetCursorPosition` / `OutputString` / `SetAttribute`
    / `ClearScreen`. A *remote* consumer serializes those to a VT/ANSI byte stream
    for an xterm-class terminal — that is exactly what @ref axl_console_mirror_install
    does, as a tap consumer. A *local* renderer binds these ops straight into its
    cell grid, with no encode→parse round trip (and can therefore express state,
    like the alternate screen, that the VT wire can only approximate).

    The tap owns the parts a consumer must not have to re-derive: the wrapped
    protocol structs, an owned `SIMPLE_TEXT_OUTPUT_MODE` (the cursor/attribute state
    the Shell reads back), the remote-geometry override, alt-screen tracking, the
    key-injection ring, and the `WaitForKey` survivor rule.

    Single global console ⇒ single tap: a second install while one is active fails.

    @code
    static void grid_put(void *u, const char *utf8, size_t n) { term_feed(u, utf8, n); }
    static void grid_pen(void *u, const AxlConsolePen *p) { term_set_pen(u, p); }
    static int  grid_prop(void *u, AxlConsoleProp prop, const AxlConsoleValue *v) {
        if (prop == AXL_CONSOLE_PROP_ALT_SCREEN) term_alt_screen(u, v->u.boolean);
        return 1;   // accept everything, including what we ignore
    }
    static const AxlConsoleOps ops = {
        .output_text = grid_put, .set_pen = grid_pen, .set_term_prop = grid_prop,
    };

    AxlConsoleTap *tap;
    AxlConsoleTapConfig cfg = {
        .cols = 80, .rows = 25,
        .passthrough_local = false,   // we render it; don't paint the firmware console
        .auto_alt_screen   = true,    // a nested full-screen app enters the alt buffer
        .input_capture     = true,    // WE own the key queue; only injected keys reach it
    };
    axl_console_tap_install(&tap, &ops, my_term, &cfg);
    axl_loop_attach_driver(loop, 10);   // pump our loop while the Shell blocks
    axl_shell_launch(NULL);             // real Shell in the foreground; blocks
    axl_console_tap_uninstall(tap);
    @endcode
**/

#ifndef AXL_CONSOLE_TAP_H
#define AXL_CONSOLE_TAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-console-ops.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque console-tap instance.
 *
 * Created by @ref axl_console_tap_install, torn down by
 * @ref axl_console_tap_uninstall. There is one global console, so a tap is
 * effectively a singleton: a second install while one is active fails.
 */
typedef struct AxlConsoleTap AxlConsoleTap;

/**
 * @brief Console-tap configuration. A zeroed config is safe and behaves like the
 *     untouched firmware console (passthrough on, no alt-screen heuristic, the
 *     physical keyboard readable).
 */
typedef struct {
    uint32_t cols;              ///< consumer terminal width  (0 = use the physical console's)
    uint32_t rows;              ///< consumer terminal height (0 = use the physical console's)
    bool     passthrough_local; ///< also write to the physical console. When FALSE the
                                ///< tap is the only console writer, so it OWNS and
                                ///< maintains the `SIMPLE_TEXT_OUTPUT_MODE` the guest
                                ///< reads back (cursor, attribute, visibility).
    bool     auto_alt_screen;   ///< best-effort bracket of an opaque nested full-screen
                                ///< app (SIMPLE_TEXT_OUTPUT cannot express the alt-screen,
                                ///< so it is inferred): ENTER on a backward cursor jump
                                ///< after a `ClearScreen` (a TUI repainting), LEAVE on a
                                ///< newline (linear shell flow resumes). Safe to mirror a
                                ///< shell from boot -- a bare prompt clear never enters. A
                                ///< consumer that owns its own TUI leaves this false and
                                ///< drives the explicit enter/leave API.
    bool     input_capture;     ///< capture input exclusively: the wrapped ConIn/ConInEx
                                ///< serve ONLY injected keys and `WaitForKey[Ex]` signal
                                ///< only on injected content; the physical key queue is
                                ///< never read. Set this when the consumer drains the
                                ///< firmware key queue itself, so the guest can't steal
                                ///< or double its keys. Default false = passthrough.
} AxlConsoleTapConfig;

/**
 * @brief Install the tap: wrap the system console, report ops to @p ops.
 *
 * Saves `gST->ConOut`/`ConIn`(/`StdErr`) and the `ConsoleInHandle`'s
 * SimpleTextInputEx, swaps in AXL wrappers, and begins reporting console
 * operations. From here the firmware — and any app the caller starts in the
 * foreground (e.g. via @ref axl_shell_launch) — talks to the wrappers.
 *
 * The tap does *not* create a pump. Drive your loop in the background (e.g.
 * `axl_loop_attach_driver`) so the consumer keeps running while the foreground
 * app blocks the thread. An atexit hook restores the console if the process exits
 * without an explicit uninstall.
 *
 * @return AXL_OK on success (`*out` set). AXL_ERR on bad arguments (NULL @p out /
 *     @p ops / @p cfg), if a tap is already installed, or on allocation /
 *     event-creation failure.
 */
int
axl_console_tap_install(
    AxlConsoleTap             **out,   ///< [out] receives the tap handle
    const AxlConsoleOps        *ops,   ///< consumer callbacks (copied by pointer; must outlive the tap)
    void                       *user,  ///< opaque context passed back to every callback
    const AxlConsoleTapConfig  *cfg    ///< configuration (copied)
);

/**
 * @brief Restore the original console protocols and free the tap. NULL-safe.
 *
 * Always pair with @ref axl_console_tap_install.
 */
void
axl_console_tap_uninstall(
    AxlConsoleTap *t  ///< tap handle (NULL-safe)
);

/**
 * @brief Inject one keystroke into the guest's console input.
 *
 * UEFI key shape: a printable key sets @p unicode (and @p scan == 0); a special
 * key sets @p scan (and @p unicode == 0) — e.g. Up=0x01, Down=0x02, Right=0x03,
 * Left=0x04, Home=0x05, End=0x06, Delete=0x08, PageUp=0x09, PageDown=0x0A,
 * F1=0x0B, F2=0x0C … F12=0x16, Esc=0x17. The key is pushed into the wrapped ConIn
 * ring and `WaitForKey` is signalled, so a Shell blocked in `WaitForEvent` wakes.
 *
 * Equivalent to @ref axl_console_tap_inject_key_ex with no modifier state. To
 * reach a guest's Ctrl- or Alt-qualified key notify (the Shell's Ctrl+C, for
 * one), use that function instead — a registration naming a modifier will not
 * match a key injected without one.
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p t or a full ring.
 */
int
axl_console_tap_inject_key(
    AxlConsoleTap *t,        ///< tap handle
    uint16_t       scan,     ///< UEFI scan code (0 for printable keys)
    uint16_t       unicode   ///< UCS-2 char (0 for special keys)
);

/**
 * @brief Inject one keystroke together with its modifier / toggle state.
 *
 * Same as @ref axl_console_tap_inject_key, but the key also carries an
 * `EFI_KEY_STATE`, which is what a guest's `RegisterKeyNotify` matches against
 * and what `ReadKeyStrokeEx` reports back.
 *
 * **This is how you break a nested Shell.** When @c input_capture is set the tap
 * owns the console's key queue *and* its notify registry, and UEFI fires notifies
 * at queue-insert time. The Shell registers Ctrl+C four ways — @a unicode of
 * `'c'` or `3`, each crossed with @ref AXL_CONSOLE_LEFT_CONTROL_PRESSED and
 * @ref AXL_CONSOLE_RIGHT_CONTROL_PRESSED, all OR'd with
 * @ref AXL_CONSOLE_SHIFT_STATE_VALID. The match rule (UEFI 2.11 §12.2.5) compares
 * @a scan and @a unicode exactly, then treats a registered shift/toggle state of 0
 * as "don't care" and any nonzero one as an exact-match requirement. So a bare
 * `0x03` with @a shift_state 0 will *not* trigger it; inject `unicode = 3` with
 * `AXL_CONSOLE_SHIFT_STATE_VALID | AXL_CONSOLE_LEFT_CONTROL_PRESSED` (the
 * `EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED` bits).
 *
 * Without @c input_capture the firmware still owns the queue and its own
 * notifies; an injected key is readable but fires nothing.
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p t or a full ring.
 */
int
axl_console_tap_inject_key_ex(
    AxlConsoleTap *t,             ///< tap handle
    uint16_t       scan,          ///< UEFI scan code (0 for printable keys)
    uint16_t       unicode,       ///< UCS-2 char (0 for special keys)
    uint32_t       shift_state,   ///< EFI_KEY_STATE.KeyShiftState bits, or 0 for none
    uint8_t        toggle_state   ///< EFI_KEY_STATE.KeyToggleState bits, or 0 for none
);

/**
 * @brief Inject a run of terminal input bytes (xterm/VT), decoded to keystrokes.
 *
 * Decodes CSI/SS3 escape sequences (arrows, F-keys, Home/End, PgUp/PgDn, Delete)
 * and UTF-8 printables (BMP) into the keystrokes of
 * @ref axl_console_tap_inject_key. Each call is self-contained: feed whole escape
 * / multi-byte sequences in a single call. A sequence left open at end-of-call is
 * treated as a bare Esc plus literal bytes rather than held into the next call, so
 * a dropped final byte can't corrupt the next keystroke.
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p t / NULL @p bytes.
 */
int
axl_console_tap_inject_text(
    AxlConsoleTap *t,      ///< tap handle
    const char    *bytes,  ///< terminal input bytes (xterm/VT)
    size_t         len     ///< number of bytes
);

/**
 * @brief Update the consumer's terminal size.
 *
 * The wrapped `QueryMode` / `Mode` report this size, so full-screen apps lay
 * themselves out for the consumer's terminal rather than the physical console.
 * Zero values fall back to the physical console size. NULL-safe.
 */
void
axl_console_tap_set_size(
    AxlConsoleTap *t,     ///< tap handle (NULL-safe)
    uint32_t       cols,  ///< terminal width
    uint32_t       rows   ///< terminal height
);

/**
 * @brief Read the tap's resolved terminal geometry.
 *
 * Returns the size the wrapped console currently reports: the configured remote
 * size from @ref axl_console_tap_set_size, or the physical console's own size for
 * any axis left 0. Either output may be 0 if unconfigured and the physical size
 * is unavailable. NULL-safe (both outputs set to 0 when @p t is NULL); either
 * output pointer may be NULL to ignore that axis.
 */
void
axl_console_tap_get_size(
    const AxlConsoleTap *t,     ///< tap handle (NULL-safe)
    uint32_t            *cols,  ///< [out] resolved width (may be NULL)
    uint32_t            *rows   ///< [out] resolved height (may be NULL)
);

/**
 * @brief Reset per-session tap state. NULL-safe.
 *
 * Drains the key ring, clears cursor / escape-decoder tracking, and leaves the
 * alternate screen if in it. Use between Shell restarts or when a new consumer
 * attaches so stale state doesn't bleed through.
 */
void
axl_console_tap_reset(
    AxlConsoleTap *t  ///< tap handle (NULL-safe)
);

/**
 * @brief Enter the alternate screen: reports
 *     `set_term_prop(user, AXL_CONSOLE_PROP_ALT_SCREEN, &(bool))` once.
 *
 * The alternate screen is the off-scrollback buffer a full-screen app should
 * paint into. `SIMPLE_TEXT_OUTPUT` cannot express it, so the tap *asserts* it —
 * either explicitly through this call, or heuristically via
 * @ref AxlConsoleTapConfig::auto_alt_screen. Idempotent (a second call while
 * already inside reports nothing) and NULL-safe.
 */
void
axl_console_tap_enter_alt_screen(
    AxlConsoleTap *t  ///< tap handle (NULL-safe)
);

/**
 * @brief Leave the alternate screen: reports
 *     `set_term_prop(user, AXL_CONSOLE_PROP_ALT_SCREEN, &(bool))` once.
 *
 * Idempotent (a call while not inside reports nothing) and NULL-safe.
 */
void
axl_console_tap_leave_alt_screen(
    AxlConsoleTap *t  ///< tap handle (NULL-safe)
);

/**
 * @brief Whether the tap is currently in the alternate screen.
 *
 * @return true if inside (an enter was reported without a matching leave); false
 *     otherwise, or when @p t is NULL.
 */
bool
axl_console_tap_in_alt_screen(
    const AxlConsoleTap *t  ///< tap handle (NULL-safe)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_TAP_H */
