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
print a clear error. `axl_stderr` (text) has no such gap: it falls
back to `gST->ConOut` when `gST->StdErr` is NULL, so diagnostics
still land somewhere instead of going silent.

### Standard streams and their sinks

| Stream | Direction | Encoding | Sink |
|---|---|---|---|
| `axl_stdin` | in | raw bytes (redirected) / line-cooked (interactive) | shell **StdIn** handle (`EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`) — `<` / `\|`, else console line editor |
| `axl_stdin_text()` | in | text (UTF-8, auto-sniffed) | `axl_text_stream_wrap` over `axl_stdin` — decodes the UCS-2 `\|` pipe to UTF-8 |
| `axl_stdout` | out | text (UTF-8 in → UCS-2 out) | `gST->ConOut` — honors `>` / `>>` |
| `axl_stderr` | out | text (UTF-8 in → UCS-2 out) | `gST->StdErr`, falling back to `gST->ConOut` when NULL — honors `2>` / `2>>` |
| `axl_stdout_raw` | out | raw bytes | shell **StdOut** handle (direct `WriteFile`, bypasses the console) |
| `axl_stderr_raw` | out | raw bytes | shell **StdErr** handle (direct `WriteFile`); sibling of `axl_stdout_raw` — also works in a resident shared-driver via the stdio bridge |

Diagnostic logging (`axl_log`, `axl_warning`, including the level-color
attribute) writes to `axl_stderr`, not `axl_stdout` — so `tool > out.txt`
captures only the tool's own stdout text, with no AXL log lines mixed in.
A script that used to scrape AXL diagnostics out of a `>`-redirected file
must switch to `2>`.

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
