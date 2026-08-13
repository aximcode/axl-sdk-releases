# Newlib as a substrate under AXL — an investigation, not a plan

> **SUPERSEDED 2026-08-11 by `AXL-Libc-Substrate-Design.md`.** The direction
> is agreed; this is no longer "not a plan". §4 below calls `_sbrk` the reason
> it could not be one — that objection is resolved by INVERTING the allocator
> (AXL's implementation takes the standard names, so newlib never needs a heap)
> rather than by bridging to it. The measurements here remain valid and are
> why the design doc does not repeat them.

> **Status: NOT SCHEDULED. Recorded 2026-08-10 because the option was
> only discovered while solving a different problem, and the measurements
> are cheap to lose.**
>
> Everything marked *measured* below was run on this tree during the C++
> exception work. Everything else is explicitly flagged as untested.

---

## 1. The idea

Today AXL implements its own C library: `axl_malloc` over `AllocatePool`,
`AxlFormat` as a zero-dependency printf engine, `axl_str*`, `axl-fs.c`
over `EFI_FILE_PROTOCOL`, and `include/compat/` to fake seven standard
headers for code that expects them.

The alternative is to sit the GLib-shaped public API on **newlib**
instead of on raw UEFI: newlib supplies the C library, AXL supplies a
*libgloss* board-support layer that maps newlib's platform hooks onto EFI
calls, and `axl_*` stays exactly the API it is today.

The API identity and the substrate are orthogonal. That is the part worth
recording — an earlier version of this analysis wrongly framed newlib as
"a different project", when in fact the GLib-shaped surface layers over
either substrate.

## 2. What newlib would cost to host — measured

`libc.a` from the `aarch64-none-elf` toolchain needs **35 symbols** from
the platform. The classic libgloss set, and most map onto EFI directly:

| newlib hook | EFI mapping | notes |
|---|---|---|
| `_open _read _write _close _lseek _fstat _stat _unlink _mkdir` | `EFI_FILE_PROTOCOL` | **`src/fs/axl-fs.c` already does exactly this**, behind a different API |
| `_exit` | `gBS->Exit` | |
| `_isatty` | console-protocol check | |
| `_gettimeofday` `_times` | `GetTime` | |
| `_getentropy` | `EFI_RNG_PROTOCOL` | AXL has `axl-rng.h` |
| `_sbrk` | a carved arena | **the problem — see §4** |
| `_fork _execve _wait _kill _getpid _link` | — | no meaning under UEFI; `ENOSYS` |
| `regcomp` `regexec` `regfree`, `__multf3` and the soft-float `long double` helpers | — | pulled by newlib itself |

So the port is roughly: nine file operations AXL has already written
against the same protocol, five trivial ones, six stubs, and one genuine
design problem.

## 3. Licence — verified

From `sourceware.org/newlib/COPYING.NEWLIB`:

> Each file may have its own copyright/license that is embedded in the
> source file… Copyright (c) 1994-2009 Red Hat, Inc… Copyright (c)
> 1981-2000 The Regents of the University of California.

**BSD-family, per-file, permissive — no copyleft.** A different category
from libstdc++/libsupc++ (GPL-3 + RLE), and compatible with AXL's
Apache-2.0. `libc.a` is 8 MB, though selective linking means an image
carries only what it uses.

## 4. The reason this is an investigation and not a plan

**`_sbrk` is a linear-heap model, and `AllocatePool` is not.** Newlib's
malloc wants a contiguous region it can extend; UEFI hands out pool
allocations from a firmware-managed map. Bridging them means carving a
fixed arena up front, and the consequences are not cosmetic:

- The **firmware's memory map stops being accurate**. Today every
  `axl_malloc` is an `AllocatePool` the firmware knows about; with a
  carved arena it sees one opaque block. For a tool that runs *inside*
  firmware, that is a real loss.
- **AXL's allocator instrumentation disappears.** Fence posts, the
  `0xDA`/`0xDF` fills, the live-allocation list behind the leak gate, and
  the free quarantine that catches use-after-free and double frees all
  live in `src/mem/axl-mem.c`. Newlib's malloc has none of it, and the
  leak gate is currently a hard build gate.
- Newlib's `printf` pulls float and locale machinery that `AxlFormat`
  avoids by design (it is zero-dependency to break the Log -> Data
  circular dependency).

None of that makes the idea wrong. It makes the interesting question
narrower: **could newlib sit under AXL for everything EXCEPT the
allocator**, with `axl_malloc` kept as the memory path? That is the
version worth spiking, and it has not been tried.

## 5. What is already proven, and it is not nothing

The C++ exception work used newlib **without anybody planning to**:

- An aa64 image compiled against **newlib's headers** (which is what
  dropping `-Iinclude/compat` yields under the bare-metal toolchain) and
  linked against **`libaxl.a` as the C library** — no `libc.a` at all —
  passed 7/7: `std::vector`, `std::string`, `std::map`,
  `std::runtime_error`, `vector::at` throwing `out_of_range` through
  libstdc++'s own frames, and a destructor running during the unwind.
  It needed **four** stubs: `getenv`, `strtoul`, `_impure_ptr`,
  `__xpg_strerror_r`.

So the split that already works is **newlib's headers, AXL's
implementations**. That is the cheapest useful subset and it is the
natural first step of any larger adoption.

**A hard limit found the same day:** newlib's headers alone do NOT help
x64. libstdc++'s headers are configured at build time for a specific C
library — the host's expect glibc's `uselocale`/`locale_t`, and compiling
them against newlib's headers fails in `bits/c++locale.h`. C headers and
C++ headers are a matched pair that arrives as a *toolchain*, which is
why `--hosted` cannot be retired on x64 without an `x86_64-elf` toolchain
(nobody publishes one; bootlin ships only linux-gnu/musl, and
crosstool-NG is not packaged).

## 6. If this is ever picked up

Order, cheapest first:

1. **Vendor newlib's headers for x64** so both arches share one header
   set — bounded: only `math.h`, `sys/config.h` and `machine/ieeefp.h`
   are arch-specific out of the C headers, but note §5's limit, which
   means this buys consistency rather than retiring `--hosted`.
2. **Spike newlib-minus-malloc**: libgloss over EFI for the file and time
   hooks, `axl_malloc` retained, and measure what breaks.
3. Only then consider whether any of `src/format/`, `src/data/axl-str*.c`
   should be retired in favour of newlib's equivalents — and weigh that
   against the size and the loss of the zero-dependency property.

## 7. Related

- [`AXL-Cxx-Design.md`](AXL-Cxx-Design.md) §6a — freestanding vs hosted,
  and why the two modes exist.
- [`AXL-Cxx-Unwinder-Design.md`](AXL-Cxx-Unwinder-Design.md) §U1 — the
  exception runtime work that surfaced all of this.
- `src/mem/README.md` — the allocator instrumentation §4 would cost.
