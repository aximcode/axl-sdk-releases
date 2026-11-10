/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-lines.c
    Console text, a line at a time: a line exactly the console's width.

    A UEFI text console wraps the moment a character lands in its last column:
    GraphicsConsole outputs CR LF right there, and TerminalDxe's and
    ConSplitter's cursor records move to column 0 of the next row (scrolling
    on the last). A Unix terminal defers that wrap until the next printable
    (xterm's and the Linux VT's "pending wrap"), so the line's own CR LF only
    finishes the row it is on. Written as it stands, a line that exactly fills
    a row is therefore followed by an EMPTY row on a UEFI console.

    So the line break after a wrap is not written as CR LF through the
    console. The wrap is judged device by device -- each device the output
    reaches is followed through the line's last character, as a narrower one
    can wrap a line a wider one does not -- and each gets what leaves it
    where it now stands:

    - a terminal TerminalDxe drives with any type but TtyTerm (PC-ANSI,
      VT-100, VT-100+, VT-UTF8, Linux, Xterm-R6, VT400, SCO): CR LF, then
      the cursor put back on the row the wrap left it on. TerminalDxe does
      not send its wrap down the wire (only its TtyTerm type does), so the
      terminal itself sits in the last column of
      the full row in ITS pending wrap: the CR LF is the newline it needs --
      and a serial capture keeps one line per line -- and the cursor move
      (an absolute one on the wire) puts TerminalDxe's record back in step
      with it;
    - anything else -- GraphicsConsole, a TtyTerm terminal, AXL's own
      console device, whose encoder sends the wrap itself -- a space and a
      backspace: no motion on a console that has wrapped, and a terminal
      this cannot name (a terminal driver that is not EDK2's) that still sits
      in a pending wrap is taken out of it, so the Shell's next prompt,
      placed by an absolute cursor move, does not land on the full row when
      it is the screen's last;
    - a device the line did not wrap -- it runs a geometry of its own (in
      the mode OVMF boots x64 in, the display is 100 columns and the serial
      terminal 80) -- gets the break as written.

    "Has just wrapped" is OBSERVED, never computed: the line's last character
    took that device's cursor from a column past 0 to column 0. That needs
    no width, and it is false for anything whose cursor does not move -- the
    Shell's `>`/`2>` file is a SimpleTextOut whose Mode is a frozen copy of
    the console's, so a file gets every CR LF, byte for byte. A newline written
    in a later call than its line (`print(line); print("\n")`) is matched to
    the wrap by every cursor still standing where that wrap left it.

    Freestanding over the system table it is handed, so the pack stub,
    which runs before libaxl, links this same object.
**/

#include "axl-backend-native-lines.h"

/* How many outputs' pending wraps are remembered across calls. */
#define TEXT_LINES_WRAP_SLOTS  4

/* The stack buffer a piece of text is copied into (CHAR16s, its NUL
   included): 512 bytes, so a line of the widest common console (an 80- or
   100-column row) is one OutputString, and the stack a write may be made on
   at a raised TPL is not strained. */
#define TEXT_LINES_CHUNK  256

/* How many devices one write is followed on; a console splitter with more
   is treated as one device. */
#define TEXT_LINES_DEVICES  8

/* One device a write reaches, and what the line's last character did to
   its cursor. */
typedef struct {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out;
    EFI_HANDLE                       handle;    ///< NULL when not known
    INT32                            before;    ///< column before the last character
    bool                             wrapped;   ///< it moved the column from past 0 to 0
    UINTN                            cols;      ///< its width; 0 when it would not say
} TextLinesDev;

/* The devices one write reaches: a verified console splitter's, or the
   output itself. */
typedef struct {
    EFI_BOOT_SERVICES *bs;
    UINTN              count;
    TextLinesDev       dev[TEXT_LINES_DEVICES];
    INT32              out_before;    ///< the output's own column before the last character
    UINTN              out_cols;      ///< the output's own width (QueryMode); 0 when unknown
    bool               out_wrapped;   ///< the line filled the output's own row (its width)
    bool               cr_after;      ///< a CR followed the wrapping character
} TextLinesTo;

/* A write that ended on a wrap, its break still to come: the cursor record
   (Mode) it was written through, that record's cursor, and each device the
   write reached -- which protocol, on which handle -- with its cursor and
   wrap.

   Keyed by the Mode, because a Mode is one console's cursor: the Shell's
   console logger and the splitter beneath it are two outputs sharing one,
   so a full line standard output ends through the logger and the newline
   standard error writes through the splitter are one line, as on a
   terminal. What tells a NEW output apart -- one handed a gone output's
   Mode memory, a redirect's file, say -- is the devices: the slot is
   pending only while the same device protocols, on the same handles, still
   stand where they were recorded. Any later write through the same Mode
   that does not end on a wrap clears it. */
typedef struct {
    SIMPLE_TEXT_OUTPUT_MODE         *mode;      ///< NULL = free
    INT32                            col;
    INT32                            row;
    UINTN                            count;
    EFI_HANDLE                       handle[TEXT_LINES_DEVICES];
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *dev[TEXT_LINES_DEVICES];
    INT32                            dcol[TEXT_LINES_DEVICES];
    INT32                            drow[TEXT_LINES_DEVICES];
    bool                             wrapped[TEXT_LINES_DEVICES];
    bool                             out_wrapped;
    bool                             cr_after;
} TextLinesWrap;

/* The last fan-out verdict (fanout_verified), and what it was taken for. */
typedef struct {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out;
    SIMPLE_TEXT_OUTPUT_MODE         *mode;
    EFI_HANDLE                       conout;
    EFI_HANDLE                       stderr_handle;
    bool                             err;
    bool                             verdict;
} TextLinesFanout;

/* The terminal types EDK2's TerminalDxe drives without sending its wrap
   down the wire (TerminalConOut.c sends it for TtyTerm alone): the four UEFI
   ones and MdeModulePkg's Linux, Xterm-R6, VT400 and SCO. Bytes, not
   EFI_GUIDs or pointers, so the table costs the pack stub no relocations;
   in .data, not .rodata, for the stub's FileAlignment (src/pack/README.md). */
static UINT8 mDeferredWrapTypes[][16] = {
    { 0x53, 0x47, 0xc1, 0xe0, 0xbe, 0xf9, 0xd2, 0x11,     /* PC-ANSI */
      0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d },
    { 0x65, 0x60, 0xa6, 0xdf, 0x19, 0xb4, 0xd3, 0x11,     /* VT-100 */
      0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d },
    { 0x0b, 0xc7, 0xae, 0x7b, 0xe0, 0x57, 0x76, 0x4c,     /* VT-100+ */
      0x8e, 0x87, 0x2f, 0x9e, 0x28, 0x08, 0x83, 0x43 },
    { 0xd6, 0xa0, 0x15, 0xad, 0xec, 0x8b, 0xcf, 0x4a,     /* VT-UTF8 */
      0xa0, 0x73, 0xd0, 0x1d, 0xe7, 0x7e, 0x2d, 0x88 },
    { 0x7f, 0x4a, 0x36, 0xe4, 0x25, 0xf8, 0x0e, 0x43,     /* gEdkiiLinuxTermGuid */
      0x9d, 0x3a, 0x9c, 0x9b, 0xe6, 0x81, 0x7c, 0xa5 },
    { 0x6b, 0xa5, 0xfc, 0xfb, 0x36, 0xbb, 0x78, 0x4b,     /* gEdkiiXtermR6Guid */
      0xaa, 0xab, 0xbe, 0x1b, 0x97, 0xec, 0x7c, 0xcb },
    { 0xdd, 0xdd, 0x46, 0x8e, 0x49, 0x3d, 0x9d, 0x4a,     /* gEdkiiVT400Guid */
      0xb8, 0x75, 0x3c, 0x08, 0x6f, 0x6a, 0xa2, 0xbd },
    { 0xe0, 0xd6, 0x7d, 0xfc, 0x3c, 0x81, 0x4d, 0x43,     /* gEdkiiSCOTermGuid */
      0xb4, 0xda, 0x3b, 0xd6, 0x49, 0xe9, 0xe1, 0x5a },
};

static TextLinesWrap   mWrap[TEXT_LINES_WRAP_SLOTS];
/* Two, for the console's two writers: standard output through the Shell's
   console logger and standard error through the splitter beneath it. Both
   name the ConOut tag, so a cache indexed by the tag would still re-verify
   on every alternation; these are kept per output, the older replaced. */
static TextLinesFanout mFanout[2];
static UINTN           mFanoutNext;

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/* Write @p n characters of @p s to @p out, in pieces of up to
   TEXT_LINES_CHUNK - 1 characters, each copied out to be NUL-terminated. */
static void
text_put(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out,
    const CHAR16                    *s,
    UINTN                            n
    )
{
    CHAR16 chunk[TEXT_LINES_CHUNK];
    UINTN  cap = sizeof(chunk) / sizeof(chunk[0]) - 1;
    while (n > 0) {
        UINTN k = (n < cap) ? n : cap;
        for (UINTN i = 0; i < k; i++) {
            chunk[i] = s[i];
        }
        chunk[k] = 0;
        out->OutputString(out, chunk);
        s += k;
        n -= k;
    }
}

/* Write a line's text, @p n > 0 characters with no line break, to @p out,
   following each device in @p to. Returns whether its last character
   wrapped any of them, each device's own verdict in its entry. A CR after
   that character ("<line>\r", its LF in the next write) leaves the wrap
   pending: on a console that has wrapped it moves nothing, and the LF that
   follows completes the one break. */
static bool
text_line(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out,
    TextLinesTo                     *to,
    const CHAR16                    *s,
    UINTN                            n
    )
{
    UINTN m   = n;   /* the text before any trailing CRs */
    bool  any = false;
    while (m > 0 && s[m - 1] == L'\r') {
        m--;
    }
    for (UINTN i = 0; i < to->count; i++) {
        to->dev[i].wrapped = false;
    }
    to->out_wrapped = false;
    to->cr_after    = n > m;
    if (out->Mode == NULL || m == 0 || s[m - 1] == L'\b') {
        text_put(out, s, n);
        return false;
    }
    text_put(out, s, m - 1);
    for (UINTN i = 0; i < to->count; i++) {
        to->dev[i].before = to->dev[i].out->Mode->CursorColumn;
    }
    to->out_before = out->Mode->CursorColumn;
    text_put(out, s + m - 1, 1);
    /* By the output's own width, not its record's movement: the Shell's
       console logger breaks its scroll-back at the splitter's QueryMode
       columns, which a first device narrower than the splitter would not
       show. The movement only when the width is unknown. */
    to->out_wrapped = (to->out_cols != 0)
                      ? (UINTN)to->out_before + 1 >= to->out_cols
                      : (to->out_before > 0 && out->Mode->CursorColumn == 0);
    for (UINTN i = 0; i < to->count; i++) {
        TextLinesDev *d = &to->dev[i];
        d->wrapped = d->before > 0 && d->out->Mode->CursorColumn == 0;
        any        = any || d->wrapped;
    }
    text_put(out, s + m, n - m);
    return any;
}

// ---------------------------------------------------------------------------
// Which devices, and which kind
// ---------------------------------------------------------------------------

/* Whether the 16 bytes at @p p are @p g. */
static bool
guid_at(
    const UINT8     *p,
    const EFI_GUID  *g
    )
{
    const UINT8 *b = (const UINT8 *)g;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
        if (p[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* Whether @p device is a terminal TerminalDxe drives without sending its
   wrap: its device path ends in a vendor node naming one of those types. */
static bool
deferred_wrap_terminal(
    EFI_BOOT_SERVICES  *bs,
    EFI_HANDLE          device
    )
{
    const UINT8 *p    = NULL;
    const UINT8 *last = NULL;
    if (device == NULL
        || bs->HandleProtocol(device, &gEfiDevicePathProtocolGuid, (VOID **)&p) != EFI_SUCCESS
        || p == NULL) {
        return false;
    }
    while (p[0] != END_DEVICE_PATH_TYPE) {
        UINTN len = (UINTN)p[2] | ((UINTN)p[3] << 8);
        if (len < 4) {
            return false;   /* malformed: no answer, so the safe one */
        }
        last = p;
        p += len;
    }
    if (last == NULL || last[0] != MESSAGING_DEVICE_PATH || last[1] != MSG_VENDOR_DP
        || last[2] < 4 + sizeof(EFI_GUID)) {
        return false;
    }
    for (UINTN t = 0; t < sizeof(mDeferredWrapTypes) / sizeof(mDeferredWrapTypes[0]); t++) {
        if (guid_at(last + 4, (const EFI_GUID *)mDeferredWrapTypes[t])) {
            return true;
        }
    }
    return false;
}

/* The Mode of the SimpleTextOut on @p handle, or NULL. */
static SIMPLE_TEXT_OUTPUT_MODE *
mode_on(
    EFI_BOOT_SERVICES  *bs,
    EFI_HANDLE          handle
    )
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t = NULL;
    if (handle == NULL
        || bs->HandleProtocol(handle, &gEfiSimpleTextOutputProtocolGuid, (VOID **)&t)
           != EFI_SUCCESS
        || t == NULL) {
        return NULL;
    }
    return t->Mode;
}

/* Whether @p out reaches the devices @p fanout tags: it is that console's
   splitter -- the SimpleTextOut on the system table's handle for it -- or
   shares its Mode, as the Shell's console logger does. Under the Shell's `>`
   the ConOut handle is the file's, so the logger, on the handle carrying
   EFI_SHELL_PROTOCOL, is asked too. A console tap, or any other wrapper with
   a Mode of its own, is not: it decides what reaches the devices, and with
   its local output off it promised them nothing. The verdict is kept for
   the output, its Mode and the two console handles it was taken against,
   for the last two outputs asked about. */
static bool
fanout_verified(
    EFI_SYSTEM_TABLE                 *st,
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *out,
    EFI_GUID                         *fanout
    )
{
    EFI_BOOT_SERVICES *bs      = st->BootServices;
    bool               err     = guid_at((const UINT8 *)fanout, &gEfiStandardErrorDeviceGuid);
    bool               found   = false;
    UINTN              count   = 0;
    EFI_HANDLE        *handles = NULL;
    if (out->Mode == NULL) {
        return false;
    }
    for (UINTN i = 0; i < 2; i++) {
        TextLinesFanout *c = &mFanout[i];
        if (c->out == out && c->mode == out->Mode && c->err == err
            && c->conout == st->ConsoleOutHandle
            && c->stderr_handle == st->StandardErrorHandle) {
            return c->verdict;
        }
    }
    found = mode_on(bs, err ? st->StandardErrorHandle : st->ConsoleOutHandle) == out->Mode;
    if (!found && !err
        && bs->LocateHandleBuffer(ByProtocol, &gEfiShellProtocolGuid, NULL, &count, &handles)
           == EFI_SUCCESS) {
        for (UINTN i = 0; i < count && !found; i++) {
            found = mode_on(bs, handles[i]) == out->Mode;
        }
        bs->FreePool(handles);
    }
    TextLinesFanout *c = &mFanout[mFanoutNext];
    mFanoutNext      = 1 - mFanoutNext;
    c->out           = out;
    c->mode          = out->Mode;
    c->err           = err;
    c->conout        = st->ConsoleOutHandle;
    c->stderr_handle = st->StandardErrorHandle;
    c->verdict       = found;
    return found;
}

/* Fill @p to with the devices @p out reaches: each device @p fanout tags
   when @p out verifiably is their splitter, else @p out itself, on
   @p device. */
static void
devices_of(
    EFI_SYSTEM_TABLE                 *st,
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *out,
    EFI_GUID                         *fanout,
    EFI_HANDLE                        device,
    TextLinesTo                      *to
    )
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_HANDLE         handles[TEXT_LINES_DEVICES];
    UINTN              size = sizeof(handles);
    to->bs       = bs;
    to->out_cols = 0;
    if (out->Mode != NULL && out->Mode->Mode >= 0) {
        UINTN rows = 0;
        if (out->QueryMode(out, (UINTN)out->Mode->Mode, &to->out_cols, &rows) != EFI_SUCCESS) {
            to->out_cols = 0;
        }
    }
    to->count = 0;
    if (fanout != NULL && fanout_verified(st, out, fanout)
        && bs->LocateHandle(ByProtocol, fanout, NULL, &size, handles) == EFI_SUCCESS) {
        for (UINTN i = 0; i < size / sizeof(EFI_HANDLE); i++) {
            EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *d = NULL;
            if (bs->HandleProtocol(handles[i], &gEfiSimpleTextOutputProtocolGuid,
                                   (VOID **)&d) == EFI_SUCCESS
                && d != NULL && d->Mode != NULL) {
                to->dev[to->count].out    = d;
                to->dev[to->count].handle = handles[i];
                to->count++;
            }
        }
    }
    if (to->count == 0) {
        to->dev[0].out    = out;
        to->dev[0].handle = (fanout == NULL) ? device : NULL;
        to->count         = 1;
    }
    for (UINTN i = 0; i < to->count; i++) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *d    = to->dev[i].out;
        UINTN                            rows = 0;
        to->dev[i].cols = 0;
        if (d->Mode != NULL
            && d->QueryMode(d, (UINTN)d->Mode->Mode, &to->dev[i].cols, &rows) != EFI_SUCCESS) {
            to->dev[i].cols = 0;
        }
    }
}

/* Whether the @p n characters of @p s, written from where each device's
   cursor stands, could put a character in any device's last column. Only
   printable ASCII is counted (one column each); anything else, or a device
   that would not give its width, answers yes, and the line is followed
   character by character. No: the line and its break go out in one write,
   as they did before any of this. */
static bool
can_wrap(
    const TextLinesTo  *to,
    const CHAR16       *s,
    UINTN               n
    )
{
    for (UINTN i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) {
            return true;
        }
    }
    for (UINTN i = 0; i < to->count; i++) {
        const TextLinesDev *d = &to->dev[i];
        if (d->cols == 0 || d->out->Mode->CursorColumn < 0
            || (UINTN)d->out->Mode->CursorColumn + n >= d->cols) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The break after a wrap
// ---------------------------------------------------------------------------

/* The line break @p brk (its @p n characters) after a wrap of at least one
   device in @p to.

   Whether the line filled @p out's OWN row -- by @p out's own width, from
   QueryMode -- decides how. Where it did, the console's own bookkeeping
   already ended the line: the Shell's console logger, which shares the
   splitter's Mode, breaks its scroll-back copy at exactly that width. So the
   break goes to the devices one by one (above), and one the line did not
   wrap gets it as it is.

   Where it did not -- a narrower device wrapped the line, the first one
   did not (OVMF's x64 boot mode: display 100 columns, serial terminal 80)
   -- the break goes THROUGH @p out, so the splitter's record and the
   logger's copy both end the line, and then each device that had wrapped
   is put back where its wrap left it: a TerminalDxe terminal by a cursor
   move (the break was the newline its wire needed), anything else by a
   cursor move to the row below the line -- one up from where the break left
   it, or, where the break scrolled the screen, two. Only cursor moves: no
   cell is written on a device that has already wrapped.

   A CR written after the wrapping character ("<line>\r", then "\n") has
   already ended a pending wrap, so a device that is not a TerminalDxe
   terminal gets nothing more. A TerminalDxe terminal still gets CR LF and
   its cursor move: the CR took the terminal itself back to column 0 of the
   FULL row, and the LF is what moves it -- and its wire -- to the next.

   After any write straight to a device, an empty string through @p out
   makes the splitter copy its first device's cursor again. */
static void
wrapped_break(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out,
    TextLinesTo                     *to,
    const CHAR16                    *brk,
    UINTN                            n
    )
{
    /* The characters it writes on its own, built on the stack: as string
       literals they are .rodata, and the pack stub's .rodata sits on a
       FileAlignment step on x64 (its .data does on aa64), where these few
       bytes cost every wrapped image 512 (src/pack/README.md). text_put
       copies what it writes, so they need no NUL; the empty string does. */
    CHAR16 crlf[2]      = { L'\r', L'\n' };
    CHAR16 spaceback[2] = { L' ', L'\b' };
    CHAR16 none         = 0;
    INT32  row[TEXT_LINES_DEVICES];
    if (to->out_wrapped || to->dev[0].out == out) {
        for (UINTN i = 0; i < to->count; i++) {
            TextLinesDev *d = &to->dev[i];
            if (!d->wrapped) {
                text_put(d->out, brk, n);
            } else if (deferred_wrap_terminal(to->bs, d->handle)) {
                INT32 r = d->out->Mode->CursorRow;
                text_put(d->out, crlf, 2);
                d->out->SetCursorPosition(d->out, 0, (UINTN)r);
            } else if (!to->cr_after) {
                text_put(d->out, spaceback, 2);
            }
            /* After a CR the wrap is already over: nothing to end. */
        }
    } else {
        for (UINTN i = 0; i < to->count; i++) {
            row[i] = to->dev[i].out->Mode->CursorRow;
        }
        text_put(out, brk, n);
        for (UINTN i = 0; i < to->count; i++) {
            TextLinesDev *d = &to->dev[i];
            if (!d->wrapped) {
                continue;
            }
            INT32 back = row[i];
            if (!deferred_wrap_terminal(to->bs, d->handle) && d->out->Mode->CursorRow == row[i]
                && back > 0) {
                back--;   /* the break scrolled: the line moved up a row with it */
            }
            d->out->SetCursorPosition(d->out, 0, (UINTN)back);
        }
    }
    if (to->dev[0].out != out) {
        out->OutputString(out, &none);
    }
}

/* The slot for @p mode, or NULL when it has none. */
static TextLinesWrap *
wrap_slot(
    SIMPLE_TEXT_OUTPUT_MODE  *mode
    )
{
    for (UINTN i = 0; i < TEXT_LINES_WRAP_SLOTS; i++) {
        if (mWrap[i].mode == mode) {
            return &mWrap[i];
        }
    }
    return NULL;
}

/* Whether @p out's last write ended on a wrap every cursor still stands
   at; when it did, @p to's devices take their recorded verdicts. */
static bool
wrap_pending(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *out,
    TextLinesTo                      *to
    )
{
    TextLinesWrap *slot = (out->Mode != NULL) ? wrap_slot(out->Mode) : NULL;
    if (slot == NULL || slot->count != to->count
        || out->Mode->CursorColumn != slot->col || out->Mode->CursorRow != slot->row) {
        return false;
    }
    for (UINTN i = 0; i < to->count; i++) {
        SIMPLE_TEXT_OUTPUT_MODE *m = to->dev[i].out->Mode;
        if (to->dev[i].out != slot->dev[i] || to->dev[i].handle != slot->handle[i]
            || m->CursorColumn != slot->dcol[i] || m->CursorRow != slot->drow[i]) {
            return false;
        }
    }
    for (UINTN i = 0; i < to->count; i++) {
        to->dev[i].wrapped = slot->wrapped[i];
    }
    to->out_wrapped = slot->out_wrapped;
    to->cr_after    = slot->cr_after;
    return true;
}

/* Record whether @p out's write ended on a wrap, and every cursor. */
static void
wrap_record(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *out,
    TextLinesTo                      *to,
    bool                              wrapped
    )
{
    TextLinesWrap *slot = wrap_slot(out->Mode);
    if (!wrapped) {
        if (slot != NULL) {
            slot->mode = NULL;
        }
        return;
    }
    if (slot == NULL) {
        slot = wrap_slot(NULL);
    }
    if (slot == NULL) {
        slot = &mWrap[0];   /* more outputs than slots: reuse one */
    }
    slot->mode  = out->Mode;
    slot->col   = out->Mode->CursorColumn;
    slot->row   = out->Mode->CursorRow;
    slot->count = to->count;
    for (UINTN i = 0; i < to->count; i++) {
        slot->dev[i]     = to->dev[i].out;
        slot->handle[i]  = to->dev[i].handle;
        slot->dcol[i]    = to->dev[i].out->Mode->CursorColumn;
        slot->drow[i]    = to->dev[i].out->Mode->CursorRow;
        slot->wrapped[i] = to->dev[i].wrapped;
    }
    slot->out_wrapped = to->out_wrapped;
    slot->cr_after    = to->cr_after;
}

void
axl_text_out_lines(
    EFI_SYSTEM_TABLE                 *st,
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *out,
    EFI_GUID                         *fanout,
    EFI_HANDLE                        device,
    const CHAR16                     *str
    )
{
    TextLinesTo   to;
    devices_of(st, out, fanout, device, &to);
    /* A wrap an earlier call ended on, if every cursor still stands there. */
    bool          wrapped = wrap_pending(out, &to);
    const CHAR16 *p       = str;
    while (*p != 0) {
        const CHAR16 *q = p;
        while (*q != 0 && *q != L'\n' && !(q[0] == L'\r' && q[1] == L'\n')) {
            q++;
        }
        UINTN brk = (*q == 0) ? 0 : (*q == L'\r') ? 2 : 1;
        if (q > p && out->Mode != NULL && !can_wrap(&to, p, (UINTN)(q - p))) {
            text_put(out, p, (UINTN)(q - p) + brk);   /* no wrap anywhere: as it was */
            wrapped = false;
            p       = q + brk;
            continue;
        }
        if (q > p) {
            wrapped = text_line(out, &to, p, (UINTN)(q - p));
        }
        if (*q == 0) {
            break;
        }
        if (wrapped) {
            wrapped_break(out, &to, q, brk);
        } else {
            text_put(out, q, brk);
        }
        wrapped = false;
        p       = q + brk;
    }
    if (out->Mode != NULL) {
        wrap_record(out, &to, wrapped);
    }
}
