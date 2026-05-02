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

## File Read/Write

The simplest way to read or write files:

```c
// Read entire file into memory
void *data;
size_t len;
if (axl_file_get_contents("fs0:/config.json", &data, &len) == 0) {
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

`axl_stream_init` populates four globals:

| Stream | Direction | Encoding | Backed by |
|---|---|---|---|
| `axl_stdout` | out | text (UTF-8 in → UCS-2 to console) | firmware console (ConOut) |
| `axl_stderr` | out | text (same path as stdout) | firmware console |
| `axl_stdin` | in | raw bytes | `EFI_SHELL_PARAMETERS_PROTOCOL.StdIn` |
| `axl_stdout_raw` | out | raw bytes | `EFI_SHELL_PARAMETERS_PROTOCOL.StdOut` (direct WriteFile) |

For shell pipe invocations (`tool1 | tool2`) the LHS output is
captured by the shell into a stream that becomes the RHS's StdIn, so
`axl_read(axl_stdin, ...)` consumes the piped bytes.

When the shell-params protocol isn't published (cross-volume
launches, BDS contexts, non-Shell-2.0 launches), `axl_stdin` reads
return EOF (0 bytes) and `axl_stdout_raw` writes return -1 — tools
that opt in should fall back to a file argument or print a clear
error.

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
