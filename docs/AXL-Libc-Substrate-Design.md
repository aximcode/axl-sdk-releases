# AXL on a libc substrate — `axl_*` over POSIX, the way GLib sits on libc

> **Status: DIRECTION AGREED 2026-08-11, not yet scheduled as phases.**
> Supersedes the framing of `AXL-Newlib-Investigation.md`, which recorded
> this as "an investigation, not a plan". It is a plan now. The
> measurements in that document remain valid and are not repeated here.

---

## 1. The shape

AXL's public API does not change. `axl_malloc`, `axl_strdup`, `axl_printf`
stay exactly what they are — that is the GLib relationship, and the API
identity is orthogonal to what sits underneath it.

What changes is the substrate. Today AXL implements its own C library over raw
UEFI: an allocator over `AllocatePool`, `AxlFormat` as a zero-dependency printf
engine, `axl_str*`, and `include/compat/` faking seven standard headers for
code that expects them. The direction is to sit on **newlib** instead, with one
deliberate exception.

    consumer code          axl_malloc / axl_printf / axl_str*      (unchanged)
    ----------------------------------------------------------------------
    substrate              newlib: string, math, stdio, stdlib
    ALLOCATOR EXCEPTION    AXL's own, exported under the STANDARD names
    ----------------------------------------------------------------------
    platform               UEFI boot services

## 2. The allocator inversion, which is the load-bearing idea

The natural reading of "sit on libc" is `axl_malloc` calling newlib's `malloc`.
That reading is what made this a non-starter, and `AXL-Newlib-Investigation.md`
§4 says why: newlib's malloc wants a contiguous region it extends through
`_sbrk`, UEFI hands out pool allocations from a firmware-managed map, and
bridging them means carving a fixed arena. Two consequences follow that are not
cosmetic — the firmware's memory map stops being accurate (today every
allocation is an `AllocatePool` the firmware knows about; with an arena it sees
one opaque block), and AXL's fence posts, `0xDA`/`0xDF` fills, live-allocation
list and free quarantine all disappear, taking the leak gate with them.

**Invert it instead. AXL's allocator keeps its implementation and takes the
standard NAMES:**

```c
    malloc(size)      ->  axl_malloc_impl(size, "<libc>", 0)
    axl_malloc(size)  ->  axl_malloc_impl(size, __FILE__, __LINE__)
```

One allocator, two entry points, differing only in call-site attribution. The
instrumentation lives in `axl_malloc_impl`, so both names carry it.

That removes `_sbrk` from the picture entirely: newlib never needs a heap
because it never allocates one — we replace its allocator rather than feeding
it a region. No arena, no opaque block in the firmware's map, no dlmalloc, and
the leak gate keeps working unchanged.

**It is also strictly better than today on coverage.** Third-party C in this
tree — mbedtls, lzma, stb, newlib itself — allocates through `malloc`, which
AXL does not currently provide, so none of it is visible to the leak gate.
Under the standard names it all becomes tracked. This is not hypothetical: the
libstdc++ emergency-pool leak fixed in `a0234245` was findable only because
`src/cxxrt/axl-cxxrt-alloc.c` hand-bridges `malloc` onto `axl_malloc` for the
C++ path. That file exists *solely* because AXL's allocator has non-standard
names, and this design makes it unnecessary.

## 2b. Why this IS the GLib model, not a deviation from it

The obvious objection to AXL supplying `malloc` is that it looks like the
opposite of "call libc the way GLib does". It is not, and the distinction is
worth stating because it decides how far the substrate goes.

`g_malloc` calls `malloc`, and on Linux that is **glibc's own implementation**
over the platform primitive (`brk`/`mmap`). GLib ships no allocator; it uses
the libc's, and the libc's allocator is written against whatever the platform
provides.

Our platform primitive is `AllocatePool`, and it is *better* than a linear
break: it is already an allocator, and the firmware tracks every block. Newlib's
dlmalloc is not a platform allocator -- it is a portable fallback for targets
whose only primitive is `sbrk`. Adopting it here would be like glibc shipping
dlmalloc over a fixed arena carved from `brk` and discarding `mmap`.

So AXL supplying `malloc` is AXL *being the libc* for the one function where
UEFI offers something better than newlib assumes exists. Everywhere else --
`str*`, `math`, `strtod`/`dtoa`, and probably `printf` -- newlib is strictly
better than what AXL wrote, and `axl_*` becomes the thin wrapper the GLib
comparison implies.

Stated as a rule: **AXL implements a libc function only where the UEFI
primitive beats newlib's portable assumption. Today that is the allocator, and
nothing else has been shown to qualify.**

## 2c. Link order, and why it should not be a convention

Both `libaxl.a` and `libc.a` defining `malloc` makes resolution depend on link
order, which is a fragile thing to rely on: a consumer linking `libc.a` first
silently gets dlmalloc, whose `_sbrk` returns -1, so every allocation fails.

**Delete the ambiguity rather than document an ordering.** Newlib's allocator
occupies eight discrete archive members, so it is separable (measured, x64
libc.a):

    libc_a-malloc.o    malloc, free     libc_a-mallocr.o   _malloc_r
    libc_a-calloc.o    calloc           libc_a-callocr.o   _calloc_r
    libc_a-realloc.o   realloc          libc_a-reallocr.o  _realloc_r
                                        libc_a-freer.o     _free_r

Strip those from the shipped `libc.a` -- or build newlib without them, since
AXL builds the toolchain -- and exactly one definition of `malloc` exists in
any link. Order stops mattering because there is nothing to order.

Two things follow, both wanted:

- It **forces** the `_r` bridge of §4.2, because `_malloc_r` and friends
  disappear with those members. That bridge is the correctness fix regardless;
  this removes the option of shipping without it.
- It retires the `_sbrk`-returns-`-1` invariant recorded in
  `src/cxxrt/axl-cxxrt-alloc.c`. Today that is what keeps newlib's heap from
  ever obtaining a byte; afterwards newlib has no heap to obtain one for.

A gate should assert no newlib allocator symbol reaches a produced image --
the failure is silent and the regression is easy.

## 3. Why the substrate is worth having

- **`include/compat/` stops existing.** Its `typedef void FILE` collides with
  the real `<stdio.h>`, and that collision is what blocks `<string>` and
  `<memory>` under hosted C++ — i.e. it is the direct cause of the
  `--hosted` fork that `AXL-Cxx-Design.md` T3/T4 exist to retire. A real libc
  removes the REASON for the split rather than the symptom.
- **Decades-hardened numerics.** `axl-strtod.c`, `axl-dtoa.c`, `axl-math.c`
  and AxlFormat's float paths are AXL-maintained code that has produced real
  defects in this tree. Newlib's equivalents have not.
- **Vendored C stops needing shims**, and future ports get cheaper.

## 3b. The constraint: no loss of functionality or performance

Adopting newlib is only worth doing where it is a strict improvement. This is
a REQUIREMENT on the migration, not an aspiration, so it needs a test rather
than an intention.

### What is even a candidate

A function is a candidate only if newlib has a genuine counterpart. Much of
`axl_str*` does not, and the question never arises for it:

- **UTF-8 / UCS-2 conversion** (17 sites in `src/data/axl-str.c`). UEFI is
  UCS-2; newlib has no equivalent. Stays.
- **Boyer-Moore-Horspool search** (`axl-str-bmh.c`), **base64**
  (`axl-str-base64.c`), the **scan** helpers, the number parsers with explicit
  error returns rather than `errno`. No libc counterpart. Stays.
- **`AxlString`, `AxlStrBuf`** and the container family. Never was libc.

That leaves the actual overlap: `mem*`, the `str*` primitives, `strtod`/`dtoa`,
`math`, and `printf`.

### Two gates, per function, before anything is replaced

**FUNCTIONALITY — the tests are the contract.** A replacement must leave the
suite green with no assertion weakened. That is only meaningful because output
assertions here are exact-string (`axl_strcmp(buf, "...") == 0`, never
`axl_strstr`), which is what makes a formatting difference fail rather than
pass quietly. Where AXL's signature carries information libc's does not -- an
explicit error return where libc uses `errno`, a bounded write where libc's is
unbounded -- the wrapper keeps AXL's shape and the loss does not reach callers.

**PERFORMANCE — measured, both directions.** `axbench` exists; benchmark
before and after. Warm the heap first: a growing allocation measures 330 ns
against 60 ns for a reused one, and that difference has already inverted a
conclusion in this tree once.

### What measurement says so far, which is encouraging

- **`axl_memcpy` is a naive byte loop** (`src/data/axl-str.c`) -- no SIMD, no
  word-at-a-time. Newlib's is very likely FASTER, so for `mem*` this is a
  performance win rather than a risk. Still measure; "likely" is not a number.
- **`AxlFormat` implements 14 standard conversions and no custom ones**, and
  lacks `%o` and `%a`. Newlib's `printf` is a functional SUPERSET, so the
  exposure there is size and dependencies (§4.1), not capability.
- **`axl-strtod.c` / `axl-dtoa.c` / `axl-math.c`** are where AXL-maintained
  numerics have produced real defects. Newlib's have not.

### Where a regression is acceptable

Nowhere silently. If a replacement costs something real -- a slower path, a
dropped guarantee -- the answer is to keep AXL's implementation for that
function and record why here. The rule of §2b already permits exactly that:
AXL implements a libc function where the AXL version is better for this
platform. The allocator is the first entry in that list; it does not have to
be the last.

## 4. Open questions — measure, do not argue

**4.1 Does newlib's `printf` reintroduce the Log -> Data cycle?** `AxlFormat`
is zero-dependency BY DESIGN: `AxlLog` cannot call `axl_malloc`, and that is
what breaks the circular dependency between the two modules. Newlib's `printf`
pulls float and locale machinery and allocates on some paths. If `AxlLog` sat
on it, the cycle may return. This is the single measurement that decides how
deep the substrate goes — link `AxlLog` against newlib's printf and read the
undefined set. Until it is answered, `AxlFormat` stays.

**4.2 The `_r` family.** Newlib's reentrant internals call `_malloc_r`,
`_free_r`, `_realloc_r`, `_calloc_r` rather than the plain names — roughly 45
objects (stdio, `mprec`'s float formatting, `strdup_r`). Those must be bridged
onto the same allocator, or two heaps coexist and a pointer crossing between
them corrupts. Today this is contained by a different mechanism: `_sbrk`
returns -1 so newlib's heap can never obtain a byte (see the invariant recorded
in `src/cxxrt/axl-cxxrt-alloc.c`). Under this design that invariant becomes
obsolete and the `_r` bridge replaces it — the two are alternatives, and
shipping neither is the corruption case.

**4.3 Link order becomes semantic.** `libaxl.a` must precede `libc.a` or
newlib's `malloc` wins the symbol. That is already the order every build path
uses, but it stops being incidental and probably wants a gate.

**4.4 C conformance of the edges.** Real C code will call these, so the
edge cases stop being AXL's to define. Verified already: `realloc(NULL, n)`
behaves as `malloc`, `calloc` zeroes and is overflow-checked, `malloc(0)`
returns a live block. The one to decide is `realloc(p, 0)`, which AXL currently
frees and returns NULL for — matching newlib, but undefined behaviour in C23.

**4.5 Firmware-owned memory is unchanged and still a trap.** Memory the
firmware allocated (`LocateHandleBuffer`, `QueryMode`, ...) must still be freed
with raw `FreePool`, because AXL's allocator prepends a bookkeeping header its
`free` expects to find. Giving that allocator the name `free` does not change
the rule, and arguably makes it easier to get wrong — `check-dogfood`'s
`axl-pool-direct` marker is what polices it.

## 5. Relationship to the C++ work

`src/cxxrt/` (`a0234245`) is the first working instance of this design, built
for the C++ exception path before the general direction was stated: newlib
supplies the C library, `malloc` routes onto AXL's allocator, and a
containers+exceptions image runs 7/7 with zero leaks on both arches. It is
evidence the shape works, not a detour from it.

Two consequences for `AXL-Cxx-Design.md` §6a-PLAN:

- **T3/T4 become the first payoff rather than cleanup.** `include/compat`
  exists only because there is no libc; retiring it for C++ is a step toward
  deleting it outright.
- **Flipping x64's default compiler grows in scope.** Under this direction, C
  moves to the bare-metal toolchain too, not just C++. That is larger than T2
  scoped and wants its own spike.

## 6. Related

- `docs/AXL-Newlib-Investigation.md` — the measurements (size, licence, what
  newlib costs to host). Its "not a plan" status is superseded by this file.
- `docs/AXL-Cxx-Design.md` §6a-PLAN — T1-T5, and why `include/compat` blocks
  hosted C++.
- `src/cxxrt/axl-cxxrt-alloc.c` — the bridge this design makes unnecessary,
  and the `_sbrk` invariant §4.2 replaces.
