/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-mirror.c
    Mirror the firmware console to a byte sink and inject remote input.

    Wraps gST->ConIn/ConOut(/StdErr) and the ConsoleInHandle's
    SimpleTextInputEx with AXL forwarders: ConOut operations are
    translated to a VT/ANSI byte stream handed to a caller sink; injected
    remote keys are pushed into a ring the wrapped ConIn returns, waking a
    blocked Shell via the WaitForKey event.

    Ported from the EDK2 SoftBMC ConsoleWrapper, but simpler: the pump is
    the consumer's loop (axl_loop_attach_driver dispatches it from a
    firmware timer in the background), so the wrappers carry NO HTTP
    polling and the mirror owns NO timer. The wrapped ConIn only injects
    and falls through to the physical keyboard.

    Single global console ⇒ single mirror instance (a singleton guarded by
    g_mirror); the wrappers recover state through it. An atexit hook
    restores the console if the process exits without an explicit
    uninstall.
**/

#include <axl/axl-console-mirror.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-atexit.h>
#include <uefi/axl-uefi.h>

AXL_LOG_DOMAIN("conmirror");

#define KEY_RING_SIZE   64
#define ESC_BUF_SIZE    16

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
/* SCAN_ESC (0x17) comes from <uefi/axl-uefi-extra.h>. */

struct AxlConsoleMirror {
    /* Saved originals. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL    *orig_conout;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL     *orig_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *orig_coninex;

    /* Wrapper protocol structs — gST/ConsoleInHandle point into these. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL     my_conout;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL      my_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL   my_coninex;

    EFI_EVENT  wait_key;
    EFI_EVENT  wait_key_ex;

    /* Key injection ring. */
    EFI_INPUT_KEY ring[KEY_RING_SIZE];
    UINTN         head;
    UINTN         tail;

    /* Config (copied). */
    AxlConsoleSinkFn sink;
    void            *user;
    uint32_t         cols;
    uint32_t         rows;
    bool             passthrough;

    /* Cursor-dedup state: the last position emitted via SetCursorPosition
       (or 0,0 after ClearScreen). Used only to suppress redundant cursor
       escapes; NOT advanced from text output (that heuristic drifts on
       line-wrap and would falsely suppress a needed reposition). */
    int32_t cur_row;   /* -1 = unknown */
    int32_t cur_col;

    /* inject_text escape-sequence parser state (split-call safe). */
    char   esc_buf[ESC_BUF_SIZE];
    size_t esc_len;
    bool   in_esc;

    uint32_t atexit_handle;
    bool     reinstalled_ex;  /* did we ReinstallProtocolInterface ConInEx? */
};

static AxlConsoleMirror *g_mirror;  /* the one active instance */

// ---------------------------------------------------------------------------
// Sink helpers
// ---------------------------------------------------------------------------

static void
emit(AxlConsoleMirror *m, const char *bytes, size_t len)
{
    if (m->sink != NULL && len > 0) {
        m->sink(bytes, len, m->user);
    }
}

static void
emit_cstr(AxlConsoleMirror *m, const char *s)
{
    emit(m, s, axl_strlen(s));
}

/* Emit a UCS-2 string as UTF-8, chunked, no truncation. BMP only — the UEFI
   console is UCS-2 (no surrogate pairs); a lone surrogate code unit would be
   emitted as its 3-byte form, which the console never produces in practice. */
static void
emit_ucs2(AxlConsoleMirror *m, const CHAR16 *s)
{
    if (m->sink == NULL || s == NULL) {
        return;
    }
    char   buf[256];
    size_t n = 0;
    for (; *s != 0; s++) {
        unsigned c = (unsigned)*s;
        char     tmp[3];
        size_t   tn;
        if (c < 0x80) {
            tmp[0] = (char)c;
            tn = 1;
        } else if (c < 0x800) {
            tmp[0] = (char)(0xC0 | (c >> 6));
            tmp[1] = (char)(0x80 | (c & 0x3F));
            tn = 2;
        } else {
            tmp[0] = (char)(0xE0 | (c >> 12));
            tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F));
            tn = 3;
        }
        if (n + tn > sizeof(buf)) {
            m->sink(buf, n, m->user);
            n = 0;
        }
        for (size_t i = 0; i < tn; i++) {
            buf[n++] = tmp[i];
        }
    }
    if (n > 0) {
        m->sink(buf, n, m->user);
    }
}

/* UEFI text attribute → ANSI SGR "ESC[0;fg;bgm". 0..15 fg, 0..7 bg. */
static void
emit_attr(AxlConsoleMirror *m, UINTN attr)
{
    /* UEFI fg 0-15 → ANSI SGR. 0-7 standard (30-37), 8-15 bright (90-97),
       EXCEPT index 14: ANSI bright-yellow (93) renders as lime/green on many
       terminals, so map UEFI "yellow" to plain 33 (matches the EDK2 original
       — deliberate, do not "fix" to 93). */
    static const uint8_t fg_map[16] = {
        30, 34, 32, 36, 31, 35, 33, 37,
        90, 94, 92, 96, 91, 95, 33, 97
    };
    static const uint8_t bg_map[8] = { 40, 44, 42, 46, 41, 45, 43, 47 };

    if (m->sink == NULL) {
        return;
    }
    unsigned fg = (unsigned)(attr & 0x0F);
    unsigned bg = (unsigned)((attr >> 4) & 0x07);
    char buf[20];
    int  n = axl_snprintf(buf, sizeof(buf), "\x1b[0;%u;%um",
                          fg_map[fg], bg_map[bg]);
    if (n > 0) {
        emit(m, buf, (size_t)n);
    }
}

// ---------------------------------------------------------------------------
// Key ring
// ---------------------------------------------------------------------------

/* The ring is touched from two TPLs: injection runs from the consumer's
   timer-pumped loop at TPL_CALLBACK; ReadKeyStroke runs from the foreground
   Shell at TPL_APPLICATION. Raise to TPL_HIGH_LEVEL to make head/tail updates
   atomic against that preemption (the re-entrancy lesson from the EDK2
   wrapper, design §7). SignalEvent is callable at TPL_HIGH_LEVEL; the notify
   just runs once TPL drops. */
static bool
ring_push(AxlConsoleMirror *m, EFI_INPUT_KEY key)
{
    EFI_TPL old  = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    UINTN   next = (m->head + 1) % KEY_RING_SIZE;
    bool    ok   = (next != m->tail);
    if (ok) {
        m->ring[m->head] = key;
        m->head = next;
        if (m->wait_key != NULL) {
            gBS->SignalEvent(m->wait_key);
        }
        if (m->wait_key_ex != NULL) {
            gBS->SignalEvent(m->wait_key_ex);
        }
    }
    gBS->RestoreTPL(old);
    return ok;
}

static bool
ring_pop(AxlConsoleMirror *m, EFI_INPUT_KEY *key)
{
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    bool    ok  = (m->head != m->tail);
    if (ok) {
        *key = m->ring[m->tail];
        m->tail = (m->tail + 1) % KEY_RING_SIZE;
    }
    gBS->RestoreTPL(old);
    return ok;
}

static void
ring_drain(AxlConsoleMirror *m)
{
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    m->head = 0;
    m->tail = 0;
    gBS->RestoreTPL(old);
}

static void
ring_resignal_if_more(AxlConsoleMirror *m)
{
    if (m->head != m->tail) {
        if (m->wait_key != NULL) {
            gBS->SignalEvent(m->wait_key);
        }
        if (m->wait_key_ex != NULL) {
            gBS->SignalEvent(m->wait_key_ex);
        }
    }
}

// ---------------------------------------------------------------------------
// ConOut wrappers
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
wrap_out_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    if (m->passthrough && m->orig_conout != NULL) {
        return m->orig_conout->Reset(m->orig_conout, ext);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    EFI_STATUS st = EFI_SUCCESS;
    if (m->passthrough && m->orig_conout != NULL) {
        st = m->orig_conout->OutputString(m->orig_conout, String);
    }
    emit_ucs2(m, String);
    return st;
}

static EFI_STATUS EFIAPI
wrap_out_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
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
    AxlConsoleMirror *m = g_mirror;
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
    AxlConsoleMirror *m = g_mirror;
    if (m->orig_conout != NULL) {
        return m->orig_conout->SetMode(m->orig_conout, ModeNumber);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    if (m->passthrough && m->orig_conout != NULL) {
        m->orig_conout->SetAttribute(m->orig_conout, Attribute);
    }
    emit_attr(m, Attribute);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    EFI_STATUS st = EFI_SUCCESS;
    if (m->passthrough && m->orig_conout != NULL) {
        st = m->orig_conout->ClearScreen(m->orig_conout);
    }
    emit_cstr(m, "\x1b[2J\x1b[H");
    m->cur_row = 0;   /* cursor is now home; dedup tracks from here */
    m->cur_col = 0;
    return st;
}

static EFI_STATUS EFIAPI
wrap_out_set_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    if (m->passthrough && m->orig_conout != NULL) {
        m->orig_conout->SetCursorPosition(m->orig_conout, Column, Row);
    }
    /* Dedup: full-screen apps re-position to the same cell to blink the
       cursor; suppress the redundant escape flood. */
    if ((int32_t)Row == m->cur_row && (int32_t)Column == m->cur_col) {
        return EFI_SUCCESS;
    }
    char buf[24];
    int  n = axl_snprintf(buf, sizeof(buf), "\x1b[%u;%uH",
                          (unsigned)(Row + 1), (unsigned)(Column + 1));
    if (n > 0) {
        emit(m, buf, (size_t)n);
    }
    m->cur_row = (int32_t)Row;
    m->cur_col = (int32_t)Column;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_out_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    if (m->passthrough && m->orig_conout != NULL) {
        m->orig_conout->EnableCursor(m->orig_conout, Visible);
    }
    emit_cstr(m, Visible ? "\x1b[?25h" : "\x1b[?25l");
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
    ring_drain(g_mirror);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_in_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    if (ring_pop(m, Key)) {
        ring_resignal_if_more(m);
        return EFI_SUCCESS;
    }
    if (m->orig_conin != NULL) {
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
    ring_drain(g_mirror);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_inex_read_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
    EFI_INPUT_KEY key;
    if (ring_pop(m, &key)) {
        ring_resignal_if_more(m);
        axl_memset(KeyData, 0, sizeof(*KeyData));
        KeyData->Key = key;
        return EFI_SUCCESS;
    }
    if (m->orig_coninex != NULL) {
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

static EFI_STATUS EFIAPI
wrap_inex_register_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                          EFI_KEY_DATA *KeyData,
                          EFI_KEY_NOTIFY_FUNCTION fn,
                          void **NotifyHandle)
{
    (void)This;
    AxlConsoleMirror *m = g_mirror;
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
    AxlConsoleMirror *m = g_mirror;
    if (m->orig_coninex != NULL && m->orig_coninex->UnregisterKeyNotify != NULL) {
        return m->orig_coninex->UnregisterKeyNotify(m->orig_coninex, handle);
    }
    return EFI_SUCCESS;
}

/* WaitForKey EVT_NOTIFY_WAIT callback: fires from WaitForEvent/CheckEvent when
   a reader waits and the ring is empty — poll the physical keyboard so the
   local keyboard keeps working under a foreground Shell. The push MUST signal
   (via ring_push): unlike the EDK2 original we have no separate 10ms timer to
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
    AxlConsoleMirror *m = g_mirror;
    if (m == NULL || m->head != m->tail) {
        return;  /* keys already pending */
    }
    if (m->orig_conin != NULL) {
        EFI_INPUT_KEY k;
        if (!EFI_ERROR(m->orig_conin->ReadKeyStroke(m->orig_conin, &k))) {
            ring_push(m, k);
        }
    }
}

// ---------------------------------------------------------------------------
// inject_text — xterm/VT escape decoder
// ---------------------------------------------------------------------------

/* Decode an accumulated escape body (the bytes AFTER the leading ESC) to a
   UEFI scan code, or 0 if not (yet) a recognized complete sequence. Sets
   *complete=true once the body is a full sequence (recognized or not). */
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

    /* ESC followed by something that isn't a CSI/SS3 introducer ⇒ the byte
       is the bare Esc key; the introducer byte is handled by the caller. */
    *complete = true;
    return SCAN_ESC;
}

static void
inject_unicode(AxlConsoleMirror *m, uint16_t ch)
{
    EFI_INPUT_KEY k = { .ScanCode = 0, .UnicodeChar = ch };
    ring_push(m, k);
}

static void
inject_scan(AxlConsoleMirror *m, uint16_t scan)
{
    EFI_INPUT_KEY k = { .ScanCode = scan, .UnicodeChar = 0 };
    ring_push(m, k);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void
mirror_atexit(void *data)
{
    axl_console_mirror_uninstall((AxlConsoleMirror *)data);
}

int
axl_console_mirror_install(AxlConsoleMirror **out, const AxlConsoleMirrorConfig *cfg)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || cfg == NULL || cfg->sink == NULL) {
        return AXL_ERR;
    }
    if (g_mirror != NULL) {
        axl_warning("console mirror already installed");
        return AXL_ERR;
    }

    AxlConsoleMirror *m = axl_calloc(1, sizeof(*m));
    if (m == NULL) {
        return AXL_ERR;
    }
    m->sink        = cfg->sink;
    m->user        = cfg->user;
    m->cols        = cfg->cols;
    m->rows        = cfg->rows;
    m->passthrough = cfg->passthrough_local;
    m->cur_row     = -1;
    m->cur_col     = -1;

    m->orig_conout = gST->ConOut;
    m->orig_conin  = gST->ConIn;

    /* ConOut wrapper: copy the original (preserves Mode), override methods. */
    if (m->orig_conout != NULL) {
        m->my_conout = *m->orig_conout;
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
    m->my_coninex.Reset               = (void *)wrap_inex_reset;
    m->my_coninex.ReadKeyStrokeEx     = wrap_inex_read_key;
    m->my_coninex.SetState            = (void *)wrap_inex_set_state;
    m->my_coninex.RegisterKeyNotify   = wrap_inex_register_notify;
    m->my_coninex.UnregisterKeyNotify = wrap_inex_unregister_notify;

    st = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                          wait_key_cb, NULL, &m->wait_key_ex);
    if (EFI_ERROR(st)) {
        gBS->CloseEvent(m->wait_key);
        axl_free(m);
        return AXL_ERR;
    }
    m->my_coninex.WaitForKeyEx = m->wait_key_ex;

    /* Publish the singleton BEFORE swapping gST so the wrappers (which may
       fire from a ReinstallProtocolInterface notify) see a live instance. */
    g_mirror = m;

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
            axl_warning("console mirror: ConInEx reinstall failed (editor keys "
                        "may not work)");
        }
    } else {
        m->orig_coninex = NULL;
    }

    /* Swap gST pointers AFTER the reinstall (which can make ConSplitter
       rewrite gST->ConIn). */
    gST->ConOut = &m->my_conout;
    gST->ConIn  = &m->my_conin;
    gST->StdErr = &m->my_conout;

    m->atexit_handle = axl_atexit(mirror_atexit, m);

    *out = m;
    axl_info("console mirror installed (%ux%u)", m->cols, m->rows);
    return AXL_OK;
}

void
axl_console_mirror_uninstall(AxlConsoleMirror *m)
{
    if (m == NULL || g_mirror != m) {
        return;
    }

    if (m->reinstalled_ex && m->orig_coninex != NULL) {
        EFI_GUID ex_guid = gEfiSimpleTextInputExProtocolGuid;
        gBS->ReinstallProtocolInterface(gST->ConsoleInHandle, &ex_guid,
                                        &m->my_coninex, m->orig_coninex);
    }
    if (m->orig_conout != NULL) {
        gST->ConOut = m->orig_conout;
        gST->StdErr = m->orig_conout;
    }
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

    g_mirror = NULL;
    axl_free(m);
    axl_info("console mirror uninstalled");
}

int
axl_console_mirror_inject_key(AxlConsoleMirror *m, uint16_t scan, uint16_t unicode)
{
    if (m == NULL) {
        return AXL_ERR;
    }
    EFI_INPUT_KEY k = { .ScanCode = scan, .UnicodeChar = unicode };
    return ring_push(m, k) ? AXL_OK : AXL_ERR;
}

int
axl_console_mirror_inject_text(AxlConsoleMirror *m, const char *bytes, size_t len)
{
    if (m == NULL || bytes == NULL) {
        return AXL_ERR;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];

        if (m->in_esc) {
            /* Bare ESC followed by a CSI/SS3 introducer? keep accumulating;
               otherwise the pending ESC is the Esc key and this byte restarts
               normal handling. */
            if (m->esc_len == 0 && b != '[' && b != 'O') {
                inject_scan(m, SCAN_ESC);
                m->in_esc = false;
                /* fall through to handle b as a normal byte below */
            } else {
                if (m->esc_len < sizeof(m->esc_buf)) {
                    m->esc_buf[m->esc_len++] = (char)b;
                }
                bool     complete = false;
                uint16_t scan = decode_escape(m->esc_buf, m->esc_len, &complete);
                if (complete) {
                    if (scan != 0) {
                        inject_scan(m, scan);
                    }
                    m->in_esc  = false;
                    m->esc_len = 0;
                } else if (m->esc_len >= sizeof(m->esc_buf)) {
                    /* Overlong / unrecognized — drop to avoid wedging. */
                    m->in_esc  = false;
                    m->esc_len = 0;
                }
                continue;
            }
        }

        if (b == 0x1B) {
            m->in_esc  = true;
            m->esc_len = 0;
            continue;
        }

        /* UTF-8 → a single BMP unicode key (input is typically ASCII). */
        if (b < 0x80) {
            /* Terminals (xterm.js) send 0x7f (DEL) for the Backspace key;
               UEFI backspace is UnicodeChar 0x08, so remap it here — exactly
               what TerminalDxe does for an incoming 0x7f. (The Delete *key*
               arrives as the CSI "3~" escape and is decoded to SCAN_DELETE by
               decode_escape; it never reaches this byte path.) */
            inject_unicode(m, (b == 0x7f) ? 0x08 : b);
        } else if ((b & 0xE0) == 0xC0 && i + 1 < len) {
            uint16_t cp = (uint16_t)((b & 0x1F) << 6)
                        | (uint16_t)(bytes[i + 1] & 0x3F);
            inject_unicode(m, cp);
            i += 1;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < len) {
            uint16_t cp = (uint16_t)((b & 0x0F) << 12)
                        | (uint16_t)((bytes[i + 1] & 0x3F) << 6)
                        | (uint16_t)(bytes[i + 2] & 0x3F);
            inject_unicode(m, cp);
            i += 2;
        }
        /* else: incomplete/invalid lead byte — skip. */
    }

    /* Flush any escape state that didn't complete within this call. Each
       inject_text call is self-contained: xterm.js delivers a whole escape
       sequence per keypress, so a sequence that is still open at end-of-call
       is a bare Esc (its own write) or a truncated/garbled run. Treat the
       leading ESC as the Esc key and re-inject the accumulated body bytes as
       literal keys, rather than holding state into the next call — a held
       partial would otherwise splice onto the next call's bytes and corrupt
       that keystroke (e.g. a dropped final byte turning a later 'A' into Up). */
    if (m->in_esc) {
        inject_scan(m, SCAN_ESC);
        for (size_t j = 0; j < m->esc_len; j++) {
            inject_unicode(m, (unsigned char)m->esc_buf[j]);
        }
        m->in_esc  = false;
        m->esc_len = 0;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Test seam (no public header). Exercises the REAL inject_text/inject_key
// byte->key decoder without installing the mirror — install wraps the live
// gST->ConIn/ConOut and wedges the combined unit boot (see the AxlConsoleMirror
// test note in axl-test-util.c). Construct a bare, un-wrapped instance (ring +
// esc state only; no console wrap, no g_mirror, no WaitForKey event so
// ring_push won't SignalEvent), drive inject_* against its ring, and pop the
// decoded keys. Free with axl_free. The console-mirror unit test calls these.
// ---------------------------------------------------------------------------

AxlConsoleMirror *
_axl_console_mirror_new_for_test(void)
{
    return axl_calloc(1, sizeof(AxlConsoleMirror));
}

bool
_axl_console_mirror_test_pop_key(AxlConsoleMirror *m, uint16_t *scan,
                                 uint16_t *unicode)
{
    EFI_INPUT_KEY k;
    if (m == NULL || !ring_pop(m, &k)) {
        return false;
    }
    if (scan != NULL) {
        *scan = k.ScanCode;
    }
    if (unicode != NULL) {
        *unicode = k.UnicodeChar;
    }
    return true;
}

void
axl_console_mirror_set_size(AxlConsoleMirror *m, uint32_t cols, uint32_t rows)
{
    if (m == NULL) {
        return;
    }
    m->cols = cols;
    m->rows = rows;
}

void
axl_console_mirror_reset(AxlConsoleMirror *m)
{
    if (m == NULL) {
        return;
    }
    ring_drain(m);
    m->cur_row = -1;
    m->cur_col = -1;
    m->in_esc  = false;
    m->esc_len = 0;
    /* Alt-screen enter/leave lands with the full-screen P2 work; until then
       there is no alt-screen state to leave here. */
}
