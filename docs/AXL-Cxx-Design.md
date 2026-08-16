# AXL C++ layer — the standard's containers, AXL's runtime under them

> **Status: C0 and C1 SHIPPED 2026-08-06; C2/C3 fall out of C1.** Spiked
> 2026-08-04 against the real toolchain and run under QEMU; every number below
> is measured, not estimated. §4.5 was added 2026-08-04 after the fifth spike
> closed the last open assumption.
>
> **The title changed with the conclusion.** This document was written to
> decide which of AXL's C data structures a C++ container should be built *on*
> versus *beside* — §4 is that analysis and its measurements still stand. The
> answer turned out to be "neither": `std::vector`, `std::string`, `std::map`
> and `std::unordered_map` all run under UEFI (§2b.1), so the layer writes no
> containers at all. Sections before §2b.1 are preserved as the reasoning that
> got there, including the two claims it had to retract; where one of them
> reads as a plan, §2b.1 supersedes it.

**Scope:** what a C++ API surface for axl-sdk should be, and specifically which
of AXL's C data structures a C++ container should be built *on* versus *beside*.
The answer differs per structure and the dividing line is not the obvious one.

**Non-goal:** a C++ JSON API. That was the question that started this, and it is
deliberately deferred until the foundation below exists — a JSON header written
first would invent the naming, error-handling and ownership conventions
everything after it has to follow. See §9.

---

## 1. Where this starts

axl-sdk has **zero `.hpp` headers**. Its C++ story today is "the C API is
callable from C++": `libaxl-cxx.a` supplies the ABI ops, `AXL_AUTOPTR` gives
RAII, `axl-c++` is the driver. The C++ *API* lives in AGT, a separate project.

AGT is also the only C++ consumer (delldiags is C). What it hand-rolls is the
demand signal, counted:

| Hand-rolled in AGT | Count | Missing primitive |
|---|--:|---|
| manual `axl_free` / `axl_*_free` | 84 | owning pointer |
| owning `char *` members, strdup + `if (x) free` | 12 | owning string |
| `AxlArray *` as a typed container + manual per-element free loops | 4 members, 3 files | typed container |
| `AxlGfxBuffer *saved_target_` — *"target active at ctor; restored at dtor"* | 1 | scope guard |

It hand-rolls because `<string>`, `<vector>` and `<map>` are not available
under `-ffreestanding`. That turns out to be a flag we could drop rather than a
wall — see §2b, which an earlier draft of this document got wrong twice.

The last row decides the shape of the project: AGT independently invented, by
hand, the RAII scope-guard mechanism. When your only C++ consumer has already
built a mechanism, that belongs in the foundation.

## 2. The single biggest decision is a DON'T

GCC 14.3.1 implements [P1642](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p1642r11.html),
the C++23 freestanding library subset. Verified by compiling, linking **and
running** under UEFI, not by reading a table:

```
<algorithm> <iterator> <ranges> <concepts> <compare> <expected>
<memory> <bit> <functional> <numeric> <limits> <tuple>
<variant> <optional> <string_view> <array> <span> <initializer_list>
```

`std::sort` plus a `views::filter` pipeline plus `std::expected` ran correctly
in QEMU for **545 bytes** over the C++ hello baseline.

**So we write no algorithms.** A type with `begin()`/`end()` gets
`std::sort`, `std::find`, every view and the whole iterator vocabulary for
free. [ETL](https://github.com/ETLCPP/etl) and
[EASTL](https://github.com/jwdevel/EASTL) both reimplemented the algorithms —
ETL targets C++03, EASTL predates all of this. We would be copying their
constraints, not their solution.

> This section originally continued "**so we write containers**", on the
> grounds that the containers are unavailable under `-ffreestanding`. §2b
> found the gate is the flag and §2b.1 removed it, so we write no containers
> either.

Two traps found while proving it:

- **C++20 iterator concepts are stricter than `iterator_traits`.** A hand-rolled
  proxy iterator satisfied `std::sort` but was rejected by `views::filter`,
  purely for lacking a default constructor — `std::input_or_output_iterator`
  refines `std::semiregular`. Cheap to get wrong, loud when you do.
- **`std::expected::value()` does not link** under `-fno-exceptions`: it lowers
  the throw to `abort`, which is undefined here. A *link* error, not silent UB.
  Either use `value_or`/`has_value`/`operator*`, or define `abort` in
  `libaxl-cxx.a` as `axl_panic` and let `.value()` mean "panic on misuse."
  **Resolved in C0:** `abort` is defined, loudly, in `src/runtime/axl-cxxabi-ops.cpp`.

## 2b. What actually blocks `std::vector`, measured

An earlier draft said the containers "require exceptions and the heap" and so
"never will be" available, citing cppreference. That is true of the STANDARD's
freestanding SUBSET and false as a statement about our toolchain, which is what
the sentence implied. The real situation, measured:

```
#include <vector>  with -fno-exceptions  ->  compiles clean
#include <vector>  with -ffreestanding   ->  #error "This header is not
                                             available in freestanding mode"
```

**`-ffreestanding` is the gate, not `-fno-exceptions`.** libstdc++ gates the
containers on `__STDC_HOSTED__` via `bits/requires_hosted.h`, and
`-D_GLIBCXX_HOSTED=1` does not override it. A hosted `std::vector<int>`
translation unit compiled with exceptions OFF needs only:

| Symbol | Status |
|---|---|
| `memcpy` | AXL provides |
| `operator new` / `delete` (3 forms) | `libaxl-cxx.a` provides |
| `std::__throw_bad_alloc()` | missing — a one-line stub |
| `std::__throw_bad_array_new_length()` | missing — a one-line stub |
| `std::__throw_length_error(char const*)` | missing — a one-line stub |

Three stubs, each a call to `abort`. And with those, **it works** — verified
running under QEMU, including `std::vector<std::vector<int>>` and `std::sort`
over it:

```
size=10 front=1 back=10
nested=3 inner=99
```

68 KB against a 44 KB C++ hello baseline, so ~24 KB for `<vector>` plus
`<algorithm>`.

> **A retracted claim.** An earlier version of this section reported that the
> image "faults before `main`" and left it un-diagnosed. Root-caused
> afterwards, and it was a HARNESS defect, not a container one: the experiment
> was hand-compiled with `g++` and omitted the Makefile's `$(GCC_ARCH)`, which
> is `-mno-red-zone -march=x86-64`. This host's gcc defaults to
> `-march=x86-64-v3`, so it emitted AVX (`vpxor`, `vmovdqa`), and UEFI runs
> with `CR4.OSXSAVE` clear — the dump said `#UD - Invalid Opcode` with
> `CR2 = 0`, which is not a bad pointer and should have been read as such
> immediately. Recompiling with the SDK's own arch flags took the AVX
> instruction count from 3 to 0 and the program ran. **A hand-rolled compile
> that omits the build system's flags is not a valid experiment**, and the
> conclusion drawn from one is worth nothing.

### 2b.1 What shipped, and the two symbols the table above missed

C1 landed. `axl-c++ --hosted` drops `-ffreestanding` and the `include/compat`
shims per translation unit, keeps `$(GCC_ARCH)`, and links the toolchain's own
`libstdc++.a` inside a `--start-group` with `libaxl-cxx.a` and `libaxl.a` —
grouped rather than ordered because the three archives are mutually dependent
(libstdc++ members reference `operator new` and `memcpy`; `libaxl-cxx.a`
references `axl_malloc`).

The stub count went from three to **five** — `__throw_logic_error` and
`__throw_out_of_range_fmt` arrive with `<string>`'s `replace`/`insert` and with
`at()` — and the "AXL provides" column had two gaps that only a LINK could
show:

| Symbol | Where it came from |
|---|---|
| `ceil` | **x86-64 only.** `_M_bkt_for_elements` is inline in the header and rounds a load-factor quotient; AArch64 folds that to `frintp` (base ISA) while x86-64 needs SSE4.1's `roundsd`, above our `-march=x86-64` baseline, so gcc emits a call. Forwarded to `axl_ceil` in `src/runtime/axl-cxx-libm.cpp`. **An aa64-only measurement cannot see this symbol at all** — which is exactly how the earlier "five libc symbols" figure was reached |
| `_Prime_rehash_policy::_M_next_bkt` / `_M_need_rehash` | Written in `src/runtime/axl-cxx-rehash.cpp` rather than taken from `hashtable_c++0x.o` — see §2b.2 |

Verified running on **both** arches: `std::vector` + `std::sort`,
`std::string` `+=`/`append`, `std::map<std::string,int>` ordering, and
`std::unordered_map` through 400 inserts, `reserve`, a `max_load_factor`
change and an erase/rehash churn — with `load_factor() <= max_load_factor()`
asserted, not just the values. `test/integration/test-cxx-hosted-qemu.sh`.

### 2b.2 Why we define two libstdc++ internals ourselves

`hashtable_c++0x.o` is the only place libstdc++ does floating point in the
container path. On a distribution whose gcc baseline is above plain `x86-64` —
RHEL 10 ships `-march=x86-64-v3` — that arithmetic is rendered in VEX-encoded
AVX: 49 instructions. `tree.o` beside it is VEX-free purely because a
red-black tree does no float math, which is why `std::map` ran on the same
build where `std::unordered_map` faulted.

AVX is not the problem; **ungated** AVX is. AXL ships AVX2 kernels and runs
them — `blend_row_over_avx2` dispatches behind `axl_cpu_avx_usable()`, which
reads `CR4.OSXSAVE` and `XCR0` on the live core, and `axl_cpu_enable_avx()`
turns the state on at CPL0. `hashtable_c++0x.o` has no such check: its AVX
runs on the first insert.

"Call `axl_cpu_enable_avx()` at startup" was considered and rejected, for
three reasons the header itself documents. It is **per logical processor**, so
an AP worker touching the container would still fault. It returns `false` on a
CPU without AVX, which would make `std::unordered_map` require an AVX2
machine. And the AVX is an artifact of the BUILD HOST, so whether a consumer's
`unordered_map` needed AVX would depend on which machine built the SDK.

The two functions are fully specified by the comments on their own
declarations, and compiling them with the SDK's `$(GCC_ARCH)` makes them
AVX-free by construction. Written from that contract, not derived from
libstdc++'s implementation; the prime table is our own. They are supplied as a
PAIR — they share `_M_next_resize`, so taking one from each side links cleanly
and then disagrees with itself about when to grow. `ld -y` asserts both
resolved to `libaxl-cxx.a`.

`make check-no-avx` is the standing guard, and it allowlists by SYMBOL with a
recorded justification, so a deliberately-dispatched kernel keeps its
exemption while a second function in the same object acquiring VEX by accident
still fails.

### 2b.3 `operator new` may not return NULL

Making the containers reachable turned a documented quirk into a defect.
`libaxl-cxx.a`'s `operator new` used to return NULL on exhaustion and ask
callers to be defensive. `__new_allocator::allocate` does not check it — the
standard guarantees the throwing form never returns NULL — so a container OOM
became a `#PF` near address 0 inside a template, with nothing pointing at the
allocation.

It now halts through `std::__throw_bad_alloc`. `new (std::nothrow)` is the
supported way to get a NULL, and those overloads were added alongside. Nothing
in-tree or in AGT null-checked `new`, so the old contract was buying nothing.

Writing that sentence exposed two forms that had never linked at all, in
either mode, and neither looks unusual in source:

- **`new (std::nothrow) T`** needs the `std::nothrow` OBJECT, not only the
  overloads that take it. It lives in `libsupc++`, which a firmware image does
  not link — so the escape hatch the paragraph above points at did not exist
  until it was tested.
- **an over-aligned `new`** calls a different operator entirely
  (`operator new(size_t, align_val_t)`), chosen whenever `alignof(T)` exceeds
  16. `alignas(64)` on a cache-line-sized struct is the whole trigger, and the
  link error names a mangled symbol without mentioning alignment. `axl_malloc`
  aligns to `sizeof(size_t)`, so these over-allocate and stash the original
  pointer below the returned block.

`test/integration/cxx-new-forms.cpp` is the standing guard, link-checked in
BOTH modes because the freestanding half is where `std::nothrow` goes missing
— hosted builds get it from `libstdc++.a` and hide the gap.

### 2b.4 The two flags that were not actually forced

`-fno-stack-protector` and `-fno-rtti` had been carried as if they were
properties of firmware. Both turned out to be fixable, and the reasons are
worth recording because neither is what the flag name suggests.

**`-fno-stack-protector`** was a missing canary SOURCE, not a missing feature.
x86-64 GCC reads the canary from `%fs:0x28` — glibc's TLS block — and UEFI
sets up no TLS, so the default form faults instead of protecting.
`-mstack-protector-guard=global` redirects the read to a plain symbol, which
is what AArch64 already defaults to. With that flag plus
`src/runtime/axl-stack-guard.c`, `-fstack-protector-strong` is now ON by
default in the library and in `axl-cc`, and a deliberate overflow halts under
QEMU on both arches. Cost measured at **+1.6% on `libaxl.a`, +6.7% (3.7 KB) on
a produced `.efi`**. Opt out per invocation with `-fno-stack-protector`.

**`-fno-rtti`** was a missing libsupc++, and only in one mode. `libstdc++.a`
already carries the `__cxxabiv1::__class_type_info` vtables and
`__dynamic_cast`, so `axl-c++ --hosted -frtti` gives working `typeid` and
`dynamic_cast` — verified running under UEFI, positive and negative cast. It
stays off by default because `type_info` objects cost image size per
polymorphic class. Freestanding `-frtti` still cannot link, and the test
asserts that failure so the capability is never overstated.

Enabling it found a latent aa64 defect that had nothing to do with RTTI.
`DT_RELA` pointed at a linker-synthesized `.rela.dyn` while `DT_RELASZ`
counted both it and the script-placed `.rela`, at non-contiguous addresses —
so the crt0's relocation walk ran off the end of the first section and applied
the bytes after it. Any aa64 image whose dynamic relocations split had this;
RTTI was merely the first workload to split them. Fixed by naming the aa64
output section `.rela.dyn`, and guarded by `make check-reloc-coverage`.

The full measured picture — what works, what does not, and what each remaining
"no" would cost in symbols — is `AXL-Cxx-Stdlib-Surface.md`.

### How much of the standard library is actually reachable

Measured per container, link-tested (not compile-tested — see the note at the
end of this section about why that distinction kept mattering).

**Two separate gates, and only the first is about `-ffreestanding`:**

1. `-ffreestanding` makes libstdc++ refuse the containers outright via
   `bits/requires_hosted.h`. Compiling a C++ TU `-fhosted` lifts it.
2. Some containers then need OUT-OF-LINE code that lives in `libstdc++.a`.

| Header | Cost |
|---|---|
| `<vector>` `<deque>` `<array>` `<span>` `<string_view>` `<algorithm>` `<ranges>` | **free** |
| `<string>` | free to construct; **1–2 symbols** for `+=`, `append`, `replace`, `insert` — i.e. for real use |
| `<list>` | 1 symbol |
| `<unordered_map>` | 1 symbol (`_Prime_rehash_policy::_M_need_rehash`) |
| `<set>` / `<map>` | 2 / 3 symbols — the red-black tree core (`_Rb_tree_increment`, `_Rb_tree_decrement`, `_Rb_tree_insert_and_rebalance`) |

About **five distinct symbols** in total. They are not in the headers by design:
libstdc++ puts them in the archive precisely because they are ABI, not
templates. And `libstdc++.a` is **not installed for either of our targets** on
the current box — only a 32-bit copy exists, and `aarch64-linux-gnu-g++`
reports none.

**`-fhosted` is a PER-TU flag and costs nothing measurable.** `libaxl.a` stays
`-ffreestanding`; only the C++ TUs that want containers compile hosted, and a
mixed image links and runs (verified under QEMU). The complete libc footprint
of a hosted C++ TU using `<vector>`, `<string>`, `<map>` and `<unordered_map>`
is `memcpy`, `memmove`, `memset`, `memcmp`, `strlen` — **all five already
provided by `libaxl.a`**. The one adjustment is that such a TU must not put
`include/compat` on its include path: those shims exist for the freestanding C
build, and shadowing the real headers with a stub `FILE` is what blocks
`<string>` and `<memory>`.

### So the choice, stated honestly

`std::vector` and the sequence containers are free today. `std::map`,
`std::set`, `std::unordered_map` and most of `std::string` are five symbols
away, in two self-contained object files.

**TESTED 2026-08-04, and it works.** Linking `libstdc++.a` pulls exactly two
archive members — `tree.o` (31 KB, the red-black tree) and
`hashtable_c++0x.o` (28 KB, the rehash policy) — and **both have ZERO undefined
symbols**. They drag in no locale, no iostreams, no unwinder. Verified end to
end on AARCH64 under UEFI in QEMU:

```
map size=3 ordered: apple=1 fig=2 pear=3
umap size=200 u[13]=169
```

`std::map<std::string,int>` ordering correctly, `std::unordered_map` with 200
entries, `-fno-exceptions`, five stubs, a 119 KB image.

The library came from the ARM bare-metal toolchain the SDK already installs
(`/opt/arm-gnu-toolchain-.../aarch64-none-elf/lib/libstdc++.a`) — a
freestanding build, which is the harder and more representative case. On x64
it is the distro's `libstdc++-static`, available but not installed by default.

**So: do not write containers.** Use `std::vector`, `std::string`, `std::map`
and `std::unordered_map`. What remains is integration, not implementation:

1. The five `std::__throw_*` stubs, in `libaxl-cxx.a` beside `abort`.
2. The two archive members, or a `-lstdc++` on the link line.
3. A per-TU hosted mode in `axl-c++` — drop `-ffreestanding` and
   `-Iinclude/compat` for TUs that opt in.
4. An arena allocator for paths where halting on OOM is unacceptable.

Two things to settle rather than assume:

- **Distribution.** If `axl-cc` needs `libstdc++.a`, every SDK consumer needs
  it present. aa64 gets it from the ARM toolchain automatically; x64 needs a
  distro package. Vendoring the two `.o` files instead raises a licensing
  question worth answering properly — libstdc++ is GPL-3 with the Runtime
  Library Exception, which is written for *linking*, and redistributing archive
  members inside an SDK is a different act.
- **Which of the five stubs belong to us.** They are the exception-free lowering
  points; putting them in `libaxl-cxx.a` makes every consumer inherit our
  halt-on-OOM policy, which is exactly the decision §2b's last part is about.

### The one property the standard containers cannot give us

Whichever path: with exceptions off, a failed allocation inside any of them
calls `__throw_bad_alloc`, which can only abort. **The firmware halts.**

That is a real divergence from the rest of this SDK, and not a theoretical one.
AXL ships `axl_mem_fail_next_alloc()` in a PUBLIC header, the unit suite
carries 43 OOM assertions, and some are degradation contracts rather than
error propagation — "failure at ANY allocation point -> valid lossy bbox
superset". Recoverable OOM here is exercised, not aspirational, and a container
that halts creates two tiers inside one binary: C recovers, C++ stops the
machine.

It does NOT follow that we should write a container library to fix it. That was
this document's position and it was wrong: writing and maintaining containers
is the largest permanent cost in this plan, and it would buy one property in an
unknown fraction of paths. The proportionate answer is an **allocator**, not a
container — `std::vector<T, arena_allocator<T>>` over a pre-sized `AxlArena`
cannot OOM at all, because exhaustion becomes a caller-visible condition
checked up front. That is ETL's fixed-capacity model expressed through the
standard containers, and it costs one allocator instead of four containers.

### A note on how this section kept being wrong

Three times in one session a conclusion here rested on evidence that stopped
one step short:

- "The containers require exceptions" — read from a standards document, never
  compiled. `-ffreestanding` was the actual gate.
- "It faults before `main`" — compiled and run, but with a hand-written `g++`
  line missing `$(GCC_ARCH)`, so it emitted AVX that UEFI `#UD`s.
- "The containers are free" — compiled, not LINKED. The associative ones need
  five symbols that only appear at link time.

Compile is not link; link is not run. Each step admits a different class of
defect, and this section has now been corrected by each of them in turn.

### Corroboration: Dell EPSA

A shipping UEFI diagnostic, 389 C++ files, built `-ffreestanding
-fno-exceptions -fno-rtti -std=c++11` — the same box. Its answer is to avoid
the standard library outright: total `std::` usage is `std::ignore` (17 sites),
`std::array` (3), `std::optional` / `std::nullopt` (2), all freestanding-legal.
Its only two `std::string` references are commented out. No containers, no
workaround, no unwinder.

That is independent confirmation that the wall is real. It is also a
cautionary data point rather than a model: EPSA does not have containers, so it
pays the hand-rolled cost AGT's 84 manual `axl_free` calls quantify (§1). The
point of this layer is to stop paying it.

## 3. Method: the confound that inverted a conclusion

`axl_malloc` costs **~330 ns while the heap is GROWING and ~60 ns when reusing
freed blocks** — 5.5×, and flat in live-object count (measured 0 → 140,000
live). No degradation, just a warm/cold split.

Consequence: **in any AXL benchmark, whichever variant runs first pays a tax the
others do not.** This is not a footnote. It made `AxlHashTable`'s insert look
5.8× slower than a hand-written template; with a warm-up phase the same
comparison came out at **parity** (777 µs vs 845 µs), and the first conclusion
was reported before it was caught.

Every benchmark in this document therefore begins:

```c
const size_t W = n * 4;
void **warm = axl_calloc(W, sizeof(void *));
for (size_t i = 0; i < W; i++) warm[i] = axl_malloc(32);
for (size_t i = 0; i < W; i++) axl_free(warm[i]);
axl_free(warm);
```

and the ordering-sensitive ones are re-run with the variant order reversed to
prove the confound is gone. Do this in any future AXL measurement.

## 4. The dividing line, and the three ways a skin loses

Copying objects is the first and worst reason, but it is not the only one.
The three no-go verdicts below each fail for a DIFFERENT reason, and none of
them predicts the others:

| Structure | Why the skin loses | Cost |
|---|---|---|
| `AxlArray` | it **copies objects** by memcpy | unsound for non-trivial `T` |
| `AxlTree` | indirection paid **log n times** per lookup, not once | 1.4x lookup |
| `AxlString` | cannot express **inline storage** | 9x on the dominant ops |

`AxlHashTable` fails none of them, which is why it is the one GO.

Five spikes, all run under QEMU/KVM x64.

### 4.1 `AxlArray` — NO-GO, and not because of speed

`axl_array_append` takes `const void *` and **memcpy**s `element_size` bytes. It
has `axl_array_set_clear_func` (a *destroy* hook, run on every discard path) but
**no construction or copy hook**. So a C++ skin never runs `T`'s copy
constructor.

Demonstrated, not argued. With `T` owning a heap pointer, and ctor/dtor counted
separately:

```
B  in-scope elem[0]=0            ctor=200 dtor=200  BALANCED
A  in-scope elem[0]=phd0         ctor=100 dtor=100  BALANCED
```

`phd0` is the debug allocator's fill pattern — a **use-after-free**. The
temporaries' destructors freed `owned`; the memcpy'd elements still point at it.
Note both report BALANCED: the healthy-looking count is the trap, because those
elements were never constructed *or* destroyed. An earlier version of this test
counted only live objects and detected nothing.

Speed says the same thing less importantly (N=200k, release):

| | push_back | indexed sum | `std::sort` | size |
|---|--:|--:|--:|--:|
| skin over `AxlArray` | 5569 µs (16.0×) | 163 µs (4.2×) | 13632 µs (19.4×) | +2913 B |
| **standalone template** | **347 µs** | **39 µs** | **702 µs** | **+1057 B** |
| skin + a hypothetical `axl_array_data()` | 1839 µs (5.3×) | 39 µs (1.0×) | 666 µs (0.95×) | +1144 B |

The third row is instructive: inlining element access recovers *all* of the read
and sort cost for +87 bytes. It is still unsound, because it still appends
through the memcpy. `AxlArray` is opaque with no data accessor, so the first row
is what "base it on the existing structure" means today.

### 4.2 `AxlHashTable` — GO, at measured parity

Stores `void *` keys and values; never copies an object; no soundness issue.

Integer keys, N=8000, warm heap:

| | insert | hit | miss |
|---|--:|--:|--:|
| skin over `AxlHashTable` | **777** | 86 | 100 |
| template, chained (same algorithm) | 845 | 78 | 92 |
| template, open addressing | **418** | 80 | 132 |

The skin **beats** the equivalent hand-written template on insert and ties on
lookup. Confirmed independently with string keys and 16-byte values, where a
variant identical to the template except for routing hash and compare through
the *same function pointers the C table uses* came out at 2676 µs vs 2779 µs —
i.e. **function-pointer genericity costs nothing measurable here.**

Chaining also **wins misses by 2×** (283 µs vs 526 µs): a miss stops at a null
bucket, where linear probing keeps walking. Open addressing is the only real win
left and it is a C-side change, not a reason to write a C++ hash table.

The skin's one genuine cost is boxing: `V` larger than a pointer needs an
allocation per entry (~60 ns warm), which showed up as exactly 1.33× on insert.

### 4.3 `AxlTree` — GO for the structure, NO-GO for the skin

Same soundness story (stores `void *`), and it **beats a hand-written flat_map
outright** — 38× on insert at N=8k and, more surprisingly, on lookup too
(360 µs vs 517 µs). A sorted-array `lower_bound` with a fully inlined comparator
lost to the AVL's function-pointer compare.

But the head-to-head that matters is against the *same algorithm*. Templated
AVL, identical tree heights (18 at 8k, 22 at 50k), warm heap, both orders:

| N=50000 | insert | hit | miss |
|---|--:|--:|--:|
| `AxlTree` (fn-ptr compare) | 11531 / 11057 | 3220 / 3242 | 3198 / 3414 |
| **templated AVL (inlined)** | **10481 / 10338** | **2297 / 2255** | **2330 / 2242** |

**Insert is parity (~7%); lookup is 1.40–1.44× faster templated.**

This corrects an inference. Having measured that genericity cost nothing for the
hash table, this document's earlier draft inferred the same for the tree. Wrong,
and the reason is structural: a hash lookup pays the indirect compare **once**;
a tree lookup pays it **log₂ n ≈ 22 times**. The hash result could never have
predicted the tree result.

### 4.5 `AxlString` — GO for the structure, NO-GO for the skin

Strings are `char`, so §4.1's soundness objection cannot apply, and unlike
`AxlArray` there IS a data accessor (`axl_string_str`), so the hybrid shape
needs no C change. The objection turns out to be a third, different one.

N=200000, warm heap, both orders (µs):

| | ctor (short) | copy (short) | read | append | ctor LONG |
|---|--:|--:|--:|--:|--:|
| skin | 29055 / 29138 | 29336 / 31196 | 4968 / 5511 | **1122** / 1275 | 301056 |
| skin + cached base | 29565 / 30788 | 28849 / 29314 | 677 / **504** | 2095 / 2294 | 310199 |
| standalone, always heap | 13749 / 14398 | 14481 / 14356 | **484** / 549 | 1147 / **1100** | 311547 |
| **standalone, SSO** | **3142 / 3255** | **3034 / 2966** | 971 / 974 | 1120 / 1082 | 339522 |

**SSO is 9.2× on constructing a short string and 9.7× on copying it** — the two
operations firmware does most, on the strings it actually holds (`fs0:\EFI\BOOT`
is 13 bytes; a PCI id key is 9; a config key ~12).

The control is `ctor LONG`, which forces a heap allocation in every variant:
**~300 ms for all four, SSO marginally the worst.** When allocation is
unavoidable they converge exactly, which is the proof that the entire
difference is allocation *avoidance* — and `AxlString` cannot express it. It is
a heap builder by construction; its handle is a pointer to a heap object, so
there is nowhere to put 23 inline bytes.

Two secondary results. Caching the base pointer recovers read cost (7–10×,
exactly as it did for `AxlArray` in §4.1) but costs 2× on append, because every
mutation must re-`sync()` through two out-of-line calls — and it does not touch
ctor/copy, which dominate. And **appending ties across all four**: that is the
case `AxlString` was designed for, and it is good at it.

### 4.4 The interop argument, measured — and it inverts

The case for skinning was interop with C APIs. Counted by files naming each
type:

| | axl-sdk | agt (C++) | softbmc |
|---|--:|--:|--:|
| `AxlArray` | 18 | **9** | 0 |
| `AxlHashTable` | 22 | **0** | 0 |
| `AxlTree` | 3 | **0** | 0 |
| `AxlNTree` | 4 | 2 | 0 |
| `AxlRadixTree` | 3 | 0 | 0 |

**Zero C++ consumers touch `AxlTree` or `AxlHashTable`**, so interop is
hypothetical exactly where the skin is cheapest — and the structure with the
most C++ demand, `AxlArray` at 9 files, is the one that must be templated for
soundness. Interop is therefore a *seam*, not a base: `std::span` outward, and a
small `axl::c_array_ref` adaptor for iterating an `AxlArray *` you were handed,
which is AGT's actual direction of travel.

## 5. Decision

| Container | Built | Reason |
|---|---|---|
| `axl::vector<T>` | **DON'T — use `std::vector`** (§2b) | it is free and works, verified under QEMU. §4.1's finding still stands as the reason not to skin `AxlArray` if we ever did write one |
| `axl::unordered_map<K,V>` | **DON'T — use `std::unordered_map`** (§2b.1) | running on both arches. AXL supplies `_Prime_rehash_policy`'s two out-of-line members itself (§2b.2), so no AVX-carrying archive member is linked |
| `axl::map<K,V>` | **DON'T — use `std::map`** (§2b.1) | running on both arches off `tree.o`, which has no undefined symbols. §4.3's 1.4× was measured against a hand-written AVL, which is no longer the alternative |
| `axl::string` | **prefer `std::string`** (§2b), which already has SSO | §4.5's measurement stands as the reason not to skin `AxlString`; it is no longer a reason to WRITE one |
| `AxlNTree`, `AxlRadixTree` | **skin** | domain wrappers, not STL analogs. `AxlNTree`'s links are **public**, so traversal inlines with no C change — the thing `AxlArray` would have needed `axl_array_data()` for |

All of them allocate through `axl_malloc`/`axl_free`, so leak tracking, the
debug fill pattern that exposed §4.1's use-after-free, and the suite's leak gate
keep working unchanged. Sharing the **allocator** is the part of "build on the C
structures" that survives everywhere; sharing the **container** survives only
where the container does not copy objects.

That default is right until a path cannot afford to halt on OOM (§2b.3) or
runs on an application processor, where `axl_malloc` has no boot services to
call. `axl::arena_allocator` (`include/axl/axl-arena-allocator.hpp`) is the
opt-in for both: fixed capacity checked once up front, and a lock-free CAS
bump underneath. Its cost is that `deallocate` cannot reclaim, so an
arena-backed container must be sized for its PEAK — including the buffers it
allocates while growing, which is why `reserve()` up front is a correctness
concern there and not a performance note.

## 6b. Naming: `axl::` lowercase

`axl::result`, `axl::err`, `axl::arena_allocator`. Headers keep the tree's
file convention — `include/axl/axl-arena-allocator.hpp` — and macros stay
`AXL_SCREAMING_CASE`, since namespaces do not contain them.

> **Read this section as reasoning, not as a roster.** It was written while
> `axl::vector` / `axl::string` / `axl::map` were still expected to exist, and
> uses them as the worked examples. §2b.1 settled that they will not: the
> standard containers work. The naming CONCLUSION is unaffected and still
> governs everything the layer does add — the `AxlString` collision it turns on
> is real either way.

**For `string` this is forced, not chosen.** `typedef struct AxlString
AxlString;` already exists in C and STAYS — §4.5 keeps it deliberately as the
streaming builder behind the JSON and XML writer sinks. So the C++ value type
cannot be `AxlString`; the name belongs to a type the C++ layer has to
interoperate with rather than replace. The alternatives are worse than the
problem: `AxlStr` reads as an abbreviation of the thing it is not and collides
conceptually with the existing `axl-str.h` module, and `AxlCxxString` is a name
nobody types twice. A namespace dissolves it — `axl::string` and `::AxlString`
are visibly different, and converting between them reads naturally.

Since `string` forces a namespace, `axl::string` beside `AxlVector` would be
the worst outcome available: two conventions in one library, split on the
accident of which C names happen to be taken.

On the merits it is also right:

- **It reads correctly next to the standard.** The central decision here is
  write containers, no algorithms (§2), so this code is full of
  `std::sort(v.begin(), v.end())` and `v | std::views::filter(...)`.
  `axl::vector<int>` beside `std::sort` reads as the same kind of thing from a
  different provider; `AxlVector<int>` reads as two worlds meeting.
- **It marks the layer boundary.** `axl::` is the C++ API; `axl_` / `Axl` is
  the C API called from C++. Given that some containers wrap a C structure and
  some deliberately do not (§4), seeing which side of that line you are on at a
  glance is worth having.
- **Namespaces cost nothing here** — C++23, no ABI concern, no macro pollution.

**This is a deliberate divergence from AGT**, which uses `AgtButton` with no
namespace. That is right for AGT: a widget toolkit modeled on FOX/MFC, where
`AgtButton` is idiomatic and nothing in C collides with it. A container library
sitting directly alongside `std::` is a different problem. Recorded as a
divergence so it does not later read as drift — the cost is that AximCode now
has two C++ conventions, which was weighed and accepted.

**Corollary: do not wrap C types into `axl::` for its own sake.** `axl::vector`
exists because there is no `std::vector`. There is no reason for an
`axl::json_writer` that forwards to `axl_json_writer_*`. The C++ layer earns
its place only where C++ gives something C cannot — RAII, templates, iterator
concepts.

## 6. Error handling

`std::expected<T, AxlStatus>` — standard, freestanding-available, and it matches
the house convention that errors are **queried, not thrown** (JSON decision 16).
It answers the question `-fno-exceptions` otherwise leaves open: what
`push_back` does on OOM. ETL's answer is no heap at all; EASTL's is an allocator
returning null. Ours is a value.

**`abort` is DEFINED**, in `libaxl-cxx.a` (`src/runtime/axl-cxxabi-ops.cpp`),
so `.value()` is a diagnosable halt rather than a link failure. The `-fno-
exceptions` lowering is not specific to `std::expected` — measured, both
`std::optional::value()` and `std::get` emit the same call — so the real
question was never "should `.value()` work" but whether an arbitrary future use
of a freestanding std header fails to link with an undefined reference naming
nothing a caller recognises. It lives in the C++ runtime TU rather than
`compat/stdlib.h` so a pure-C consumer does not acquire an `abort` it never
asked for.

It is still a crash, and `axl::result`'s docstring says so: `value_or`,
`has_value` or `operator*` after a check are what belongs on a path that can
actually fail.

### 6a. Freestanding vs hosted — RETIRED (T3), kept as the measurement

> **There is one C++ mode as of T3, and no user-facing flag.** This section is
> what the two modes WERE and why, kept because the header table below is the
> measurement that justified retiring them rather than an argument for keeping
> them. `axl-c++ --hosted` is REJECTED with a message naming its removal, and
> the CMake package's `HOSTED` keyword raises `FATAL_ERROR`. The rule of thumb
> at the end is superseded: include what you need.
>
> C stays freestanding, so the end state is one mode PER LANGUAGE — see the
> end of §6a-PLAN for why that is not a compromise.

Two compile modes, and the difference is **which libstdc++ headers you may
include**, not which libc you link.

`-ffreestanding` sets `__STDC_HOSTED__` to 0 (measured). libstdc++ consults
that in `bits/requires_hosted.h` and refuses most of the library. Two things
lift the refusal, and *only together*: dropping `-ffreestanding`, and dropping
the `include/compat` shims — compat's `typedef void FILE` collides with the
real `<stdio.h>` that hosted libstdc++ pulls in, and that collision is what
blocks `<string>` and `<memory>`. `-ffreestanding` is per-TU and appending
`-fhosted` after it does **not** override it, which is why this cannot be a
flag a caller passes through; `axl-c++ --hosted` has to change what the driver
builds.

Measured, this toolchain, `-std=c++23`:

| header | freestanding | notes |
|---|---|---|
| `<exception>` `<typeinfo>` `<new>` | **OK** | the C++23 freestanding subset — language support, not library |
| `<cstdint>` `<cstddef>` `<cstdlib>` | **OK** | |
| `<cstring>` `<vector>` `<string>` | **refused** | needs `--hosted` |

**Exceptions need freestanding only.** The three headers the exception runtime
depends on are exactly the three the freestanding subset guarantees, because
the language itself requires them — `throw` needs `<new>` for the exception
object and `<typeinfo>` for matching. So `axl::exception`, `try`/`catch`, the
unwinder and the ABI layer all build in the default mode. `--hosted` buys the
*containers*, and nothing about exceptions.

The two modes are not a fork: C sources are untouched, `libaxl.a` stays
freestanding, and a mixed image links and runs. The whole libc footprint of a
hosted TU using `vector` + `string` + `map` + `unordered_map` is `memcpy`,
`memmove`, `memset`, `memcmp` and `strlen`, all of which `libaxl.a` already
defines — which is why `--hosted` needs no `libstdc++.a`.

Rule of thumb, as it stood: **default (freestanding) unless you want std
containers.** Superseded by T3 — there is nothing to choose.

### 6a-PLAN. Retiring the two modes — measured, and blocked on x64

**Goal: one compilation mode, no `--hosted` flag.** It exists only to work
around borrowing a libstdc++ built for a different C library, and where that
premise is removed the flag has no job.

**Preference on record: do NOT add host-package dependencies.** Anything
that requires a consumer to install `libstdc++-static` (RHEL ships it in
CRB, which is not enabled by default, so `dnf install` would fail outright)
or to enable extra repos is out. Compiling a source subset with our own
flags is acceptable; depending on a packaged archive is not.

| task | arch | state |
|---|---|---|
| T1. Drop `-ffreestanding` + `-Iinclude/compat` for C++ TUs | aa64 | **PROVEN 7/7** — containers, `<stdexcept>`, `vector::at` throwing through libstdc++ frames, destructor during unwind |
| T2. Move x64 C++ to AXL's own `x86_64-elf-g++` | x64 | **DONE** — see 6a-T2 below. Not the flag drop this row used to describe: the flags were already right, the COMPILER was the host's |
| T3. Delete the `--hosted` flag from `axl-cc` / `axl-c++`, `CXXFLAGS_HOSTED*` from the Makefile, and the `--hosted` prose from README/§6a | both | **DONE** — see 6a-T3 below. Both spellings hard-error; only a diagnostic arm remains |
| T4. Retire `include/compat/` for C++ (C keeps it) | both | **MOOT** — `include/compat/` was deleted outright when C moved to the bare-metal cross (`AXL-Libc-Substrate-Design.md` §4.1b), so there was nothing left to retire for C++. The row outlived its subject |
| T5. Update `AXL-Cxx-Stdlib-Surface.md`, which is organised around the freestanding/hosted split | both | **DONE** — restructured around one mode, and its central "not supported" entry (the unwinder) flipped to done in the same pass |

**C stays freestanding, and that is not a compromise.** Dropping
`-ffreestanding` for C would pull glibc's `<stdio.h>` with its real `FILE`,
`errno` and locale machinery, none of which AXL implements.
`include/compat/` exists so C code gets the standard *spellings* without the
glibc *implementation*. So the end state is one mode PER LANGUAGE and no
user-facing flag — not one mode overall.

**Why T2 is blocked.** libstdc++'s headers are configured at build time for
a specific C library, and the two cannot be mixed:

- newlib's headers + the host's libstdc++ headers fails in
  `bits/c++locale.h` — the host's expect glibc's `uselocale`/`locale_t`.
- The host's headers alone leave `<stdexcept>`'s classes undefined
  (`std::runtime_error`, `std::out_of_range`), which `vector::at` needs.
  Compiling `stdexcept.cc` from GCC source hits the COW-vs-SSO string ABI
  variants, which a configured build resolves and a piecemeal one does not.

So x64 needs a **matched set**, and a matched set arrives as a toolchain.
There is no published `x86_64-elf` bare-metal GCC (bootlin ships only
linux-gnu/musl; crosstool-NG is not packaged), so T2 means building and
hosting one — which is a distribution commitment, not a code change, and is
why this is a plan rather than a change.

**T2 IS NO LONGER BLOCKED.** The toolchain was built:
`toolchain/x86_64-elf/build-toolchain.sh` produces gcc/g++, newlib, and a
`libstdc++`/`libsupc++` configured for a freestanding target — the matched set
this section says x64 needs. The same demo that proved T1 on aa64 now passes
**7/7 on x64**: `std::vector`/`string`/`map`, `std::runtime_error` caught as
`std::exception`, `vector::at` throwing from *inside* libstdc++, and a
destructor running during the unwind — with no `-ffreestanding` and no
`include/compat`. Details and the four build traps are in
`AXL-Cxx-Toolchain-Handoff.md`.

What remained at that point was not a blocker but wiring: `axl-cc`/`axl-c++`
and the Makefile still selected the host g++ for x64, so T3–T5 waited on that
rather than on any unanswered question. **That wiring is 6a-T2 below, and it is
done** — T3–T5 are now unblocked. The distribution commitment is still real — the
toolchain should install to `/opt` via a script mirroring
`install-arm-toolchain.sh`, because SDK consumers who install from the `.deb`
or `.rpm` have no source tree for a `toolchain/`-relative path to resolve
against. (Done: `scripts/install-toolchain.sh`, paths in
`scripts/axl-toolchains.conf`.)

#### 6a-T2. What T2 turned out to be — the compiler, and one silent defect

**DONE.** `axl-cc`, the Makefile and the generated CMake package all select
`AXL_X64_GXX_DEFAULT` now, so C++ compiles bare-metal on both arches exactly as
C already did. That was the last host input the SDK had
(`AXL-Libc-Substrate-Design.md` §4.1d), and the `.deb`/`.rpm` `--depends` list
is empty as a result.

**The two paragraphs below this one predicted the wrong work.** They are kept
because the prediction was reasonable and the measurement that overturned it is
the expensive part to rediscover — see *What the link actually does* after
them. Both were written from a hand-linked spike, and a spike's link is not
`axl-cc`'s.

##### The real blocker: `.ctors`, and nothing else

GCC's `x86_64-*-elf` target ships `HAVE_INITFINI_ARRAY_SUPPORT 0`, so the
compiler emits global constructors into the legacy `.ctors`. AXL's crt0 walks
`.init_array` and only that (`src/runtime/axl-cxxabi.c`), and the linker
scripts' `__CTOR_LIST__`/`__CTOR_END__` are read by nothing. So **every global
constructor silently did not run** — the consumer's, and the 26 objects' worth
inside the toolchain's own `libstdc++.a` (`libsupc++`'s emergency exception
pool among them). No diagnostic anywhere; the ctor fixture in
`test-cxx-hosted-qemu.sh` is what caught it.

aa64 never showed this because the aarch64 port forces `.init_array` at the
target level — its `auto-host.h` carries the same `0`, so the value is not the
discriminator and reading it off aa64 would mislead.

Fixed at the source, in the toolchain: `--enable-initfini-array`, published as
`14.3.0-axl2`. The alternative — teaching AXL to walk `.ctors` backward — was
weighed and rejected: it doubles the init path permanently to accommodate a
setting the toolchain can simply have. `build-toolchain.sh` now asserts both
halves (the compiler's output AND the shipped `libstdc++.a`), because a flag
that is silently dropped reproduces the identical silent failure.

##### What the link actually does — measured, not inferred

The paragraphs below say `libaxl-cxx.a` becomes a multiple-definition error
against `libstdc++.a`. **In `axl-cc`'s link it does not**, and the reason is
archive selection: `libaxl-cxx.a` is named FIRST, and an archive member is
pulled only for a symbol still undefined when the linker reaches it. So the 51
colliding definitions in `libstdc++.a` are never selected at all.

Measured on the one path that names both archives, `-frtti --hosted`
(`ld -y`):

    _Znwm           <- libaxl-cxx.a(axl-cxxabi-ops.o)
    __dynamic_cast  <- libstdc++.a(dyncast.o)

Both archives contribute to one link, no collision, and the image runs
(`test-cxx-hosted-qemu.sh` asserts `typeid` and `dynamic_cast` under QEMU).
The spike hit the error because it named the toolchain libs FIRST; that is a
property of that command line, not of the two archives.

So T2 changed no link, and the default link still names no toolchain library —
which is what keeps the SDK self-contained, and is the §8 constraint that made
`axl-cxx-rbtree.cpp`/`axl-cxx-hash.cpp` worth writing. `libaxl-cxxrt.a` (the
newlib glue described below — it exists, in `src/cxxrt/`) belongs to the
EXCEPTIONS build, `AXL-Cxx-Unwinder-Design.md` §U2/§U3, and is still unlinked
by anything.

#### 6a-T3. One C++ mode — what the flag's removal actually touched

**DONE.** `-ffreestanding` is off the C++ compile line in all three build paths
(`axl-cc`, the Makefile, the generated CMake package), so the containers need
no flag. C keeps it, so the end state is one mode PER LANGUAGE.

**Both spellings are REJECTED, not tolerated.** An earlier revision of this
task made them warned no-ops on compatibility grounds; Mike's call was that he
owns every consumer and updating them is trivial, so a flag that selects
nothing should fail rather than linger in build scripts for years. `--hosted`
exits 1 with a message naming the removal — an arm kept purely to DIAGNOSE,
since deleting it outright would leave the generic "unknown option" that says
nothing about why. CMake's `HOSTED` stays in the `cmake_parse_arguments` list
for the same reason and raises `FATAL_ERROR`: an unrecognised keyword there is
silently appended to the SOURCE list, so deleting it would produce "cannot
find source file HOSTED", which names neither the keyword nor the cause.

**Two things the flag was carrying that had nothing to do with compile mode**,
and both are now derived from the objects instead:

- **Which archive the link needs.** `--hosted` implied `libaxl-cxx.a` even with
  zero `.cpp` on the command line, because the staged build (`axl-c++ -c
  a.cpp` then `axl-c++ a.o -o app.efi`) reaches the link with only an object.
  That now comes from an `nm -u` scan for Itanium-mangled / `__cxa_*` undefined
  symbols, sharing one pass with the pre-existing RTTI detection. This CLOSES a
  gap rather than replacing one: the same staged flow *without* `--hosted` was
  already broken, and failed on an undefined `operator new` naming no flag.
- **`-frtti`'s libstdc++.** It was gated on `--hosted && RTTI_LINK`; it is now
  gated on `RTTI_LINK` alone, which is what it always meant.

**`deps/lzma` comes off the C++ include path unconditionally.** It was filtered
only for the five TUs compiled hosted. `deps/lzma/errno.h` shadows the real
one, and the shadowing is stealthy — nothing includes it directly, but
`#include <string>` reaches `ext/string_conversions.h`, which needs `errno` and
would get a vendored stub with no `ERANGE`. Every C++ TU can reach `<string>`
now, so the narrow filter would have been a trap waiting for the next
`#include`.

**One question this opens and does not answer:** `axl::string` exists because
`<string>` was unavailable freestanding. It is always available now, so
`axl::string` is a size choice rather than a necessity. That belongs to T5
(`AXL-Cxx-Stdlib-Surface.md`, which is organised around the split this task
removed) and is deliberately not decided here.

#### (superseded) T2 is bigger than "drop two flags": `libaxl-cxx.a` is superseded

Measured against the x64 bare-metal toolchain, `libaxl-cxx.a` exports **54**
symbols and **51 of them are already defined by `libstdc++.a`/`libsupc++.a`** —
`operator new`/`delete` in all sixteen forms, `__cxa_pure_virtual`, the
out-of-line `basic_string`, `_List_node_base` and `_Rb_tree` members, and
AXL's own `_Prime_rehash_policy`. The remaining three do not survive scrutiny
either: `ceil` is in `libm.a`, `abort` is in `libc.a`, and
`__libc_single_threaded` is a glibc-ism that **nothing in this toolchain's
libstdc++ references** (0 undefined refs).

So on the bare-metal path the archive is not merely redundant, it is a
multiple-definition ERROR — which is why the spike link only succeeded once it
was dropped. That was the correct configuration, not a workaround.

The consequence for sequencing: T2 cannot be "compile C++ TUs differently". It
has to select a different LINK as well — toolchain libs in, `libaxl-cxx.a`
out. What AXL still owes that link is the newlib glue the spike carried in
`ehenv.c`: `malloc`/`free`/`realloc`/`calloc`/`posix_memalign` onto `axl_*`
(which is also what keeps allocation tracking working, since the toolchain's
`operator new` calls `malloc`), the four newlib stubs (`getenv`, `strtoul`,
`_impure_ptr`, `__xpg_strerror_r`), an AXL-owned `sbrk` (newlib's references
the linker symbol `end`, which AXL's scripts do not define), and the
`__register_frame` / `__freeres` + `__deregister_frame` pair from
`AXL-Cxx-Unwinder-Design.md` §U3. That glue is a NEW small archive, not an
edit to `libaxl-cxx.a`, because the two are mutually exclusive.

Note this preserves the §U2 byte-identity constraint for free: a C image links
neither archive and is unchanged.

Note the exception runtime is NOT blocked by any of this: it is solved on
both arches (`AXL-Cxx-Unwinder-Design.md` §2-RESULT), and on x64 by
compiling libsupc++ from source with AXL's flags, which needs no package.

### 6b. The callback boundary is `noexcept`, and the compiler enforces it

Every public callback typedef carries `AXL_CB_NOEXCEPT` — `noexcept` in
C++, nothing in C. 88 of them across 46 headers, gated by
`make check-cb-noexcept`.

The reason is measured, not stylistic. AXL invokes consumer callbacks from
its own C frames, and those frames carry no landing pads, so an exception
unwinding through them runs **no cleanup at all**:

| middle frames built as | TPL after the catch |
|---|---|
| today's flags (`-fno-exceptions` C++ + plain C) | leaked 0 -> 16 |
| C++ TU rebuilt `-fexceptions` | leaked 0 -> 8 |
| + C frame uses `__attribute__((cleanup))`, gcc default | leaked 0 -> 8 |
| + **`gcc -fexceptions` as well** | **restored to 0** |

Note the third row: `__attribute__((cleanup))` alone does nothing, because
gcc emits no landing pad for it without `-fexceptions`. So today every one
of the 30 `AXL_AUTOPTR`/`AXL_AUTO_FREE` sites leaks on any throw that
crosses it — and a leaked `RaiseTPL` is worse than a leak, because
returning to the shell above `TPL_APPLICATION` wedges the machine at
*every* raised level, silently on x64.

Since C++17 `noexcept` is part of a function's type, so a throwing
callback simply does not convert. A consumer that wants exceptions
catches at its own boundary and returns a status — the trampoline
pattern, measured clean: TPL restored, error propagated, nothing unwound
through a libaxl frame. A throw that escapes anyway hits
`std::terminate` at the throw point: fatal, but loud and located rather
than a silent wedge, and gcc's `-Wterminate` often catches it at compile
time first.

This is the invariant any future unwinder work rests on — **exceptions
never cross the C boundary** — and it is worth having on its own merits
regardless, since it turns an unwritten rule into a compile error.

## 7. C-side improvements this surfaced

Independent of the C++ work; each benefits every C consumer.

1. **Open addressing in `AxlHashTable`** — the only real algorithmic win found
   (~2× insert, ~20% hit). Not a swap: it made misses 30–60% *worse* in the
   probe, so it needs a design pass of its own.
2. **`% bucket_count` → `& (bucket_count - 1)`, 7 sites** —
   `INITIAL_BUCKETS` is 64 and growth is `* 2`, so the count is *always* a power
   of two and the modulo is a hardware divide for nothing. Measured **−12%
   lookup at N=8k**, −6% at 50k, zero behaviour change.
3. **`axl_array_data()`** — the only thing that would let a C++ skin inline
   element access (§4.1, third row). Worth it only if an `AxlArray`-backed
   container is ever wanted; §5 says it is not.

## 8. Open questions

- ~~`axl::string` over `AxlString` is assumed, not measured.~~ **CLOSED — see
  §4.5.** Measured 2026-08-04: template with SSO. `AxlString` stays exactly
  where it is good, as the streaming builder behind the JSON/XML writer sinks;
  `axl::string` is the value type, and converts to/from `const char *` and
  `AxlString *` at the seam.
- ~~Naming and namespace.~~ **CLOSED 2026-08-04 — see §6b.** `axl::` lowercase.
  The examples there name `axl::vector` / `axl::string` / `axl::map`; §2b.1
  since settled that those are `std::` types and the layer writes none of them.
- **How `libstdc++.a` reaches an SDK consumer. CLOSED 2026-08-08 — it does
  not reach them at all.** Mike's call was "make the SDK self-contained", and
  measurement made that free rather than costly: `--hosted` was pulling
  exactly TWO archive members, `tree.o` and `hash_bytes.o`, each with zero
  undefined symbols. `src/runtime/axl-cxx-rbtree.cpp` and `axl-cxx-hash.cpp`
  supply those eleven functions clean-room under Apache-2.0 — the same footing
  as `axl-cxx-rehash.cpp` and `src/data/axl-rb-tree.c` — so `axl-c++ --hosted`
  no longer names `libstdc++.a` on the link line. The SDK ships nothing of
  GCC's, which means NONE of the analysis below applies to it any more; it is
  kept because it is what made the decision, and because `-frtti` is the one
  remaining exception (see the end of this entry).

  The analysis that got here:

  Read from the GCC Runtime Library Exception 3.1 as shipped with the ARM
  toolchain (`license.txt`), corroborated against the FSF's RLE FAQ:

  *Shipping the produced `.efi` is unambiguously fine.* RLE §1 grants
  permission to "propagate a work of **Target Code** formed by combining the
  Runtime Library with Independent Modules ... You may then convey such a
  combination under terms of your choice". Target Code is compiler output "in
  executable form"; our sources are Independent Modules (a file that "makes
  use of an interface provided by the Runtime Library, but is not otherwise
  based on" it); GCC makes the Compilation Process Eligible. An Apache-2.0
  `.efi` containing `tree.o`'s red-black tree is covered, with no obligation.

  That also settles `src/runtime/axl-cxx-rehash.cpp`: it uses an interface the
  Runtime Library provides (the `_Prime_rehash_policy` declaration) and is not
  otherwise based on it — written from the contract, with its own prime table.
  An Independent Module.

  *Shipping `libstdc++.a` itself is a DIFFERENT act, and the RLE does not
  cover it.* §1 speaks only of Target Code "formed by combining the Runtime
  Library with Independent Modules"; the bare archive is not combined with
  anything. The FSF FAQ is explicit: "if you distribute libstdc++ as an
  independent library, you will need to follow the terms of the GPL when doing
  so", and object-code form pulls in GPLv3 §6's corresponding-source duty.

  That would not make AXL GPL — GPLv3 §5 mere aggregation — but it would put
  GPL-3 obligations on the SDK *package*: corresponding source for that exact
  libstdc++ build, the license texts, and no added restrictions on those
  files.

  **-frtti is the one exception, and it is a consumer-side one.** `typeid` and
  `dynamic_cast` need `__cxxabiv1::__class_type_info`'s vtables and
  `__dynamic_cast` from libsupc++, a far larger surface than eleven functions.
  RTTI is opt-in and off by default, so the DEFAULT link is self-contained and
  `axl-c++` names `libstdc++.a` only when `-frtti` is passed. That is a
  consumer linking their own installed copy, not us redistributing one — and
  the `.efi` they produce is Target Code, which RLE section 1 covers
  unambiguously.

  **Superseded recommendation: keep resolving it from the installed toolchain
  and ship nothing.** NOTE 2026-08-15: `--hosted` no longer exists (T3 removed
  it), and the toolchain is now AXL's own on both arches, so the only remaining
  `-print-file-name=libstdc++.a` is on the `-frtti` path — which is exactly the
  exception this entry already flags at the end. The conclusion is unchanged and
  the mechanism named here is not; kept as written because it is the reasoning
  that settled the question. Vendoring the two members is legally
  permissible but buys the consumer out of one `dnf install libstdc++-static`
  in exchange for GPL-3 conveyance duties on every SDK tarball plus an ABI
  hazard when their libstdc++ differs from the one we vendored. The decision
  is Mike's; the legal uncertainty that made it hard is not there any more.
- **Whether halt-on-OOM is now the SDK's contract. STILL OPEN — Mike's call.**
  Shipping the `std::__throw_*` stubs in `libaxl-cxx.a` means every C++
  consumer inherits them, and §2b.3 made `operator new` halt as well. That is
  the correct behaviour for a standard container and it is a real narrowing
  against AXL's C error model, where OOM is a value and some contracts are
  *degradation* rather than propagation. `axl::arena_allocator` is the escape
  hatch, not a general answer: it only helps where a bounded worst case can be
  computed up front.
- **Small-scalar specialization** for the map skins, so `unordered_map<int,int>`
  does not box. Only if something measures as hot.
- ~~The gate surface.~~ **CLOSED — C0 shipped 2026-08-04 (`b4795ed9`).** It was
  FOUR places, not the three listed here: `Doxyfile FILE_PATTERNS`,
  `check-doc-coverage.py`, `lint.sh`, and **`scripts/install.sh`**, which copies
  `include/axl/*.h` into the SDK prefix and therefore never shipped the header
  at all. The fourth surfaced by USING it — the first consumer that tried to
  include the header from an installed SDK got "No such file or directory". The
  list of places that must learn a new file extension is discovered by using
  it, not by enumerating; assume the next extension has one more.

  Turning the C++ lint pass on immediately found a pre-existing conformance
  error: `operator new` declared `noexcept` where `<new>` declares it without
  one, which gcc had tolerated for as long as the file existed and clang
  rejects outright. Nothing had noticed because the compile database was built
  without `AXL_CPP=1` and contained zero C++ TUs. The pass now carries a count
  guard so a DB that loses its C++ entries fails loudly rather than inspecting
  nothing.

## 9. Phasing

Deliberately foundation-first. The C++ JSON API is the *last* item, not the
first, because it would otherwise set every convention unilaterally.

| Phase | Content |
|---|---|
| **C0** | **DONE 2026-08-04 (`b4795ed9`).** Gate surface (four places, §8), `axl::result<T>` = `std::expected<T, AxlStatus>` + `axl::err()` in `axl-cxx.hpp`, and `abort` defined in `libaxl-cxx.a` |
| **C1** | **DONE.** `axl-c++ --hosted`, the five `std::__throw_*` stubs, the `ceil` shim and AXL's own `_Prime_rehash_policy` in `libaxl-cxx.a`, `operator new` halting instead of returning NULL, and `make check-no-avx`. Verified running on both arches (§2b.1). **Open: the distribution question — how `libstdc++.a` reaches an SDK consumer (§8)** |
| **C2** | `std::string` — WORKS as-is. A seam to/from `const char *` and `AxlString *` is the only thing left to write |
| **C3** | `std::vector` — WORKS as-is. `axl::arena_allocator` shipped with C1. Remaining: `axl::c_array_ref` for the 9 AGT sites holding an `AxlArray *` |
| **C4** | `std::map` / `std::unordered_map` — **DONE**, running on both arches. The `AxlTree` / `AxlHashTable` skins are not needed |
| **C5** | Domain wrappers: `AxlNTree`, `AxlRadixTree`, AGT's draw-context guard migrates |
| **C6** | C++ JSON API — RAII container scopes, templated `add`, range-for over the reader. Four faces, so `w.splice(r["items"])` falls out of `axl_json_write_token` |

**Acceptance test for C1–C3: AGT shrinks.** If those 84 `axl_free` calls, 12
owning `char *` members and 4 `AxlArray *` members do not go away, the
foundation did not fit the real consumer. That is the measure, stated up front
rather than discovered later.

## 10. Prior art consulted

| | What it offers | Verdict here |
|---|---|---|
| [ETL](https://github.com/ETLCPP/etl) | Fixed-capacity, **no heap ever**, no RTTI, configurable error checking, C++03 | Constraints match; solution does not. We have a heap, and its no-heap model would forbid `axl::vector` growing at all. Its *allocator-free* variant is still interesting for AP context, where boot services are unavailable — `AxlArena` is AXL's existing answer there |
| [EASTL](https://github.com/jwdevel/EASTL) | Per-container allocators, performance-first | The allocator-parameterization is the borrow, not the containers |
| [ArduinoJson](https://arduinojson.org/) | The embedded standard; DOM, header-only, no exceptions/RTTI | DOM-shaped ergonomics need a tree and an allocator; wrong layer |
| [Glaze](https://github.com/stephenberry/glaze) | C++23 compile-time reflection, `-fno-exceptions` clean | Needs `std::string` and hosted containers. Its reflection model is the shape a future struct-mapping phase would want, and C++23 has no reflection to do it without a macro DSL |
| [RapidJSON](https://rapidjson.org/writer_8h_source.html) | SAX streaming writer, asserts on mismatched pairing | Already AXL's C API feature-for-feature |
| [UniValue](https://github.com/jgarzik/univalue) | Deliberately minimal templates/memory for embedded | Confirms the ecosystem clusters DOM-vs-streaming with nothing between |
