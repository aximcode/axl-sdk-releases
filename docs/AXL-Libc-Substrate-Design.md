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
| **libstdc++ headers, lint's C++ pass** | **host** | the cross toolchain's; recorded at the call site in `scripts/lint.sh` |
| **`--hosted` / `CXXFLAGS_HOSTED*`** | present | T3 deletes them |

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
  by `test/integration/test-pkg-deps-minimal.sh`: only the declared deps
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

## 4b. OPEN — alternatives to newlib, and where the seam belongs

Raised 2026-08-13. Not scheduled; recorded so it is decided on purpose.

**First, what the tree actually does today, because the name "newlib"
overstates it.** After §4.1b we compile against newlib's HEADERS and link
`-nostdlib` — `libc.a` is never linked, on any path. Newlib supplies
declarations; AXL supplies every definition. The complete set of standard-named
symbols `libaxl.a` defines is TWELVE:

    memchr memcmp memcpy memmove memset
    strchr strcmp strlen strncmp strncpy strstr   time

That is the whole libc in this tree (`src/data/axl-str-compat.c`,
`src/mem/axl-intrinsics.c`, and `time()` in the mbedTLS platform shim). §4.1
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
