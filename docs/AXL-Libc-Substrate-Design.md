# AXL on a libc substrate — `axl_*` over POSIX, the way GLib sits on libc

> **Status: COMPLETE 2026-08-17. DIRECTION AGREED 2026-08-11, PHASED AND
> COSTED 2026-08-17 (§4c, §4d). §2-DECISION replaced P1 the same day;
> P1', P2, P3, P4 and P5 ALL LANDED 2026-08-17** — P4 is the one C++ link
> shape (`libaxl-cxx.a` deleted; every C++ link now carries the toolchain's
> libstdc++/libsupc++), P5 was locale, measured to need nothing.
>
> This header read "P4-P5 not started" until 2026-08-21, while `P4-RESULT`
> and a ✅ P5 row sat ~800 lines below it in this same file. It was cited
> as a live blocker by ROADMAP's networking-layering item; correcting it
> released that.
> Supersedes the framing of `AXL-Newlib-Investigation.md`, which recorded
> this as "an investigation, not a plan". It is a plan now. The
> measurements in that document remain valid and are not repeated here.
>
> Owner of the facts about **what AXL still implements itself and why**.
> The two deliberate exceptions to "sit on newlib" are the allocator (§2)
> and `AxlFormat` (§4.1); everything else defers. `AXL-Cxx-Design.md`,
> `AXL-Cxx-Stdlib-Surface.md` and `AXL-Cxx-Unwinder-Design.md` link here
> rather than restating them.

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

    consumer code       axl_malloc / axl_printf / axl_str*     (unchanged)
    third-party C       printf / fopen / strdup                (NEW: just works)
    ------------------------------------------------------------------------
    substrate           newlib: string, math, stdio, stdlib
      EXCEPTION 1       the ALLOCATOR - AXL's own, under the
                        STANDARD names                          (§2)
      EXCEPTION 2       AxlFormat beneath AxlLog - zero-dependency,
                        stays permanently                       (§4.1)
    ------------------------------------------------------------------------
    porting layer       AXL's open/read/write/close/lseek/fstat  (§4c.1)
                        newlib defines NONE of these - measured
    ------------------------------------------------------------------------
    platform            UEFI boot services

The two exceptions are measured, not stylistic, and each has its own section.
The porting layer is not an exception at all: it is the floor newlib stands
on, and it is ours in every possible design.

## 2. The allocator inversion, which is the load-bearing idea

> **SUPERSEDED by §2-DECISION (2026-08-17).** The inversion described here
> — AXL's allocator taking the standard NAMES — is not the design any more.
> newlib keeps its own dlmalloc over an AXL `sbrk`, and `axl_malloc` stays
> separate. The measurements and the `AllocatePool`-vs-`sbrk` reasoning
> below remain accurate and are why the two are kept APART rather than
> merged; only the conclusion about who owns `malloc` changed.

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

## 2-DECISION 2026-08-17 — TWO ALLOCATORS, split by namespace. §2 and §2a below are SUPERSEDED

**Decided by Mike, 2026-08-17.** newlib keeps its own dlmalloc and gets a real
heap from an AXL `sbrk` over EFI. `axl_malloc` stays a separate allocator over
`AllocatePool`. They are divided by NAME and never mix:

    malloc / free / realloc / calloc / _malloc_r / ...   newlib's dlmalloc
                                                          -> AXL sbrk -> EFI
    axl_malloc / axl_free / ...                           AXL, AllocatePool

**Why the corruption argument in §2a does not apply.** That argument is sound
only while AXL holds the PLAIN names: `strdup()` allocates through `_malloc_r`
(newlib's) and a caller frees through `free` (AXL's), so a pointer crosses.
Under this decision the namespaces are DISJOINT — newlib owns the whole C
vocabulary, AXL owns the `axl_` one — so there is no crossing to protect
against. The hazard was a property of the split, not of having two allocators.

**Performance was explicitly NOT the driver.** The reasoning offered for this
shape was that dlmalloc's bins would beat an `AllocatePool` call per
allocation; the decision was taken with "I'm okay with it being slower than
`axl_malloc`". So no benchmark gates it, and none should be cited as
justification later.

**What it buys.** newlib works as designed rather than as a set of redirected
entry points: real `realloc` growth in place, chunk coalescing, and
`mallinfo`/`malloc_trim`/`malloc_usable_size` answering natively instead of
needing AXL shims (§2a's gap closes by deletion). Third-party C that expects
dlmalloc semantics gets them. And AXL's tracker stays focused on AXL's own
allocations instead of every byte newlib churns.

**What it costs, and this is the part to own.** `operator new` reaches `malloc`,
so C++ and third-party allocations leave AXL's leak gate. That is the mechanism
that found the libstdc++ emergency-pool leak (`a0234245`) — the gate keeps
working for AXL's own allocations and stops seeing the C/C++ world. The
firmware's memory map also shows one region rather than a block per allocation.
Both were listed as objections in §2; they are accepted, not refuted.

**The debug-allocator bridge: considered, MEASURED, dropped.** The obvious way
to keep AXL's instrumentation for the C/C++ world is an `#ifdef AXL_MEM_DEBUG`
bridge -- DEBUG builds point newlib's allocator names at AXL's instrumented
allocator, RELEASE builds let dlmalloc run. Both configurations are TOTAL
splits, so neither reopens the corruption case. It would be dead code:

| build | flags | links `libc.a`? |
|---|---|---|
| staged SDK (what consumers link) | **`-DNDEBUG`** only | yes, on `-fexceptions` |
| in-tree `BUILD=DEBUG` | `-DAXL_MEM_DEBUG` | **no** -- the only `libc.a` in the Makefile is a comment |

The two never coincide. Consumers link staged RELEASE objects, so the bridge
would not be compiled in -- and `axl-cc --debug` does not change that, since it
alters the CONSUMER's compile flags rather than which staged library it links.
In-tree DEBUG builds have the flag but never link `libc.a`, so newlib's
allocator names never appear for a bridge to intercept.

**Which also prices the decision honestly: the split costs consumers nothing.**
Everything listed above as lost to dlmalloc -- fences, `0xDA`/`0xDF` fills,
leak list, quarantine, file/line -- was ALREADY absent from every SDK build,
because the staged `libaxl.a` is `-DNDEBUG`. That instrumentation exists only
for in-tree development, where newlib's allocator is not in the picture. The
loss is real for AXL's own C++ work if it ever links `libc.a` in DEBUG; it is
zero for everyone downstream.

**The real gap this exposed is bigger than the bridge:** SDK consumers have no
instrumented allocator at all. Closing that is not an `#ifdef` -- it means
staging a DEBUG variant of `libaxl.a` and the glue objects and having
`axl-cc --debug` select it. Recorded in ROADMAP as its own item; it would make
the bridge meaningful as a side effect rather than on its own.

**Consequences for what already landed.** P1 (`c991ccfc`, `68caad05`) is
largely reversed by this: the `_r` bridge goes, the plain-name overrides go,
and AXL's `malloc_usable_size` goes (newlib's is correct for dlmalloc pointers
and AXL's would be wrong for them). `sbrk` stops being a fail-closed backstop
and becomes a real region allocator — the one piece of P1 that survives is the
knowledge of exactly which symbols newlib's allocator cluster defines, which is
what tells us we must now let ALL of them through rather than displace them.

P2 (`7cf04c31`, the porting layer and stdio) is unaffected: it sits below the
allocator question, and newlib's stdio allocates from whichever allocator owns
`_malloc_r` either way.

---

## 2a. "Why not just give newlib a heap?" — asked twice, answered here

> **SUPERSEDED by §2-DECISION above.** Kept because the mechanics are still
> correct and the reasoning explains what the new design must avoid: the
> corruption case below is exactly what a namespace split prevents, and it is
> the reason the plain names must move to newlib WHOLESALE rather than
> piecemeal.

The question in full: *newlib's malloc wants a linear buffer and an `sbrk`; why
not give it one, and keep `axl_malloc`/`axl_free` as the ones we use? That
would let us support things we currently cannot.*

**Yes to the premise.** Newlib's `_malloc_r` reaches dlmalloc, which grows a
single contiguous region through `_sbrk`. Today `_sbrk` returns -1, which is
why it never obtains a byte.

**No to the hybrid, and this is the sharp part.** "A heap for newlib, and
`axl_malloc` for us" is not two tidy halves — it is the specific corruption
case. AXL exports the PLAIN names (`malloc`/`free`), while 49 of newlib's
objects allocate through the REENTRANT ones (§4.2). So:

    strdup()  -> _malloc_r -> dlmalloc's heap        (newlib's bookkeeping)
    free(p)   -> AXL's free -> axl_free              (reads AXL's header)

`axl_free` steps back from the pointer to read a header dlmalloc never wrote.
That is a live corruption the moment `_sbrk` succeeds, which is exactly why
`src/cxxrt/axl-cxxrt-alloc.c` records the -1 as an INVARIANT rather than a
stub. Two heaps are safe today only because the second can never obtain
memory.

Beyond correctness, a real arena also costs what §2 already lists: the
firmware's memory map stops being accurate (one opaque block instead of
per-allocation `AllocatePool`), and everything newlib allocates leaves AXL's
tracker, taking the leak gate with it.

**But the motivation behind the question is right, and it names a real gap.**
What a genuine dlmalloc gives that the inversion does not is malloc
INTROSPECTION — newlib exports `malloc_usable_size`, `mallinfo`,
`malloc_trim`, `malloc_stats` (plus `_r` forms). AXL has **zero** equivalents
in `axl-mem.h`, measured.

That is not hypothetical. **`deps/quickjs` calls `malloc_usable_size`**, and
its fallback branch is:

    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size((void *)ptr);

QuickJS is vendored and not yet built, so this is a pending port that would
meet an undefined symbol under the inversion and silently lose its memory
accounting if patched to `return 0`.

**The fix is a shim, not a heap.** `axl_malloc` already stores `hdr->size` so
that `realloc` works without an `old_size` argument, so `malloc_usable_size`
is about three lines over bookkeeping that exists. `mallinfo`/`malloc_stats`
map onto AXL's existing live-allocation tracking, which is strictly richer
than dlmalloc's. `malloc_trim` is a legitimate no-op: AXL returns memory to
the firmware at `free`, so there is never a retained arena to trim.

**Consequence: P1 owns the introspection surface too** (§4d), not just the
allocation entry points. A port that links but silently accounts nothing is
worse than one that fails to link.

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

**4.1 Does newlib's `printf` reintroduce the Log -> Data cycle? ANSWERED
2026-08-13: YES. `AxlFormat` stays, permanently.** `AxlFormat` is
zero-dependency BY DESIGN: `AxlLog` cannot call `axl_malloc`, and that is what
breaks the circular dependency between the two modules.

Measured against the installed `x86_64-elf-gcc-14.3.0` newlib, by linking each
formatter with `-nostdlib … -lc` over libgloss stubs and reading the map:

| formatter | text + data | libc.a members | allocator pulled in? |
|---|---|---|---|
| **`AxlFormat` (today)** | **6,708 + 0 B** | — | no |
| newlib `vsniprintf` (INTEGER-ONLY) | 18,937 + 2,568 B | 25 | **yes** — `mallocr`, `freer`, `reallocr` |
| newlib `vsnprintf` | 50,773 + 3,008 B | 47 | yes, plus `dtoa` / `_Balloc` / `mprec` |
| newlib `vsnprintf`, `%f` used | 51,725 + 3,008 B | 48 | yes |

The integer-only entry point is the fair comparison — `AxlLog` formats no
floats — and it still arrives with the allocator, the whole FILE machinery
(`findfp`, `fwalk`, `fflush`, `makebuf`, `wsetup`) and the `_impure_ptr` reent
struct. Newlib's `vsnprintf` is not a string function that happens to be in
stdio; it is stdio, writing to a fake `FILE`.

So `AxlLog` -> `vsniprintf` -> `_malloc_r` -> AXL's allocator -> logs on
failure closes precisely the cycle this module exists to prevent, at 3.2x the
size for the integer path and 8x for the general one. Under the §2 inversion
the allocator would be AXL's own, which makes the recursion more certain, not
less.

**This bounds the substrate rather than blocking it.** Newlib is still the
right answer for `string`, `math` and the parts of `stdlib` that pass §3's two
gates. It is the wrong answer for formatting beneath `AxlLog`, and that is now
a measurement rather than a preference. `AxlFormat` is the second entry in
§3's "AXL implements a libc function where the AXL version is better for this
platform" list, after the allocator.

Reproduce: `x86_64-elf-gcc -ffreestanding -nostdlib probe.o stubs.o -lc
-Wl,-Map=map.txt`, then read `libc.a(...)` members out of the map.

**4.1b IMPLEMENTED 2026-08-13 — C compiles bare-metal on both arches, and
`include/compat/` is deleted.** What landed:

- **Both arches, not just x64.** aa64 moved off `aarch64-linux-gnu-gcc` too:
  that cross targets glibc, so its headers are a hosted libc's. The C compiler
  is now `AXL_{X64,AA64}_GCC_DEFAULT` from `scripts/axl-toolchains.conf`, with
  `AXL_{X64,AA64}_GCC` overrides, exactly mirroring the existing `_GXX` pattern.
  Binutils are unchanged (`$(CROSS)ld/ar/objcopy`) — they consume objects, not
  headers.
- **No silent fallback.** A missing cross is an error naming the installer, in
  the Makefile (gated on `NONCLEAN_GOALS`, so `clean`/`help`/lint gates still
  work without one) and in `axl-cc`. Falling back to host gcc would defeat the
  goal AND re-create the "a suite run silently measured the host toolchain" bug
  the build-state signature exists to catch.

  **Superseded for `axl-cc`'s CONSUMER C path by `AXL_TOOLCHAIN=auto`
  (2026-09-03).** A genuinely absent bare-metal cross now falls back to the
  host's own freestanding `gcc` there, rather than erroring — reported by
  `axl-cc --verbose` / `axl toolchain list`, so "silent" means quiet on a
  successful build, not unreported. See
  `AXL-Host-Toolchain-Design.md`. This bullet's invariant holds UNCHANGED for
  the SDK's own build (the Makefile still hard-errors with no fallback) and
  for an EXPLICIT `AXL_TOOLCHAIN=axl` on a consumer build, which still
  hard-fails rather than falling back — the fallback exists only under the
  `auto` default, and only for x64.
- **All four consumer entry points** moved with it: `axl-cc`'s C path, the
  generated CMake package (`AXL_C_COMPILER`, replacing `${AXL_CROSS}gcc`), the
  pkg-config `Cflags`, and `install.sh`'s staging step.
- **The regression test changed shape, not subject.**
  `test-axl-cc-hosted-headers.sh` used to assert the shims were staged and on
  `-isystem`. It now asserts what those were only ever a means to: a consumer's
  `<string.h>` resolves INSIDE the bare-metal toolchain, and NOTHING resolves
  under `/usr/include`. On x64 that is the assertion with teeth — the compile
  succeeds either way, and only the resolved path distinguishes a hermetic
  build from a silent host-glibc borrow.

Gates: `verify.sh` ALL GREEN both arches (10393); `check-flag-parity` clean
across all three build paths; `check-examples` 54/54; hosted-headers 9/9;
cxx-hosted 120/0; cxx-streams 78/0.

**The one open cost, and it is a distribution problem rather than a code one:**
x64's toolchain has no prebuilt tarball anywhere, so every machine that builds
AXL — contributor, CI runner, and now every C CONSUMER — needs
`toolchain/x86_64-elf/build-toolchain.sh`, ~40 minutes. aa64 is unaffected (ARM
publishes one, and C++ consumers already required it). Until that is solved, CI
cannot build this tree. The obvious answer is to build it once and publish it
as a release artifact next to the SDK packages, which turns 40 minutes into a
download and costs one workflow — see §4.1c.

### 4.1d DIRECTION 2026-08-13 — hermetic: no system compiler, headers or libraries

Stated by Mike, and broader than what §4.1b achieved: **nothing may come from
the host.** Not headers, not libraries, and not compilers — every input is
either one of AXL's own toolchains or axl-sdk itself. Removing the `--hosted`
flag is part of the same goal (`AXL-Cxx-Design.md` §6a-PLAN T3).

This SUPERSEDES the carve-out recorded above, which reads "Binutils are
unchanged (`$(CROSS)ld/ar/objcopy`) — they consume objects, not headers." True,
and no longer sufficient: consuming objects rather than headers makes host
binutils *safe*, not *ours*.

Inventory, measured 2026-08-13 — what is still the host's:

| input | today | already available to us |
|---|---|---|
| C compiler, both arches | bare-metal cross | — done (§4.1b) |
| C++ compiler, aa64 | ARM bare-metal | — done |
| ~~C++ compiler, x64~~ | **DONE 2026-08-13** — our `x86_64-elf-g++`, T2. It was the last host COMPILER | — |
| ~~`ld`/`ar`/`objcopy`, x64~~ | **DONE 2026-08-13** — our `x86_64-elf-*`, via the `-axl` rebuild | — |
| ~~`ld`/`ar`/`objcopy`, aa64~~ | **DONE 2026-08-13** — ARM's `aarch64-none-elf-*` | — |
| libc headers | our toolchain's newlib | — done (§4.1b) |
| ~~libstdc++ headers, lint's C++ pass~~ | **DONE** — `scripts/lint.sh` reads the CROSS libstdc++; the comment at that call site records that it used to read the host's | — |
| ~~`--hosted` / `CXXFLAGS_HOSTED*`~~ | **DONE** — T3 deleted them; `--hosted` survives only as a rejection message in `axl-cc` | — |

**The inventory is empty: the goal is REACHED.** Every input is one of AXL's
own toolchains or axl-sdk itself. What remains host-side is not an input to a
UEFI image: `curl` and `xz-utils` only FETCH the toolchains, and `pe-set-debug`
runs on the build machine rather than in the produced binary.

Verified 2026-08-15 by reading the code rather than the table, after both rows
above were found already satisfied while the table still listed them as
outstanding — which is the failure mode an inventory has, and the reason this
line records how it was checked.

Two notes on sequencing, because the cheap-looking entries are not the cheap
ones:

- **The x64 C++ compiler was not a one-line default flip — but not for the
  reason recorded here.** This entry predicted the blocker would be
  `libaxl-cxx.a` colliding with `libstdc++.a`. Measured against `axl-cc`'s
  actual link, it does not: `libaxl-cxx.a` is named first and archive selection
  is lazy, so the colliding members are never pulled. The real blocker was that
  GCC's `x86_64-*-elf` target emits global constructors into `.ctors`, which
  AXL's crt0 does not walk — so every one of them silently did not run.
  `AXL-Cxx-Design.md` §6a-T2 carries the measurement; the fix was
  `--enable-initfini-array` in our own toolchain, published as `14.3.0-axl2`.
  (Kept rather than deleted because the flip was attempted, reverted, and then
  diagnosed — and the wrong diagnosis is the part that cost time.)
- **Binutils: aa64 DONE 2026-08-13, x64 BLOCKED — and the blocker is ours to
  clear.** An earlier revision of this line called binutils "the only entry
  with no known blocker". That was wrong, and the check that disproves it is
  one command:

      x86_64-elf-objcopy  --info | grep pei   # nothing
      aarch64-none-elf-objcopy --info | grep pei   # pei-aarch64-little

  Our x64 binutils was configured `--target=x86_64-elf` with no
  `--enable-targets`, so it carries only `elf64-x86-64` and `elf32-i386` — and
  the step that turns the `.so` into a `.efi` is
  `objcopy --output-target=pei-x86-64`, which it cannot do. ARM's toolchain
  ships `pei-aarch64-little`, which is why aa64 could move today and x64 could
  not.

  **CLEARED 2026-08-13.** `build-toolchain.sh` gained
  `--enable-targets=x86_64-pep`, the toolchain was rebuilt and published as
  `toolchain-x86_64-elf-14.3.0-axl`, and x64's `CROSS` now resolves from the
  manifest exactly as aa64's does. The `-axl` suffix marks AximCode's BUILD of
  upstream 14.3.0 rather than a new upstream release (the distinction ARM draws
  with `14.3.rel1`); `build-toolchain.sh` carries it as `AXL_REV` and
  `check-toolchain-conf` compares the manifest against `GCC_VER + AXL_REV`,
  because the upstream version and our build revision are different facts.

  **The package now depends on NO part of the distro's toolchain** -- no
  compiler, no assembler, no linker. `binutils` went first; `g++` was the last
  one and went with T2, when x64 C++ moved to our own `x86_64-elf-g++`. Proven
  by `test/integration/test-host-deps-minimal.sh`: only the declared deps
  installed, on an image with no toolchain, building x64 C, x64 C++, aa64 C and
  aa64 C++ to correct PE machine words.

  What the packages DO still declare is `curl` and `xz-utils`, and that is a
  different category rather than a leftover: the package ships
  `bin/axl-install-toolchain`, `axl-cc` names it as the remedy in every
  missing-toolchain diagnostic, and it fetches and unpacks a tarball. Neither
  tool is on `debian:stable-slim`, so without them a user follows the advice
  the package gave them and gets `curl: command not found`. The deps test
  asserts both are reachable -- it MOUNTS the toolchains rather than installing
  them, so its four builds cannot see this and did not.

  aa64 moved via `AXL_AA64_BINUTILS_PREFIX_DEFAULT`, read by both the Makefile's
  `CROSS` and the `AXL_CROSS` baked into the generated CMake package. Verified
  end to end: the `.efi` images are converted by `aarch64-none-elf-objcopy`, and
  the unit suite is 10355/0 (10393 with skips).

Open scope question: `scripts/pe-set-debug.c` is compiled with host `gcc` and
runs ON the build machine — it post-processes the PE rather than entering the
image. That is a different category from the rows above, and whether "no system
compilers" reaches build-side tooling is a decision, not an oversight.

**4.1c DONE 2026-08-13 — the toolchain is published.**
`toolchain-x86_64-elf-14.3.0` on `aximcode/axl-sdk-releases` carries the
stripped tarball (**55 MB**, vs ~40 minutes of compiling), the three upstream
source archives, `SHA256SUMS` and a `TOOLCHAIN-SOURCES.md`. URL + SHA256 live
in `axl-toolchains.conf` beside the version, so `install-toolchain.sh x64` is
now download-and-verify with the source build as fallback — the same shape the
aa64 path always had. If it moves to its own repo later, that URL is the one
line that changes.

Sizes, measured: 1.5 GB installed as built → **235 MB stripped** → 55 MB
`.tar.xz`. `cc1` alone was 326 MB of debug info. The builder strips now, so a
local install gets the same reduction; verified that a stripped toolchain still
builds all 43 test images and runs `AxlTestData` 2078/0 with no leaks. ARM
ships theirs stripped too — their tree drops only 6.6% under the same pass,
where ours dropped 84%.

Licensing: GCC and binutils are GPL-3.0-or-later and we distribute binaries, so
the corresponding source is attached to the SAME release (GPLv3 §6(d)), the
build recipe is in git (`toolchain/x86_64-elf/build-toolchain.sh`, unmodified
upstream sources, no patches), and the release notes carry a three-year written
offer. The GCC Runtime Library Exception is what keeps consumers' compiled
output unaffected.

**4.1c ORIGINAL PLAN (for the record):** We have
the builder, a releases repo, and a release pipeline that already attaches
per-arch tarballs. Building it in a workflow on a tag and attaching
`axl-toolchain-x86_64-elf-<ver>.tar.zst` would make `install-toolchain.sh x64`
a download-and-verify, identical in shape to the aa64 path it already has
(URL + SHA256 in `axl-toolchains.conf`). Open questions: where it is hosted
(`axl-sdk-releases` alongside the SDK, or its own repo), whether it is rebuilt
per release or pinned and rebuilt only on a version bump (pinned — it changes
about once a year), and the licence/redistribution note GCC requires.

---

**4.1b SPIKE, 2026-08-13: what the x64 toolchain flip actually costs.** The
ROADMAP's blocker for this whole track is that "nothing is wired into the build
yet" — x64 still compiles C with the host's glibc-targeted gcc. Measured by
building the tree with `CC=x86_64-elf-gcc` into throwaway prefixes:

| configuration | compile | link | run |
|---|---|---|---|
| bare-metal gcc, `include/compat` KEPT | 271/271 objects, **0 errors** | ok | — |
| bare-metal gcc, **compat REMOVED** | 271/271, **0 errors** | 37 of 40 EFIs; 3 fail | `AxlTestLog.efi` **67/67, no leaks** |
| the same + `AXL_TLS=1` (mbedtls, libvterm, lzma all on newlib headers) | 321 objects, **1 error** | — | — |

**The whole cost of retiring `include/compat/` for C is two fixes:**

- `__assert_func` — 15 undefined references, all from `deps/sdefl` reaching
  newlib's real `<assert.h>`. Compat's fake `assert.h` is what hid it. Supply
  it (routing to AXL's own assert path) or compile the third-party TUs
  `-DNDEBUG`.
- `time()` — `src/net/axl-mbedtls-platform.c:99` defines `time(long long *)` as
  a stand-in for the C library's. Newlib declares the real one, and the
  signatures disagree (`time_t` vs `long long`). Reconcile ours with newlib's.

**And a structural finding that makes the rest cheap: AXL's own code never
needed `include/compat/` at all.** Across `src/` and `include/`, the only
standard headers included are `stddef.h` (204), `stdint.h` (179),
`stdbool.h` (133) and `stdarg.h` (17) — the freestanding four — plus two
`limits.h` and two `immintrin.h`. **Zero** references to any of the seven
headers compat fakes. Compat exists purely for THIRD-PARTY code (sdefl, lzma,
libvterm, mbedtls), and once newlib is present that code gets the genuine
headers — verified from the dependency files, e.g. `LzmaDec.o` resolving
`<string.h>` to `/opt/x86_64-elf-gcc-14.3.0/x86_64-elf/include/string.h`.

So §5's "retiring it for C++ is a step toward deleting it outright"
understates the position: it can go outright, for C too, behind those two
fixes. What the spike did NOT cover: aa64 (ARM's toolchain, same shape but
unmeasured), the C++ hosted path, and whether anything at runtime depends on
compat's `FILE` typedef.

Trap for whoever repeats this: overriding `CFLAGS` on the command line drops
the TLS block's appends, and `-DMBEDTLS_CONFIG_FILE='<axl-mbedtls-config.h>'`
must keep its inner quotes or `/bin/sh` reads `<...>` as a redirect and every
object fails with `axl-mbedtls-config.h: No such file or directory`. That cost
one whole spike run that reported "0 errors" having compiled nothing.

**4.2 The `_r` family.** Newlib's reentrant internals call `_malloc_r`,
`_free_r`, `_realloc_r`, `_calloc_r` rather than the plain names — roughly 45
objects (stdio, `mprec`'s float formatting, `strdup_r`). Those must be bridged
onto the same allocator, or two heaps coexist and a pointer crossing between
them corrupts. Today this is contained by a different mechanism: `_sbrk`
returns -1 so newlib's heap can never obtain a byte (see the invariant recorded
in `src/cxxrt/axl-cxxrt-alloc.c`). Under this design that invariant becomes
obsolete and the `_r` bridge replaces it — the two are alternatives, and
shipping neither is the corruption case.

**4.2-RESULT 2026-08-17 — "roughly 45" is 49, and stdio's allocation path is
in it.** Measured over newlib's 625 objects: `_malloc_r` 27, `_free_r` 32,
`_calloc_r` 2, `_realloc_r` 8; **49 distinct objects** touch at least one.
Two of them decide whether stdio can work at all — `findfp.o` (the `FILE`
table) and `fvwrite.o` (the write path) both allocate through `_malloc_r`.

That sharpens the sequencing: **implementing the porting layer does not give
you stdio.** `write`/`read`/`close`/`lseek`/`fstat` in
`src/cxxrt/axl-cxxrt-stubs.c` all return -1, and that is exactly the set
newlib's stdio asks the platform for — but with the `_r` family unbridged,
stdio fails earlier, at buffer allocation, and never reaches I/O. The `_r`
bridge is a *prerequisite* for stdio, not a companion to it.

**4.3-RESULT 2026-08-17 — the gate exists, and it currently guards the
opposite rule.** `make check-libc-overlap` (`fa54572b`) enforces the
libaxl/libc symbol overlap, having been written for the *pre-inversion*
world: AXL's 11 leaf definitions are weak so both archives can coexist, and
`__cxa_atexit` / `__stack_chk_fail` / `__stack_chk_guard` must stay strong
because newlib's are inert under UEFI. It also proved the hazard §4.3
predicted is real rather than theoretical — adding one object to the
`-fexceptions` link changed which archive was scanned first and produced five
`multiple definition` errors on `throw 42;`.

Under this design that gate INVERTS: the rule becomes "libaxl defines no
unprefixed leaf libc name at all", with the must-win set unchanged. The weak
definitions are an interim measure, not the destination.

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

## 4c. MEASURED 2026-08-17 — the phases are costed, and the C half is free

Three things §4 left as risk are now numbers.

**Putting `libc.a` on the C link line is safe.** The fear was that a C image,
which today links `libaxl.a` and *nothing else*, would drag `_impure_ptr` and
stdio in through the `getenv -> impure -> findfp` chain that bit the C++ side.
Linked by hand against six real images, stand-ins deleted and `libc.a` added:

| image | baseline | with `libc.a` | newlib members pulled |
|---|---|---|---|
| hello | 641,568 | 641,568 | none |
| gfx-demo | 956,640 | 956,640 | `memset` |
| gfx-window | 987,648 | 987,648 | `memset` |
| frame-anim-demo | 1,086,536 | 1,086,536 | `memset` |
| tar | 1,082,880 | 1,081,504 | `memcpy memmove memset` |
| fwtool | 851,160 | 849,592 | `memcpy memmove memset` |

Nothing grew, nothing pulled `findfp`/`impure`/`stdio`, and only three members
ever arrive. The eight string functions in `axl-str-compat.c` are never
referenced at all — AXL's own code calls `axl_strlen` and friends, and the only
consumers of the bare names are vendored (libvterm, lzma) plus three bulk-buffer
AXL files. **`axl-str-compat.c` + `axl-intrinsics.c`, 195 lines, are deletable
with no image cost.**

**Retiring `libaxl-cxx.a`'s substitutes costs 126 KB of `.text`, and that cost
is ACCEPTED (Mike, 2026-08-17).** `containers.cpp`, `-fno-exceptions`, linking
libstdc++ instead of `libaxl-cxx.a`:

| | `libaxl-cxx.a` | libstdc++ | delta |
|---|---|---|---|
| `.text` | 33,712 | 160,112 | **+126,400 (+375%)** |
| `.so` | 966,376 | 1,252,864 | +286,488 |
| `_Unwind_*` symbols | 0 | 34 | |
| libstdc++ members | 0 | 81 | |

`functexcept.o` is compiled *with* exceptions, so it drags the unwinder into an
image that opted out — the cascade `axl-cxxabi-ops.cpp` was written to prevent.
The decision is to pay it: the growth lands only on `-fno-exceptions` C++
images (C images are untouched, and `-fexceptions` images already link
libstdc++ today), and it buys real stdio, **iostreams**, and a route to locale.
That releases `axl-cxxabi-ops.cpp` + `axl-cxx-list.cpp`, **765 lines** — the
largest single block, and far bigger than the 195 the newlib half yields.

**The inventory, so the total is honest.** Of ~1,506 lines of C/C++ runtime
support: 195 go with the newlib half, 765 with the libstdc++ half, and the
remainder does not move — `axl-cxxrt-stubs.c` (224) is the porting layer newlib
defines **none** of (libnosys supplies 10 of 18 and carries 21
`.gnu.warning` sections, which is why it was rejected), `axl-cxxrt-alloc.c`
(154) becomes the `_r` bridge rather than disappearing, `axl-cxxabi.c`'s
`__cxa_atexit` is lifecycle-critical, and `axl-stack-guard.c` (78) stays on
evidence: newlib's `__stack_chk_init` seeds a **fixed** value with `movl` — four
bytes into an eight-byte guard, so the canary is effectively
`0x00000000ff0a0000` against AXL's `0x00a5b7c3d1e9f200`, and it must run before
any protected frame exists. Adopting newlib's would trade a strictly weaker
canary for the only file on the list that is a security control.

### 4c.1 The P2 shape — newlib's stdio sits ON AxlStream, not the reverse

**The porting layer is ours in every design.** Newlib defines **0 of the 18**
symbols in `src/cxxrt/axl-cxxrt-stubs.c`. It has no idea what
`EFI_FILE_PROTOCOL` is, so somebody must talk to UEFI and that somebody is
AXL. "Use newlib for `open`/`read`/`write`" is not an available option; what
newlib supplies is the layer ABOVE — `FILE`, buffering, `printf`, `fopen` —
built on the six calls we hand it.

    third-party C:  printf  fopen/fread/fwrite  scanf
                       │         │                 │
                       ▼         ▼                 ▼
    newlib:         ┌─────────────────────────────────┐
                    │  stdio: FILE, buffering, printf │   ◄── what we GAIN.
                    │  fopen IS supported.            │       Built on the row
                    └─────────────────────────────────┘       below, nothing else
                                     │ needs
    AXL:            axl_file_get_contents ──┐        │
                    axl_printf ─► AxlFormat │        │   (both stay DIRECT --
                                   (§4.1)   │        │    they do not route
                                            │        │    through FILE)
    ----------------------------------------┼────────┼----------------------
    AXL, always:    open / read / write / close / lseek / fstat
                    == the AxlStreamBackend vtable, which already
                       has exactly this shape
    ----------------------------------------------------------------------
    platform:       EFI_FILE_PROTOCOL / console

**Read the two AXL arrows as independent.** `fopen` working and
`axl_file_get_contents` not using it are separate facts, and an earlier draft
of this section ran them together badly enough to read as "FILE is not
supported". It is. Third-party C gets the whole of stdio; AXL's own APIs
simply do not take the long way round to reach a file they can already open.

**The fd table is an `AxlStream *` array.** `AxlStreamBackend` already
declares `read`, `write`, `pread`, `pwrite`, `seek` and `close` with the
signatures newlib's porting layer wants, and file and console backends already
exist behind it. So `open()` mints a stream and returns its index; `write(fd)`
dispatches through the vtable. This is a few dozen lines, not a subsystem —
an earlier draft of this section called it "a new descriptor table" and
overestimated it.

**`FILE` and `fopen` ARE supported, built on `AxlStream`.** Once `open` exists,
newlib's own FILE machinery does the rest: `fopen` -> `_open_r` -> `open`, then
`__sread`/`__swrite`/`__sseek`/`__sclose` drive the same six entry points. So
`fopen`, `fread`, `fwrite`, `fseek`, `fclose` and (under P4) `<fstream>` all
work, and third-party C that opens files needs no porting at all.

**What stays direct is AXL's OWN file APIs, which is a different claim.**
`axl_file_get_contents` keeps reaching EFI in one hop rather than becoming a
`fopen` wrapper -- through stdio it would be `FILE` -> fd -> `AxlStream` ->
EFI, inserting newlib's buffering under code that already works and dragging
the FILE machinery into images that merely touch a file. The dependency runs
DOWN from newlib into AxlStream, never back up. This is about our internals
and removes nothing from consumers.

**`AxlFormat` is out of scope, permanently.** This is the one place the "AXL
sits on newlib" rule is measured-wrong (§4.1): newlib's `vsnprintf` IS stdio,
so `AxlLog` calling it reinstates the Log->Data cycle at 3.2x-8x the size.
`axl_printf` keeps using `AxlFormat`; newlib's `printf` is a SEPARATE,
parallel path that third-party code uses. **Two independent formatters, on
purpose** — that is a design property to preserve, not duplication to clean up
later.

**What this buys.** Not `printf` for us — AXL has `axl_printf`, `AxlStream`
and `AxlFs` already. It is that third-party C calling `printf`/`fopen`/
`strdup` compiles and runs unmodified, which is what makes porting existing
libraries cheap. The console half (`printf`, `puts`, `scanf`, `%f`, line
buffering via `fstat`/`isatty`) needs only the six existing stubs implemented.
The file half (`fopen`/`fread`/`fwrite`, and `<fstream>` under P4) additionally
needs `open`, which AXL does not define today at all — along with `unlink`,
`stat` and `rename` for `remove`/`rename`/`stat`. That is the only reason the
two halves are sequenced separately; both are in scope for P2, and shipping
console-only would leave a libc whose `fopen` returns NULL, which is the kind
of half-thing people trip over for years.

## 4d. Phasing — ALL FIVE PHASES DONE 2026-08-17

Five phases. Each is independently shippable and must leave `verify.sh` green
on both arches; none is a prerequisite for the *next* one only because it is
listed first, so the "why here" column is the real content.

| # | What lands | Why in this position | Verified by |
|---|---|---|---|
| **P1'** ✅ | **TWO allocators, split by namespace** (§2-DECISION). newlib keeps its dlmalloc and gets a real heap from an AXL `sbrk` over `axl_alloc_pages`; `axl_malloc` stays separate over `AllocatePool`. Replaced the original P1, which had AXL taking the standard names. — plain `malloc`/`free`/`realloc`/`calloc`/`memalign` AND the reentrant `_malloc_r`/`_free_r`/`_calloc_r`/`_realloc_r` — exported from `libaxl.a` itself, not from an object on one link path. **Plus the introspection surface** (§2a): `malloc_usable_size` over the existing `hdr->size`, `mallinfo`/`malloc_stats` over AXL's live-allocation tracking, `malloc_trim` a documented no-op. `sbrk` keeps returning -1, and its comment changes from INVARIANT to backstop in the same commit. | §2's inversion and §4.2's `_r` bridge are ONE act, not two: both point every allocation at `axl_malloc`, and doing only the plain half is the mixed-allocator corruption case. Everything else depends on this. | `strdup()` round-trips through `free()`; the leak gate still SEES the allocation — that is the proof there is one heap and not two. `malloc_usable_size` returns the real capacity, which is what `deps/quickjs` needs to account for memory at all. |
| **P2** ✅ | **The porting layer over `AxlStream`** (§4c.1): implement `write`/`read`/`close`/`lseek`/`fstat`/`isatty`, add `open`/`unlink`/`stat`/`rename`, fd table = `AxlStream *` array. | Needs P1: stdio allocates its `FILE` table through `_malloc_r`, so without it stdio fails at buffer allocation before it ever reaches I/O. | `printf`/`puts` reach the console under QEMU, both arches; a `FILE*` opened on the ESP reads back what it wrote; `%f` formats. |
| **P3** ✅ | **One provider of libc names**: `libc.a` on every link, delete `axl-str-compat.c` + `axl-intrinsics.c` (195 lines), `axl-stack-guard.c` takes over `__stack_chk_fail_local` + `__stack_chk_init`, `check-libc-overlap` inverts. | Independent of P1/P2 **for AXL's own code** — measured, 0 of libaxl.a's 264 members reference plain `malloc`. But it must NOT ship before P1: the moment third-party C arrives it calls `malloc`, and with `libc.a` on the line and no inversion that reaches newlib's dlmalloc, then `sbrk`, then NULL. | The six-image spike of §4c, promoted from a hand-link to a gate; existing suites unchanged. |
| **P4** ✅ | **Retire `libaxl-cxx.a` ENTIRELY**: all 7 `.cpp` (1,696 lines), not the 2 originally scoped; every C++ link carries libstdc++, and there is ONE C++ link shape. | Needs P2 — `std::cout` needs `write`, `<fstream>` needs `open`. Buying iostreams without them would ship a stream that cannot write. | `test-cxx-iostreams-qemu.sh`: `<iostream>`/`<sstream>`/`<fstream>` compile, link and RUN on both arches, including an ESP round-trip. The size budget is a per-arch `.text` ceiling in that suite. |
| **P5** ✅ | **Locale — MEASURED 2026-08-17, and there is nothing to do.** | It was scheduled last so P4's link shape would not invalidate the measurement. Measuring early instead showed the premise was already false. | All four headers compile, link and RUN today: `<fstream>` round-trips a file on the ESP, `<sstream>` and `<format>` print, `<regex>` matches. What they needed was a C library, which P2/P3 delivered — not glibc's locale. The wall is SIZE: ~1 MB each, ~9x the `-fexceptions` baseline of 119,691 bytes. `AXL-Cxx-Stdlib-Surface.md`'s Tier 3 is retracted. |

### P1-RESULT 2026-08-17 — landed, then SUPERSEDED the same day

`c991ccfc` (the `_r` family) and `68caad05` (`malloc_usable_size`).
`test-libc-alloc-qemu.sh`, 11 assertions per arch, 22/22.

**Then superseded by §2-DECISION**, which gives newlib its own heap instead.
Recorded rather than reverted away, because two findings from it outlive the
design: the exact membership of newlib's allocator cluster (which symbols each
member defines, and therefore what a namespace split must hand over WHOLESALE),
and the measurement that newlib's `malloc_usable_size` returns 56 for a 64-byte
AXL block — the reason a half-split allocator is silently wrong rather than
loudly broken.

`strdup()` returned NULL before this and works now. dlmalloc is absent
from the linked image, asserted structurally rather than inferred:
`__malloc_av_` and its siblings are defined only by `libc_a-mallocr.o`,
so their absence is what proves there is one allocator and not two.

**Two refinements to what this row promised.**

*"Exported from `libaxl.a` itself, not from an object" moved to P3.* The
plain and `_r` names still live in `src/cxxrt/axl-cxxrt-alloc.o`, and that
is correct for now: an object DISPLACES an archive member outright, which
is the stronger mechanism, and the move only becomes necessary when C
links carry `libc.a` — which is P3's job. Doing it earlier would trade a
guarantee for an ordering question. `malloc_usable_size` is the exception
and sits in `libaxl.a`, because only `axl-mem.c` knows the header layout.

*The rest of the introspection surface is deliberately not done.*
`mallinfo`, `malloc_stats`, `malloc_trim` and `mallopt` are still
newlib's. That is safe in a way `malloc_usable_size` was not: referencing
one pulls `mallinfor.o`, which needs `__malloc_update_mallinfo`, which
pulls `mallocr.o`, which multiply-defines `_malloc_r` against AXL's — a
LOUD link error, not a silent wrong answer. `malloc_usable_size` had to
land now precisely because its failure mode was silent (it returned 56
for a 64-byte block). Add the others when a consumer names one.

> **THAT SAFETY ARGUMENT EXPIRED WITH §2-DECISION, and `mallinfo` is now
> silently WRONG.** The loud link error depended on AXL defining `_malloc_r`.
> It does not any more — newlib owns the whole allocator — so `mallinfo` links
> cleanly and returns garbage. Measured 2026-08-17 after the sbrk fix, so this
> is not the descending-break bug:
>
>     before-any-malloc: arena=0 uordblks=0 fordblks=46645422825533952
>     after-64B:         arena=4294971392 uordblks=0
>
> **Root-caused: 32-bit fields read as 64-bit.** `arena = 4294971392` is
> `0x1_00001000` — the real `arena` (`0x1000` = 4096) and the next field (`1`)
> read as ONE `size_t`. And `fordblks = 0x00A5B7C3D1E9F200` is AXL's
> `__stack_chk_guard` byte-for-byte, i.e. uninitialised stack: the tail fields
> are never written. The toolchain's `<malloc.h>` declares `struct mallinfo`
> with `size_t` members above a comment reading *"This version of struct
> mallinfo must match the one in libc/stdlib/mallocr.c"* — and this newlib
> build's `mallocr.c` does not match it.
>
> **FIXED 2026-08-17 — AXL owns the member where the toolchain is wrong.**
> `mallinfo` now reports real numbers: for a 512 KiB request, `arena=528384`,
> `uordblks=524304`, `fordblks=4080`, and `uordblks + fordblks == arena`
> exactly.
>
> *All FIVE symbols, because a member is all-or-nothing.* `libc_a-mstats.o`
> defines `mallinfo`, `malloc_stats`, `mallopt`, `mstats` and `_mstats_r`.
> Defining only `mallinfo` would leave a consumer who calls `malloc_stats`
> pulling that member and multiply-defining against ours — owning MOST of a
> member is the bug, the same trap P3 hit with `stack_protector.o`. The data
> comes from symbols OUTSIDE it (`__malloc_update_mallinfo` in `mallinfor.o`,
> `__malloc_current_mallinfo` in `mallocr.o`, both global), which is what makes
> the displacement work at all. `mstats` improved on the way: newlib's printed
> through a `stderr` firmware never wires up, so it emitted nothing.
>
> *GATED ON A BUILD-TIME PROBE, and that is the correctness story across
> arches.* **ARM's newlib is CORRECT** — its `__malloc_current_mallinfo` is 80
> bytes, ten `size_t`. Only the pinned x86_64-elf build is 40. So this is a
> property of the TOOLCHAIN BUILD, not of the arch: the Makefile reads the
> symbol's size out of `libc.a` and defines `AXL_NEWLIB_MALLINFO_INT` only when
> it is 40, and AXL displaces nothing where newlib is already right. Hard-coding
> the int layout was wrong on aa64 and the aa64 suite caught it. The block
> disappears the moment the x64 toolchain is rebuilt with matching types.
>
> *The regression test had to be found by measuring, not reasoning.* A first
> version asserted `arena > 0`, `uordblks >= requested`, `arena >= used + free`
> and `fordblks != canary` — and **all four passed against the broken build**,
> because 32-bit fields read as 64-bit produce large numbers that satisfy every
> inequality. The bound that discriminates is `arena < 2^32`: this heap cannot
> exceed the largest free run (~423 MiB), so a 4 GiB arena is field-width
> corruption and nothing else.
>
> **`malloc_usable_size` was separately re-checked and is CORRECT** (72 for a
> 64-byte request — newlib answering about its own block).

### P1'-RESULT 2026-08-17 — two allocators, and a status code read as a bool

The namespace split landed and works: `strdup`, `malloc`/`free`,
`realloc(16 -> 4096)` preserving contents through dlmalloc's grow, and
`axl_malloc` allocating independently alongside. 28/28 both arches. The
symbol overlap between `libaxl.a`+glue and `libc.a` fell from 24 to 14 on
x64, which is the split showing up as a measurement rather than a claim.

`sbrk` hands out 1 MiB chunks from `axl_alloc_pages`. Growth is chunked and
NOT contiguous -- UEFI offers no way to reserve address space for later
extension -- so dlmalloc starts a new segment when a chunk runs out. Negative
increments trim within the current chunk only; pages are never handed back,
because dlmalloc still believes it owns them.

> **RETRACTED 2026-08-17 -- both halves of that paragraph were false, and the
> heap was broken for anything over 1 MiB.** dlmalloc does NOT start a new
> segment: UEFI satisfies `AllocatePages` DOWNWARD, so each fresh chunk landed
> *below* the previous one, and a break that moves backwards is not
> non-contiguous, it is invalid -- newlib sizes its top as
> `brk + size - old_end`, which went negative and wrapped. Every allocation of
> 1 MiB or more returned NULL, and afterwards so did a 256 KiB one.
> The "multi-chunk assertion in test-libc-alloc-qemu.sh" cited as proof did
> not exist; the largest allocation any fixture made was
> `realloc(16 -> 4096)`. And UEFI *does* offer a way to place a region and
> extend it -- `AllocateAddress` -- which is what the fix uses. See
> **SBRK-RESULT** below.

**The bug worth remembering: `axl_alloc_pages` returns `AXL_OK`/`AXL_ERR`, not
a bool, and `AXL_OK` is 0.** So `if (!axl_alloc_pages(...))` read success as
failure, `sbrk` returned -1 to every request, and EVERY newlib allocation
returned NULL while `axl_malloc` carried on working perfectly. That symptom is
indistinguishable from "the two-allocator design does not work", and it was
caught only because the fixture asserts the two allocators SEPARATELY -- a
test that exercised newlib's alone would have read as a design failure rather
than an inverted condition.

### P2-RESULT 2026-08-17 — real stdio, and the design's sequencing held

`7cf04c31`. `test-libc-stdio-qemu.sh`, 14 assertions per arch, 28/28.
`printf`, `fputs`, and a full `fopen`/`fwrite`/`fclose`/`fopen`/`fread`
round-trip on the ESP, both arches.

Two predictions from §4c.1 were confirmed by the work rather than
assumed. The file half failed at **link** time (`undefined reference to
open`), not runtime -- so P2 genuinely had to ADD `open`/`unlink` rather
than only implement the six existing stubs. And the fd table really is an
`AxlStream *` array: the backend vtable already had the right shape, so
the bridge is a dispatch, not a subsystem.

`fstat` ended up IMPLEMENTED, which this phase had not planned. Including
`<fcntl.h>` brings `<sys/stat.h>`, which collided with the old
`void *st` signature -- and that signature existed *because* the layout
was out of scope, so the collision removed its own justification. newlib
now gets real `S_IFCHR`/`S_IFREG` and a live `st_size`, which is what
decides line- versus block-buffering instead of a fallback guess.

~~**Not done, and nothing has asked for them:** `stat()` and `rename()`.~~
**DONE 2026-08-17.** "Nothing has asked" was true of AXL and false of the
third-party C that P3 exists to serve, which got a link error naming a POSIX
function this platform otherwise claims to have. `stat()` fills `st_mode` and
`st_size` from `axl_file_info` — only those two, because FAT carries no owner,
link count or permission bits and inventing them would be worse than a zeroed
field. `rename()` is backed by `axl_file_move`, NOT `axl_file_rename`: the
latter refuses a cross-directory request (FAT renames within one directory)
while POSIX `rename()` is defined across directories on one filesystem, and
`axl_file_move` tries the atomic rename first and falls back to copy+delete.

### FD-LAYER COVERAGE 2026-08-17 — the descriptors get their own fixture

`test/integration/libc-fd-selftest.c`, 27 assertions, both arches.

Everything else in the suite reached `open`/`read`/`write`/`lseek`/`close`
only THROUGH `printf` and `fopen`. That cannot distinguish "the fd layer is
correct" from "stdio happens not to use it that way" — and the whole of
`<fstream>`, every `FILE *` and every ported C library sits on these thirteen
functions. What transitive coverage was missing: `lseek`'s three whences
including a NEGATIVE end-relative offset (stdio never does this, and it is the
arithmetic most likely to be wrong), `read` at EOF returning 0 rather than -1,
descriptor reuse after `close` (without it a long-running program dies after
32 opens however disciplined it is), the table-full path, and the error
returns stdio swallows into its own.

Everything passed first run, which is only worth anything because both
sabotages were caught: dropping the slot release in `close()` fails 2
assertions, and turning `SEEK_END` into `SEEK_SET` fails 4.

*A trap paid for again:* `sabotage.sh` restores the SOURCE, not the artifacts
STAGED from it. The run after the second sabotage showed two x64 failures that
were the sabotaged `lseek` still sitting in `stage/`, with the source already
correct — `make && install.sh` before believing a post-sabotage result.

### P3-RESULT 2026-08-17 — one provider, and two gates earned their keep

`9d4cd144` (libc.a on every link) and `6ec731d3` (the stand-ins deleted).
`axl-str-compat.c` and `axl-intrinsics.c` are gone -- 195 lines -- and the
symbol overlap between AXL and newlib falls **14 -> 5** on x64. The five
remaining are ABI hooks that must be AXL's, each with its reason in the gate.

The §4c spike held on the real path: `hello.c` is 47,247 bytes before and
after, and a trivial C image pulls ZERO stdio or malloc symbols. Consumers pay
only for what they call.

**AXL now owns all FOUR symbols of newlib's `stack_protector.o`,** having
owned two. A member is pulled for ANY symbol it defines, so one reference to
`__stack_chk_fail_local` or `__stack_chk_init` would have dragged newlib's
copy in and multiply-defined the other two. Latent before; live the moment
`libc.a` joined every link. **Owning most of a member is the bug.**

Two things gates caught that review would not have. clang: `AXL_NORETURN` on
the definitions but not the declarations, so a call to `__stack_chk_fail`
looked like it returned. And `make`: deleting a source leaves its `.o` in the
archive, because removing a prerequisite does not make a target out of date --
the recipe's `rm -f $@` never fires, `make` reports success, and `nm` still
shows the symbol. That contradicted a claim in CLAUDE.md, which is corrected;
it holds for a rename and not for a deletion.

**The one ordering that is a real trap:** P3 before P1. Everything else can be
resequenced on judgement; that pair cannot, and the failure mode is silent
(third-party `malloc` returning NULL rather than a link error).

**What no phase removes**, each on evidence and each already argued above:
`axl-cxxrt-stubs.c` (P2 promotes it to the bridge — newlib defines 0 of its
18), `axl-stack-guard.c` (newlib's canary is 4 bytes of a fixed value into an
8-byte guard), `axl-cxxabi.c`'s `__cxa_atexit` (newlib's table is drained by
`exit()`, which nothing calls under UEFI), and `AxlFormat` (§4.1, permanently).

### P4-RESULT 2026-08-17 — one C++ link shape, and the scope grew on measurement

`libaxl-cxx.a` is gone: all SEVEN of its sources, 1,696 lines, against the
765 this row originally scoped. And there is now ONE C++ link — the shape that
used to be selected only by `-fexceptions` — so `EH_LINK` collapsed into
`CXX_LINK` across `axl-cc`, the Makefile and `install.sh`.

**The scope grew because two of the three reasons for keeping the other five
files turned out to be measured-false.**

`axl-cxx-rbtree/hash/rehash/string-inst/libm` were not about iostreams; they
existed so a C++ link could carry NO libstdc++ at all. Once it carries one:

- *Size.* Keeping them saves **~3 KB** of `.text` (AXL 3,855 B against
  libstdc++'s `tree.o` + `hash_bytes.o` + `hashtable_c++0x.o` at 6,873)
  against a **+46,928** budget. 6% of the cost, for 931 lines that track
  libstdc++'s internal ABI.
- *AVX.* `axl-cxx-rehash.cpp` and `axl-cxx-libm.cpp` exist because the
  DISTRO's `hashtable_c++0x.o` carries 49 VEX instructions, which are `#UD`
  under UEFI. Run `scripts/check-no-avx.py` over the hermetic toolchain's
  `libstdc++.a` and `libsupc++.a`: **clean, all 189 members.** The hazard went
  away with the host toolchain and nobody had re-measured.
- *Redistribution.* `AXL-Cxx-Design.md` §8 — the RLE does not cover conveying
  the runtime library — is **untouched and was never the blocker here**. The
  SDK ships no libstdc++; `axl-cc` names the CONSUMER's installed copy through
  `-print-file-name`. And since P3 put `libc.a`/`libm.a`/`libgcc.a` on every
  link, that toolchain was already a hard prerequisite for any link at all, so
  P4 adds no install step for anybody.

**The cost is 63% smaller than this document accepted.** §4c's +126,400
`.text` was measured before the terminate-handler shrink landed (`fa54572b`,
2026-08-16). Re-measured on `sdk/examples/containers.cpp`, x64, the real P4
shape — a `-fno-exceptions` TU linked against libstdc++:

| | `libaxl-cxx.a` | libstdc++ | delta |
|---|---|---|---|
| `.text` | 33,984 | 80,912 | **+46,928 (+138%)** |

> **QUOTE THE `.efi`, NOT THE `.text`.** The row above is the one that got
> repeated everywhere as "what P4 cost", and it is the smaller half of a
> figure whose other half sits in the same table: the image went 58,758 ->
> 159,097, **+100,339**. A consumer plans against image size.
>
> And a fixture UNDERSTATES it. A consumer measured P4 on four real linked
> tools at **+153,886 to +178,118 on x64 (+28% to +36%)** and +13-20% on
> aa64, attributed to 132 libstdc++/libsupc++/`_Unwind` symbols plus an
> `.eh_frame` section the image did not previously carry. `containers.cpp`
> pulls less of libstdc++ than a real application does, and on x64
> `.eh_frame` scales with reached code, so both terms grow with the program.
> `--no-eh-frame` claws most of this back.
| `.efi` | 58,758 | 159,097 | +100,339 (+171%) |

**What it bought, booted on both arches** (`test-cxx-iostreams-qemu.sh`):
`std::cout`/`std::cerr` reach the UEFI console, `std::ostringstream` and
`std::istringstream` round-trip including `double`, and `std::ofstream` /
`std::ifstream` write and read a file back off the ESP. An iostreams image is
**734,512** bytes of `.text` on x64 and 702,576 on aa64 — an order above a
containers-only one, which is why `axl::cout` (~700 B over `axl_printf`) stays
the default for a serial console rather than being retired.

**EVERY C++ link takes the exceptions linker script**, which was not obvious
and is load-bearing. libstdc++ is compiled WITH exceptions whatever the caller
passed, so `vector::at` out of range really throws in a `-fno-exceptions`
image. With a registered frame table that reaches AXL's terminate handler,
which prints the type and `what()`; without one it arrives via
`_URC_FATAL_PHASE1_ERROR` and prints neither. The diagnostics got strictly
better as a side effect —

    terminate: uncaught exception of type St12out_of_range
      what(): vector::_M_range_check: __n (which is 99) >= this->size() (which is 3)

— where the old stub printed only `axl-cxxabi: __throw_out_of_range_fmt`.

**The one real capability LOST, and it failed silently.** C++ allocation
failure is no longer injectable. `axl_mem_fail_next_alloc()` injects into
AxlMem, and `operator new` is libstdc++'s now and reaches newlib's `malloc` —
a different allocator, per §2-DECISION. Two fixtures called it and neither
broke: `cxx-hosted-badalloc.cpp` allocated successfully and printed its own
`UNREACHABLE` line, and `cxx-hosted-selftest.cpp`'s nothrow case printed
`ALLOCATED`. Both now ask for 2^45 `int`s (128 TiB) instead, which no `sbrk`
can serve. An `sbrk`-level knob would NOT have worked: dlmalloc serves a
256-byte request from its existing top chunk without calling `sbrk` at all,
so the injection would be bypassed for exactly the small allocations a
container makes.

**`make check-no-avx` was repointed, because it would otherwise have been
watching the wrong file.** Its whole justification was scanning
`libaxl-cxx.a` — AXL's substitutes, written partly BECAUSE the distro's
libstdc++ carries AVX. With those deleted the exposure moved to the archive we
CONSUME, so the gate now scans the toolchain's `libstdc++.a`/`libsupc++.a`
(0.8 s, and it names a non-x86 archive as skipped rather than silently
passing). That also closes a pre-existing gap: the `-fexceptions` path has
linked libstdc++ since long before P4 and was never covered.

### SBRK-RESULT 2026-08-17 — the heap is placed, not accepted

Found while closing out the substrate's leftovers, by asking whether the
deferred `mallinfo`/`malloc_trim` surface now works. It links and runs — and
reported 46,645,422,825,533,952 free bytes, which is what led here.

**The defect.** `malloc` failed for every request of 1 MiB or more, on both
arches, and the heap was wedged afterwards: a 256 KiB request that had
succeeded moments earlier returned NULL once a large one had been attempted.
Post-P4 this reaches `operator new`, so a `std::vector` growing past ~1 MiB
became `bad_alloc` -> terminate.

**The cause was ours.** `axl_alloc_pages` is `AllocateAnyPages`, which every
UEFI implementation satisfies DOWNWARD from high memory. Measured:

    sbrk[0] = 0x1de1f000   sbrk[1] = 0x1dd1f000   sbrk[2] = 0x1dc1f000

A break that moves backwards is not a break. dlmalloc was blameless.

**The fix is PLACEMENT.** Growth in place is possible — `AllocateAddress`
takes an exact range — but only if the region has somewhere to grow into, and
a firmware-chosen one does not: measured on OVMF x64, the pages immediately
above such a region were occupied 4 times out of 4, because that is precisely
the stripe the firmware has been carving its own allocations from. So `sbrk`
now asks `axl_mem_largest_free_run` where the big untouched run is, takes its
LOW end with `axl_alloc_pages_at`, and extends upward into the rest.

| | before | after |
|---|---|---|
| `malloc(1 MiB)` / `malloc(32 MiB)` | NULL | ok |
| 256 KiB after a large request | NULL (wedged) | ok |
| heap ceiling | ~1 MiB | 64 MiB+ (test cap, not a limit) |
| free run available | — | 423 MiB x64 / 326 MiB aa64 |
| `hello.c` | 47,247 | 47,247 (nothing committed unless you `malloc`) |

**Two new public functions**, both tested: `axl_alloc_pages_at` (exact-address
page allocation) and `axl_mem_largest_free_run` (largest
`EfiConventionalMemory` run — the classified region view cannot answer this,
because it deliberately maps `EfiBootServicesData` and `EfiLoaderData` onto
`AXL_MEM_REGION_RAM` too).

**Routes measured and rejected**, so nobody re-runs them:

| route | x64 | aa64 |
|---|---|---|
| `AllocateMaxAddress` with a low ceiling | works | **REFUSED** — ARM's DRAM starts at `0x40000000`, so there is no RAM below a 256 MiB ceiling. An x64-only fix that would have silently reverted to the broken behaviour on ARM. |
| PI GCD `EfiGcdAllocateAnySearchBottomUp` | `EFI_NOT_FOUND` | `EFI_NOT_FOUND` |

**Why GCD cannot serve this, since it looks like it should.** It has an
explicit bottom-up search, and the search works — there is simply nothing in
its domain. The DXE Core claims ALL of SystemMemory for itself at init and
hands it to the UEFI page allocator, so GCD reports **zero** unallocated
SystemMemory: 14 ranges / 511 MiB on x64 and 1 range / 512 MiB on aa64, every
one owned. GCD is the *address-space* map — `AllocateMemorySpace` is for
claiming space nobody has DESCRIBED yet (MMIO windows, memory hot-added via
`AddMemorySpace`), not for obtaining RAM. The real gap is that
`gBS->AllocatePages` has no bottom-up option at all, which is why reading the
map and placing explicitly is the supported answer rather than a workaround.

*A measurement trap worth recording:* `EFI_GCD_MEMORY_TYPE` is
`{NonExistent, Reserved, SystemMemory, MemoryMappedIo, ...}` — SystemMemory is
**2**, not 1. The first probe here used 1, so it both filtered on `Reserved`
and asked `AllocateMemorySpace` for Reserved memory; its `EFI_NOT_FOUND` meant
nothing. The tells were DRAM printing as `MemoryMappedIo` and a 16-million-MiB
total.

## 4b. OPEN — alternatives to newlib, and where the seam belongs

Raised 2026-08-13. Not scheduled; recorded so it is decided on purpose.

> **THE PREMISE BELOW EXPIRED, and the phases in §4d are what expired it.**
> Everything from here to §4b.1 describes the tree BEFORE P3, and every load-
> bearing fact in it is now false: `libc.a` is on EVERY link, AXL does *not*
> supply every definition, and the two files named as "the whole libc in this
> tree" are absent from `libaxl.a`'s own link (verified against the current
> tree 2026-08-17 — still true). **They reappeared 2026-09-03** as the
> separate `libaxl-standin.a` archive, linked only under `AXL_TOOLCHAIN=host`
> where newlib is absent from the link entirely; see
> `AXL-Host-Toolchain-Design.md` §5.3. `libaxl.a`'s own link still has
> neither file.
>
> **The question this section framed is therefore answered, not open.** It
> asked whether "adopting newlib" should mean adopting more than its headers.
> P1'-P4 adopted the whole thing: newlib owns the entire C vocabulary and runs
> its own dlmalloc on an AXL-provided heap, and AXL keeps only what newlib
> cannot supply — the porting layer, the stack-guard canary, `AxlFormat`
> (§4.1, permanently) and `__cxa_atexit`.
>
> What remains genuinely open is the NARROWER question §4b.1 already measured:
> whether picolibc would be a better substrate than newlib. That answer stands
> — it ties `AxlFormat` and drags no allocator, so the decision was to KEEP
> `AxlFormat` and stay on newlib. Kept below as the reasoning that got there.

**What the tree did BEFORE P3 (historical).** After §4.1b we compiled against
newlib's HEADERS and linked `-nostdlib` — `libc.a` was never linked, on any
path. Newlib supplied declarations; AXL supplied every definition. The complete
set of standard-named symbols `libaxl.a` defined was TWELVE:

    memchr memcmp memcpy memmove memset
    strchr strcmp strlen strncmp strncpy strstr   time

That was the whole libc in the tree (`src/data/axl-str-compat.c` and
`src/mem/axl-intrinsics.c`, both DELETED by P3 from `libaxl.a`'s link —
restored 2026-09-03 as the separate `libaxl-standin.a` archive for
`AXL_TOOLCHAIN=host`, `AXL-Host-Toolchain-Design.md` §5.3 — plus `time()`
in the mbedTLS platform shim). §4.1
already ruled newlib's own implementations out of the one place they would have
mattered most (`printf` beneath `AxlLog`), so "adopting newlib" currently means
"adopting its headers", and the substrate question is narrower than it reads.

**Candidates, judged against this platform rather than in the abstract:**

| | what it offers HERE | verdict |
|---|---|---|
| **picolibc** | newlib's string/math with AVR-libc's stdio, restructured for embedded — a configurable printf that does NOT drag FILE, which is precisely what §4.1 rejected newlib's for | The one worth evaluating. Caveat: errno/reentrancy through `__thread` by default, and UEFI provides no TLS — the same wall that made x64 C++ need a bare-metal toolchain |
| **llvm-libc** | Explicitly modular: take `memcpy`/`strlen`/math without taking stdio. Apache-2.0-with-LLVM-exception | Best fit for how we actually consume a libc (functions, not a library), and for §3b's per-function gates. Young, incomplete |
| **musl** | Excellent quality, MIT | Wrong shape: a libc over Linux syscalls. Freestanding UEFI means writing a syscall layer beneath it |
| **EDK2 StdLib / edk2-libc** | The UEFI-native prior art | Removed from edk2 mainline and unmaintained. Its abandonment is itself evidence about this class of project |

**Second question, and the more interesting one: should `libaxl.a` split into a
GLib-shaped upper API and a POSIX/libc-shaped lower one?**

The seam is real and worth NAMING — today those twelve functions are scattered
across two files with no stated contract, and a gate asserting exactly which
standard names we define would turn a picolibc or llvm-libc evaluation into a
swap rather than an excavation. Making it a separate ARCHIVE is ceremony at
this size.

What the split actually serves is worth stating, because it is not what it
looks like: §4.1b measured that AXL's own code includes only `stddef`,
`stdint`, `stdbool` and `stdarg` — EVERY libc demand in this tree comes from
vendored third-party code. So the lower layer is not "POSIX beneath GLib", it
is a compatibility surface for FOREIGN code, and its value is measured in how
cheaply we can port the next mbedTLS-shaped dependency.

**On "we should have built POSIX first, then AXL on top":** prior art splits.
Zephyr supports several libc backends behind a stable seam with POSIX as a
subsystem ALONGSIDE the kernel API; NuttX is POSIX-first end to end; GLib — the
comparison this design is built on — ships no libc and defines no POSIX layer,
it consumes the platform's. Two pieces of evidence from this tree argue against
the counterfactual: `AxlTcp`/`AxlSocket` is inverted deliberately because a
blocking `accept()` freezes single-threaded no-preempt firmware
(`docs/ROADMAP.md`, Networking layering), and §4.1 measured that POSIX stdio
drags the allocator into the logger. POSIX-first would have imported both. And
the cost of not having done it is twelve functions.

Where it does bite, and the reason to keep this open: PORTING FOREIGN CODE.
That is what `include/compat` existed for, what newlib's headers now serve, and
what a named seam would make cheap.

**Before recommending anything, measure picolibc's printf against `AxlFormat`'s
6,708 bytes** on the §4.1 rig — same probe, same link, same map-file reading.
That number decides whether any of this is worth doing.

### 4b.1 MEASURED 2026-08-13 — picolibc drags no allocator, and is a wash on size

`scripts/measure-printf-size.sh` (committed, because this is the third
measurement of one shape and llvm-libc is queued behind it). picolibc 1.8.10
built for `x86_64-elf` with the tree's own bare-metal cross, via picolibc's
`cross-coreboot-x86_64-elf.txt` — the triple matches exactly.

Every row measured on ONE rig at `-Os` (AXL's RELEASE optimisation, and
picolibc's meson default is `minsize`, so the comparison is like for like), at
the same entry point — format into a caller's buffer — with the AXL row
carrying `CFLAGS_BASE`'s real ABI/codegen flags. IMAGE is every `SHF_ALLOC`
section that occupies file bytes; `.bss` is listed separately:

| formatter | .text | .rodata | .data | .bss | **IMAGE** | libc members | allocator |
|---|---|---|---|---|---|---|---|
| **`axl_vsnprintf`** | 6,053 | 1,688 | 8 | 256 | **8,185** | — | **no** |
| newlib `vsniprintf` (integer-only) | 13,153 | 448 | 2,480 | 896 | **16,801** | 25 | **YES** |
| newlib `vsnprintf` | 43,941 | 2,352 | 2,952 | 896 | **53,045** | 47 | **YES** |
| **picolibc, integer-only** | 2,116 | 304 | 0 | 256 | **2,624** | 4 | **no** |
| **picolibc, float-capable** | 6,458 | 1,200 | 0 | 256 | **8,150** | 9 | **no** |

**§4.1's blocking objection does not apply to picolibc.** Its four pulled
members are `vsnprintf`, `vfiprintf`, `filestrput` and `strnlen` — no
allocator, no `FILE` machinery, no `_impure_ptr`. That is the whole reason
newlib was rejected beneath `AxlLog`, and picolibc's tinystdio simply does not
have it. Even the FLOAT build stays clean: it pulls `dtoa_ryu` and the Ryu
tables, and `nm` finds no malloc-family symbol anywhere in the image. (Newlib,
by contrast, needs `sbrk` to LINK AT ALL for its integer entry point — the rig
has to supply one.)

**At equal functionality it is a dead heat: 8,185 B against 8,150 B.** Thirty-five
bytes, 0.4%, in picolibc's favour. The 3.1x win belongs only to the
integer-only build, which would mean giving up `%f` — and `axl_snprintf` is
public API that supports it. So the number §4b said would "decide whether any
of this is worth doing" decides it by declining to differ.

**The `__thread` errno caveat is a build option, and it was verified, not
assumed.** Built `-Dthread-local-storage=false -Dnewlib-global-errno=true`:
`nm` reports no TLS-typed symbols in the archive, `readelf -lS` shows no TLS
segment and no `.tdata`/`.tbss` in the linked image, and `errno` is an ordinary
undefined symbol the application defines — AXL would supply it, making the
twelve standard-named symbols thirteen. The wall §4b feared is a meson flag.

**No specifier gap.** `AxlFormat` implements `%c %d %e %E %f %F %g %G %i %p %s
%u %x %X %%` and nothing AXL-specific, so picolibc is a functional superset.
Neither `%n` nor positional args is in use here.

**RECOMMENDATION: keep `AxlFormat`; do not adopt picolibc for formatting.**
The measurement was expected to decide this on size and it declines to: at the
functionality AXL actually ships, the two differ by 35 bytes. Against a tie,
taking an external build dependency for every consumer is not worth it
— and the x64 toolchain distribution problem (§4.1c) is a fresh demonstration
of what "every consumer needs this too" costs. `AxlFormat` also writes through
`AxlWriteFunc`, which is the shape `AxlLog` streams through; picolibc's is
`FILE`-based, so a swap is not only a dependency but an adapter.

What this does NOT settle, and is the part worth keeping open: picolibc as the
**foreign-code compatibility surface** §4b is actually about. That value is
header and function BREADTH for the next mbedTLS-shaped port, not printf size,
and this measurement says nothing about it. picolibc now has a clean bill of
health on the two things that disqualified newlib, which makes it the leading
candidate there.

Two smaller findings worth recording:

- `AxlFormat` cannot be built integer-only. At `-Os` `emit_float` inlines into
  `axl_vformat`, so `--gc-sections` cannot drop it, and `axl_dtoa` is linked
  into shipped images (confirmed in `AxlTestNet`) whether or not anything
  formats a float. picolibc offers that as a build switch, and the two picolibc
  rows price it at 5,526 B — AXL's own float cost is not directly measurable
  for exactly the inlining reason above, but there is no reason to expect it to
  be smaller. An `AXL_FORMAT_NO_FLOAT` switch would buy that back with no new
  dependency: the one actionable item this measurement produced, and it is
  in-tree.
- The §4.1 rig was never committed and is not recoverable — its `AxlFormat`
  figure does not fall out of the obvious reconstructions. Every row above was
  therefore re-measured rather than half-inherited, which is why
  `scripts/measure-printf-size.sh` IS committed. The newlib rows land lower in
  absolute terms than §4.1's and identical in conclusion.

Four confounds were found and corrected while building the rig, recorded
because each one moved a number the recommendation rests on: the AXL sources
compiled without `-ffunction-sections` retained all of `axl-math.o` and
overstated `AxlFormat` by ~6 KB; `.data` went unmeasured and hid 2,480 B of
newlib's; the AXL row was measured at `axl_vformat` while the libc rows used
their full `vsnprintf`; and an allocator test matching on `dtoa` reported
picolibc's allocation-free Ryu as an allocator.

## 5. Relationship to the C++ work

`src/cxxrt/` (`a0234245`) is the first working instance of this design, built
for the C++ exception path before the general direction was stated: newlib
supplies the C library, `malloc` routes onto AXL's allocator, and a
containers+exceptions image runs 7/7 with zero leaks on both arches. It is
evidence the shape works, not a detour from it.

**2026-08-17: the C++ half is now in scope rather than adjacent.** §4c prices
the libstdc++ swap and Mike accepted it, which makes `src/cxxrt/` less "the
first instance" and more "the half that is already done". Two claims elsewhere
must be re-taken, not inherited:

- `AXL-Cxx-Design.md` §9c keeps `axl::string` because `std::string` HALTS on
  OOM under `-fno-exceptions`. If C++ links libstdc++ with exceptions, that
  premise no longer holds and the decision should be re-argued on its merits.
- `AXL-Cxx-Stdlib-Surface.md`'s four-tier table predates both the bare-metal
  toolchain and this swap. Its "needs `--hosted`" rows are already stale
  (`--hosted` is gone); its locale wall needs re-measuring.

`AXL-Cxx-Unwinder-Design.md` §4a's terminate handler is unaffected: it is
smaller than libstdc++'s *and* names the exception. Implementing the porting
layer removes the reason libstdc++'s was silent, not the reason ours is better.

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
