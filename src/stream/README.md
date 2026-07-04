# AxlStream — byte-stream abstraction

`AxlStream` is the polymorphic byte-source/sink — modeled on POSIX
`<stdio.h>`'s `FILE *`. It wraps a vtable over file handles, memory
buffers, the console, the BOM-detecting text decoder, etc. All
public functions that take `AxlStream *` live in this module.

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

`axl_stdin` (above) is shell-pipe input — bytes the shell captured
from the left-hand side of a `|`. For interactive prompts where
the tool needs to wait on a real keystroke (`y`/`n` confirmations,
"press any key", arrow-key menus), use `<axl/axl-console.h>`:

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

// Write entire file
axl_file_set_contents("fs0:/output.txt", buf, buf_len);
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

For shell pipe invocations (`tool1 | tool2`) the LHS output is
captured by the shell into a stream that becomes the RHS's StdIn, so
`axl_read(axl_stdin, ...)` consumes the piped bytes.

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
| `axl_stdin` | in | raw bytes | shell **StdIn** handle (`EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`) — `<` / `\|` |
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
| Text output (the common case) | `axl_print` / `axl_printf` / `axl_write(axl_stdout, ...)` — UTF-8 in, UCS-2 to console, captured-as-UCS-2 by the shell on `>` / `|` |
| Binary output (RAM-disk dump, captured SPD blob, etc.) | `axl_write(axl_stdout_raw, ...)` — raw bytes, bypasses the CHAR16 console path so they survive a pipe intact |

Don't use `axl_stdout_raw` for text; the firmware console only knows
UCS-2, so writing raw 8-bit bytes to it (when no shell redirection
is in play) would mangle the display. The raw path is only useful
when the caller knows the shell wired their StdOut to a file or pipe.

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

BOM detection happens lazily on the first read:

| Leading bytes | Mode | Behavior |
|---|---|---|
| `FF FE`    | UTF-16 LE | BOM consumed, body transcoded to UTF-8 |
| `FE FF`    | UTF-16 BE | BOM consumed, body transcoded to UTF-8 |
| `EF BB BF` | UTF-8 BOM | BOM stripped, body returned verbatim |
| anything else | passthrough | raw bytes returned as-is |

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

## File Operations

```c
bool exists = axl_file_exists("fs0:/data.bin");
bool is_dir = axl_file_is_dir("fs0:/logs");

axl_file_delete("fs0:/temp.txt");
axl_file_rename("fs0:/old.txt", "fs0:/new.txt");
axl_dir_mkdir("fs0:/output");
```
