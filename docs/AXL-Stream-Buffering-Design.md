# AXL Stream Buffering + Interactive Read-Discipline — Design

**Status:** implemented in v2.8.1. Two small, orthogonal additions to
`AxlStream` that bring it closer to the C/POSIX stdio model without
importing that model's footguns:

1. **Output buffering** (`axl_stream_set_buffering` + the `setvbuf`
   family) — the stdio *buffering* axis.
2. **Interactive / no-EOF source marking** (`axl_stream_set_interactive`)
   — the stdio-adjacent *line-discipline* axis (C's `termios`/ICANON).

Both are new API; no existing signature changed. Public surface lives in
`include/axl/axl-stream.h`; implementation in `src/stream/axl-stream.c`
(buffering machinery in `axl_write`) and `src/stream/axl-stream-text.c`
(the wrapper short-circuit).

---

## Motivation

The interactive axis fell out of a v2.8.0 regression: `axl_stdin_text()`
= `axl_text_stream_wrap(axl_stdin)` hung when read interactively. The
wrapper's construction-time encoding sniff loops `src->read` until it
fills a 64-byte probe *or hits EOF* — correct for a file / `<` / `|`
(they reach EOF), but 2.8.0 gave interactive `axl_stdin` proper
line-cooked semantics that **never return EOF**, so the sniff swallowed
line after line. The initial fix special-cased `axl_stdin`; this design
generalizes it to a first-class stream property so *any* no-EOF source a
consumer wraps is safe.

The buffering axis is the natural neighbor the maintainer asked to add at
the same time — AXL had no stdio-style buffering *mode* at all (only
transient per-`printf` stack coalescing and a 3-byte transcode straddle;
every sink wrote straight through, i.e. effectively `_IONBF`).

## The two axes are orthogonal — and must stay so

C stdio conflates them under one `FILE`, which invites the misconception
that `setlinebuf` gives "one line per read." It does not: line buffering
is an **output** concept, and "one line at a time" on a terminal comes
from the **kernel tty line discipline** (`termios`/ICANON), a layer below
stdio. AXL keeps them explicitly separate:

| Layer | Linux | AXL |
|---|---|---|
| line-cooking / "one line per read" | kernel tty `ICANON` (`termios`) | `console_read_interactive` + `axl_stream_set_interactive` |
| read-ahead / write coalescing | stdio `setvbuf` mode | `axl_stream_set_buffering` / `axl_setvbuf` |

Consequences pinned by this design:
- `axl_setlinebuf` means exactly what C means — an **output** flush-on-`\n`
  policy. It is never overloaded to mean line-at-a-time *input*.
- AXL performs no input read-ahead, so the buffering mode never affects
  reads. The only read-ahead that ever existed was the wrapper's sniff,
  which the interactive mark now governs.

## Design decisions

### Buffering default is NONE (opt-in), not isatty-driven

C stdio auto-selects by `isatty()`: terminal → line, pipe/file → full,
stderr → none. AXL deliberately does **not**, for two UEFI-specific
reasons:

1. **No guaranteed flush-on-exit.** `axl_stream_init` runs from three
   entry paths (runtime, driver, and the *minimal* crt0); only the first
   two run the atexit registry. A default-buffered `axl_stdout` in a
   minimal-crt0 app that writes and returns would **silently lose**
   output.
2. **Prompt-visibility regression.** A line-buffered prompt written
   without a trailing `\n` before a blocking stdin read would stall in the
   buffer unseen — reintroducing exactly the failure class the
   interactive fix removed. C survives this via a tty→stdout flush
   coupling and decades of `fflush(stdout)` discipline; AXL has neither.

So buffering is strictly opt-in and the caller owns the final flush
(`axl_fflush` — which on a file stream also pushes the firmware's cache
through to the volume; `axl_fclose` drains and frees but never calls the
sink's flush, so it is not a substitute). A future release could
add the isatty-driven defaults *if* it also builds the flush-before-read
coupling and flush-on-exit across all crt0 paths — that is a
behavior-change (minor-version) undertaking, out of scope here.

### The buffer lives in `axl_write`

`axl_write` is the single choke point every text-output entry point
funnels through: `axl_print`/`axl_printf`/`axl_fprintf` format into a
scratch buffer and issue one `axl_write`; `axl_fwrite` calls it directly.
Placing the persistent buffer there — *ahead* of the per-stream
UTF-8 → UCS-2 transcode and the tee — coalesces every path uniformly and
keeps transcode/tee semantics identical to the unbuffered case (the same
`stream_write_now` sink handles a direct write and a buffer flush).

### `setvbuf` family: keep the names, drop the borrowed buffer

`axl_setvbuf` / `axl_setlinebuf` / `axl_setbuf` exist for muscle memory,
but **AXL always owns the buffer**. A caller-supplied buffer whose
lifetime must outlive the stream is a use-after-free hazard in RAII/UEFI
code, so the `buf` argument is ignored (a debug assert catches a mistaken
hand-off); only `axl_setbuf`'s NULL-vs-non-NULL is consulted, per C's
`buf ? _IOFBF : _IONBF`. `mode` is the `AxlStreamBuffering` enum, not the
C `_IO*` ints. BSD `setbuffer` is not provided (reducible to
`set_buffering`).

## Semantics (the sharp edges)

- **LINE** flushes everything through the last `'\n'` on each write and
  retains a partial trailing line; if the buffer fills with no `'\n'`, the
  whole buffer is flushed so a newline that never comes can't wedge the
  stream (matches glibc `_IOLBF`).
- **Over-size write** (≥ buffer capacity) flushes any pending bytes first
  — preserving order — then writes directly; buffering never truncates,
  splits, or reorders it.
- **Mode switch** flushes the old mode's pending bytes first.
- A buffered write returns bytes **accepted into the buffer**; a sink
  error surfaces at the later `axl_fflush`/`axl_fclose` and via
  `axl_ferror`, not at the buffered write.
- Flushing drains the buffering layer through the transcoder; a non-UTF-8
  stream may still retain an *incomplete* trailing multi-byte unit until
  the completing bytes arrive (pre-existing transcode behavior).

## Testing

Unit (`test/unit/axl-test-io.c`, deterministic over `axl_bufopen`):
LINE/FULL hold-and-flush, buffer-fills-without-newline, over-size
ordering, mode-switch flush, the `axl_fprintf` path coalescing (proving
the buffer catches the printf family), the three shims, and a file
round-trip proving `axl_fclose` flushes. Interactive: set/get round-trip,
wrapper inheritance, and a headerless-UCS-2 payload that is *decoded* when
unflagged but *passes raw* when the source is flagged interactive —
proving the short-circuit generalizes beyond `axl_stdin`.

Integration (`test/integration/test-console-readline-qemu.sh`): the
`TEXTWRAP` step feeds one short line to `axl_stdin_text()` over a real
interactive serial console and asserts it returns on a single Enter —
the end-to-end regression guard for the original hang.
