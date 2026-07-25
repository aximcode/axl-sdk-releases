/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-device.h
    Take-over console producer: from a resident `load`-ed driver's DriverEntry,
    become the SOLE ConOut (and optionally ConInEx) of a UEFI Shell already running
    at the prompt — no child shell, no `gST->ConOut` reassignment, no NVRAM write.
    Delivers every console output call as a structured @ref AxlConsoleOps op for a
    consumer (e.g. AGT's `AgtTerminal`) to render.

    **The swap-strategy sibling is @ref axl_console_tap_install.** The tap swaps the
    `gST` console pointers and is right when the installer is the foreground and then
    `StartImage`s a child shell, or runs at BDS. The device never touches
    `gST->ConOut`'s value — which is exactly why it can interpose on a shell already
    running at the prompt, the case the tap cannot serve. Both producers report the
    same @ref AxlConsoleOps, so a consumer binds either unchanged.

    **How the take-over works** (no NVRAM, no `gST` swap): from `DriverEntry` at
    `TPL_APPLICATION`, `InstallMultipleProtocolInterfaces` publishes our own
    `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` + a Vendor device path + a self-installed
    `gEfiConsoleOutDeviceGuid` tag on a fresh handle; `ConnectController(ours)` makes
    EDK2's ConSplitter fan console output to us; then the tag is uninstalled from the
    OTHER console-out handles, so ConSplitter drops GraphicsConsole from the fan-out.
    It survives `load`'s `ConnectAllEfi` because uninstalling the tag disconnects
    ConSplitter (the tag's consumer), not ConPlatform (which holds the SimpleTextOut
    open) — so GraphicsConsole is never re-tagged.

    **Portability:** this relies on EDK2's console-device model (ConSplitter, the
    `gEfiConsoleOutDeviceGuid` tag, ConPlatform binding), which is not UEFI-spec.
    EDK2-family firmware (OVMF, AMI Aptio, Insyde) is fine; verify per target. See
    `AXL-Console-Device-Design.md`.
**/

#ifndef AXL_CONSOLE_DEVICE_H
#define AXL_CONSOLE_DEVICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-console-ops.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque take-over console-device instance. Singleton (one console). */
typedef struct AxlConsoleDevice AxlConsoleDevice;

/**
 * @brief Device configuration. A zeroed config = physical geometry, output-only
 *     (input passthrough to the firmware keyboard), no alt-screen heuristic.
 */
typedef struct {
    uint32_t cols;              ///< geometry our SimpleTextOut advertises (0 = physical)
    uint32_t rows;              ///< (0 = physical)
    bool     take_input;        ///< also become the sole ConInEx (input relay): evict the
                                ///< firmware keyboard from the console-in split so the guest
                                ///< reads ONLY keys delivered through us (the @ref
                                ///< axl_console_device_inject_key family and/or the internal
                                ///< read loop below). false = output-only, the physical
                                ///< keyboard still drives the shell.
    bool     read_physical;     ///< (take_input only) run an internal timer that reads the
                                ///< evicted firmware keyboard and re-injects its keys, so a
                                ///< resident take-over with no foreground input loop still
                                ///< gets live typing. false = the device is a pure sink; the
                                ///< consumer feeds keys via inject_*.
    uint32_t input_poll_ms;     ///< (read_physical only) internal read-timer period in ms;
                                ///< 0 = a sensible default.
    uint32_t debounce_ms;       ///< (read_physical only) drop a repeat of the SAME key within
                                ///< this window. 0 = OFF (default). A conservative
                                ///< key-repeat gate, tunable once the real-HW A/B lands; NOT
                                ///< claimed to cure the KVM key bounce (see
                                ///< AXL-Console-Device-Design.md §5 / kbtune §7).
    uint32_t min_gap_ms;        ///< (read_physical only) drop ANY key arriving within this
                                ///< gap of the previous one. 0 = OFF (default). Same gating
                                ///< caveat as @a debounce_ms.
    bool (*key_filter)(void *user, const void *key);
                                ///< (read_physical only) peek each physical key the read loop
                                ///< reads BEFORE it is forwarded to the shell; return true to
                                ///< CONSUME it (the shell never sees it) -- e.g. a terminal
                                ///< claiming Shift+PgUp for scrollback. NULL = forward all.
                                ///< @a key points at the firmware key record (scan code /
                                ///< unicode / shift state); a consumer that needs the fields
                                ///< casts it to its UEFI key type (kept void* so this header
                                ///< stays free of UEFI types).
    void    *key_filter_user;   ///< opaque context passed to @a key_filter
    bool     auto_alt_screen;   ///< best-effort bracket of a nested full-screen app: enter
                                ///< on a backward cursor jump after a ClearScreen, leave on
                                ///< a newline. Same semantics as
                                ///< AxlConsoleTapConfig::auto_alt_screen.
    bool     passthrough_local; ///< keep the FIRMWARE consoles in the fan-out instead of
                                ///< evicting them, so GraphicsConsole (and serial) carry on
                                ///< painting the local display while we also receive every
                                ///< op. Default false = sole console, which is right when
                                ///< the consumer OWNS the framebuffer (it renders the grid
                                ///< itself, so a co-painting GraphicsConsole would fight it).
                                ///< Set true when the consumer only OBSERVES — mirroring the
                                ///< console to a remote viewer — where evicting would blank
                                ///< the local monitor and freeze anything sampling the GOP
                                ///< (a BMC's KVM stream). REQUIRES physical geometry:
                                ///< @a cols and @a rows must both be 0, or install fails.
                                ///< Two consoles painting one screen must agree on the grid,
                                ///< and advertising a size the firmware console does not
                                ///< share re-opens the ConsoleLogger stale-scrollback
                                ///< deadloop this mode otherwise avoids. Consequence: a
                                ///< remote viewer adapts to the console's size, and
                                ///< @ref axl_console_device_set_size is ignored here. To
                                ///< honour a far-end resize WITHOUT leaving reader mode,
                                ///< switch text modes: a passthrough device mirrors the
                                ///< physical console's mode list, so
                                ///< @ref axl_console_text_find_mode +
                                ///< @ref axl_console_text_set_mode reshape BOTH consoles
                                ///< together and the new geometry arrives on
                                ///< @ref AxlConsoleOps::resize. Snapping to the firmware's
                                ///< enumerated modes is a rounding, not a limit.
    bool     take_pointer;      ///< take over the pointer: at install, uninstall every real
                                ///< EFI_SIMPLE_POINTER_PROTOCOL from the handle database and
                                ///< interpose ONE yielding proxy in their place, so a guest
                                ///< running under us (the UEFI `edit`) reads only the proxy.
                                ///< The proxy fixes two problems at once: (1) it and the guest
                                ///< no longer both drain the same consume-once real pointer, so
                                ///< the guest's mouse cursor tracks correctly; (2) its GetState
                                ///< blocks briefly (a guest busy-polls GetState -> the poll HLTs
                                ///< and its loop stops spinning a core), and it forwards a real
                                ///< pointer's movement, so the cursor still works. The evicted
                                ///< real interfaces stay valid (their drivers are NOT stopped)
                                ///< and are cached + exposed via
                                ///< @ref axl_console_device_pointer_count /
                                ///< @ref axl_console_device_pointer_iface, so the CONSUMER keeps
                                ///< direct pointer access (feed them to
                                ///< @ref axl_input_attach_mouse_ifaces for its own scroll /
                                ///< drag-select / copy). Proxy removed + real pointers restored
                                ///< on uninstall. false = leave the pointer alone. Independent
                                ///< of @a take_input.
} AxlConsoleDeviceConfig;

/**
 * @brief Install the take-over device from a resident driver's DriverEntry
 *     (requires TPL_APPLICATION).
 *
 * Publishes our SimpleTextOut + a Vendor device path + a self-installed
 * `gEfiConsoleOutDeviceGuid` tag on a fresh handle, connects it so ConSplitter fans
 * to us, then evicts the other console-out devices — unless
 * @ref AxlConsoleDeviceConfig::passthrough_local, which keeps them in the fan-out so
 * the local display keeps painting. `gST->ConOut`'s pointer value is
 * never touched; no NVRAM write. Output is reported as @p ops. The device OWNS the
 * `SIMPLE_TEXT_OUTPUT_MODE` (cursor / attribute / visibility); caret rendering is the
 * consumer's job. Reports @ref AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT via
 * `set_cell_rule` once at bind. Singleton: a second install fails.
 *
 * Finishes with one `gST->ConOut->SetMode` to re-sync any UEFI-Shell ConsoleLogger
 * stacked above us to our (possibly non-80x25) geometry — without it a taller grid
 * overflows the logger's stale scrollback bound and it deadloops on scroll. This
 * routes a `clear_screen` op to the consumer at take-over (expected).
 *
 * @return AXL_OK (`*out` set) or AXL_ERR (bad args, already installed, surgery failed,
 *     or @ref AxlConsoleDeviceConfig::passthrough_local combined with an explicit
 *     @c cols / @c rows).
 */
int
axl_console_device_install(
    const AxlConsoleOps           *ops,   ///< consumer callbacks (borrowed; must outlive the device)
    void                          *user,  ///< opaque context passed back to every callback
    const AxlConsoleDeviceConfig  *cfg,   ///< configuration (copied)
    AxlConsoleDevice             **out    ///< [out] receives the device handle
);

/**
 * @brief Undo the take-over: re-tag + reconnect the evicted firmware consoles (the
 *     local display comes back), disconnect ConSplitter from our handle so it drops
 *     our SimpleTextOut from the fan-out, then uninstall our protocols and free.
 *     NULL-safe.
 *
 * The disconnect is load-bearing: ConSplitter holds a raw pointer to our
 * SimpleTextOut (which lives in the freed instance), so without it the next
 * `gST->ConOut` write jumps through a freed vtable. See AXL-Console-Device-Design.md.
 */
void
axl_console_device_uninstall(
    AxlConsoleDevice *dev  ///< device handle (NULL-safe)
);

/* --- Input relay (effective only when cfg.take_input=true) --------------------
 * Mirrors the tap's inject family exactly (same key shape, same Ctrl+letter fold via
 * shift_state, same VT decode). Implemented in the input increment; when
 * take_input=false these return AXL_ERR. */

/** @brief Inject one keystroke. See @ref axl_console_tap_inject_key. */
int
axl_console_device_inject_key(AxlConsoleDevice *dev, uint16_t scan, uint16_t unicode);

/** @brief Inject one keystroke with modifier / toggle state. See
 *     @ref axl_console_tap_inject_key_ex. */
int
axl_console_device_inject_key_ex(AxlConsoleDevice *dev, uint16_t scan, uint16_t unicode,
                                 uint32_t shift_state, uint8_t toggle_state);

/** @brief Inject a run of terminal input bytes (xterm/VT). See
 *     @ref axl_console_tap_inject_text. */
int
axl_console_device_inject_text(AxlConsoleDevice *dev, const char *bytes, size_t len);

/* --- Runtime geometry + session (output side) -------------------------------- */

/**
 * @brief Update advertised terminal size; wrapped QueryMode/Mode report it.
 *     0 = physical. NULL-safe.
 *
 * **Ignored (with a warning) under @ref AxlConsoleDeviceConfig::passthrough_local**,
 * where the firmware console is painting the same screen and the grid is not ours
 * alone to set — moving only our half of that agreement is the desync passthrough
 * exists to avoid, which is also why install refuses an explicit geometry there.
 * A co-painting consumer reshapes with @ref axl_console_text_set_mode instead: the
 * device mirrors the physical mode list, so a switch moves BOTH consoles and reports
 * the new size through @ref AxlConsoleOps::resize.
 */
void
axl_console_device_set_size(AxlConsoleDevice *dev, uint32_t cols, uint32_t rows);

/**
 * @brief Read the device's resolved terminal geometry — the size our advertised
 *     mode reports, i.e. what the guest lays itself out for.
 *
 * The configured @ref AxlConsoleDeviceConfig::cols / @c rows, with any axis left 0
 * resolved to the physical console's size at install (and 80x25 if that was
 * unavailable). **A @ref AxlConsoleDeviceConfig::passthrough_local consumer needs
 * this:** passthrough forces geometry to physical, so the resolved size is not
 * something the caller passed in, and a consumer that guessed 80x25 would size its
 * screen model wrong. It also TRACKS a passthrough reshape — read it once after
 * install and let @ref AxlConsoleOps::resize maintain it, or re-read it after a
 * @ref axl_console_text_set_mode; both report the same geometry. NULL-safe (both outputs set to 0 when @p dev is NULL); either
 * output pointer may be NULL to ignore that axis. Mirrors
 * @ref axl_console_tap_get_size.
 */
void
axl_console_device_get_size(
    const AxlConsoleDevice *dev,   ///< device handle (NULL-safe)
    uint32_t               *cols,  ///< [out] resolved width (may be NULL)
    uint32_t               *rows   ///< [out] resolved height (may be NULL)
);

/** @brief Reset per-session state (key ring, cursor/escape tracking, leave
 *     alt-screen). NULL-safe. */
void
axl_console_device_reset(AxlConsoleDevice *dev);

/* --- Pointer take-over (effective only when cfg.take_pointer=true) ------------
 * At install the device uninstalls every EFI_SIMPLE_POINTER_PROTOCOL from the
 * handle database (so guests run mouse-free) and caches the interfaces here; at
 * uninstall it reinstalls them. Between install and uninstall the CONSUMER reads
 * the cached interfaces directly (they outlive the database uninstall) to keep its
 * own pointer features -- typically by handing them to
 * @ref axl_input_attach_mouse_ifaces. When take_pointer=false the count is 0. */

/**
 * @brief How many EFI_SIMPLE_POINTER_PROTOCOL interfaces the device evicted and now
 *     holds for the consumer. 0 when take_pointer was false, no pointer was present,
 *     or @p dev is NULL.
 * @return the cached pointer-interface count.
 */
size_t
axl_console_device_pointer_count(
    const AxlConsoleDevice *dev   ///< device handle (NULL-safe -> 0)
);

/**
 * @brief Borrow the @p index-th cached pointer interface (an
 *     `EFI_SIMPLE_POINTER_PROTOCOL *`, returned as `void *` so this header stays
 *     free of UEFI types). Valid until @ref axl_console_device_uninstall restores it.
 * @return the interface pointer, or NULL if @p dev is NULL or @p index is out of
 *     range (>= @ref axl_console_device_pointer_count).
 */
void *
axl_console_device_pointer_iface(
    const AxlConsoleDevice *dev,   ///< device handle (NULL-safe -> NULL)
    size_t                  index  ///< 0-based, < axl_console_device_pointer_count(dev)
);

/* --- Alt-screen (mirrors the tap) -------------------------------------------- */

/** @brief Enter the alternate screen: reports `set_term_prop(ALT_SCREEN, true)`. */
void
axl_console_device_enter_alt_screen(AxlConsoleDevice *dev);

/** @brief Leave the alternate screen: reports `set_term_prop(ALT_SCREEN, false)`. */
void
axl_console_device_leave_alt_screen(AxlConsoleDevice *dev);

/** @brief Whether the device is currently in the alternate screen. NULL-safe. */
bool
axl_console_device_in_alt_screen(const AxlConsoleDevice *dev);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_DEVICE_H */
