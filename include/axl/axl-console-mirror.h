/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-mirror.h
    Mirror the firmware console to a byte sink and inject remote input.

    Lets a loop-owning AXL application host the **real UEFI Shell** (or
    any console app, via @ref axl_shell_launch) with its console
    transparently mirrored to — and driven from — a remote terminal,
    including full-screen interactive apps like the Shell's `edit`.

    The mirror wraps the system console protocols
    (`gST->ConIn`/`ConOut`/`StdErr`): every console *output* the
    foreground app makes is translated to a terminal byte stream (UTF-8
    text + ANSI/VT control sequences) and handed to a caller-supplied
    sink; *input* injected from a remote terminal is pushed into the
    console's key queue and the Shell's blocked reader is woken. The
    physical console keeps working in parallel (local keyboard input
    falls through; local output is mirrored too unless disabled).

    This is the reusable, finicky firmware surgery; transport and policy
    (which shell, terminal size from the browser, the sink⇄WebSocket
    bridge, RBAC) belong to the consumer. See `docs/AXL-Console-Mirror-
    Design.md`.

    @code
    static void to_ws(const char *bytes, size_t len, void *user) {
        axl_ws_send(user, bytes, len);   // ship to the browser terminal
    }
    AxlConsoleMirror *m;
    AxlConsoleMirrorConfig cfg = {
        .sink = to_ws, .user = conn, .cols = 80, .rows = 25,
        .passthrough_local = true,
    };
    axl_console_mirror_install(&m, &cfg);
    axl_loop_attach_driver(loop, 10);    // HTTP/WS pumped in the background
    axl_shell_launch(NULL);              // real Shell in the foreground; blocks
    axl_loop_detach_driver(loop);
    axl_console_mirror_uninstall(m);
    @endcode
**/

#ifndef AXL_CONSOLE_MIRROR_H
#define AXL_CONSOLE_MIRROR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-console-screen.h>   /* AxlConsoleScreenSink, composed for snapshot() */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque console-mirror instance.
 *
 * Created by @ref axl_console_mirror_install, torn down by
 * @ref axl_console_mirror_uninstall. There is one global console, so a
 * mirror is effectively a singleton: a second install while one is
 * active fails.
 */
typedef struct AxlConsoleMirror AxlConsoleMirror;

/**
 * @brief Sink for the mirrored console output stream.
 *
 * Receives console output already translated to a terminal byte stream
 * (UTF-8 text interleaved with ANSI/VT control sequences) suitable for
 * an xterm.js / VT100 terminal. Called from within the wrapped console
 * output path — keep it cheap and non-blocking (enqueue / async send);
 * do not re-enter the mirror from here.
 *
 * @p bytes is not NUL-terminated; honor @p len.
 */
typedef void (*AxlConsoleSinkFn)(
    const char *bytes,  ///< output bytes (NOT NUL-terminated)
    size_t      len,    ///< number of bytes
    void       *user    ///< sink context (cfg->user)
);

/**
 * @brief Console-mirror configuration.
 */
typedef struct {
    AxlConsoleSinkFn sink;              ///< receives the mirrored output stream
    void            *user;              ///< sink context, passed back verbatim
    uint32_t         cols;              ///< remote terminal width  (e.g. 80)
    uint32_t         rows;              ///< remote terminal height (e.g. 25)
    bool             passthrough_local; ///< also write to the physical console
    bool             auto_alt_screen;   ///< best-effort bracket of the terminal alt-screen
                                        ///< (see @ref axl_console_mirror_enter_alt_screen):
                                        ///< ENTER on a backward cursor jump after a
                                        ///< `ClearScreen` (a TUI repainting), LEAVE on a
                                        ///< newline (linear flow). Safe to mirror a shell
                                        ///< from boot. For a consumer hosting an opaque
                                        ///< nested full-screen app; a consumer that owns
                                        ///< its own TUI leaves this false and drives
                                        ///< the explicit enter/leave API instead.
    bool             input_capture;     ///< capture input exclusively: the wrapped
                                        ///< ConIn/ConInEx serve ONLY the inject ring
                                        ///< and never read the physical key queue.
                                        ///< Default false = the physical keyboard
                                        ///< passes through (today's behavior — a
                                        ///< zeroed config is safe). Set true when
                                        ///< another owner drains the firmware key
                                        ///< queue and injects (AGT / axterm), so the
                                        ///< nested shell can't steal/double its keys.
                                        ///< (This is the opt-in inverse of the
                                        ///< handoff's proposed `input_passthrough`:
                                        ///< `input_capture = true` == that ask's
                                        ///< `input_passthrough = false`, but
                                        ///< zero-init keeps existing consumers safe.)
} AxlConsoleMirrorConfig;

/**
 * @brief Install the mirror: wrap the system console, route output to the sink.
 *
 * Saves `gST->ConIn`/`ConOut`(`/StdErr`) and the `ConsoleInHandle`'s
 * SimpleTextInputEx, swaps in AXL wrappers, and begins routing console
 * output to @p cfg->sink. From here the firmware — and any app the
 * caller starts in the foreground (e.g. via @ref axl_shell_launch) —
 * talks to the wrappers. Pair every install with
 * @ref axl_console_mirror_uninstall; a registry/atexit hook restores the
 * console if the process exits without an explicit uninstall.
 *
 * The mirror does *not* create a pump. Drive your loop in the background
 * (e.g. `axl_loop_attach_driver`) so the sink's transport keeps
 * running while the foreground app blocks the thread.
 *
 * @return AXL_OK on success (`*out` set). AXL_ERR on bad arguments
 *     (NULL @p out / @p cfg / `cfg->sink`), if a mirror is already
 *     installed, or on allocation / event-creation failure.
 */
int
axl_console_mirror_install(
    AxlConsoleMirror             **out,  ///< [out] receives the mirror handle
    const AxlConsoleMirrorConfig  *cfg   ///< configuration (copied)
);

/**
 * @brief Restore the original console protocols. NULL-safe.
 *
 * Always pair with @ref axl_console_mirror_install. After this returns,
 * `gST->ConIn`/`ConOut`/`StdErr` and the `ConsoleInHandle`
 * SimpleTextInputEx are the originals again and the mirror handle is
 * freed.
 */
void
axl_console_mirror_uninstall(
    AxlConsoleMirror *m  ///< mirror handle (NULL-safe)
);

/**
 * @brief Inject one keystroke from the remote terminal.
 *
 * UEFI key shape: a printable key sets @p unicode (and @p scan == 0); a
 * special key sets @p scan (and @p unicode == 0) — e.g. Up=0x01,
 * Down=0x02, Right=0x03, Left=0x04, Home=0x05, End=0x06, Delete=0x08,
 * PageUp=0x09, PageDown=0x0A, F1=0x0B, F2=0x0C … F12=0x16, Esc=0x17.
 * The key is pushed into the wrapped ConIn ring and the WaitForKey
 * event is signalled, so a Shell blocked in `WaitForEvent` wakes and
 * reads it.
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p m or a full ring.
 */
int
axl_console_mirror_inject_key(
    AxlConsoleMirror *m,        ///< mirror handle
    uint16_t          scan,     ///< UEFI scan code (0 for printable keys)
    uint16_t          unicode   ///< UCS-2 char (0 for special keys)
);

/**
 * @brief Inject a run of terminal input bytes (xterm/VT), decoded to keys.
 *
 * Decodes CSI/SS3 escape sequences (arrows, F-keys, Home/End,
 * PgUp/PgDn, Delete) and UTF-8 printables (BMP) from a raw terminal
 * byte stream into the keystrokes of @ref axl_console_mirror_inject_key.
 * This is the reusable half that makes `edit` usable from a browser:
 * feed raw xterm.js keydown bytes straight in.
 *
 * Each call is self-contained: feed whole escape / multi-byte UTF-8
 * sequences in a single call (xterm.js delivers a complete sequence per
 * keypress). A sequence left open at end-of-call is treated as a bare
 * Esc plus literal bytes rather than held into the next call, so a
 * dropped final byte can't corrupt the next keystroke.
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p m / NULL @p bytes.
 */
int
axl_console_mirror_inject_text(
    AxlConsoleMirror *m,      ///< mirror handle
    const char       *bytes,  ///< terminal input bytes (xterm/VT)
    size_t            len     ///< number of bytes
);

/**
 * @brief Update the remote terminal size (browser resize).
 *
 * The wrapped `QueryMode` / `Mode` report this size, so full-screen
 * apps lay themselves out for the remote terminal rather than the
 * physical console. Zero values fall back to the physical console size.
 */
void
axl_console_mirror_set_size(
    AxlConsoleMirror *m,     ///< mirror handle (NULL-safe)
    uint32_t          cols,  ///< remote terminal width
    uint32_t          rows   ///< remote terminal height
);

/**
 * @brief Serialize the mirror's current screen as a self-contained VT repaint.
 *
 * The late-join counterpart to the live stream: when a new client connects
 * mid-session it has missed everything already on screen. Rather than replay a
 * raw byte tail (which cannot recover screen contents, the cursor, or the
 * alternate-screen selection), call this to emit — through @p sink — one burst of
 * VT bytes that, applied to a **blank terminal of the mirror's current size**,
 * reproduces exactly what the console shows right now: the visible glyphs and
 * colours, the cursor position and visibility, and the primary/alternate-screen
 * state. Point @p sink at the new client, then join it to the live stream.
 *
 * The mirror keeps an internal @ref AxlConsoleScreen fed from the same VT stream
 * it emits, so the repaint stays authoritative automatically — it tracks the
 * geometry the mirror reports (@ref axl_console_mirror_set_size) and the
 * alternate-screen transitions the mirror drives, with no consumer-side parallel
 * parser to keep in sync. The output is coalesced (blank cells and rows emit
 * nothing, so a mostly-empty 80x25 is a handful of bytes) and the model is left
 * unchanged, so this may be called once per connecting client.
 *
 * Like the live stream, the snapshot reflects only what the mirror has observed
 * since it was installed — it cannot reconstruct console content written before
 * the tap was in place. See @ref axl_console_screen_snapshot for the underlying
 * repaint format.
 *
 * @param m    mirror handle.
 * @param sink receives the serialized VT bytes in one or more chunks; the
 *     concatenation is the whole repaint. Structurally identical to
 *     @ref AxlConsoleSinkFn, so one sink function serves both APIs.
 * @param user opaque context passed back to @p sink.
 * @return AXL_OK once the repaint has been handed to @p sink; AXL_ERR on a NULL
 *     @p m / @p sink, in which case nothing is written to @p sink.
 */
int
axl_console_mirror_snapshot(
    AxlConsoleMirror     *m,     ///< mirror handle
    AxlConsoleScreenSink  sink,  ///< receives the serialized repaint
    void                 *user   ///< opaque context for the sink
);

/**
 * @brief Enter the remote terminal's alternate screen (emits `ESC[?1049h`).
 *
 * The alt-screen is the off-scrollback buffer a full-screen app (a nested
 * `edit`, a TUI) should paint into, so its full-screen clears and cursor
 * addressing never dump into the terminal's scrollback history. Emits the
 * DECSET 1049 enter sequence to the sink once; idempotent (a second call while
 * already in the alt-screen emits nothing) and NULL-safe.
 *
 * Drive this explicitly when the consumer knows it is entering a full-screen
 * mode, or set @ref AxlConsoleMirrorConfig::auto_alt_screen to infer it (enter on
 * a backward cursor jump after a `ClearScreen`, leave on a newline).
 */
void
axl_console_mirror_enter_alt_screen(
    AxlConsoleMirror *m  ///< mirror handle (NULL-safe)
);

/**
 * @brief Leave the remote terminal's alternate screen (emits `ESC[?1049l`).
 *
 * Restores the terminal's normal screen + scrollback. Emits the DECRST 1049
 * leave sequence to the sink once; idempotent (a call while not in the
 * alt-screen emits nothing) and NULL-safe.
 */
void
axl_console_mirror_leave_alt_screen(
    AxlConsoleMirror *m  ///< mirror handle (NULL-safe)
);

/**
 * @brief Whether the mirror is currently in the alternate screen.
 *
 * @return true if inside the alt-screen (an enter has been emitted without a
 *     matching leave); false otherwise, or when @p m is NULL.
 */
bool
axl_console_mirror_in_alt_screen(
    const AxlConsoleMirror *m  ///< mirror handle (NULL-safe)
);

/**
 * @brief Reset per-session mirror state. NULL-safe.
 *
 * Drains the key ring, clears cursor / escape-decoder tracking, and leaves the
 * alternate screen if in it (emits `ESC[?1049l`), so stale state from the
 * previous session doesn't bleed through. Use between Shell restarts or when a
 * new client attaches.
 */
void
axl_console_mirror_reset(
    AxlConsoleMirror *m  ///< mirror handle (NULL-safe)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_MIRROR_H */
