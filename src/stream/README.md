# AxlStream — byte-stream abstraction

`AxlStream` is the polymorphic byte-source/sink — modeled on POSIX
`<stdio.h>`'s `FILE *`. It wraps a vtable over file handles, memory
buffers, the console, the BOM-detecting text decoder, etc. The set is
not closed: `axl_stream_open_custom` lets a consumer supply its own
backend on the same footing as the built-ins (see *Custom Backends*).
All public functions that take `AxlStream *` live in this module.

For path-based filesystem operations (read whole file, dir walk,
volume enumerate, stat), see the sibling **AxlFs** module
(`<axl/axl-fs.h>`, source in `src/fs/`).

Three-layer API shape:

1. **Simple helpers** (GLib-style): `axl_print`, `axl_printerr`
2. **Stream I/O** (POSIX-style): `axl_fopen`, `axl_fread`, `axl_fprintf`,
   `axl_fgets`, `axl_readline`, `axl_walk_lines`
3. **Low-level**: `axl_read`, `axl_write`, `axl_pread`, `axl_pwrite`

All strings are UTF-8. Paths in `axl_fopen` are converted to UCS-2
internally. Per-stream wire encoding (UCS-2 LE/BE/ASCII) is
available via `axl_stream_set_encoding`.

Header: `<axl/axl-stream.h>`

## Overview

AXL provides familiar POSIX-style I/O on top of UEFI's file protocols.
Paths use UTF-8 with forward slashes (`fs0:/path/to/file.txt`); AXL
converts to UCS-2 and backslashes internally.

## Console Output

`axl_printf` writes to the UEFI console (ConOut). It's available
immediately after `AXL_APP` or `axl_driver_init`:

```c
axl_printf("Hello %s, value=%d\n", name, 42);
axl_printerr("Error: %s\n", msg);  // writes to ConErr
```

### Tee (`-o:<file>` log support)

`axl_stream_set_stdout_tee` installs an additional stream that
receives every byte written to `axl_stdout` — `axl_print`,
`axl_printf`, direct `axl_write(axl_stdout, ...)`. Pass NULL to
clear; multiple calls replace the previous tee with no chaining.
The caller owns the tee stream and is responsible for closing it
(typical pairing: `axl_atexit`):

```c
AxlStream *log = axl_fopen("fs0:\\app.log", "a");
axl_stream_set_stdout_tee(log);
axl_atexit(close_log_stream, log);

axl_printf("greet %s\n", who);   /* lands in the console AND log */
```

A symmetric `axl_stream_set_stderr_tee` covers `axl_printerr`.
Tee write errors are swallowed — a broken log file must not break
the primary console.

### Interactive console input

`<axl/axl-console.h>` covers two kinds of console input.

For a single keystroke (`y`/`n` confirmations, "press any key",
arrow-key menus):

```c
#include <axl/axl-console.h>

axl_print("Continue? [y/n]: ");
AxlKey k;
if (axl_console_read_key(5000, &k) == 0
    && (k.unicode_char == 'y' || k.unicode_char == 'Y'))
{
    /* user said yes */
}
```

Three timeout modes: `0` is non-blocking (returns -1 immediately
if no key is buffered), `UINT64_MAX` blocks forever,
anything else is a millisecond bound. Pair with
`axl_console_flush_input()` before a prompt to eat type-ahead.

For a whole line typed by a human (`do -f`, a REPL, a `name? `
prompt), use `axl_console_readline` — it echoes printable keys,
erases on Backspace, and returns the accumulated UTF-8 text (minus
the CR/LF) on Enter:

```c
char *line = NULL;
axl_print("Name: ");
if (axl_console_readline(UINT64_MAX, &line) == 0) {
    axl_printf("Hello, %s\n", line);   /* Enter already echoed a newline */
    axl_free(line);
}
```

`axl_console_readline_ex(timeout, max_len, echo, &line)` adds a
character cap and echo suppression — pass `echo = false` for a
password prompt (typed characters are hidden; the terminating
newline is still echoed). The `timeout` is a whole-line deadline;
on expiry (or Ctrl-C) the partial input is discarded and the call
returns -1.

**`axl_stdin` routes automatically.** `axl_stdin` reads the shell
StdIn handle — captured pipe/redirect bytes for `cmd | tool` and
`tool < file`. But when a command is typed with no redirection,
StdIn *is* the console, so `axl_readline(axl_stdin)` /
`axl_stdin_text()` transparently fall back to the same console
line editor (`axl_stdin_is_interactive()` is the predicate). A
prompt therefore "just works" whether piped or typed. One
consequence of this POSIX tty-vs-pipe behavior: a read of
`axl_stdin` at an interactive console now **blocks** until Enter
(as a tty does) rather than returning EOF. Use `axl_console_read_key`
for raw keystrokes with no line assembly.

**Unattended scripts:** an interactive console does *not* mean a human
is present. An automated `startup.nsh` at boot has StdIn = the console
but nobody typing, so a bare stdin read blocks until the boot timeout.
In a script, redirect (`tool < in`) or gate on
`axl_stdin_is_interactive()` and skip the read when it would block.

### Text-console modes

The same header exposes the text-output mode surface the UEFI Shell's
`mode` command shows — enumerate the character-cell geometries the active
console supports and switch between them (the graphics-free peer of the
AxlGfx display-mode API):

```c
uint32_t n = axl_console_text_mode_count();
for (uint32_t i = 0; i < n; i++) {
    AxlConsoleTextMode m;
    if (axl_console_text_query_mode(i, &m) == AXL_OK)   /* skip unsupported */
        axl_printf("  mode %u: %ux%u\n", m.index, m.columns, m.rows);
}
AxlConsoleTextMode big;
if (axl_console_text_max_mode(&big) == AXL_OK)
    axl_console_text_set_mode(big.index);   /* clears the screen; repaint */
```

`axl_console_text_find_mode(columns, rows, &idx)` looks up a specific size
and `axl_console_text_current_mode(&idx)` reports the active one. Only mode
0 (80x25) is guaranteed; higher modes are optional and a conformant console
may reject `QueryMode` on an in-range mode (real OVMF does, for one of its
graphics-console modes) — the enumerating helpers skip those. These operate
on the active console, so under an installed `AxlConsoleMirror` they reflect
the mirror's fixed remote geometry until it is uninstalled.

## File Read/Write

The simplest way to read or write files:

```c
// Read entire file into memory
void *data;
size_t len;
if (axl_file_get_contents("fs0:/config.json", &data, &len) == AXL_OK) {
    // process data...
    axl_free(data);
}

// Write entire file. The status is must-check for a reason: it is AXL_OK
// only once the bytes are flushed through to the volume, and a close can
// never tell you that (EFI_FILE_PROTOCOL.Close returns only EFI_SUCCESS).
if (axl_file_set_contents("fs0:/output.txt", buf, buf_len) != AXL_OK) {
    // the file is NOT on disk — report it; don't carry on as if it were
}
```

## Stream I/O

For line-by-line reading or incremental writes:

```c
AxlStream *f = axl_fopen("fs0:/log.txt", "r");
if (f != NULL) {
    char *line;
    while ((line = axl_readline(f)) != NULL) {
        axl_printf("  %s\n", line);
        axl_free(line);
    }
    axl_fclose(f);
}
```

### `axl_fread` / `axl_fwrite` count ITEMS, and they loop

Both behave like C's, which means they **loop** until the requested item
count is satisfied. A backend is allowed to transfer less than it was
asked for — a socket or ring buffer does so routinely — so one call to the
backend is not enough, and a caller who received a short item count would
have no way to tell a legal short transfer from end of input. After the
loop, only a real ending can shorten the result:

| Return < `count` | What it means | How to tell |
|---|---|---|
| `axl_fread` | end of input, or a read error | `axl_feof` / `axl_ferror` |
| `axl_fwrite` | sink error | `axl_ferror` is **true** |
| `axl_fwrite` | sink accepted nothing more (try again later) | `axl_ferror` is **false** |

A `0` from a backend write is a legal "accepted nothing" rather than an
error, and `axl_fwrite` stops at the first one instead of retrying —
nothing can drain the sink while the loop holds the CPU, so a retry would
only spin. Resume from the returned item count at your own cadence.

That resume rule is for an **unbuffered** stream. Under `LINE`/`FULL` the
bytes are copied into the stream's buffer before any sink call, so a stall
surfaces as a hard error (the "try again" row cannot occur) *and the bytes
stay queued for the next flush* — resending from the returned count would
duplicate them. On a buffered stream a short return means "ask
`axl_fflush` what happened", not "resume from byte N".

Only **complete** items are counted; a partial trailing item is not,
though for `axl_fread` its bytes are still in your buffer. Use `size == 1`
when you care about the exact byte count. `size * count` overflowing is
refused outright (0 items, nothing reaches the backend).

The loop is the *right* semantics but not always the semantics you want: a
filter reading an interactive console with `axl_fread(buf, 1, 4096, in)`
blocks until 4096 bytes have been typed. Streaming code wants `axl_read`
(and this is why `tr` uses it).

## Output Buffering

By default every AxlStream is **unbuffered** — each write goes straight to
the sink (stdio `_IONBF`). Opt into coalescing with
`axl_stream_set_buffering`, or the C-compatible `setvbuf` family:

```c
axl_setlinebuf(log);                             /* flush on each '\n'  */
axl_stream_set_buffering(out, AXL_STREAM_BUF_FULL, 4096);  /* block-buffer */
axl_setvbuf(out, NULL, AXL_STREAM_BUF_FULL, 4096);         /* same, C-shaped */

axl_fprintf(out, "many small writes ...");       /* coalesced in the buffer */
axl_fflush(out);                                 /* drain to the sink       */
```

| Mode | Flushes when | stdio |
|---|---|---|
| `AXL_STREAM_BUF_NONE` | every write (default) | `_IONBF` |
| `AXL_STREAM_BUF_LINE` | a `'\n'` is written, or the buffer fills | `_IOLBF` |
| `AXL_STREAM_BUF_FULL` | the buffer fills | `_IOFBF` |

The buffer lives in `axl_write`, which `axl_print` / `axl_printf` /
`axl_fprintf` / `axl_fwrite` all funnel through, so every text-output path
is coalesced uniformly, ahead of any UTF-8 → UCS-2 transcode and tee.

**Unlike C stdio, AXL does not auto-select buffering from tty-ness.** A
UEFI app can exit through a crt0 path that runs no atexit hook, so
auto-buffered output could be silently lost, and a line-buffered prompt
before a read could stall unseen. Buffering is therefore strictly opt-in,
and **you own the final flush**: `axl_fflush` drains the buffer *and*
pushes the sink; `axl_fclose` drains and frees but never calls the sink's
flush, so it is not a substitute (spelled out below). Nothing closes
`axl_stdout` / `axl_stderr` for you at exit, so code that buffers them
must `axl_fflush` before it goes.

On a **file** stream `axl_fflush` does double duty: it drains the AXL-side
buffer *and* pushes the firmware's own cache through to the volume, so a
successful return is the point at which the bytes are durable. Flushing a
stream opened read-only is a no-op success — there is nothing dirty behind
it, and the firmware would otherwise refuse the call outright.

`axl_fclose` is **not** an equivalent durability point: it drains the
AXL-side buffer and frees it, but never invokes the sink's own flush. If
you need to know the bytes reached the volume, `axl_fflush` and check it,
*then* close.

This is orthogonal to the interactive/line-discipline axis (see the
text-wrapper section): buffering governs how *writes* coalesce, the
interactive mark governs whether *reads* over-consume.

## Buffer Streams

In-memory streams for building data without files:

```c
AxlStream *buf = axl_open_buffer();
axl_fprintf(buf, "name=%s\n", name);
axl_fprintf(buf, "value=%d\n", value);

void *data;
size_t len;
axl_stream_get_bufdata(buf, &data, &len);
// data contains the formatted text
axl_fclose(buf);
```

## Standard Streams

`axl_stream_init` populates five globals: `axl_stdout`, `axl_stderr`,
`axl_stdin`, `axl_stdout_raw`, `axl_stderr_raw`.

All five are **statically allocated**, and `axl_fclose` knows it: closing
one drains any buffered output and resets it, but does not destroy it — the
pointer stays valid and the stream keeps working. Generic code handed an
arbitrary `AxlStream` can close it without first checking whether it
happens to be one of these. "Resets" means the **ground state**: unbuffered,
UTF-8, no tee, not interactive, `eof`/`err` clear, no half-transcoded bytes.
Any configuration you applied is gone and must be re-applied — and dropping
the tee is deliberate, since it is what stops the stream outliving a tee
stream you close next.

`axl_stream_init` establishes that same ground state — one shared helper, so
the two cannot drift — and only then publishes the globals. Being `.data`,
these five carry whatever the last caller left on them for the life of the
image; a decoder left on `axl_stdin` makes every later `axl_stdin_text()`
return NULL, and in a resident driver that is every later dispatch, not one
run. On the first call the reset is invisible (the statics start at ground);
what it buys is that a **second** call is a real reset, which is the
supported way back after code that configured a standard stream did not
restore it. Pending buffered output is drained, not dropped. Nothing calls
it per dispatch, so a resident driver that wants a clean slate between
launcher invocations has to call it itself.

For shell pipe invocations (`tool1 | tool2`) the LHS output is
captured by the shell into a stream that becomes the RHS's StdIn, so
`axl_read(axl_stdin, ...)` consumes the piped bytes. When StdIn is
instead an interactive console (no redirection), `axl_stdin` reads
deliver canonical line-edited input — see *Interactive console
input* above.

When the shell-params protocol isn't published (cross-volume
launches, BDS contexts, non-Shell-2.0 launches), `axl_stdin` reads
return EOF (0 bytes) and `axl_stdout_raw` / `axl_stderr_raw` writes
return -1 — tools that opt in should fall back to a file argument or
print a clear error. `axl_stderr` (text) has no such gap: with no Shell
handles it is the console's (below), and with no `gST->StdErr` it is
`gST->ConOut`, so diagnostics still land somewhere instead of going
silent.

### Standard streams and their sinks

| Stream | Direction | Encoding | Sink |
|---|---|---|---|
| `axl_stdin` | in | raw bytes (redirected) / line-cooked (interactive) | shell **StdIn** handle (`EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`) — `<` / `\|`, else console line editor |
| `axl_stdin_text()` | in | text (UTF-8, auto-sniffed) | `axl_text_stream_wrap` over `axl_stdin` — decodes the UCS-2 `\|` pipe to UTF-8 |
| `axl_stdout` | out | text (UTF-8 in → UCS-2 out) | `gST->ConOut` — honors `>` / `>>` |
| `axl_stderr` | out | text (UTF-8 in → UCS-2 out) | on the console, the console's ConOut splitter plus every StdErr-only device; otherwise `gST->StdErr` — honors `2>` / `2>>` (see *Standard error on the console*) |
| `axl_stdout_raw` | out | raw bytes | shell **StdOut** handle (direct `WriteFile`); when that handle IS the Shell's console file, the `axl_stdout` text path instead |
| `axl_stderr_raw` | out | raw bytes | shell **StdErr** handle (direct `WriteFile`); when that handle IS the Shell's console file, the `axl_stderr` text path instead; sibling of `axl_stdout_raw` — also works in a resident shared-driver via the stdio bridge |

Diagnostic logging (`axl_log`, `axl_warning`, including the level-color
attribute) writes to `axl_stderr`, not `axl_stdout` — so `tool > out.txt`
captures only the tool's own stdout text, with no AXL log lines mixed in.
A script that used to scrape AXL diagnostics out of a `>`-redirected file
must switch to `2>`.

### Standard error on the console

On a terminal, stdout and stderr are one stream with one cursor: an error
line starts where stdout left the cursor, and the next prompt starts on the
row after it. UEFI's console is two: `gST->ConOut` and `gST->StdErr` are
separate ConSplitter instances, each keeping its own cursor record, and the
Shell places its prompt from ConOut's (`SetCursorPosition(0,
ConOut->Mode->CursorRow)`). A line written to `gST->StdErr` moves the
devices' cursors but not that record -- and where ConOut's first device is
not a StdErr device at all (OVMF's display), not even that device's -- so the
Shell drew its next prompt over the error lines: the teardown's
`mem: no leaks detected`, a tool's log line.

So when standard error is the console, it is written through the console's
ConOut splitter -- the stream stdout reaches the screen through, and the
cursor record the prompt is placed from -- and also directly to every device
that is in the StdErr fan-out but not ConOut's (the serial terminal while a
take-over holds the screen, a port only `ErrOut` names). Every console a
person watches -- the display, the serial terminal, an ssh session -- shows
the line once, in order with stdout. That holds when stdout is redirected
too: `tool > out.txt` and `tool | other` leave the error lines on the
screen, each on its own row, and the file or pipe gets stdout only.

It is the splitter, not `gST->ConOut` itself: under the Shell,
`gST->ConOut` is the Shell's console logger, which keeps the scroll-back,
stops at each screenful under page break (`-b`) and on Ctrl-S to wait for a
key, and is not written to be entered from a timer callback -- where a
resident image's log line comes from. The logger shares the splitter's
`Mode`, so a line written beneath it still moves the cursor record. The
splitter is found once, at image start: the SimpleTextOut that shares `Mode`
with the logger, which is installed on the Shell's own image handle (the
handle carrying `EFI_SHELL_PROTOCOL`) -- still there when `>` has swapped
`gST->ConOut` for a file. Error lines are therefore not in the Shell's
scroll-back; they never were, as StdErr bypassed the logger too.

A deliberate decision, for Unix parity: error lines now reach every ConOut
device, including one the platform's `ErrOut` leaves out. On OVMF that is
the display, which shows error and log lines now as a terminal shows
stderr. A device only `ErrOut` names still gets them.

"The console" is decided by identity: standard error is the console only
when its Shell handle IS the Shell's console file (what
`EFI_SHELL_PROTOCOL.OpenFileByName` returns for `>e`). A size probe cannot
tell: `NUL` and an environment variable (`2>v name`) have no file size
either, so `2> NUL` would print on the screen and `2>v` would lose the line.
In a Shell started from the Shell, a command inherits the OUTER Shell's
console file while `OpenFileByName` answers with the inner one's; both are
the same static of the Shell binary, so a handle at the same offset in
another image of the same size counts as the console too. The handle is the
live one -- the image's own Shell StdErr, or, for a
resident shared driver, the StdErr the CURRENT launcher lends it through the
stdio bridge -- judged on every write, so each launcher's
redirection applies to its own dispatch. A bridge is aimed at the interface
the launcher dispatches through, and only the image that published that
interface reads it: no other resident image -- a service, another shared
driver -- is lent a launcher's handles. With no live handle at all (a
service driver writing on its own clock, between launchers or while another
image is dispatched into), the verdict taken when the image started
applies: its own handle then, or its parent's -- the launcher a driver was
loaded from, running as it starts. A resident image whose standard error
was the console therefore keeps writing to the splitter while another
command runs under `>` or `2>` -- a shared-driver launcher's included --
and its lines land in no command's file (`test-stderr-row-qemu.sh`,
`resident` and `bridge`).

Otherwise the line goes to `gST->StdErr`, as it always did: under `2>`
(which swaps `gST->StdErr` for the file, so it captures the line), and with
nothing to compare -- no `EFI_SHELL_PARAMETERS_PROTOCOL` on the image or its
parent (the EFI 1.x shell, a launch from the boot manager), no
`EFI_SHELL_PROTOCOL`, no splitter found. With no `gST->StdErr`, or one the
console tap aliased to `gST->ConOut`, everything goes to `gST->ConOut`.

The AxlLog console sink and the runtime's own notices (the Ctrl-C line)
take the same path; the pack stub, which runs before libaxl, makes the same
choice itself (`src/pack/README.md`).

### A line exactly the console's width

A UEFI text console wraps the moment a character lands in its LAST column:
EDK2's GraphicsConsole writes CR LF right there, and TerminalDxe's and
ConSplitter's cursor records move to column 0 of the next row. A Unix
terminal defers that wrap until the next printable character (xterm's and
the Linux VT's "pending wrap"), so a line's own CR LF only finishes the row
it is on. Written as it stood, a line that exactly filled a row was followed
by an EMPTY row on the display and in an ssh session, and the serial
terminal, whose real cursor had not wrapped, drifted a row from TerminalDxe's
record until the Shell's next absolute cursor move showed the gap.

So AXL's console writes -- stdout's, standard error's on the console, the
log sink's; one function, `axl_text_out_lines`
(`src/backend/native/axl-backend-native-lines.c`) -- watch for the wrap on each
device the output reaches -- following each one's own cursor through the
line's last character, since a narrower device wraps a line a wider one does
not -- and write the line break that follows it to each device, as what
leaves that device where it now stands:

| device | the break after a wrap | why |
|--------|------------------------|-----|
| an EDK2 TerminalDxe terminal of any type but `TtyTerm` -- PC-ANSI, VT-100, VT-100+, VT-UTF8, Linux, Xterm-R6, VT400, SCO (its device path ends in that vendor node) | CR LF, then the cursor put back on the row the wrap left it on | TerminalDxe never sends its wrap down the wire, so the terminal is still in its pending wrap on the full row: the CR LF is the newline it needs -- and a serial capture keeps one line per line -- and the absolute cursor move puts TerminalDxe's record back in step with it |
| anything else: GraphicsConsole, a `TtyTerm` terminal (TerminalDxe sends that one's wrap itself), AXL's console device (its encoder sends the wrap), a terminal driver that is not EDK2's | a space and a backspace | no motion on a console that has wrapped; a terminal still in a pending wrap is taken out of it, so the Shell's next prompt (placed with an absolute cursor move) cannot land on the full row when it is the screen's last |
| a device the line did not wrap | the break as written | it runs a geometry of its own: in the mode OVMF boots x64 in, the display is 100 columns and the serial terminal 80, so a line of 80 wraps the terminal and not the display |

When the devices disagree -- a narrower one wrapped the line, the console's
own record (the splitter's, which copies its first device's cursor, and the
Shell's console logger, which keeps the scroll-back at that width) did not --
the break goes through the console as written, so the record and the
scroll-back both end the line, and each device that had wrapped is then put
back where its wrap left it by a cursor move alone. Where the record did
wrap, the breaks above go to the devices one by one; either way an empty
write through the console afterwards makes the splitter copy its first
device's cursor again.

The devices are the console splitter's -- every handle carrying the ConOut
(or, for `gST->StdErr`, the StdErr) device tag -- only when the output
verifiably IS that splitter: the SimpleTextOut on the system table's console
handle, or one sharing its Mode, as the Shell's console logger does (under
`>`, the logger is found on the handle carrying `EFI_SHELL_PROTOCOL`). Any
other output -- AXL's console tap, which with local output off promised the
physical devices nothing, or another firmware wrapper -- is one device of its
own and gets the break itself; so does the one device a standard-error line
is written to directly. An empty line after a full one
is still exactly one empty row, everywhere.

The wrap is OBSERVED, not computed from a width: the line's last character
moved a device's cursor from a column past 0 to column 0. Anything whose
cursor does not move never shows it, and that -- not the path taken -- is what
keeps redirected output byte-for-byte. Standard error under `2> file`,
`2> NUL` and `2>v name` DOES reach this writer: it is written to
`gST->StdErr`, which the Shell has swapped for a SimpleTextOut over the
file, `NUL` or variable, and that wrapper's `Mode` is a frozen copy of the
console's taken when the redirect was set up, so it never wraps and gets
every CR LF (`test-stderr-row-qemu.sh`, `wrap`: the `>` and `2>` files
byte for byte). Standard output under `>` or `|` does not reach it: it is
written to the Shell's handle as UCS-2. A newline written in a later call
than its line (`axl_print(line)` then `axl_print("\n")`) is matched to the
wrap by every cursor still standing where the wrap left it -- also when the
line went to standard output and its newline to standard error, which share
the console's one cursor. One thing that case does not reach: the Shell's
scroll-back joins that full line with the next, because standard error is
written beneath the Shell's console logger and never reaches its copy.
The pack stub, which runs before libaxl, links the same object for its own
lines (`src/pack/README.md`).

`axl_stdout_raw` and `axl_stderr_raw` write their bytes to the Shell's
handle untouched when it is a file, a pipe, `NUL` or a variable. When it IS
the Shell's console file (the same identity test), they take the text path
instead, through `axl_stdout`'s or `axl_stderr`'s sink. A writer may hand
the bytes over in any pieces -- `cat --raw` in buffer-sized chunks, `sum -z`
a byte at a time -- so each sink keeps a decoder: the encoding is decided by
the first bytes (UTF-16LE when they start with its mark, as the Shell's
pipes and files do -- `echo x | clip` then `paste` shows `x`; UTF-8
otherwise; and, with nothing held, a later write that starts with that mark
switches to UTF-16LE, as `cat --raw a.txt b.utf16` writes a UTF-16 file
after a UTF-8 one -- FF FE never occurs in UTF-8) and up to three bytes of
a character split between writes are held for the next one. Ill-formed UTF-8 -- an overlong form such as
`C0 80`, a surrogate, a value past U+10FFFF, a byte that begins no
character -- is U+FFFD, one per maximal subpart (Unicode 3.9: a valid
prefix cut short is one, `E1 80 41` showing U+FFFD then `A`; each byte that
begins no valid prefix is one), as is an unpaired UTF-16 surrogate.
A sink's decoder belongs to one stream: a new handle, or -- in a resident
shared driver -- a new launcher lending it a handle, starts a fresh one
(every launcher's console handle is the same Shell file, so the handle alone
would let one launcher's UTF-16 mode or half a character reach the next).
Bytes still held when a decoder starts fresh, or when the image exits, are
DROPPED, not shown as U+FFFD: they are the unfinished end of a stream that
has ended, and the console they were bound for may no longer be its own --
writing a replacement there would put a stray character on another
command's output. The console file
ignores the size it is given and prints its buffer as a NUL-terminated UCS-2
string, so raw bytes handed to it read on past their end. The Shell handles
themselves are read live on every call, never cached: the Shell closes a
redirect's file when the launch command ends, and a resident image writing
after that must not use the freed handle.

A known firmware hazard, not AXL's: when a command ends, the EDK2 Shell
(`RestoreStdInStdOutStdErr`) closes a `2>` file's console wrapper BEFORE it
puts the original `gST->StdErr` back, so for that moment `gST->StdErr`
points at freed memory. A writer that runs then -- a timer callback at
`TPL_CALLBACK`, a resident image's log line taking the `gST->StdErr` path --
touches it. Nothing in AXL can close that window; standard error on the
console never uses `gST->StdErr`, which keeps most resident lines out of it.

### Redirect-encoding boundary

The UEFI shell — not AXL — owns the transcoding at a console redirect:
`>` / `>>` write **UCS-2 with a leading BOM**, `>a` / `>>a` write **ASCII**.
**Console redirection never produces a UTF-8 file**, regardless of what the
tool wrote. A tool that needs an actual UTF-8 file must open one with
`axl_fopen` and write to that stream directly — `tool > file.txt` is not a
substitute.

### Output: text vs binary

The split is symmetric with stdin (raw bytes vs `axl_text_stream_wrap`
for text decoding):

| Use case | API |
|---|---|
| Text output (the common case) | `axl_print` / `axl_printf` / `axl_write(axl_stdout, ...)` — UTF-8 in, UCS-2 out; displays on the interactive console AND pipes/redirects correctly (see below) |
| Binary output (RAM-disk dump, captured SPD blob, etc.) | `axl_write(axl_stdout_raw, ...)` — raw bytes, bypasses the UTF-8→UCS-2 transcode so arbitrary bytes survive byte-for-byte |

`axl_stdout` picks its sink per invocation: the interactive console
writes `gST->ConOut` (so the console subsystem — tap / mirror / device —
sees the bytes), while a **non-interactive** stdout (a `|` pipe, or a
`>` / `>a` redirect) writes the transcoded UCS-2 to the shell's
`EFI_SHELL_PARAMETERS_PROTOCOL.StdOut` handle. That last part matters:
the UEFI shell wires StdOut for a pipe but does NOT swap `gST->ConOut`,
so a ConOut-only write would print to the screen instead of feeding the
next stage. Because axl_stdout now does this, `tool | other`,
`tool > f` and `tool >a f` all carry a tool's text output; the shell's
handle wrapper supplies the leading BOM and any `>a` / `|a` ASCII
downconversion.

Don't use `axl_stdout_raw` for text; the firmware console only knows
UCS-2, so writing raw 8-bit bytes to it (on the interactive path) would
mangle the display, and it emits UTF-8 (not the shell's expected UCS-2)
into a `>` / `>a` redirect. `axl_stdout_raw` is for **binary** payloads
that must not be transcoded — not merely to make text pipe, which
`axl_stdout` now does on its own.

## Text-Decoding Stream Wrapper

UEFI Shell pipes carry text as UCS-2 LE with a `FF FE` BOM (because
shell built-ins write to console handles that wrap text that way).
A tool reading `axl_stdin` directly sees raw UCS-2 bytes. To get
UTF-8 text regardless of source encoding, wrap any byte stream with
`axl_text_stream_wrap`:

```c
AxlStream *in = axl_text_stream_wrap(axl_stdin);   /* or any other AxlStream */
char *line;
while ((line = axl_readline(in)) != NULL) {
    /* `line` is UTF-8 — search, parse, etc. */
    axl_free(line);
}
axl_fclose(in);    /* does NOT close the wrapped src */
```

Classification happens eagerly at construction — the wrapper reads a
probe window from `src` to detect the encoding:

| Leading bytes | Mode | Behavior |
|---|---|---|
| `FF FE`    | UTF-16 LE | BOM consumed, body transcoded to UTF-8 |
| `FE FF`    | UTF-16 BE | BOM consumed, body transcoded to UTF-8 |
| `EF BB BF` | UTF-8 BOM | BOM stripped, body returned verbatim |
| interleaved NULs (≥16 B, no BOM) | headerless UCS-2 | body transcoded to UTF-8 |
| anything else | passthrough | raw bytes returned as-is |

**Interactive / no-EOF sources skip the probe.** The classify read fills
its window until it sees enough bytes or EOF — which would hang on an
interactive console, since typed input never signals EOF and the probe
would swallow line after line waiting to fill. So when `src` is an
interactive source — `axl_stdin` on an interactive console
(`axl_stdin_is_interactive()`), or any stream you mark with
`axl_stream_set_interactive(src, true)` — the probe is skipped entirely
(the input is already UTF-8 and line-cooked, with no BOM or UCS-2 to
detect) and the wrapper is a plain passthrough returning one line per
read. Redirected / piped stdin reaches EOF, so it is classified normally.
The returned wrapper inherits the interactive mark.

Transcoding is incremental and bounded-memory regardless of source
size — wrap a multi-MB pipe and read line by line. The wrapper holds
back partial transcoded sequences when the caller's buffer cuts
mid-character, so reads return valid UTF-8 even for tiny buffers.

The wrapper does **not** take ownership of `src` — the caller closes
both eventually. Use this when the source might be UCS-2; pass
binary data through the raw stream (e.g. `hexdump` reads `axl_stdin`
directly so it shows wire bytes including the BOM).

Codepoints above U+FFFF (surrogate pairs) round-trip as their
UTF-16 code-unit shape, not the proper UTF-8 4-byte form. Almost
all real-world UEFI Shell content is BMP-only ASCII / Latin-1 /
common scripts; a follow-up can add full SMP support if a real
consumer needs it.

That permissiveness is the reason this transcode keeps its own
codepoint encoder rather than calling `axl_utf8_encode`, which
refuses a surrogate because it encodes Unicode *scalars*. A UCS-2
wire carries code *units*, where an unpaired half is representable,
so routing this path through the shared encoder would silently drop
a code unit the wire can hold. Pinned by
`encoding: reading it back restores the 3-byte shape` in
`test/unit/axl-test-io.c`.

## Custom Backends

`AxlStream` is not a closed set. `axl_stream_open_custom` builds a
stream from your own byte operations — a socket, a ring buffer, a UEFI
protocol, a deliberately-failing test double — and everything AXL layers
on top comes for free:

```c
static axl_ssize_t my_write(void *ctx, const void *buf, size_t count);
static void        my_close(void *ctx);

AxlStreamOps ops = AXL_STREAM_OPS_INIT;   /* always start here */
ops.write = my_write;
ops.close = my_close;

AxlStream *s = axl_stream_open_custom(sink, &ops, "my-sink");
axl_fprintf(s, "hello %s\n", who);        /* printf, encoding, buffering, tee */
axl_fclose(s);                            /* calls my_close(sink) exactly once */
```

This is not a second-tier path bolted on beside the built-ins: **every
constructor AXL publishes is built with exactly the call above** —
`axl_bufopen`, `axl_fopen`, `axl_text_stream_wrap` and
`axl_compress_writer` all fill an `AxlStreamOps` and hand it to
`axl_stream_open_custom`, with the same operations they always used.
Anything they give you — `axl_fprintf`, seeking, positional I/O,
`axl_stream_name`, the capability queries — a stream you build here gets
on the same terms. Only the five static console streams (`axl_stdout` and
friends) are constructed differently, because they are objects in `.data`
rather than allocations and so cannot come out of a constructor at all.

A backend supplies **raw byte movement only**. The UTF-8 → wire
transcode, LINE/FULL buffering, the stdout tee, the interactive hint and
the sticky `eof`/`err` flags all sit *above* these operations. Three
consequences that catch people out:

- The bytes an operation sees are always **wire-side**. On write the
  transcode has already happened; on read it happens after you return.
  A backend never needs to know the stream's encoding.
- **One `axl_write` is not one backend `write`.** It is with the defaults
  (`AXL_ENC_UTF8` + `AXL_STREAM_BUF_NONE`), but LINE/FULL buffering
  coalesces and a non-UTF-8 encoding issues one call **per code point**.
- `flush` is **not** called before `close`, so a backend holding bytes of
  its own must push them from `close`.

Zero-initialize with `AXL_STREAM_OPS_INIT` and fill in only what you
support. A NULL slot means "unsupported", but what the caller then sees
differs per slot:

| NULL slot | what the caller sees |
|---|---|
| `read` / `write` | `-1` from `axl_read` / `axl_write` |
| `pread` / `pwrite` | `-1` from `axl_pread` / `axl_pwrite` |
| `seek` | `AXL_ERR` from `axl_fseek` |
| `tell` | `-1` from `axl_ftell` |
| `flush` | **`AXL_OK`** — nothing to push is not an error |
| `close` | no-op |

**Return contract.** `read`/`write`/`pread`/`pwrite` return bytes
transferred; a count *less than requested is legal and is not an error*,
and `0` from `read` is end of input. `seek`/`flush` return 0, `tell`
returns the offset, and all three return `-1` on error. `-1` is the only
defined failure value — every other negative is **reserved** (dispatch
treats all negatives as errors but propagates the value unchanged, which
leaves room for a future "try again later" without encoding it as a lie).

Short transfers cost a backend author nothing, because they are handled
above the vtable: `axl_fread` / `axl_fwrite` loop until the item count is
met, and a buffered flush retains whatever the sink declined. Move what
you can and return it.

**Ownership.** `ctx` belongs to the backend; `axl_fclose` calls
`ops->close(ctx)` exactly once, after draining buffered output. If
`axl_stream_open_custom` returns NULL, `close` is **not** called and `ctx`
is still yours. `ops` and `name` are both copied, so the ops struct may
live on the stack.

#### Recovering the context — `axl_stream_ctx`

A backend often wants a **stream-keyed accessor**, the shape `axl_bufdata`
has: one handle in, the answer out. That needs the context back from a
stream you were *handed*, not from one you kept — otherwise the
constructor must return two things and every caller must carry both.

`axl_stream_ctx` does it, and it takes your operations as well as the
stream:

```c
size_t my_sink_dropped(AxlStream *s) {
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;
    ops.write = my_sink_write;
    ops.close = my_sink_close;
    MySink *sink = (MySink *)axl_stream_ctx(s, &ops);
    return (sink != NULL) ? sink->dropped : 0;
}
```

**The `ops` argument is the safety property, not boilerplate.** The context
comes back only if the stream was opened with a matching set of operations;
any other stream is a NULL return. A bare `axl_stream_ctx(s)` would hand a
*file* stream's private context to code about to cast it — which is not
hypothetical, because `axl_bufdata` shipped with exactly that hole:
reinterpreting a 16-byte context as a 32-byte one and zeroing it wrote past
the end of the allocation and took the running image down, silently.

Every slot holds the address of a function *you* wrote, so matching **shape**
is never enough — a stream with the same read/write/seek/tell signature but
different functions is refused. A match means "this came out of my
constructor", which is what an accessor needs before it casts. It does not
mean "this is *that* stream": every stream from one backend matches, which is
correct, since they all carry a context of the same type. Telling individual
streams apart is the context's own job.

Built-in streams are **structurally** out of reach rather than excluded by a
rule: the file, buffer, text, compress and console backends' operations are
file-static, so no `ops` you can build will ever match one. There is no
spelling of this call that reaches a `FileCtx`.

Two more things worth knowing. A backend opened with a NULL context is
indistinguishable from a refusal — which costs nothing, since it has nothing
to recover. And version skew resolves exactly as it does for
`axl_stream_open_custom`, because both go through the same normalisation: a
caller that opens and queries with the same struct always matches itself.

This diverges from glibc's `fopencookie` and BSD's `funopen`, which withhold
the cookie entirely. The divergence is deliberate and narrow: those APIs have
no versioned operations struct and therefore no token a caller could use to
prove which backend it means, so withholding was their only safe option. AXL
already copies such a struct at open time, so it can offer the getter *with*
the check — and still does not offer it without.

`axl_bufdata`/`axl_bufsteal` are built on exactly this call, and that is the
proof it is sufficient: `src/stream/axl-stream-buf.c` names no private header
and reaches its context the same way your code does. So do
`axl-stream-file.c` and `axl-compress-stream.c`.

#### Wrapping another stream — the filter rule

A backend whose context is *another `AxlStream`* — a filter, a tee, an
encoding sniffer, a compressor — builds and serves itself through the calls
above like any other, and moves bytes through its peer with the ordinary
public `axl_read` / `axl_write`. There is one rule, and it is the reason AXL
publishes **no** "read below a stream's decode" call:

> **A filter that moves opaque bytes through another stream requires that
> stream to be at `AXL_ENC_UTF8`.** Not because the bytes are text, but
> because any other setting means that stream is transforming them, and a
> filter's bytes are not characters.

At `AXL_ENC_UTF8` — the default, and where every stream starts — `axl_read`
and `axl_write` **are** the wire calls: the encoding layer is a passthrough
and dispatch goes straight to the backend. So the capability that looks
missing is not missing; it is spelled "require the peer undecoded". Set the
encoding on **your** stream, which is where a caller reads from anyway.

Refuse a peer that breaks the rule rather than reaching past it. All three
in-tree filters do so at construction, and the plain form is:

```c
if (axl_stream_get_encoding(peer) != AXL_ENC_UTF8) {
    return NULL;                     /* refuse; leave the peer untouched */
}
```

- `axl_text_stream_wrap` would otherwise decode a second time, on top of what
  the source already decoded. It re-checks on every **read**, because a caller
  can reach around a live wrapper and set an encoding on its source; the check
  is live rather than latched, so restoring the source revives the wrapper.
  Its check is the plain form plus one exemption: *another text wrapper* is
  accepted — recognised via `axl_stream_ctx` — because its output is UTF-8 by
  construction, so that whether a text stream can be wrapped again does not
  depend on the bytes in the file underneath it. Over such a source the new
  wrapper does **not** classify; re-sniffing already-decoded text is the
  double decode by another route, since decoded UCS-2 containing `U+0000`
  alternates ASCII with NUL bytes and looks exactly like headerless UCS-2.
- `axl_compress_writer` / `axl_compress_reader` would otherwise transcode
  DEFLATE bytes code point by code point. The writer re-checks when it
  finalizes, since that is when the sink is actually written.

A refusal is **inert**: the peer keeps its encoding *and* its position, which
is what distinguishes "refused" from "read it, mangled it and failed".

`src/stream/axl-stream-text.c` names no private header — it constructs with
`axl_stream_open_custom`, reads with `axl_read`, and identifies its own kind
with `axl_stream_ctx`. Nothing in `src/stream/` has a construction or
composition path you lack.

**Versioning.** `AXL_STREAM_OPS_INIT` stamps `struct_size` and `version`,
and AXL copies `min(ops->struct_size, sizeof(AxlStreamOps))`. That
survives skew in both directions: an older caller against a newer library
leaves the newer slots NULL (unsupported, always safe), and a newer caller
against an older library has its extra slots ignored rather than read out
of bounds. A `struct_size` below the first published version, or a
`version` the library does not know, is rejected with NULL.

### Capability queries

Generic code that accepts any `AxlStream` can ask before it calls, instead
of discovering `-1` and having to guess whether that meant "unsupported"
or "failed":

```c
if (axl_stream_can_seek(s)) { axl_fseek(s, 0, AXL_SEEK_SET); }
```

`axl_stream_can_read`, `_can_write`, `_can_seek`, `_can_tell`,
`_can_pread`, `_can_pwrite` each mirror one vtable slot's NULL check —
they are deliberately **independent**. `seek` and `tell` are separate
slots, so a ring buffer can report a position it cannot seek to; likewise
a read-only mapping has `pread` without `pwrite`.

`axl_stream_name(s)` returns the label passed to
`axl_stream_open_custom`, or the built-in name for a stream AXL
constructed. The built-in set is closed: `"file"`, `"buffer"`, `"text"`,
`"compress"`, `"stdout"`, `"stderr"`, `"stdin"`, `"stdout-raw"`,
`"stderr-raw"`. It is never NULL — `""` when the stream has no name.

### Fault injection

A custom backend can already fail on demand, but only where the test
*constructs* the stream. For a function that opens its own file, or that
operates on an `AxlStream` handed to it, there are four global hooks —
the `AxlStream` counterpart of `axl_mem_fail_next_alloc`, and they work on
**every** stream including the built-ins:

```c
axl_stream_fail_next_write(1);
test_check(axl_fprintf(s, "x") < 0, "the sink error is reported");
axl_stream_fail_next_write(0);              /* disarm in teardown */
```

```c
void axl_stream_fail_next_write (size_t n);
void axl_stream_fail_next_read  (size_t n);
void axl_stream_fail_next_flush (size_t n);
void axl_stream_short_next_write(size_t n, size_t limit);
```

`short_next_write` produces a **short** write rather than a failed one —
the case with no `AxlMem` analogue, because a partial allocation is not a
thing. `limit` is clamped against the backend call's own count and may be
`0`, which is the "accepted nothing, try again" result that drives the
buffered path's retain-the-tail branch.

The counter ticks **once per call into the backend's operation, not once
per `axl_write`**, and the three ways those differ are the whole reason to
read this paragraph:

- Under `AXL_STREAM_BUF_LINE` / `_FULL`, a write that only fills the
  buffer reaches no backend and consumes no tick — the armed failure
  fires at the eventual flush.
- With a non-UTF-8 encoding the transcode issues one backend write **per
  code point**, so one `axl_write` of an n-character string consumes n
  ticks. That is about `axl_stream_set_encoding`, *not* about the console:
  `axl_stdout` and `axl_stderr` are UTF-8 streams whose UCS-2 conversion
  happens inside their backend, below the injection point, so a console
  write costs one tick however long the string is.
- Writes to a stdout/stderr **tee never consume a tick**; only the primary
  sink is injected. `axl_pread`/`axl_pwrite` are never injected at all.

If the armed operation never reaches a backend the counter **stays armed**
and fires on a later, unrelated operation — always disarm with `0`. The
hooks are not reentrancy-safe: a callback that writes at raised TPL can
consume the armed tick, so arm and observe within one non-reentrant
sequence. In practice that means capturing results into locals and
asserting *after* disarming, since a test framework's own PASS line is
itself a write.

## File Operations

```c
bool exists = axl_file_exists("fs0:/data.bin");
bool is_dir = axl_file_is_dir("fs0:/logs");

axl_file_delete("fs0:/temp.txt");
axl_file_rename("fs0:/old.txt", "fs0:/new.txt");
axl_dir_mkdir("fs0:/output");
```
