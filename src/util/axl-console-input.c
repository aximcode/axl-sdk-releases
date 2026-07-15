/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-input.c
    Shared input engine for the console producers (axl-console-tap swap form,
    axl-console-device take-over form). See axl-console-input.h. The ring / notify
    / fold / VT-decode logic here is the input twin of axl-console-emit's output
    translation, factored out so both producers condition injected keys identically.
**/

#include "axl-console-input.h"

#include <axl/axl-macros.h>
#include <axl/axl-str.h>

// UEFI scan codes (UEFI 2.11 §12.3.3) — the subset full-screen apps use.
#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_RIGHT  0x03
#define SCAN_LEFT   0x04
#define SCAN_HOME   0x05
#define SCAN_END    0x06
#define SCAN_INSERT 0x07
#define SCAN_DELETE 0x08
#define SCAN_PGUP   0x09
#define SCAN_PGDN   0x0A
#define SCAN_F1     0x0B
/* SCAN_ESC (0x17) comes from <uefi/axl-uefi-extra.h> via axl-uefi.h. */

// ---------------------------------------------------------------------------
// Ring + notify registry (TPL-safe against injection-vs-read preemption)
// ---------------------------------------------------------------------------

void
axl_console_input_init(AxlConsoleInput *e)
{
    axl_memset(e, 0, sizeof(*e));
}

void
axl_console_input_set_wait_events(AxlConsoleInput *e, EFI_EVENT wait_key,
                                  EFI_EVENT wait_key_ex)
{
    e->wait_key    = wait_key;
    e->wait_key_ex = wait_key_ex;
}

bool
axl_console_input_pending(const AxlConsoleInput *e)
{
    return e->head != e->tail;
}

/* The ring is touched from two TPLs: injection runs from the consumer's
   timer-pumped loop at TPL_CALLBACK; ReadKeyStroke runs from the foreground Shell
   at TPL_APPLICATION. Raise to TPL_HIGH_LEVEL to make head/tail updates atomic
   against that preemption. SignalEvent is callable at TPL_HIGH_LEVEL; the notify
   just runs once TPL drops. */
bool
axl_console_input_push(AxlConsoleInput *e, EFI_KEY_DATA key)
{
    EFI_TPL old  = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    UINTN   next = (e->head + 1) % AXL_CONSOLE_KEY_RING_SIZE;
    bool    ok   = (next != e->tail);
    if (ok) {
        e->ring[e->head] = key;
        e->head = next;
        if (e->wait_key != NULL) {
            gBS->SignalEvent(e->wait_key);
        }
        if (e->wait_key_ex != NULL) {
            gBS->SignalEvent(e->wait_key_ex);
        }
    }
    gBS->RestoreTPL(old);
    return ok;
}

/* EDK2's IsKeyRegistered (MdeModulePkg Ps2KbdTextIn.c), reproduced exactly:
   ScanCode and UnicodeChar must be equal, while a registered KeyShiftState or
   KeyToggleState of 0 means "don't care" and a nonzero one must match exactly.
   Getting the zero case backwards would make every plain-key notify stop firing;
   getting the nonzero case backwards would fire the Shell's Ctrl+C on a bare 'c'. */
static bool
key_registered(const EFI_KEY_DATA *reg, const EFI_KEY_DATA *in)
{
    if (reg->Key.ScanCode != in->Key.ScanCode ||
        reg->Key.UnicodeChar != in->Key.UnicodeChar) {
        return false;
    }
    if (reg->KeyState.KeyShiftState != 0 &&
        reg->KeyState.KeyShiftState != in->KeyState.KeyShiftState) {
        return false;
    }
    if (reg->KeyState.KeyToggleState != 0 &&
        reg->KeyState.KeyToggleState != in->KeyState.KeyToggleState) {
        return false;
    }
    return true;
}

/* Fire every registration matching `key`. UEFI semantics: notifies run when a key
   is INSERTED into the queue, not when it is read, and they do not consume it.
   Called after the push has dropped back to the caller's TPL -- a notify handler
   may signal events or touch protocols, neither safe at TPL_HIGH_LEVEL.
   TPL_NOTIFY matches where EDK2's keyboard drivers dispatch. A producer that is
   NOT the queue owner leaves notify[] empty (it forwards registrations to the real
   console), so this loop finds nothing and is a no-op there. */
static void
fire_key_notifies(AxlConsoleInput *e, const EFI_KEY_DATA *key)
{
    /* Skip the TPL raise entirely when no notify is registered — the common case
       for a pure-inject producer, and the case (empty notify[]) a non-owner tap is
       always in. Injection runs at <= TPL_NOTIFY, so a registration (at
       TPL_APPLICATION) cannot preempt this scan to add a slot mid-loop. */
    bool any = false;
    for (size_t i = 0; i < AXL_CONSOLE_KEY_NOTIFY_MAX; i++) {
        if (e->notify[i].in_use) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    EFI_TPL old = gBS->RaiseTPL(TPL_NOTIFY);
    for (size_t i = 0; i < AXL_CONSOLE_KEY_NOTIFY_MAX; i++) {
        AxlConsoleKeyNotify *n = &e->notify[i];
        if (n->in_use && n->fn != NULL && key_registered(&n->key, key)) {
            EFI_KEY_DATA copy = *key;   /* callee takes a mutable pointer */
            n->fn(&copy);
        }
    }
    gBS->RestoreTPL(old);
}

bool
axl_console_input_push_notify(AxlConsoleInput *e, EFI_KEY_DATA key)
{
    if (!axl_console_input_push(e, key)) {
        return false;
    }
    fire_key_notifies(e, &key);
    return true;
}

static void
resignal_if_more(AxlConsoleInput *e)
{
    if (e->head != e->tail) {
        if (e->wait_key != NULL) {
            gBS->SignalEvent(e->wait_key);
        }
        if (e->wait_key_ex != NULL) {
            gBS->SignalEvent(e->wait_key_ex);
        }
    }
}

static bool
ring_pop(AxlConsoleInput *e, EFI_KEY_DATA *key)
{
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    bool    ok  = (e->head != e->tail);
    if (ok) {
        *key = e->ring[e->tail];
        e->tail = (e->tail + 1) % AXL_CONSOLE_KEY_RING_SIZE;
    }
    gBS->RestoreTPL(old);
    return ok;
}

bool
axl_console_input_pop(AxlConsoleInput *e, EFI_KEY_DATA *out)
{
    if (!ring_pop(e, out)) {
        return false;
    }
    resignal_if_more(e);
    return true;
}

void
axl_console_input_drain(AxlConsoleInput *e)
{
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    e->head = 0;
    e->tail = 0;
    gBS->RestoreTPL(old);
}

// ---------------------------------------------------------------------------
// ConInEx read helpers
// ---------------------------------------------------------------------------

/* The SIMPLE ReadKeyStroke has no KeyState, so a Ctrl+<letter> must fold to its C0
   control code (Ctrl+A=1 .. Ctrl+Z=26). EDK2's ConSplitter does exactly this on its
   Simple path while the Ex path returns the letter plus the shift state: a line
   editor reading Simple then sees Ctrl+C as 0x03 (a control action it ignores)
   instead of the bare 'c' it would insert, and a full-screen app reading Ex (the
   Shell's `edit`) still gets the letter + KeyState to map its own control commands.
   We inject Ctrl+<letter> as the letter + control state (mirroring what a real
   keyboard delivers), so this fold is what makes the Simple side correct. Only the
   injected (ring) path folds; a physical key from the real ConIn was already folded
   by the firmware's own ConSplitter. */
EFI_INPUT_KEY
axl_console_input_fold_ctrl_letter(EFI_KEY_DATA kd)
{
    EFI_INPUT_KEY k = kd.Key;
    /* The control bits are only meaningful when VALID is set (UEFI 2.11), so gate
       on VALID too -- a stale control bit must never fold a normally-typed letter. */
    if ((kd.KeyState.KeyShiftState & EFI_SHIFT_STATE_VALID) != 0 &&
        (kd.KeyState.KeyShiftState &
         (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)) != 0) {
        CHAR16 u = k.UnicodeChar;
        if (u >= L'A' && u <= L'Z') {
            u = (CHAR16)(u + 0x20);
        }
        if (u >= L'a' && u <= L'z') {
            k.UnicodeChar = (CHAR16)(u - L'a' + 1);
        }
    }
    return k;
}

bool
axl_console_input_read_key(AxlConsoleInput *e, EFI_INPUT_KEY *out)
{
    EFI_KEY_DATA kd;
    if (!axl_console_input_pop(e, &kd)) {
        return false;
    }
    *out = axl_console_input_fold_ctrl_letter(kd);
    return true;
}

bool
axl_console_input_read_key_ex(AxlConsoleInput *e, EFI_KEY_DATA *out)
{
    return axl_console_input_pop(e, out);
}

// ---------------------------------------------------------------------------
// Notify registry
// ---------------------------------------------------------------------------

EFI_STATUS
axl_console_input_register_notify(AxlConsoleInput *e, EFI_KEY_DATA *kd,
                                  EFI_KEY_NOTIFY_FUNCTION fn, void **handle)
{
    if (kd == NULL || fn == NULL || handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (size_t i = 0; i < AXL_CONSOLE_KEY_NOTIFY_MAX; i++) {
        AxlConsoleKeyNotify *n = &e->notify[i];
        if (!n->in_use) {
            n->key    = *kd;
            n->fn     = fn;
            n->in_use = true;
            *handle = n;   /* slot address; the array never moves */
            return EFI_SUCCESS;
        }
    }
    /* Deliberately silent: this runs inside the wrapped console path, and a warning
       would write to a console the producer owns. The caller gets the status, which
       is what EFI_SIMPLE_TEXT_INPUT_EX specifies. */
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS
axl_console_input_unregister_notify(AxlConsoleInput *e, void *handle)
{
    /* Only accept a handle we minted: it must point at one of our slots. */
    for (size_t i = 0; i < AXL_CONSOLE_KEY_NOTIFY_MAX; i++) {
        AxlConsoleKeyNotify *n = &e->notify[i];
        if (handle == n && n->in_use) {
            n->in_use = false;
            n->fn     = NULL;
            return EFI_SUCCESS;
        }
    }
    return EFI_INVALID_PARAMETER;
}

// ---------------------------------------------------------------------------
// inject_text — xterm/VT escape decoder
// ---------------------------------------------------------------------------

/* Decode an accumulated escape body (the bytes AFTER the leading ESC) to a UEFI
   scan code, or 0 if not (yet) a recognized complete sequence. Sets *complete=true
   once the body is a full sequence (recognized or not). */
static uint16_t
decode_escape(const char *body, size_t len, bool *complete)
{
    *complete = false;
    if (len == 0) {
        return 0;  /* just "ESC" so far */
    }

    if (body[0] == 'O') {
        /* SS3: ESC O P..S → F1..F4 */
        if (len < 2) {
            return 0;
        }
        *complete = true;
        switch (body[1]) {
            case 'P': return SCAN_F1;        /* F1 */
            case 'Q': return SCAN_F1 + 1;    /* F2 */
            case 'R': return SCAN_F1 + 2;    /* F3 */
            case 'S': return SCAN_F1 + 3;    /* F4 */
            default:  return 0;
        }
    }

    if (body[0] == '[') {
        /* CSI: ESC [ params final. Final byte is 0x40..0x7E. */
        size_t i = 1;
        unsigned num = 0;
        bool     have_num = false;
        for (; i < len; i++) {
            char c = body[i];
            if (c >= '0' && c <= '9') {
                num = num * 10 + (unsigned)(c - '0');
                have_num = true;
            } else if (c == ';') {
                /* Modifier params follow — keep the FIRST number, ignore rest
                   (e.g. ESC[1;5C ⇒ Ctrl+Right ⇒ base Right). */
                while (i + 1 < len && body[i + 1] != '~'
                       && !(body[i + 1] >= '@' && body[i + 1] <= 'Z')) {
                    i++;
                }
            } else {
                break;  /* final byte */
            }
        }
        if (i >= len) {
            return 0;  /* no final byte yet */
        }
        *complete = true;
        char final = body[i];
        switch (final) {
            case 'A': return SCAN_UP;
            case 'B': return SCAN_DOWN;
            case 'C': return SCAN_RIGHT;
            case 'D': return SCAN_LEFT;
            case 'H': return SCAN_HOME;
            case 'F': return SCAN_END;
            case '~':
                if (!have_num) {
                    return 0;
                }
                switch (num) {
                    case 1:  return SCAN_HOME;
                    case 2:  return SCAN_INSERT;
                    case 3:  return SCAN_DELETE;
                    case 4:  return SCAN_END;
                    case 5:  return SCAN_PGUP;
                    case 6:  return SCAN_PGDN;
                    case 15: return SCAN_F1 + 4;   /* F5 */
                    case 17: return SCAN_F1 + 5;   /* F6 */
                    case 18: return SCAN_F1 + 6;   /* F7 */
                    case 19: return SCAN_F1 + 7;   /* F8 */
                    case 20: return SCAN_F1 + 8;   /* F9 */
                    case 21: return SCAN_F1 + 9;   /* F10 */
                    case 23: return SCAN_F1 + 10;  /* F11 */
                    case 24: return SCAN_F1 + 11;  /* F12 */
                    default: return 0;
                }
            default: return 0;
        }
    }

    /* ESC followed by something that isn't a CSI/SS3 introducer ⇒ the byte is the
       bare Esc key; the introducer byte is handled by the caller. */
    *complete = true;
    return SCAN_ESC;
}

/* inject_text decodes a byte stream, which carries no modifier state, so keys from
   it have KeyState 0. That still fires any registration whose own KeyShiftState is
   0 (the "don't care" case), just not a Ctrl-qualified one -- see inject_key_ex for
   reaching those. */
static void
inject_unicode(AxlConsoleInput *e, uint16_t ch)
{
    EFI_KEY_DATA k = {0};
    k.Key.UnicodeChar = ch;
    axl_console_input_push_notify(e, k);
}

static void
inject_scan(AxlConsoleInput *e, uint16_t scan)
{
    EFI_KEY_DATA k = {0};
    k.Key.ScanCode = scan;
    axl_console_input_push_notify(e, k);
}

int
axl_console_input_inject_key_ex(AxlConsoleInput *e, uint16_t scan, uint16_t unicode,
                                uint32_t shift_state, uint8_t toggle_state)
{
    if (e == NULL) {
        return AXL_ERR;
    }
    EFI_KEY_DATA k = {0};
    k.Key.ScanCode            = scan;
    k.Key.UnicodeChar         = unicode;
    k.KeyState.KeyShiftState  = shift_state;
    k.KeyState.KeyToggleState = toggle_state;
    return axl_console_input_push_notify(e, k) ? AXL_OK : AXL_ERR;
}

int
axl_console_input_inject_text(AxlConsoleInput *e, const char *bytes, size_t len)
{
    if (e == NULL || bytes == NULL) {
        return AXL_ERR;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];

        if (e->in_esc) {
            /* Bare ESC followed by a CSI/SS3 introducer? keep accumulating;
               otherwise the pending ESC is the Esc key and this byte restarts
               normal handling. */
            if (e->esc_len == 0 && b != '[' && b != 'O') {
                inject_scan(e, SCAN_ESC);
                e->in_esc = false;
                /* fall through to handle b as a normal byte below */
            } else {
                if (e->esc_len < sizeof(e->esc_buf)) {
                    e->esc_buf[e->esc_len++] = (char)b;
                }
                bool     complete = false;
                uint16_t scan = decode_escape(e->esc_buf, e->esc_len, &complete);
                if (complete) {
                    if (scan != 0) {
                        inject_scan(e, scan);
                    }
                    e->in_esc  = false;
                    e->esc_len = 0;
                } else if (e->esc_len >= sizeof(e->esc_buf)) {
                    /* Overlong / unrecognized — drop to avoid wedging. */
                    e->in_esc  = false;
                    e->esc_len = 0;
                }
                continue;
            }
        }

        if (b == 0x1B) {
            e->in_esc  = true;
            e->esc_len = 0;
            continue;
        }

        /* UTF-8 → a single BMP unicode key (input is typically ASCII). */
        if (b < 0x80) {
            /* Terminals (xterm.js) send 0x7f (DEL) for the Backspace key;
               UEFI backspace is UnicodeChar 0x08, so remap it here — exactly
               what TerminalDxe does for an incoming 0x7f. (The Delete *key*
               arrives as the CSI "3~" escape and is decoded to SCAN_DELETE by
               decode_escape; it never reaches this byte path.) */
            inject_unicode(e, (b == 0x7f) ? 0x08 : b);
        } else if ((b & 0xE0) == 0xC0 && i + 1 < len) {
            uint16_t cp = (uint16_t)((b & 0x1F) << 6)
                        | (uint16_t)(bytes[i + 1] & 0x3F);
            inject_unicode(e, cp);
            i += 1;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < len) {
            uint16_t cp = (uint16_t)((b & 0x0F) << 12)
                        | (uint16_t)((bytes[i + 1] & 0x3F) << 6)
                        | (uint16_t)(bytes[i + 2] & 0x3F);
            inject_unicode(e, cp);
            i += 2;
        }
        /* else: incomplete/invalid lead byte — skip. */
    }

    /* Flush any escape state that didn't complete within this call. Each
       inject_text call is self-contained: xterm.js delivers a whole escape
       sequence per keypress, so a sequence that is still open at end-of-call is a
       bare Esc (its own write) or a truncated/garbled run. Treat the leading ESC
       as the Esc key and re-inject the accumulated body bytes as literal keys,
       rather than holding state into the next call — a held partial would
       otherwise splice onto the next call's bytes and corrupt that keystroke
       (e.g. a dropped final byte turning a later 'A' into Up). */
    if (e->in_esc) {
        inject_scan(e, SCAN_ESC);
        for (size_t j = 0; j < e->esc_len; j++) {
            inject_unicode(e, (unsigned char)e->esc_buf[j]);
        }
        e->in_esc  = false;
        e->esc_len = 0;
    }
    return AXL_OK;
}
