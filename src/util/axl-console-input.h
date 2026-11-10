/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-input.h
    Internal shared INPUT engine for the console producers. The output side has
    axl-console-emit (SIMPLE_TEXT_OUTPUT -> AxlConsoleOps); this is its input twin:
    the injected-key ring, the EFI_SIMPLE_TEXT_INPUT_EX key-notify registry, the
    Ctrl+letter fold for the Simple read path, and the xterm/VT inject_text decoder
    -- the queue-owner semantics BOTH producers share.

    A producer embeds one AxlConsoleInput, points its ConInEx `ReadKeyStroke(Ex)` /
    `Reset` / `Register`/`UnregisterKeyNotify` at the helpers here, and creates the
    two WaitForKey events itself (handing them to the engine so ring pushes signal
    them). The swap-form tap layers passthrough to the real firmware console on top
    when it is NOT the sole owner (input_capture off); the take-over device is always
    the sole owner and uses the engine directly. NOT a public header.
**/

#ifndef AXL_CONSOLE_INPUT_H
#define AXL_CONSOLE_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <uefi/axl-uefi.h>

#define AXL_CONSOLE_KEY_RING_SIZE   64
#define AXL_CONSOLE_KEY_NOTIFY_MAX  16
#define AXL_CONSOLE_ESC_BUF_SIZE    16   /* longest escape body inject_text decodes */

/* One registered EFI_SIMPLE_TEXT_INPUT_EX key notify. The slot's address is the
   opaque NotifyHandle handed back, so the array must never be reallocated. */
typedef struct {
    EFI_KEY_DATA            key;   /* the registration pattern */
    EFI_KEY_NOTIFY_FUNCTION fn;
    bool                    in_use;
} AxlConsoleKeyNotify;

typedef struct {
    /* Key injection ring. Carries the full EFI_KEY_DATA, not just EFI_INPUT_KEY:
       ReadKeyStrokeEx must report KeyState, and the notify match rule keys off it
       (a Ctrl-qualified registration never matches a key with no modifiers).
       ReadKeyStroke, which has no KeyState, reads the .Key half through the fold. */
    EFI_KEY_DATA  ring[AXL_CONSOLE_KEY_RING_SIZE];
    UINTN         head;
    UINTN         tail;

    /* WaitForKey / WaitForKeyEx events the PRODUCER created and owns; ring pushes
       signal them so a blocked reader wakes. NULL until set. */
    EFI_EVENT     wait_key;
    EFI_EVENT     wait_key_ex;

    /* Key notifies WE own (populated when the producer is the queue owner). */
    AxlConsoleKeyNotify  notify[AXL_CONSOLE_KEY_NOTIFY_MAX];

} AxlConsoleInput;

/** @brief Zero the ring / notify registry / escape state. */
void
axl_console_input_init(AxlConsoleInput *e);

/** @brief Hand the engine the producer-owned WaitForKey / WaitForKeyEx events so
 *     ring pushes signal them. Either may be NULL. */
void
axl_console_input_set_wait_events(AxlConsoleInput *e, EFI_EVENT wait_key,
                                  EFI_EVENT wait_key_ex);

/* --- Ring (TPL-safe against injection-vs-read preemption) --------------------- */

/** @brief Push a raw key and fire matching notifies (the injection path). The
 *     notifies fire even when the ring is full and the key is dropped (false),
 *     as a keyboard driver fires them on the press. */
bool
axl_console_input_push_notify(AxlConsoleInput *e, EFI_KEY_DATA key);

/** @brief Fire the notifies matching a key read from an evicted physical
 *     keyboard, WITHOUT queuing it: the producer calls this when the key
 *     ARRIVES (as the keyboard's driver fires them on insert) and queues the
 *     key separately with axl_console_input_push() once the ring has room.
 *     Matched as that keyboard's own driver would have: a key with no
 *     EFI_SHIFT_STATE_VALID comes from a source that reports no modifiers (a
 *     serial terminal), whose driver (EDK2 TerminalDxe) matches a registration
 *     by ScanCode + UnicodeChar alone -- which is how a serial 0x03 reaches the
 *     Shell's Ctrl-C notify, registered with a Ctrl state. A key that carries
 *     a shift state keeps the exact rule. */
void
axl_console_input_notify_physical(AxlConsoleInput *e, const EFI_KEY_DATA *key);

/** @brief Push a raw key WITHOUT firing notifies (the producer's physical-poll
 *     path, which fired them when the key arrived). false = ring full. */
bool
axl_console_input_push(AxlConsoleInput *e, EFI_KEY_DATA key);

/** @brief Pop the oldest key; re-signals the wait events if more remain. */
bool
axl_console_input_pop(AxlConsoleInput *e, EFI_KEY_DATA *out);

/** @brief Discard every queued key. */
void
axl_console_input_drain(AxlConsoleInput *e);

/** @brief Whether any key is queued (for a WaitForKey poll's early-return). */
bool
axl_console_input_pending(const AxlConsoleInput *e);

/** @brief How many keys are queued and unread (0..AXL_CONSOLE_KEY_RING_SIZE - 1);
 *     head and tail read as one pair at TPL_HIGH_LEVEL. */
size_t
axl_console_input_queued(const AxlConsoleInput *e);

/** @brief How many more keys the ring takes now (AXL_CONSOLE_KEY_RING_SIZE - 1
 *     minus axl_console_input_queued()). */
size_t
axl_console_input_room(const AxlConsoleInput *e);

/* --- ConInEx read helpers ----------------------------------------------------- */

/** @brief Simple-protocol read: pop + Ctrl+letter fold. false = ring empty. */
bool
axl_console_input_read_key(AxlConsoleInput *e, EFI_INPUT_KEY *out);

/** @brief Ex-protocol read: pop the full EFI_KEY_DATA (KeyState included). */
bool
axl_console_input_read_key_ex(AxlConsoleInput *e, EFI_KEY_DATA *out);

/* --- Notify registry (queue-owner semantics) ---------------------------------- */

/** @brief Record a key-notify registration in a free slot; *handle = its address. */
EFI_STATUS
axl_console_input_register_notify(AxlConsoleInput *e, EFI_KEY_DATA *kd,
                                  EFI_KEY_NOTIFY_FUNCTION fn, void **handle);

/** @brief Release a registration previously handed back by register_notify. */
EFI_STATUS
axl_console_input_unregister_notify(AxlConsoleInput *e, void *handle);

/* --- Injection ---------------------------------------------------------------- */

/** @brief Inject one keystroke with modifier / toggle state.
 *  @return AXL_OK; AXL_BUSY when the ring is full (the key is dropped, but its
 *     notifies have fired); AXL_ERR on a NULL @p e. */
int
axl_console_input_inject_key_ex(AxlConsoleInput *e, uint16_t scan, uint16_t unicode,
                                uint32_t shift_state, uint8_t toggle_state);

/** @brief Inject a run of terminal input bytes (xterm/VT), decoding CSI/SS3 to
 *     UEFI scan codes and each key to the KeyState a USB keyboard reports
 *     (axl_console_tap_inject_text() documents the shape), in whole units
 *     while the ring has room for all of a
 *     unit's keys; *consumed is the offset reached (a unit boundary). Each call
 *     is self-contained: a sequence open at the end is a bare Esc + literals.
 *     The contract is axl_console_device_inject_text_partial()'s.
 *  @return AXL_OK (all consumed); AXL_BUSY (ring filled first); AXL_ERR on NULL
 *     arguments (*consumed 0 when @p consumed is not NULL). */
int
axl_console_input_inject_text_partial(AxlConsoleInput *e, const char *bytes, size_t len,
                                      size_t *consumed);

/** @brief inject_text_partial() without the count: stops at the first unit that
 *     does not fit. @return AXL_OK; AXL_BUSY; AXL_ERR on NULL arguments. */
int
axl_console_input_inject_text(AxlConsoleInput *e, const char *bytes, size_t len);

/* --- Conditioning (exposed for tests + the tap's Simple-read seam) ------------ */

/** @brief The Simple-protocol Ctrl+<letter> -> C0 fold (Ctrl+A=1 .. Ctrl+Z=26),
 *     gated on EFI_SHIFT_STATE_VALID. A physical key already folded by the
 *     firmware's ConSplitter is unaffected (its UnicodeChar is already the C0). */
EFI_INPUT_KEY
axl_console_input_fold_ctrl_letter(EFI_KEY_DATA kd);

#endif /* AXL_CONSOLE_INPUT_H */
