/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-input.c
    Shared input engine for the console producers (axl-console-tap swap form,
    axl-console-device take-over form). See axl-console-input.h. The ring / notify
    / fold / VT-decode logic here is the input twin of axl-console-emit's output
    translation, factored out so both producers condition injected keys identically.
**/

#include "axl-console-input.h"
#include "../backend/axl-backend.h"   /* axl_efi_call */

#include <axl/axl-console-ops.h>
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
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
   getting the nonzero case backwards would fire the Shell's Ctrl+C on a bare 'c'.
   `key_only` is TerminalDxe's rule instead (its IsKeyRegistered compares the key
   alone): for a key from a source that reports no shift state at all. */
static bool
key_registered(const EFI_KEY_DATA *reg, const EFI_KEY_DATA *in, bool key_only)
{
    if (reg->Key.ScanCode != in->Key.ScanCode ||
        reg->Key.UnicodeChar != in->Key.UnicodeChar) {
        return false;
    }
    if (key_only) {
        return true;
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
fire_key_notifies(AxlConsoleInput *e, const EFI_KEY_DATA *key, bool key_only)
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
        if (n->in_use && n->fn != NULL && key_registered(&n->key, key, key_only)) {
            EFI_KEY_DATA copy = *key;   /* callee takes a mutable pointer */
            n->fn(&copy);
        }
    }
    gBS->RestoreTPL(old);
}

/* The notifies fire whether or not the key fits: a keyboard driver runs them on
   the key PRESS, before it asks whether its queue has room, and drops the key
   when it has none (EDK2 TerminalDxe EfiKeyFiFoInsertOneKey, TerminalConIn.c:
   the notify scan precedes the IsEfiKeyFiFoFull check). Otherwise a Ctrl-C
   typed behind a type-ahead into a command that never reads the console --
   a running script -- could never break it. */
bool
axl_console_input_push_notify(AxlConsoleInput *e, EFI_KEY_DATA key)
{
    bool queued = axl_console_input_push(e, key);
    fire_key_notifies(e, &key, false);
    return queued;
}

void
axl_console_input_notify_physical(AxlConsoleInput *e, const EFI_KEY_DATA *key)
{
    fire_key_notifies(e, key, (key->KeyState.KeyShiftState & EFI_SHIFT_STATE_VALID) == 0);
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

/* The modifier bits a byte stream can express, all left-hand (as xterm reports
   them), and the Shift pair a shifted printable drops. */
#define MOD_SHIFT   EFI_LEFT_SHIFT_PRESSED
#define MOD_CTRL    EFI_LEFT_CONTROL_PRESSED
#define MOD_ALT     EFI_LEFT_ALT_PRESSED
#define MOD_LOGO    EFI_LEFT_LOGO_PRESSED
#define SHIFT_BITS  (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)

#define XTERM_MOD_MAX   16       /* 1 + Shift|Alt|Ctrl|Meta */
#define CSI_PARAM_MAX   3        /* ESC [ 27 ; m ; code ~ */
#define CSI_PARAM_LIMIT 65535u

/* The most keys one unit decodes to: an escape sequence left open at the end of
   the run is a bare Esc plus each body byte as a literal. */
#define UNIT_KEYS_MAX  (1 + AXL_CONSOLE_ESC_BUF_SIZE)

/* One decoded unit: how many input bytes it spans and the keys it produces,
   each with the KeyShiftState a USB keyboard reports for it. */
typedef struct {
    size_t   len;
    size_t   nkeys;
    uint16_t scan[UNIT_KEYS_MAX];
    uint16_t uni[UNIT_KEYS_MAX];
    uint32_t shift[UNIT_KEYS_MAX];
} InputUnit;

/* Finals naming a scan key, shared by CSI (`ESC [ 1 ; m X`) and SS3 (`ESC O X`). */
static const char    final_keys[] = "ABCDHFPQRS";
static const uint8_t final_scan[] = {
    SCAN_UP, SCAN_DOWN, SCAN_RIGHT, SCAN_LEFT, SCAN_HOME, SCAN_END,
    SCAN_F1, SCAN_F1 + 1, SCAN_F1 + 2, SCAN_F1 + 3,
};

/* `ESC [ n ~` by n (1..24); 0 = no key. */
static const uint8_t tilde_scan[25] = {
    [1] = SCAN_HOME, [2] = SCAN_INSERT, [3] = SCAN_DELETE, [4] = SCAN_END,
    [5] = SCAN_PGUP, [6] = SCAN_PGDN,
    [15] = SCAN_F1 + 4, [17] = SCAN_F1 + 5, [18] = SCAN_F1 + 6, [19] = SCAN_F1 + 7,
    [20] = SCAN_F1 + 8, [21] = SCAN_F1 + 9, [23] = SCAN_F1 + 10, [24] = SCAN_F1 + 11,
};

/* Append one key in a USB keyboard's shape. Its driver reports Shift only where
   Shift did not already change the character: a shifted printable ('A', '!')
   arrives without it, Space, Tab, Backspace, Enter and every scan key keep it. */
static void
unit_key(InputUnit *u, uint16_t scan, uint16_t uni, uint32_t mods)
{
    if (scan == 0 && uni > 0x20) {
        mods &= ~(uint32_t)SHIFT_BITS;
    }
    u->scan[u->nkeys]  = scan;
    u->uni[u->nkeys]   = uni;
    u->shift[u->nkeys] = EFI_SHIFT_STATE_VALID | mods;
    u->nkeys++;
}

/* The key for one ASCII byte. 0x08/0x7f, 0x09, 0x0d and 0x1b are what a terminal
   sends for Backspace, Tab, Enter and Esc, so they stay those keys; every other
   C0 byte is the Ctrl chord that produced it -- the letter (or Space, or one of
   \ ] ^ _) plus Ctrl, as a USB keyboard reports Ctrl+A. (TerminalDxe on the
   serial console is no model here: it passes C0 bytes through raw and, in the
   OVMF build measured, turns 0x7f into the Delete scan key.) */
static void
byte_key(InputUnit *u, unsigned char b, uint32_t mods)
{
    if (b == 0x1B) {
        unit_key(u, SCAN_ESC, 0, mods);
    } else if (b == 0x7F || b == 0x08 || b == 0x09 || b == 0x0D || b >= 0x20) {
        unit_key(u, 0, b == 0x7F ? 0x08 : b, mods);
    } else {
        uint16_t c = (b == 0x00) ? ' ' : (uint16_t)(b + (b <= 0x1A ? 0x60 : 0x40));
        unit_key(u, 0, c, mods | MOD_CTRL);
    }
}

/* xterm's modifier parameter: 1 + a bit each for Shift, Alt, Ctrl, Meta. */
static bool
xterm_mod_to_shift(unsigned m, uint32_t *out)
{
    static const uint32_t bit[4] = { MOD_SHIFT, MOD_ALT, MOD_CTRL, MOD_LOGO };

    if (m < 1 || m > XTERM_MOD_MAX) {
        return false;
    }
    *out = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (((m - 1) >> i) & 1u) {
            *out |= bit[i];
        }
    }
    return true;
}

/* A CSI-u / modifyOtherKeys key code (a Unicode code point). */
static bool
code_key(InputUnit *u, unsigned code, uint32_t mods)
{
    if (code == 27) {
        unit_key(u, SCAN_ESC, 0, mods);
        return true;
    }
    if (code == 127) {
        code = 0x08;
    } else if (code < 0x20 ? (code != 0x09 && code != 0x0D)
                           : (code >= 0xD800 && code <= 0xDFFF)) {
        return false;
    }
    if ((mods & SHIFT_BITS) != 0 && code >= 'a' && code <= 'z') {
        code -= 0x20;
    }
    unit_key(u, 0, (uint16_t)code, mods);
    return true;
}

/* Up to CSI_PARAM_MAX decimal parameters; an empty one is 0. false for any
   other byte, a fourth parameter, or a value above CSI_PARAM_LIMIT. */
static bool
csi_params(const char *p, size_t n, unsigned v[CSI_PARAM_MAX], size_t *count)
{
    size_t c = 0;

    for (size_t i = 0; i < CSI_PARAM_MAX; i++) {
        v[i] = 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (c == 0) {
            c = 1;
        }
        if (p[i] == ';') {
            if (c == CSI_PARAM_MAX) {
                return false;
            }
            c++;
        } else if (p[i] >= '0' && p[i] <= '9') {
            v[c - 1] = v[c - 1] * 10u + (unsigned)(p[i] - '0');
            if (v[c - 1] > CSI_PARAM_LIMIT) {
                return false;
            }
        } else {
            return false;
        }
    }
    *count = c;
    return true;
}

/* The key of a complete CSI sequence: params p[0..n) and its final byte. An
   unrecognized one adds no key. */
static void
csi_key(InputUnit *u, const char *p, size_t n, char final)
{
    unsigned v[CSI_PARAM_MAX];
    size_t   c    = 0;
    uint32_t mods = 0;

    if (!csi_params(p, n, v, &c)) {
        return;
    }
    if (final == '~' && c == 3 && v[0] == 27) {
        /* modifyOtherKeys: ESC [ 27 ; m ; code ~ */
        if (xterm_mod_to_shift(v[1], &mods)) {
            code_key(u, v[2], mods);
        }
        return;
    }
    if (c > 2 || !xterm_mod_to_shift(c == 2 ? v[1] : 1, &mods)) {
        return;
    }
    const char *f = axl_strchr(final_keys, final);
    if (final == 'u') {
        code_key(u, v[0], mods);
    } else if (final == 'Z') {
        unit_key(u, 0, 0x09, mods | MOD_SHIFT);
    } else if (final == '~') {
        if (v[0] < AXL_ARRAY_SIZE(tilde_scan) && tilde_scan[v[0]] != 0) {
            unit_key(u, tilde_scan[v[0]], 0, mods);
        }
    } else if (f != NULL) {
        unit_key(u, final_scan[f - final_keys], 0, mods);
    }
}

/* Decode the escape unit starting at the ESC at bytes[0]. */
static void
decode_escape_unit(const char *bytes, size_t len, InputUnit *u)
{
    unsigned char b1 = len >= 2 ? (unsigned char)bytes[1] : 0x80;

    /* A bare Esc: nothing follows, or a non-ASCII byte (which starts the next
       unit), or an ESC that itself starts a sequence (Esc, then an arrow). */
    if (b1 >= 0x80
        || (b1 == 0x1B && len >= 3 && (bytes[2] == '[' || bytes[2] == 'O'))) {
        byte_key(u, 0x1B, 0);
        return;
    }
    /* An Alt chord: ESC and one byte that introduces no sequence. */
    if (b1 != '[' && b1 != 'O') {
        u->len = 2;
        byte_key(u, b1, MOD_ALT);
        return;
    }

    /* CSI: ESC [ then parameter / intermediate bytes (0x20-0x3f) up to a final
       (0x40-0x7e). SS3: ESC O then any one byte 0x20-0x7e. */
    const char *body  = bytes + 1;
    size_t      avail = len - 1;
    size_t      k     = 1;
    if (b1 == '[') {
        while (k < avail && k < AXL_CONSOLE_ESC_BUF_SIZE
               && body[k] >= 0x20 && body[k] <= 0x3F) {
            k++;
        }
        if (k == AXL_CONSOLE_ESC_BUF_SIZE) {
            u->len = 1 + k;   /* overlong: dropped, no key */
            return;
        }
    }
    char f = k < avail ? body[k] : '\0';
    if (k == avail || f < (b1 == '[' ? 0x40 : 0x20) || f > 0x7E) {
        /* Still open at the end of the run, or broken off by a byte that cannot
           continue it. Each call is self-contained -- xterm.js and an SSH client
           deliver a whole sequence per keypress -- so it is a bare Esc plus its
           body bytes as literal keys, never held into the next call, where it
           would splice onto that call's bytes (a dropped final byte turning a
           later 'A' into Up). The breaking byte starts the next unit. */
        u->len = 1 + k;
        byte_key(u, 0x1B, 0);
        for (size_t j = 0; j < k; j++) {
            byte_key(u, (unsigned char)body[j], 0);
        }
        return;
    }
    u->len = 1 + k + 1;
    if (b1 == '[') {
        csi_key(u, body + 1, k - 1, f);
    } else {
        const char *p = axl_strchr(final_keys, f);
        if (p != NULL) {
            unit_key(u, final_scan[p - final_keys], 0, 0);
        }
    }
}

/* Keys queued and unread. Read at TPL_HIGH_LEVEL so head and tail are one
   consistent pair against a reader at a lower TPL. */
size_t
axl_console_input_queued(const AxlConsoleInput *e)
{
    EFI_TPL old  = axl_efi_call(gBS->RaiseTPL, 1, TPL_HIGH_LEVEL);
    UINTN   used = (e->head + AXL_CONSOLE_KEY_RING_SIZE - e->tail)
                   % AXL_CONSOLE_KEY_RING_SIZE;
    axl_efi_call(gBS->RestoreTPL, 1, old);
    return (size_t)used;
}

/* One slot is the full/empty sentinel, so an empty ring has
   AXL_CONSOLE_KEY_RING_SIZE - 1 of room. */
size_t
axl_console_input_room(const AxlConsoleInput *e)
{
    return (size_t)(AXL_CONSOLE_KEY_RING_SIZE - 1) - axl_console_input_queued(e);
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
    return axl_console_input_push_notify(e, k) ? AXL_OK : AXL_BUSY;
}

/* Decode the unit at bytes[0] (len >= 1). */
static void
decode_unit(const char *bytes, size_t len, InputUnit *u)
{
    unsigned char b = (unsigned char)bytes[0];

    u->nkeys = 0;
    u->len   = 1;
    if (b == 0x1B) {
        decode_escape_unit(bytes, len, u);
    } else if (b < 0x80) {
        /* Terminals (xterm.js, xterm, tmux) send 0x7f (DEL) for the Backspace
           key; UEFI backspace is UnicodeChar 0x08, as a USB keyboard reports
           it, so byte_key remaps it. (The Delete *key* arrives as the CSI "3~"
           escape and is decoded to SCAN_DELETE; it never reaches here.) */
        byte_key(u, b, 0);
    } else if ((b & 0xE0) == 0xC0 && len >= 2) {
        unit_key(u, 0, (uint16_t)((uint16_t)((b & 0x1F) << 6)
                                  | (uint16_t)(bytes[1] & 0x3F)), 0);
        u->len = 2;
    } else if ((b & 0xF0) == 0xE0 && len >= 3) {
        unit_key(u, 0, (uint16_t)((uint16_t)((b & 0x0F) << 12)
                                  | (uint16_t)((bytes[1] & 0x3F) << 6)
                                  | (uint16_t)(bytes[2] & 0x3F)), 0);
        u->len = 3;
    }
    /* else: an incomplete or invalid lead byte -- consumed, no key. */
}

int
axl_console_input_inject_text_partial(AxlConsoleInput *e, const char *bytes, size_t len,
                                      size_t *consumed)
{
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (e == NULL || bytes == NULL || consumed == NULL) {
        return AXL_ERR;
    }
    size_t i = 0;
    while (i < len) {
        InputUnit u;
        decode_unit(bytes + i, len - i, &u);
        /* Whole or not at all: a character or a sequence is never split between
           two calls, so the caller can offer bytes + i again as its own call. */
        if (u.nkeys > axl_console_input_room(e)) {
            *consumed = i;
            return AXL_BUSY;
        }
        for (size_t k = 0; k < u.nkeys; k++) {
            EFI_KEY_DATA key = {0};
            key.Key.ScanCode            = u.scan[k];
            key.Key.UnicodeChar         = u.uni[k];
            key.KeyState.KeyShiftState  = u.shift[k];
            key.KeyState.KeyToggleState = AXL_CONSOLE_TOGGLE_KEYBOARD;
            axl_console_input_push_notify(e, key);
        }
        i += u.len;
    }
    *consumed = len;
    return AXL_OK;
}

int
axl_console_input_inject_text(AxlConsoleInput *e, const char *bytes, size_t len)
{
    size_t consumed = 0;
    return axl_console_input_inject_text_partial(e, bytes, len, &consumed);
}
