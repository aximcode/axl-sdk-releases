/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-tap.c
    Tap the firmware console as structured operations, and inject input.

    Wraps gST->ConIn/ConOut(/StdErr) and the ConsoleInHandle's
    SimpleTextInputEx with AXL forwarders: every console *output* call is
    reported to a consumer's AxlConsoleOps vtable (clear/cursor/text/attr/
    cursor-visibility/mode/alt-screen); injected keys are pushed into a ring
    the wrapped ConIn returns, waking a blocked Shell via the WaitForKey event.

    This is the reusable, finicky firmware surgery. It carries no wire format:
    a REMOTE consumer serializes the ops to VT bytes (that is AxlConsoleMirror,
    src/util/axl-console-mirror.c), while a LOCAL renderer binds them straight
    into a cell grid.

    Ported from the EDK2 SoftBMC ConsoleWrapper, but simpler: the pump is
    the consumer's loop (axl_loop_attach_driver dispatches it from a
    firmware timer in the background), so the wrappers carry NO HTTP
    polling and the tap owns NO timer.

    Single global console => single tap instance (a singleton guarded by
    g_tap); the wrappers recover state through it. An atexit hook restores
    the console if the process exits without an explicit uninstall.
**/

#include <axl/axl-console-tap.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-atexit.h>
#include <uefi/axl-uefi.h>

#include "axl-console-emit.h"
#include "axl-console-input.h"

AXL_LOG_DOMAIN("contap");

struct AxlConsoleTap {
    /* Saved originals. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL    *orig_conout;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL     *orig_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *orig_coninex;

    /* Saved gST->StdErr, exactly as found: NULL (no error console), the
       same pointer as orig_conout (the common aliased case), or a
       genuinely distinct protocol instance. Restored verbatim on
       uninstall -- do NOT derive it from orig_conout, which is only
       correct in the aliased case and would otherwise hand a caller's
       real distinct StdErr (or an absent one) back as ConOut. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL    *orig_stderr;

    /* Wrapper protocol structs — gST/ConsoleInHandle point into these. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL     my_conout;

    /* The Mode we OWN and publish through my_conout.Mode when we are the only
       console writer (passthrough_local off). A plain struct copy of the original
       protocol aliases its Mode POINTER, and the original driver is the only
       thing that maintains CursorRow/CursorColumn/Attribute/CursorVisible — so
       with passthrough off that state freezes and a nested Shell scribbles over
       itself. With passthrough on the real driver maintains its own Mode and we
       keep aliasing it (unchanged). */
    SIMPLE_TEXT_OUTPUT_MODE             my_mode;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL      my_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL   my_coninex;

    EFI_EVENT  wait_key;
    EFI_EVENT  wait_key_ex;

    /* Shared input engine: injected-key ring, the ConInEx key-notify registry (used
       only when input_capture owns the queue), the Ctrl+letter fold, and the
       inject_text VT decoder. The tap owns wait_key/wait_key_ex above and hands them
       to the engine so ring pushes signal them; it layers passthrough to orig_conin
       on top when input_capture is off. */
    AxlConsoleInput  in;

    /* Shared SIMPLE_TEXT_OUTPUT -> AxlConsoleOps translation: the consumer vtable
       + context, the owned Mode (emit.mode == &my_mode), the alt-screen state
       machine, and the '\n'-scroll tracker its auto-leave heuristic uses. Geometry
       (cols/rows below) stays here: the tap resolves its physical fallback from
       orig_conout, which the engine does not know about. */
    AxlConsoleEmit   emit;

    /* Config (copied). */
    uint32_t         cols;
    uint32_t         rows;
    bool             passthrough;         /* passthrough_local: also write ConOut */
    bool             input_capture;       /* wrapped reads serve ONLY injected keys */

    uint32_t atexit_handle;
    bool     reinstalled_ex;  /* did we ReinstallProtocolInterface ConInEx? */
};

static AxlConsoleTap *g_tap;  /* the one active instance */


// ---------------------------------------------------------------------------
// Alt-screen (DECSET/DECRST 1049). Asserted BY the wrapper — SIMPLE_TEXT_OUTPUT
// has no way to express it, so it's inferred (explicit API, or the
// auto_alt_screen heuristic) rather than received from the protocol. The state
// machine + emission live in the shared engine; these are the public delegators.
// ---------------------------------------------------------------------------

void
axl_console_tap_enter_alt_screen(AxlConsoleTap *m)
{
    if (m != NULL) {
        axl_console_emit_enter_alt_screen(&m->emit);
    }
}

void
axl_console_tap_leave_alt_screen(AxlConsoleTap *m)
{
    if (m != NULL) {
        axl_console_emit_leave_alt_screen(&m->emit);
    }
}

bool
axl_console_tap_in_alt_screen(const AxlConsoleTap *m)
{
    return m != NULL && axl_console_emit_in_alt_screen(&m->emit);
}


// ---------------------------------------------------------------------------
// Owned SIMPLE_TEXT_OUTPUT_MODE upkeep. When the mirror is the only console
// writer (passthrough_local off) nothing else advances the cursor, so we must
// maintain it exactly as the reference console driver does — otherwise a nested
// Shell reads a frozen CursorRow/CursorColumn and overwrites its own output.
// ---------------------------------------------------------------------------

/* The geometry the wrapped Mode is expressed in: the remote size when configured
   (QueryMode already reports it for the current mode), else the physical
   console's current mode. Either may be 0/unknown, which disables wrap/clamp. */
static void
tap_geometry(AxlConsoleTap *m, uint32_t *cols, uint32_t *rows)
{
    uint32_t c = m->cols;
    uint32_t r = m->rows;
    if ((c == 0 || r == 0) && m->orig_conout != NULL
        && m->orig_conout->Mode != NULL && m->orig_conout->QueryMode != NULL) {
        UINTN oc = 0;
        UINTN orow = 0;
        if (!EFI_ERROR(m->orig_conout->QueryMode(m->orig_conout,
                           (UINTN)m->orig_conout->Mode->Mode, &oc, &orow))) {
            if (c == 0) { c = (uint32_t)oc; }
            if (r == 0) { r = (uint32_t)orow; }
        }
    }
    *cols = c;
    *rows = r;
}


// ---------------------------------------------------------------------------
// ConOut wrappers
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
wrap_out_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    axl_console_emit_home_cursor(&m->emit);   /* Reset homes the cursor */
    if (m->passthrough && m->orig_conout != NULL) {
        return m->orig_conout->Reset(m->orig_conout, ext);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    EFI_STATUS st = EFI_SUCCESS;
    if (m->passthrough && m->orig_conout != NULL) {
        st = m->orig_conout->OutputString(m->orig_conout, String);
    }
    /* Resolved geometry (configured, or the physical fallback) drives cursor
       autowrap/clamp; the CONFIGURED rows (m->rows) drive the alt-screen
       auto-leave heuristic -- deliberately distinct. */
    uint32_t rc;
    uint32_t rr;
    tap_geometry(m, &rc, &rr);
    axl_console_emit_text(&m->emit, String, rc, rr);
    return st;
}

static EFI_STATUS EFIAPI
wrap_out_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    if (m->orig_conout != NULL) {
        return m->orig_conout->TestString(m->orig_conout, String);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
                    UINTN *Columns, UINTN *Rows)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    EFI_STATUS st = EFI_SUCCESS;
    if (m->orig_conout != NULL) {
        st = m->orig_conout->QueryMode(m->orig_conout, ModeNumber, Columns, Rows);
    }
    /* Override ONLY the current mode's geometry with the remote terminal size
       (so a full-screen app sizing itself via QueryMode(Mode->Mode) lays out
       for the web terminal). Other mode numbers pass through unchanged so the
       app's mode enumeration stays truthful, and an invalid ModeNumber keeps
       its original error status. */
    bool is_current = (m->orig_conout != NULL && m->orig_conout->Mode != NULL
                       && ModeNumber == (UINTN)m->orig_conout->Mode->Mode);
    if (!EFI_ERROR(st) && is_current && m->cols > 0 && m->rows > 0
        && Columns != NULL && Rows != NULL) {
        *Columns = m->cols;
        *Rows    = m->rows;
    }
    return st;
}

static EFI_STATUS EFIAPI
wrap_out_set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    axl_console_emit_set_mode(&m->emit, (uint32_t)ModeNumber);
    if (m->orig_conout != NULL) {
        return m->orig_conout->SetMode(m->orig_conout, ModeNumber);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    if (m->passthrough && m->orig_conout != NULL) {
        m->orig_conout->SetAttribute(m->orig_conout, Attribute);
    }
    axl_console_emit_set_attribute(&m->emit, (uint32_t)Attribute);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    EFI_STATUS st = EFI_SUCCESS;
    if (m->passthrough && m->orig_conout != NULL) {
        st = m->orig_conout->ClearScreen(m->orig_conout);
    }
    axl_console_emit_clear_screen(&m->emit);
    return st;
}

static EFI_STATUS EFIAPI
wrap_out_set_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    if (m->passthrough && m->orig_conout != NULL) {
        m->orig_conout->SetCursorPosition(m->orig_conout, Column, Row);
    }
    axl_console_emit_set_cursor(&m->emit, (uint32_t)Column, (uint32_t)Row);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    if (m->passthrough && m->orig_conout != NULL) {
        m->orig_conout->EnableCursor(m->orig_conout, Visible);
    }
    axl_console_emit_enable_cursor(&m->emit, Visible ? true : false);
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// ConIn / ConInEx wrappers
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
wrap_in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    axl_console_input_drain(&g_tap->in);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_in_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    /* The engine pops + applies the Simple Ctrl+letter fold (see
       axl_console_input_fold_ctrl_letter). */
    if (axl_console_input_read_key(&m->in, Key)) {
        return EFI_SUCCESS;
    }
    /* Fall through to the physical keyboard only when NOT capturing input. In
       capture mode (AGT/axterm owns the queue), the wrapped ConIn serves ONLY
       injected keys, so the nested shell can't steal keys from the drainer. */
    if (!m->input_capture && m->orig_conin != NULL) {
        EFI_STATUS st = m->orig_conin->ReadKeyStroke(m->orig_conin, Key);
        if (!EFI_ERROR(st)) {
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
wrap_inex_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    axl_console_input_drain(&g_tap->in);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_inex_read_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData)
{
    (void)This;
    AxlConsoleTap *m = g_tap;
    /* KeyState included: the guest reads back exactly what was injected. */
    if (axl_console_input_read_key_ex(&m->in, KeyData)) {
        return EFI_SUCCESS;
    }
    if (!m->input_capture && m->orig_coninex != NULL) {
        EFI_STATUS st = m->orig_coninex->ReadKeyStrokeEx(m->orig_coninex, KeyData);
        if (!EFI_ERROR(st)) {
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_READY;
}

/* SetState is typed `void *` in the AXL ConInEx struct (the toggle-state
   type isn't generated); take a void* and ignore it. */
static EFI_STATUS EFIAPI
wrap_inex_set_state(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *toggle_state)
{
    (void)This;
    (void)toggle_state;
    return EFI_SUCCESS;
}

/* Whoever owns the key queue must own the notify registry: UEFI fires notifies
   at queue-insert time, so a registry attached to a queue nobody inserts into is
   dead, and one attached to a queue the guest no longer reads is a leak.

   With input_capture the tap IS the queue, so it records registrations in the
   engine and fires them from its push-notify path. Forwarding them instead would mean an
   injected key never breaks a nested Shell (nothing inserts into the firmware's
   queue) while a physical keystroke still does (the keyboard driver's timer
   inserts and fires) -- exactly backwards from what input_capture promises.

   Without input_capture the firmware still owns the queue, so we forward and
   stay out of the way. */
static EFI_STATUS EFIAPI
wrap_inex_register_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                          EFI_KEY_DATA *KeyData,
                          EFI_KEY_NOTIFY_FUNCTION fn,
                          void **NotifyHandle)
{
    (void)This;
    AxlConsoleTap *m = g_tap;

    if (m->input_capture) {
        return axl_console_input_register_notify(&m->in, KeyData, fn, NotifyHandle);
    }

    if (m->orig_coninex != NULL && m->orig_coninex->RegisterKeyNotify != NULL) {
        return m->orig_coninex->RegisterKeyNotify(m->orig_coninex, KeyData, fn,
                                                  NotifyHandle);
    }
    if (NotifyHandle != NULL) {
        *NotifyHandle = (void *)(uintptr_t)0x1;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_inex_unregister_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *handle)
{
    (void)This;
    AxlConsoleTap *m = g_tap;

    if (m->input_capture) {
        return axl_console_input_unregister_notify(&m->in, handle);
    }

    if (m->orig_coninex != NULL && m->orig_coninex->UnregisterKeyNotify != NULL) {
        return m->orig_coninex->UnregisterKeyNotify(m->orig_coninex, handle);
    }
    return EFI_SUCCESS;
}

/* Wire the wrapped ConInEx vtable. Shared by install() and the headless test
   seam so a test drives the same Register/UnregisterKeyNotify the Shell would.
   WaitForKeyEx is filled in separately: it needs a created event. */
static void
tap_wire_coninex(AxlConsoleTap *m)
{
    m->my_coninex.Reset               = (void *)wrap_inex_reset;
    m->my_coninex.ReadKeyStrokeEx     = wrap_inex_read_key;
    m->my_coninex.SetState            = (void *)wrap_inex_set_state;
    m->my_coninex.RegisterKeyNotify   = wrap_inex_register_notify;
    m->my_coninex.UnregisterKeyNotify = wrap_inex_unregister_notify;
}

/* WaitForKey EVT_NOTIFY_WAIT callback: fires from WaitForEvent/CheckEvent when
   a reader waits and the ring is empty — poll the physical keyboard so the
   local keyboard keeps working under a foreground Shell. The push MUST signal
   (the engine's push does): unlike the EDK2 original we have no separate 10ms timer to
   signal WaitForKey, so without the signal a polled physical key would sit in
   the ring while WaitForEvent keeps returning "not signalled" and never wakes
   the reader. Signalling a NOTIFY_WAIT event from inside its own wait-notify
   only sets its signalled state (it does not re-invoke the notify), so there
   is no recursion; the early-return on a non-empty ring prevents re-polling. */
static void EFIAPI
wait_key_cb(EFI_EVENT Event, void *Context)
{
    (void)Event;
    (void)Context;
    AxlConsoleTap *m = g_tap;
    if (m == NULL || axl_console_input_pending(&m->in)) {
        return;  /* keys already pending */
    }
    /* In capture mode, WaitForKey must signal ONLY on injected keys (the engine's
       push signals), never by polling the physical keyboard here — else the wrapped
       event would wake the shell to read a key that AGT owns. */
    if (m->input_capture) {
        return;
    }
    if (m->orig_conin != NULL) {
        EFI_INPUT_KEY k;
        if (!EFI_ERROR(m->orig_conin->ReadKeyStroke(m->orig_conin, &k))) {
            /* Plain push, not push_notify: this path only runs with input_capture
               off, where orig_coninex owns the registry and the firmware has
               already fired its own notifies for this key. */
            EFI_KEY_DATA kd = {0};
            kd.Key = k;
            axl_console_input_push(&m->in, kd);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/* Build the wrapped ConOut: copy the original (methods + geometry), then override
   every method with ours. Requires m->passthrough / m->orig_conout to be set. */
static void
tap_init_conout(AxlConsoleTap *m)
{
    if (m->orig_conout != NULL) {
        m->my_conout = *m->orig_conout;
        if (m->orig_conout->Mode != NULL) {
            m->my_mode = *m->orig_conout->Mode;   /* seed our owned Mode */
        }
    }
    m->my_conout.Reset             = wrap_out_reset;
    m->my_conout.OutputString      = wrap_out_string;
    m->my_conout.TestString        = wrap_out_test_string;
    m->my_conout.QueryMode         = wrap_out_query_mode;
    m->my_conout.SetMode           = wrap_out_set_mode;
    m->my_conout.SetAttribute      = wrap_out_set_attribute;
    m->my_conout.ClearScreen       = wrap_out_clear_screen;
    m->my_conout.SetCursorPosition = wrap_out_set_cursor;
    m->my_conout.EnableCursor      = wrap_out_enable_cursor;

    /* OWN the Mode when nothing else maintains it. The struct copy above aliased
       the ORIGINAL's Mode pointer, and only the original driver updates its
       CursorRow/CursorColumn/Attribute/CursorVisible -- with passthrough_local
       off we never call it, so that state would freeze and a nested Shell would
       scribble over itself. With passthrough on the real driver keeps its Mode
       current, so keep aliasing it (unchanged behavior). */
    if (!m->passthrough) {
        m->my_conout.Mode = &m->my_mode;
    }
}

static void
tap_atexit(void *data)
{
    axl_console_tap_uninstall((AxlConsoleTap *)data);
}

int
axl_console_tap_install(AxlConsoleTap **out, const AxlConsoleOps *ops, void *user,
                        const AxlConsoleTapConfig *cfg)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || ops == NULL || cfg == NULL) {
        return AXL_ERR;
    }
    if (g_tap != NULL) {
        axl_warning("console tap already installed");
        return AXL_ERR;
    }

    AxlConsoleTap *m = axl_calloc(1, sizeof(*m));
    if (m == NULL) {
        return AXL_ERR;
    }
    m->cols              = cfg->cols;
    m->rows              = cfg->rows;
    m->passthrough       = cfg->passthrough_local;
    m->input_capture     = cfg->input_capture;

    m->orig_conout = gST->ConOut;
    m->orig_conin  = gST->ConIn;
    m->orig_stderr = gST->StdErr;

    tap_init_conout(m);   /* seeds m->my_mode before the engine binds to it */
    axl_console_emit_init(&m->emit, ops, user, &m->my_mode, cfg->auto_alt_screen);

    /* ConIn wrapper. */
    m->my_conin.Reset         = wrap_in_reset;
    m->my_conin.ReadKeyStroke = wrap_in_read_key;

    EFI_STATUS st = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                                     wait_key_cb, NULL, &m->wait_key);
    if (EFI_ERROR(st)) {
        axl_free(m);
        return AXL_ERR;
    }
    m->my_conin.WaitForKey = m->wait_key;

    /* ConInEx wrapper. */
    tap_wire_coninex(m);

    st = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                          wait_key_cb, NULL, &m->wait_key_ex);
    if (EFI_ERROR(st)) {
        gBS->CloseEvent(m->wait_key);
        axl_free(m);
        return AXL_ERR;
    }
    m->my_coninex.WaitForKeyEx = m->wait_key_ex;

    /* Hand both events to the engine so its ring pushes signal a blocked reader. */
    axl_console_input_set_wait_events(&m->in, m->wait_key, m->wait_key_ex);

    /* Publish the singleton BEFORE swapping gST so the wrappers (which may
       fire from a ReinstallProtocolInterface notify) see a live instance. */
    g_tap = m;

    /* Replace SimpleTextInputEx on ConsoleInHandle. The Shell's editor uses
       HandleProtocol(ConsoleInHandle) directly, bypassing gST->ConIn — without
       this, the keyboard is dead in `edit`. Best-effort: absence is non-fatal. */
    EFI_GUID ex_guid = gEfiSimpleTextInputExProtocolGuid;
    st = gBS->HandleProtocol(gST->ConsoleInHandle, &ex_guid,
                             (void **)&m->orig_coninex);
    if (!EFI_ERROR(st) && m->orig_coninex != NULL) {
        st = gBS->ReinstallProtocolInterface(gST->ConsoleInHandle, &ex_guid,
                                             m->orig_coninex, &m->my_coninex);
        if (!EFI_ERROR(st)) {
            m->reinstalled_ex = true;
        } else {
            axl_warning("console tap: ConInEx reinstall failed (editor keys "
                        "may not work)");
        }
    } else {
        m->orig_coninex = NULL;
    }

    /* Swap gST pointers AFTER the reinstall (which can make ConSplitter
       rewrite gST->ConIn). ConOut and StdErr are independent fields in
       EFI_SYSTEM_TABLE (never unioned/aliased at the storage level, only
       sometimes equal in value), so StdErr needs its own explicit
       assignment even when it started out equal to ConOut. Route it
       through the SAME wrapper instance as ConOut, not a separate
       my_stderr -- one shared wrapper is enough to mirror both streams
       and keeps the common aliased case from double-wrapping. */
    gST->ConOut = &m->my_conout;
    gST->ConIn  = &m->my_conin;
    /* Repoint StdErr unconditionally, even when orig_stderr is NULL (no
       error console at all) -- deliberate, not an oversight. Raw save/
       restore is correct uniformly across the NULL / aliased-to-ConOut /
       genuinely-distinct cases: a NULL original is restored verbatim on
       uninstall (see orig_stderr field comment), and while installed, the
       backend's console_write_err already falls back to ConOut when StdErr
       is NULL, so routing a previously-absent StdErr through this wrapper
       still reaches ConOut instead of silently discarding output. */
    gST->StdErr = &m->my_conout;

    m->atexit_handle = axl_atexit(tap_atexit, m);

    *out = m;

    /* Report this producer's cell-boundary rule once, now that the wrappers are
       live. */
    axl_console_emit_report_cell_rule(&m->emit);

    axl_info("console tap installed (%ux%u)", m->cols, m->rows);
    return AXL_OK;
}

void
axl_console_tap_uninstall(AxlConsoleTap *m)
{
    if (m == NULL || g_tap != m) {
        return;
    }

    if (m->reinstalled_ex && m->orig_coninex != NULL) {
        EFI_GUID ex_guid = gEfiSimpleTextInputExProtocolGuid;
        gBS->ReinstallProtocolInterface(gST->ConsoleInHandle, &ex_guid,
                                        &m->my_coninex, m->orig_coninex);
    }
    if (m->orig_conout != NULL) {
        gST->ConOut = m->orig_conout;
    }
    /* Restore StdErr to exactly what was saved -- NULL, aliased to
       ConOut's original, or a distinct instance -- independent of the
       ConOut restore above (see the orig_stderr field comment). */
    gST->StdErr = m->orig_stderr;
    if (m->orig_conin != NULL) {
        gST->ConIn = m->orig_conin;
    }
    if (m->wait_key != NULL) {
        gBS->CloseEvent(m->wait_key);
    }
    if (m->wait_key_ex != NULL) {
        gBS->CloseEvent(m->wait_key_ex);
    }
    if (m->atexit_handle != 0) {
        axl_atexit_remove(m->atexit_handle);
    }

    g_tap = NULL;
    axl_free(m);
    axl_info("console tap uninstalled");
}

int
axl_console_tap_inject_key(AxlConsoleTap *m, uint16_t scan, uint16_t unicode)
{
    return axl_console_tap_inject_key_ex(m, scan, unicode, 0, 0);
}

int
axl_console_tap_inject_key_ex(AxlConsoleTap *m, uint16_t scan, uint16_t unicode,
                              uint32_t shift_state, uint8_t toggle_state)
{
    if (m == NULL) {
        return AXL_ERR;
    }
    return axl_console_input_inject_key_ex(&m->in, scan, unicode, shift_state,
                                           toggle_state);
}

int
axl_console_tap_inject_text(AxlConsoleTap *m, const char *bytes, size_t len)
{
    if (m == NULL || bytes == NULL) {
        return AXL_ERR;
    }
    return axl_console_input_inject_text(&m->in, bytes, len);
}

// ---------------------------------------------------------------------------
// Test seam (no public header). Exercises the REAL inject_text/inject_key
// byte->key decoder without installing the mirror — install wraps the live
// gST->ConIn/ConOut and wedges the combined unit boot (see the AxlConsoleTap
// test note in axl-test-util.c). Construct a bare, un-wrapped instance (engine
// ring + esc state only; no console wrap, no g_tap, no WaitForKey event so
// the engine push won't SignalEvent), drive inject_* against its ring, and pop the
// decoded keys. Free with axl_free. The console-mirror unit test calls these.
// ---------------------------------------------------------------------------

AxlConsoleTap *
_axl_console_tap_new_for_test(void)
{
    return axl_calloc(1, sizeof(AxlConsoleTap));
}

bool
_axl_console_tap_test_pop_key(AxlConsoleTap *m, uint16_t *scan,
                                 uint16_t *unicode)
{
    EFI_KEY_DATA kd;
    if (m == NULL || !axl_console_input_pop(&m->in, &kd)) {
        return false;
    }
    /* Represent the SIMPLE-read view a consumer sees, including the Ctrl+letter
       fold that wrap_in_read_key applies. */
    EFI_INPUT_KEY k = axl_console_input_fold_ctrl_letter(kd);
    if (scan != NULL) {
        *scan = k.ScanCode;
    }
    if (unicode != NULL) {
        *unicode = k.UnicodeChar;
    }
    return true;
}

void
axl_console_tap_set_size(AxlConsoleTap *m, uint32_t cols, uint32_t rows)
{
    if (m == NULL) {
        return;
    }
    m->cols = cols;
    m->rows = rows;
}

void
axl_console_tap_get_size(const AxlConsoleTap *m, uint32_t *cols, uint32_t *rows)
{
    uint32_t c = 0, r = 0;
    if (m != NULL) {
        /* The resolved geometry: the configured remote size, or the physical
           console's when an axis was left 0. Cast away const — the query only
           reads the wrapped protocol's current mode. */
        tap_geometry((AxlConsoleTap *)m, &c, &r);
    }
    if (cols != NULL) { *cols = c; }
    if (rows != NULL) { *rows = r; }
}

void
axl_console_tap_reset(AxlConsoleTap *m)
{
    if (m == NULL) {
        return;
    }
    axl_console_input_drain(&m->in);
    m->in.in_esc  = false;
    m->in.esc_len = 0;
    /* Leave the alt-screen cleanly (emits ESC[?1049l if we were in it) so the
       next session starts on the normal screen; reset the scroll tracker. */
    axl_console_emit_reset(&m->emit);
}

// ---------------------------------------------------------------------------
// Test seams for the alt-screen + input-passthrough paths (no public header).
// Drive the REAL output/input wraps against a bare instance published as
// g_tap (no gST wrap — a full install wedges the combined unit boot). Pair
// _test_setup with _test_teardown so g_tap doesn't leak across tests.
// ---------------------------------------------------------------------------

void
_axl_console_tap_test_setup(AxlConsoleTap *m, const AxlConsoleOps *ops, void *user,
                            uint32_t cols, uint32_t rows,
                            bool auto_alt, bool input_capture)
{
    if (m == NULL) {
        return;
    }
    m->cols              = cols;
    m->rows              = rows;
    m->input_capture     = input_capture;
    /* The bare test instance never runs tap_init_conout, so seed the engine's
       Mode pointer at the (zeroed) my_mode the wrap seams below will advance. */
    axl_console_emit_init(&m->emit, ops, user, &m->my_mode, auto_alt);
    axl_console_input_init(&m->in);   /* zero the ring / notify registry / esc state */
    tap_wire_coninex(m);   /* same vtable install() publishes */
    g_tap = m;

    /* Report the cell rule exactly as install() does, so headless tests see the
       same contract. */
    axl_console_emit_report_cell_rule(&m->emit);
}

EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *
_axl_console_tap_test_coninex(AxlConsoleTap *m)
{
    return (m == NULL) ? NULL : &m->my_coninex;
}

void
_axl_console_tap_test_clear(void)
{
    if (g_tap != NULL) {
        wrap_out_clear_screen(&g_tap->my_conout);
    }
}

void
_axl_console_tap_test_puts(const char *ascii)
{
    if (g_tap == NULL || ascii == NULL) {
        return;
    }
    CHAR16 buf[256];
    size_t i = 0;
    for (; ascii[i] != '\0' && i < (sizeof(buf) / sizeof(buf[0])) - 1; i++) {
        buf[i] = (CHAR16)(unsigned char)ascii[i];
    }
    buf[i] = 0;
    wrap_out_string(&g_tap->my_conout, buf);
}

/* A stub physical ConIn whose ReadKeyStroke returns a key when armed — used to
   prove input_passthrough=false never reads the physical keyboard. */
static bool s_stub_conin_armed;

static EFI_STATUS EFIAPI
stub_conin_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
    (void)This;
    if (!s_stub_conin_armed) {
        return EFI_NOT_READY;
    }
    Key->ScanCode    = 0;
    Key->UnicodeChar = (CHAR16)'p';   /* a "physical" key */
    return EFI_SUCCESS;
}

static EFI_SIMPLE_TEXT_INPUT_PROTOCOL s_stub_conin = {
    .Reset         = NULL,
    .ReadKeyStroke = stub_conin_read_key,
    .WaitForKey    = NULL,
};

void
_axl_console_tap_test_set_stub_conin(AxlConsoleTap *m, bool always_key)
{
    if (m == NULL) {
        return;
    }
    s_stub_conin_armed = always_key;
    m->orig_conin      = &s_stub_conin;
}

int
_axl_console_tap_test_read_key(AxlConsoleTap *m)
{
    if (m == NULL) {
        return -1;
    }
    g_tap = m;
    EFI_INPUT_KEY key;
    return (wrap_in_read_key(&m->my_conin, &key) == EFI_SUCCESS) ? 0 : -1;
}

void
_axl_console_tap_test_pump(AxlConsoleTap *m)
{
    g_tap = m;
    wait_key_cb(NULL, NULL);
}

/* A stub physical ConOut with its OWN Mode, so a test can tell whether the mirror
   publishes the original's Mode (aliased) or one it owns. */
static SIMPLE_TEXT_OUTPUT_MODE s_stub_mode;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL s_stub_conout;

void
_axl_console_tap_test_conout_begin(AxlConsoleTap *m, bool passthrough)
{
    if (m == NULL) {
        return;
    }
    s_stub_mode = (SIMPLE_TEXT_OUTPUT_MODE){
        .MaxMode = 1, .Mode = 0, .Attribute = 0x07,
        .CursorColumn = 0, .CursorRow = 0, .CursorVisible = TRUE,
    };
    s_stub_conout = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){ .Mode = &s_stub_mode };
    m->orig_conout = &s_stub_conout;
    m->passthrough = passthrough;
    tap_init_conout(m);
    g_tap = m;
}

/* True when my_conout.Mode is the mirror's OWN mode (not the original's). */
bool
_axl_console_tap_test_mode_owned(AxlConsoleTap *m)
{
    return m != NULL && m->my_conout.Mode == &m->my_mode;
}

void
_axl_console_tap_test_get_cursor(AxlConsoleTap *m, int32_t *col, int32_t *row)
{
    if (m == NULL || m->my_conout.Mode == NULL) {
        return;
    }
    *col = (int32_t)m->my_conout.Mode->CursorColumn;
    *row = (int32_t)m->my_conout.Mode->CursorRow;
}

int32_t
_axl_console_tap_test_get_attr(AxlConsoleTap *m)
{
    return (m != NULL && m->my_conout.Mode != NULL)
         ? (int32_t)m->my_conout.Mode->Attribute : -1;
}

bool
_axl_console_tap_test_get_cursor_visible(AxlConsoleTap *m)
{
    return m != NULL && m->my_conout.Mode != NULL && m->my_conout.Mode->CursorVisible;
}

void
_axl_console_tap_test_orig_cursor(int32_t *col, int32_t *row)
{
    *col = (int32_t)s_stub_mode.CursorColumn;
    *row = (int32_t)s_stub_mode.CursorRow;
}

void
_axl_console_tap_test_set_cursor(uint32_t col, uint32_t row)
{
    if (g_tap != NULL) {
        wrap_out_set_cursor(&g_tap->my_conout, col, row);
    }
}

void
_axl_console_tap_test_set_attr(uint32_t attr)
{
    if (g_tap != NULL) {
        wrap_out_set_attribute(&g_tap->my_conout, attr);
    }
}

void
_axl_console_tap_test_enable_cursor(bool visible)
{
    if (g_tap != NULL) {
        wrap_out_enable_cursor(&g_tap->my_conout, (BOOLEAN)visible);
    }
}

void
_axl_console_tap_test_teardown(void)
{
    g_tap = NULL;
}
