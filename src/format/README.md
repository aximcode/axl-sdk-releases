# AxlFormat — Printf Engine

Callback-driven printf engine. Format text directly into any sink
(buffer, network socket, file, hash) without intermediate allocation.

This is the engine behind `axl_printf`, `axl_snprintf`, `axl_asprintf`,
and `axl_string_append_printf`. It has **zero dependencies** (no memory
allocator, no I/O) — it breaks the Log -> Data circular dependency by
being self-contained.

**It stays, even once AXL sits on newlib, and that is measured rather than
sentimental.** Newlib's `vsnprintf` is not a string function that happens to
live in stdio — it *is* stdio, writing to a fake `FILE`, and it arrives with
the allocator, `findfp` and `_impure_ptr` behind it. `AxlLog` calling it would
reinstate the exact Log -> Data cycle this engine exists to break, at 3.2x the
size for the integer-only path and 8x for the general one. So AXL keeps two
formatters on purpose: this one under `axl_printf`, and newlib's under
third-party C's `printf`. See `docs/AXL-Libc-Substrate-Design.md` §4.1 for the
measurements and §4c.1 for how the two paths sit side by side.

Header: `<axl/axl-format.h>`

## What it costs to use `printf` instead

Since AXL sits on newlib, `printf` and `fopen` work for third-party C —
so the choice between `axl_printf` and `printf` is now real, and it is
not close. Measured on `sdk/examples/hello.c`, x64 `--release`, only the
call changed:

| the program calls | `.efi` | delta |
|---|---|---|
| `axl_strcmp` → `strcmp` | 47,265 | **+18** |
| `axl_printf` (baseline) | 47,247 | — |
| `axl_malloc` → `malloc` | 60,760 | **+13,513** |
| `axl_printf` → `printf` | 109,147 | **+61,900 (2.3x)** |

**String and memory functions are free.** `strcmp`, `memcpy`, `strlen`
and friends are leaf code with no state behind them — 18 bytes, which is
alignment noise. Use whichever reads better.

**The allocator costs ~13 KB on disk for ~7 KB of content**, and the gap
is worth understanding because it applies to everything here:

| section | delta | |
|---|---|---|
| `.rela` | **+6,240** | relocations — the largest single item |
| `.text` | +3,952 | dlmalloc itself |
| `.data` | +2,480 | `__malloc_av_` (2 KB bin array) + `_impure_data` |
| `.bss` | +656 | |

Nearly half is RELOCATIONS, not code: dlmalloc's bin array is 128 pointer
pairs, and a `-fpic` UEFI image carries a `.rela` entry per pointer. A
data structure full of pointers costs roughly twice its own size here.

Note also what comes with it — `_impure_data` (344 B) and **`__sf`
(528 B), newlib's static `FILE` array**. `_malloc_r` takes a
`struct _reent *`, callers pass `_impure_ptr`, and that is one large
struct with the stdio slots embedded. So `malloc` alone drags a fragment
of stdio's state.

`axl_malloc` adds ~0 — not because it is clever, but because
`AllocatePool` and the backend are already linked into every image. It is
a wrapper over something already present, against importing a whole
allocator.

**`printf` costs ~62 KB, and that is the one worth knowing.** The
breakdown says why:

| bytes | symbol |
|---|---|
| 12,380 | `_vfprintf_r` |
| 6,192 | `_vfiprintf_r` — the integer-only twin; **both** arrive |
| 6,066 | `_dtoa_r` — newlib's float conversion |
| 2,304 + 2,064 | `_malloc_r` + `__malloc_av_` — `FILE` buffers allocate |
| 1,409 + 1,305 | `_realloc_r`, `__sfvwrite_r` |

Newlib's `vsnprintf` is not a formatter that happens to live in stdio —
it *is* stdio, writing to a `FILE`. So asking for one format call brings
the buffering layer, the allocator, and both the integer and float
engines. You also end up carrying each engine twice: `axl_vformat` **and**
two `vfprintf` variants, `axl_dtoa` **and** `_dtoa_r`.

**So:** use `axl_printf` in AXL code and in anything size-sensitive;
`printf` exists so third-party C compiles unmodified, which is worth
62 KB when you are porting a library and worth nothing when you are not.
The same reasoning is why `AxlLog` may never call newlib's formatter —
see below.

## Callback-Driven Formatting

The core API takes a write callback that receives formatted output
in chunks. No memory is allocated — all formatting uses a small
stack buffer.

```c
#include <axl.h>

// Write directly to a TCP socket
void net_write(const char *data, size_t len, void *ctx) {
    axl_tcp_send((AxlTcp *)ctx, data, len, 0);
}

// Format an HTTP request line directly into the socket
axl_format(net_write, sock, "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n",
           path, host);
```

## API

Two functions:

- `axl_format(write_fn, ctx, fmt, ...)` — variadic convenience
- `axl_vformat(write_fn, ctx, fmt, args)` — va_list version

The callback type:

```c
typedef void (*AxlWriteFunc)(const char *data, size_t len, void *ctx);
```

## Use Cases

- Format directly into a network send buffer (no intermediate string)
- Stream formatted output into a hash computation
- Write a custom logging backend that formats in-place
- Build protocol messages without allocating temporary strings

## Supported Format Specifiers

| Specifier | Type | Example |
|-----------|------|---------|
| `%s` | `char *` string | `axl_printf("%s", name)` |
| `%d` | signed int | `axl_printf("%d", -42)` |
| `%u` | unsigned int | `axl_printf("%u", 42)` |
| `%x` / `%X` | hex (lower/upper) | `axl_printf("0x%x", 0xFF)` |
| `%llu` | `uint64_t` | `axl_printf("%llu", big)` |
| `%zu` | `size_t` | `axl_printf("%zu", len)` |
| `%c` | char | `axl_printf("%c", ch)` |
| `%p` | pointer | `axl_printf("%p", ptr)` |
| `%f` / `%F` | fixed-point double | `axl_printf("%.2f", 3.14159)` |
| `%e` / `%E` | exponential double | `axl_printf("%e", 1.5e300)` |
| `%g` / `%G` | shorter of the two | `axl_printf("%g", 0.0001)` |
| `%%` | literal `%` | `axl_printf("100%%")` |

Width and zero-padding are supported: `%08x`, `%-20s`, `%5d`.

## Floating Point

The float conversions are built on `axl_dtoa` (Grisu2) — one shortest-digits
conversion feeds all three styles — so there is no `libm` dependency and no
arbitrary-precision arithmetic. `%f`/`%e` round that shortest digit string to
the requested precision, which is accurate to roughly 15 significant digits;
that is the accepted tradeoff of the design, and it is documented on
`axl_snprintf` itself. `%f` rounds half-up rather than glibc's half-to-even.

**For bit-exact round-tripping of a double, do not use `%f`/`%g`.** Use
`axl_double_to_str` (`<axl/axl-str.h>`), which emits the shortest text that
parses back to the identical double via `axl_str_to_double`. The printf
family is for humans; the conversion family is for data.
