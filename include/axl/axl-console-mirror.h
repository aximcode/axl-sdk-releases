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
 * @brief Reset per-session mirror state. NULL-safe.
 *
 * Drains the key ring and clears cursor / escape-decoder tracking. Use
 * between Shell restarts or when a new client attaches so stale state
 * from the previous session doesn't bleed through. (Alt-screen
 * enter/leave arrives with the full-screen P2 work.)
 */
void
axl_console_mirror_reset(
    AxlConsoleMirror *m  ///< mirror handle (NULL-safe)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_MIRROR_H */
