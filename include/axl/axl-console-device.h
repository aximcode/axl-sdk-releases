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

    The handle also carries the `gEfiStandardErrorDeviceGuid` tag, so ConSplitter
    fans standard error (`gST->StdErr`, where a tool's error lines go) to us too,
    as it does to a screen listed in `ErrOut`. A take-over moves the screen off
    StdErr with ConOut: on firmware whose `ErrOut` lists the display, each SCREEN
    console it evicts (a handle carrying a GOP or UGA, i.e. GraphicsConsole)
    loses its StdErr tag too, so an error line is drawn once, by us, not a second
    time over our picture. Every other StdErr device -- the serial terminal --
    keeps its tag, and uninstall puts a screen's StdErr tag back with its ConOut
    one. A passthrough_local device evicts nothing, so it joins StdErr only.

    **Portability:** this relies on EDK2's console-device model (ConSplitter, the
    `gEfiConsoleOutDeviceGuid` / `gEfiStandardErrorDeviceGuid` tags, ConPlatform
    binding), which is not UEFI-spec.
    EDK2-family firmware (OVMF, AMI Aptio, Insyde) is fine; verify per target. See
    `AXL-Console-Device-Design.md`.
**/

#ifndef AXL_CONSOLE_DEVICE_H
#define AXL_CONSOLE_DEVICE_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

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
                                ///< Key notifies: EDK2's ConSplitter does not unregister
                                ///< anything from a console-in device it drops, so an
                                ///< evicted keyboard would keep every notify registered
                                ///< before the take-over (the Shell's Ctrl-C/Ctrl-S) while
                                ///< the registrant's unregister reaches only the members
                                ///< left; after `exit` freed the Shell, the keyboard's next
                                ///< 'c' would call freed code. So eviction removes from
                                ///< each keyboard the notifies ConSplitter registered on
                                ///< it -- the ones it registered on us when we joined --
                                ///< by registering the same (key, function) pair there and
                                ///< unregistering the handle that returns. While evicted,
                                ///< the keyboard's keys reach the guest through our ring,
                                ///< which fires our own copies; on uninstall ConSplitter
                                ///< re-registers its current list on the restored
                                ///< keyboard. This relies on EDK2's input drivers
                                ///< returning the EXISTING handle for an identical pair
                                ///< (TerminalConInRegisterKeyNotify, the PS/2
                                ///< KeyboardRegisterKeyNotify and
                                ///< USBKeyboardRegisterKeyNotify all do). On a driver that
                                ///< adds a duplicate instead, the duplicate is removed and
                                ///< the original stays -- no worse than without this.
    bool     read_physical;     ///< (take_input only) run an internal timer that reads the
                                ///< evicted firmware keyboard and re-injects its keys, so a
                                ///< resident take-over with no foreground input loop still
                                ///< gets live typing. Each key fires the guest's key
                                ///< notifies as its own keyboard driver would: a key with no
                                ///< shift state (a serial terminal) matches by key alone, so
                                ///< a serial Ctrl-C (0x03) still breaks the Shell. false =
                                ///< the device is a pure sink; the consumer feeds keys via
                                ///< inject_*.
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
    bool (*key_filter)(void *user, const void *key) AXL_CB_NOEXCEPT;
                                ///< (read_physical only) peek each physical key the read loop
                                ///< reads BEFORE it is forwarded to the shell -- as it is read,
                                ///< also while the guest is not reading; return true to
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
 * Publishes our SimpleTextOut + a Vendor device path + self-installed
 * `gEfiConsoleOutDeviceGuid` and `gEfiStandardErrorDeviceGuid` tags on a fresh
 * handle, connects it so ConSplitter fans console output and standard error to
 * us, then evicts the other console-out devices (never a StdErr device) — unless
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
 *
 * **Never frees under a call in flight.** The device counts the firmware's
 * calls into it that have not returned yet -- every method of its
 * SimpleTextOut, SimpleTextIn/InEx and pointer proxy, from its first statement
 * to its return, which spans every @ref AxlConsoleOps callback the call makes.
 * While one is live, uninstall changes NOTHING and returns #AXL_BUSY. That is
 * the case of a loop callback or timer at `TPL_CALLBACK` that preempted the
 * Shell part-way through its own output at `TPL_APPLICATION` (an op callback
 * of yours is suspended mid-way, and freeing would resume it in freed
 * memory), and of an uninstall made from inside an op callback. Retry from a
 * later point, such as the next tick: the preempted call finishes as soon as
 * the preempting callback returns. Keep the ops' target alive until uninstall
 * returns #AXL_OK. A call the firmware had already dispatched to a method
 * when the uninstall preempted it -- before the method's first statement --
 * finds no device when it resumes, returns `EFI_DEVICE_ERROR` without
 * touching the freed instance (TestString `EFI_UNSUPPORTED`, and the key
 * notify pair `EFI_INVALID_PARAMETER`, the statuses their specs define),
 * and leaves the count balanced. The pointer
 * proxy's GetState (installed only with @c take_pointer) waits up to 4 ms
 * inside the call, so while a guest polls it -- the UEFI `edit` does,
 * continuously -- a call is in flight nearly all the time and a retry from
 * a tick sees #AXL_BUSY almost every time, until that guest exits. Retry
 * for as long as it takes; do not assume it converges within a few ticks.
 * At `TPL_APPLICATION` outside every op callback -- a driver's Unload -- no
 * call can be in flight, so #AXL_BUSY does not occur there.
 *
 * @return #AXL_OK when the device is gone, and for a NULL @p dev (nothing to
 *     do); #AXL_BUSY when a call into the device is in flight -- nothing
 *     changed, call again later; #AXL_INVALID when @p dev is not the
 *     installed device; #AXL_ERR when the firmware would not let go --
 *     the instance is then leaked deliberately rather than freed under a
 *     holder, and a later call resumes from where this one stopped. What
 *     the refusal leaves depends on where it came:
 *     - ConSplitter could not be disconnected from our handle: the
 *       console stays taken and the firmware keyboards stay evicted, so
 *       the @c read_physical loop runs again and keys keep reaching the
 *       guest through the device. On EDK2 this is rare: its
 *       DisconnectController returns `EFI_NOT_FOUND` when NO binding
 *       stopped (a total refusal), which counts as a successful
 *       disconnect here, so an error comes only from a bad parameter or
 *       a resource failure -- and the latter can come after some bindings
 *       stopped, when the ConIn aggregate may already have dropped us and
 *       keys the loop reads reach no guest until a retry succeeds.
 *     - Our console protocols could not be uninstalled (ConIn or ConOut):
 *       the firmware consoles and keyboards are already restored, so the
 *       console is NOT taken any more and input reaches the guest through
 *       them; only the instance is kept, for the protocols still pointing
 *       into it. This is where an EDK2 total refusal is caught: a binding
 *       that would not stop still holds our protocols, so their uninstall
 *       is refused.
 */
AXL_WARN_UNUSED int
axl_console_device_uninstall(
    AxlConsoleDevice *dev  ///< device handle (NULL-safe)
);

/**
 * @brief Whether a take-over console device is installed ANYWHERE on the system —
 *     by this image or by any other resident driver.
 *
 * The "a second install fails" singleton of @ref axl_console_device_install is PER
 * IMAGE: each statically linked libaxl keeps its own instance, so it cannot see a
 * device another image installed. This probe asks the firmware instead — it looks
 * for a handle carrying `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` whose device path is
 * EXACTLY the device's own Vendor node (a prefix match does not count), so it sees
 * every image's take-over alike. Use it to refuse a second take-over with a clear
 * message BEFORE doing anything with side effects; an install racing another image
 * still fails on its own, because the firmware refuses a duplicate device path.
 *
 * true from the moment install publishes the handle until uninstall removes it —
 * and also after an uninstall that could not remove its protocols (it then leaks the
 * instance deliberately, and the console is still taken). It does NOT see the swap
 * producer (@ref axl_console_tap_install), which publishes no handle. Read-only:
 * opens no protocol, allocates nothing, callable at any TPL up to `TPL_NOTIFY`.
 *
 * @return true if a take-over device's handle is present, false otherwise
 *     (including when the firmware cannot be asked, e.g. no boot services).
 */
bool
axl_console_device_present(void);

/* --- Input relay (effective only when cfg.take_input=true) --------------------
 * Mirrors the tap's inject family exactly (same key shape, same Ctrl+letter fold via
 * shift_state, same VT decode). Implemented in the input increment; when
 * take_input=false these return AXL_ERR. */

/**
 * @brief Inject one keystroke. See @ref axl_console_tap_inject_key.
 *
 * Keys wait in a ring of 64 (63 usable) that drains only as the guest reads.
 *
 * Every notify the guest registered for the key fires when it is injected,
 * EVEN WHEN THE RING IS FULL and the key itself is dropped -- as a keyboard
 * driver fires them on the key press before it looks for room in its queue
 * (EDK2 TerminalDxe does exactly this). So the Shell's Ctrl-C reaches it
 * behind any amount of type-ahead into a command that never reads the
 * console. The flip side: offering a refused key again fires its notifies
 * again, so do not retry a key that is a notify chord (Ctrl-C); a plain key
 * retried has no notify to repeat.
 *
 * @return #AXL_OK when queued; #AXL_BUSY when the ring is full (the key is
 *     not queued, but its notifies have fired; a plain key can be offered
 *     again once the guest has read); #AXL_ERR on a NULL @p dev or an
 *     output-only device (take_input false).
 */
int
axl_console_device_inject_key(AxlConsoleDevice *dev, uint16_t scan, uint16_t unicode);

/**
 * @brief Inject one keystroke with modifier / toggle state. See
 *     @ref axl_console_tap_inject_key_ex.
 *
 * @return as @ref axl_console_device_inject_key.
 */
int
axl_console_device_inject_key_ex(AxlConsoleDevice *dev, uint16_t scan, uint16_t unicode,
                                 uint32_t shift_state, uint8_t toggle_state);

/**
 * @brief Inject a run of terminal input bytes (xterm/VT). See
 *     @ref axl_console_tap_inject_text.
 *
 * Takes the bytes in whole units, exactly as
 * @ref axl_console_device_inject_text_partial does, and stops at the first
 * unit whose keys do not all fit in the ring; the bytes from there on are
 * NOT injected. Use the partial form to learn where that was.
 *
 * @return #AXL_OK when every byte was consumed; #AXL_BUSY when the ring
 *     filled first; #AXL_ERR on a NULL @p dev / @p bytes or an output-only
 *     device.
 */
int
axl_console_device_inject_text(AxlConsoleDevice *dev, const char *bytes, size_t len);

/**
 * @brief Inject as much of a run of terminal input bytes as the key ring has
 *     room for, and report how many bytes that was.
 *
 * The ring holds 63 keys and drains only as the guest reads them, so a paste
 * longer than that cannot go in at once; this is the form that lets a caller
 * hold the rest and offer it again. Bytes are taken in whole UNITS, each
 * decoded exactly as @ref axl_console_tap_inject_text decodes it, to the key
 * shape that function documents (KeyShiftState VALID plus the modifiers,
 * KeyToggleState @ref AXL_CONSOLE_TOGGLE_KEYBOARD, as a USB keyboard reports
 * them): one ASCII byte, one UTF-8 character, one complete CSI/SS3 escape
 * sequence, an Alt chord (ESC followed by one byte that is not `[`, `O` or
 * `>= 0x80`), a bare Esc (ESC alone, or before a byte `>= 0x80`), or the
 * escape sequence still open at the end of @p bytes -- whose keys are a bare
 * Esc plus each of its bytes as a literal key, because every call is
 * self-contained. A
 * unit is injected only if ALL of its keys fit, so a character or a sequence
 * is never split between two calls; one that decodes to no key at all (an
 * invalid UTF-8 byte, an unrecognized complete sequence) is consumed.
 *
 * Stops at the first unit that does not fit. @p consumed is then that unit's
 * offset, always a unit boundary, so `bytes + *consumed` can be offered again
 * later as a call of its own and decodes to the same keys it would have
 * here. Exact when called from `TPL_CALLBACK` (a loop callback, the usual
 * place): the guest's reads only ever free room, and the device's own
 * physical-keyboard timer runs at that same TPL, so nothing can take the
 * room between the check and the push. From a lower TPL that timer can, and
 * a unit's keys that then miss are lost, as with
 * @ref axl_console_device_inject_text.
 *
 * @return #AXL_OK when all @p len bytes were consumed (`*consumed == len`,
 *     including @p len 0); #AXL_BUSY when the ring filled first
 *     (`*consumed < len`, possibly 0); #AXL_ERR on a NULL @p dev, @p bytes or
 *     @p consumed, or an output-only device -- with `*consumed` set to 0
 *     whenever @p consumed is not NULL.
 */
int
axl_console_device_inject_text_partial(
    AxlConsoleDevice *dev,       ///< device handle
    const char       *bytes,     ///< terminal input bytes (xterm/VT)
    size_t            len,       ///< number of bytes
    size_t           *consumed   ///< [out] bytes taken, a unit boundary
);

/**
 * @brief How many keys the device holds that the guest has not read yet.
 *
 * Every key waiting in the ring counts -- injected through this API, or
 * forwarded from an evicted physical keyboard -- until a ReadKeyStroke or
 * ReadKeyStrokeEx takes it, and so does every physical key already read from
 * its keyboard (read_physical) and waiting for room in the ring. 0 means the
 * guest has read everything it was
 * given. The unit is one entry per EFI key: an escape sequence injected as
 * text (an arrow key's `ESC [ A`) is one key and counts once.
 *
 * That is what a caller needs before a key whose NOTIFY must act after the
 * keys typed ahead of it. A registered notify fires when the key is PUSHED
 * (the UEFI rule, which the inject calls follow), not when the guest reads
 * it, so a notify chord pushed behind unread keys acts before the guest has
 * seen them. The UEFI Shell's Ctrl-S is the case: its notify halts output,
 * and the key that resumes it is the next one READ -- one of the unread keys
 * ahead of the Ctrl-S, which is then lost from the command line. Holding the
 * chord until this returns 0 puts its effect after them, as at a keyboard
 * typed at human speed.
 *
 * The count falls only as the guest reads. A guest that is not reading
 * ConIn -- a command running -- leaves it non-zero for as long as that
 * lasts, so a caller waiting for 0 waits that long. For the Shell's Ctrl-S
 * this loses nothing: pushed behind unread keys its halt would be resumed at
 * once by one of them.
 *
 * Callable at any TPL up to `TPL_NOTIFY`. The count is a snapshot; it is
 * stable within one `TPL_CALLBACK` callback provided that callback is the
 * only injector: the guest reads at a lower TPL and cannot run until the
 * callback returns, and the device's physical-keyboard timer, the other
 * source, runs at that same TPL.
 *
 * @return the number of unread keys: 0..63 in the ring plus up to 256 held
 *     physical keys; 0 for a NULL @p dev or an output-only device
 *     (take_input false), which has no ring.
 */
size_t
axl_console_device_queued_keys(
    const AxlConsoleDevice *dev   ///< device handle (NULL-safe)
);

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
