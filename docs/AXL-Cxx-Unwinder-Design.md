# Tier 2 — the unwinder, and the three quarters of it that need no unwinder

> **Status: U0 DONE 2026-08-09. §2 DECIDED 2026-08-10 — YES, with the
> boundary invariant of §6b. A working `try`/`catch` demo ran under QEMU
> on x64 (9/9) during the decision spike; U1+ is now a build-out, not a
> question. See §2-RESULT and the corrected §U1 prior art — two verdicts
> in the original table were wrong.**
> Measured on this tree, not estimated.
>
> The one-line summary: **`AXL-Cxx-Stdlib-Surface.md` §4 tier 2 conflates four
> things with wildly different costs, and three of them do not need an unwinder
> at all.** One of those three is already proven running under QEMU. The
> fourth — real `try`/`catch` — does need one, and it reverses an invariant
> `axl-cxx.hpp` calls "not negotiable", so it is a decision before it is a
> project.

---

## U5 — `--no-eh-frame`, and the crash a naive opt-out actually produces

Added 2026-08-17, on a request from AGT: a C++ image that uses neither
exceptions nor iostreams pays for a frame table it can never consult, and §U2
measured that KEEP at +16.8% on a C image. AGT measured 11.6-11.8% of each of
its own binaries, ~5.3 MB across 34 tools.

**The request was to select the non-`_eh` script. That alone does not work, in
two ways that only measurement surfaced.**

**It does not LINK.** `axl-cxxrt-eh.o` — on every C++ link since P4 —
references `__eh_frame_start`, which only the exceptions script defines, and
every build path passes `--no-undefined`. So the change is an OBJECT-SET
change, not a script change.

**It does not degrade, it CRASHES.** The expectation, stated in the request and
in this tree's own comments, was that a throw without a frame table reaches
`std::terminate` through `_URC_FATAL_PHASE1_ERROR` and loses the type name and
`what()`. Measured on x64: `vector::at(99)` in such an image takes an unhandled
CPU fault, dumps registers, and WEDGES the machine — QEMU had to be killed by
timeout after 34 s of spin. That is the failure mode `axl-cxxrt-stubs.c`'s
`_exit` docstring says AXL routes `abort` through `axl_exit` specifically to
avoid.

An empty-but-valid `.eh_frame` — a lone CIE-list terminator, so
`__register_frame` gets a well-formed table with no FDEs — was tried and faults
identically. **The fault is inherent to unwinding a frame with no FDE**, not to
the table being absent, which is why the fix cannot live in the linker script.

### What shipped instead

`axl-c++ --no-eh-frame` selects the default script AND swaps
`axl-cxxrt-eh.o` for `axl-cxxrt-nothrow.o`, whose `__cxa_throw` intercepts
**before the unwinder is entered**: it prints the type and `what()`, then
`axl_exit`s. So the image keeps the diagnostic the frame table was being held
for, and drops the table.

Three mechanism details, each of which cost a wrong first attempt:

- **`--wrap=__cxa_throw`, not a definition.** libsupc++'s `eh_throw.o` lands on
  the link anyway — the terminate handler needs other symbols from it — so a
  second definition is `multiple definition`. `--wrap` collides with nothing
  and leaves `__real___cxa_throw` reachable.
- **`what()` needs no unwinder.** `__cxa_throw` receives the `std::type_info *`,
  and `typeid(std::exception).__do_catch(tinfo, &obj, 1)` answers "is this
  catchable as `std::exception`" using `type_info`'s own virtual. The call goes
  ON THE CATCH TYPE with the thrown type as argument; the reverse compiles,
  returns false for everything, and silently costs the `what()` line —
  confirmed by building it both ways and running both.
- **The saving is bigger than dropping the KEEP.** With nothing calling the
  real `__cxa_throw`, `--gc-sections` collects more of libsupc++'s throw path
  too, so the flag reclaims more than the table it removes.

### How much it saves, and why a percentage does not transfer

**Do not budget from a percentage.** Measured on
`test/integration/cxx-hosted-throw.cpp` — a SMALL fixture — this flag is -18%
on x64 and -15% on aa64. AGT then measured its own 34-binary fleet and got
**-12.5 to -13.9% on x64 and -3.1 to -4.4% on aa64**: x64 close, aa64 off by
~4x. Their decomposition is the right way to state it, and it is quoted here
because it travels where the percentage does not:

    saving  =  .eh_frame  +  the gc'd throw path

| term | x64 | aa64 |
|---|---|---|
| `.eh_frame` | **scales with the program** — 67 KB (kitchen-sink) to 93 KB (axedit) | **constant** — 8,264-8,328 B across four very different programs, a 64-byte spread |
| gc'd throw path | ~13.5 KB, constant | ~16.5 KB, constant |

**The mechanism is a per-target DEFAULT**, which is worth knowing because it
lets a consumer predict their own number rather than copy someone else's.
Measured with `gcc -Q --help=common`, and on an identical six-function C TU at
`-O2`:

| toolchain | `-fasynchronous-unwind-tables` | `.eh_frame` under `-fno-exceptions` |
|---|---|--:|
| `x86_64-elf` | enabled | 144 B |
| `aarch64-none-elf` | disabled | **0 B** |

So under `-fno-exceptions` x64 emits per-function CFI and aa64 emits none:
on x64 the table grows with the consumer's own code, while on aa64 the consumer
contributes none of it and what remains is fixed CIEs from the prebuilt
libstdc++/libsupc++ objects.

**It is NOT an architecture property, and an earlier revision of this section
said it was.** That claim ("x86-64's psABI requires async unwind tables") is
refuted by the same measurement one triple over:

    aarch64-linux-gnu-gcc   -fasynchronous-unwind-tables [enabled]

Same architecture, opposite default — so the split is bare-metal versus Linux
on aarch64, not aarch64 versus x86-64. Nor is it a hard requirement on x64:
`-fno-asynchronous-unwind-tables` takes that 144 B to 0 cleanly, which a
mandate would not permit.

What actually differs is **asynchronous** versus **on-demand** tables.
aarch64-none-elf emits them when something asks — `-fexceptions` on the same TU
produces 144 B, as does forcing `-fasynchronous-unwind-tables` — while x86-64
additionally defaults the ASYNC form on, which exists for debuggers, profilers
and async signals rather than for exceptions, and which x86-64 leans on because
it omits frame pointers by default.

The numbers and every conclusion below are unaffected; only the causal story
was wrong. Recorded because a default is not a requirement and a target triple
is not an architecture, and this document asserted both.

Which gives the shape to budget against:

- **x64** — table (grows with your code) + ~13.5 KB. Both terms grow, so the
  percentage stays near 13% across tool sizes.
- **aa64** — ~8.3 KB fixed table + ~16.5 KB = **~25 KB flat, whatever the
  binary size**. The percentage is then purely a function of how big the binary
  is, which is why AGT's ranged 3.1-4.4% across tools differing mainly in size.

This is the same trap `axl-cc`'s accepted-cost note fell into from the other
direction (+46,928 `.text` -- but **+100,339 on the `.efi`**, 58,758 -> 159,097 -- on `sdk/examples/containers.cpp`, which
under-predicted a real linked tool by 3-4x). A figure from one fixture is a
figure from one fixture: name the fixture, and prefer a decomposition the
reader can apply to their own program.

### The guard is what makes this a diagnostics trade

`--no-eh-frame` with `-fexceptions` is REFUSED. Together they produce an image
whose `catch` blocks can never run — a throw reaches the interceptor and exits,
silently skipping every handler in the source. That is a correctness failure,
not a size trade, and it is the one outcome the flag must not be able to
produce. Detected from the flag on the command line and from a pre-built object
referencing `__gxx_personality_v0`, because a staged `-c`-then-link build
reaches the link with no source to inspect.

Pinned by `test-cxx-noeh-qemu.sh`, 19 assertions per arch — including that a
non-throwing image is behaviourally identical, that no CPU-fault dump appears,
and that both guard paths refuse AND produce no output file.

---

## 1. What the tier-2 entry says, and what measurement says

The surface doc says tier 2 is ~13 `_Unwind_*` symbols and that it

> Blocks `std::list::sort`, `<stdexcept>`, `shared_ptr`, and real
> `try`/`catch`.

Measured. A translation unit using `std::list`, `list::sort` and
`make_shared`, compiled with the SDK's own flags (`-fno-exceptions
-fno-rtti`), has these undefined symbols:

```
std::__detail::_List_node_base::_M_hook
std::__detail::_List_node_base::_M_transfer
std::__detail::_List_node_base::swap
std::_Sp_make_shared_tag::_S_eq
```

**Zero `_Unwind_*`.** Our own code, compiled without exceptions, never
references the unwinder. The cascade appears only when the link pulls
libstdc++'s `list.o` — 4816 bytes, compiled WITH exceptions — whose undefined
set is:

```
__gxx_personality_v0   _Unwind_Resume   __cxa_call_unexpected
std::__glibcxx_assert_fail
```

So the unwinder is not a prerequisite of `std::list`. It is a consequence of
sourcing five functions from a member that happens to carry landing pads.

This is the same shape as `tree.o` and `hash_bytes.o`, which
`src/runtime/axl-cxx-rbtree.cpp` and `axl-cxx-hash.cpp` already replaced for
exactly this reason.

### Proven, not argued

`_List_node_base`'s five members are doubly-linked-list pointer surgery —
`_M_hook`, `_M_unhook`, `_M_transfer`, `_M_reverse`, `swap`. Written out
against the layout `<bits/stl_list.h>` declares, the object has **zero
undefined symbols**, and:

```
$ ld ... l2.o listprobe.o libaxl-cxx.a libaxl.a      # no libstdc++.a
$ run-qemu.sh --arch X64 l2.efi
list 5 9 42
```

`std::list` with `sort()` and `reverse()`, **running under QEMU, with no
unwinder and no `libstdc++.a`**. That is the whole of "unblocks
`std::list::sort`", delivered for five functions.

### The other three claims

| tier-2 claim | needs an unwinder? | evidence |
|---|---|---|
| `std::list::sort` | **No** | proven running, above |
| `shared_ptr` / `make_shared` | **No** — confirmed in U0 | `_S_eq` IS referenced under `-fno-rtti` (the opposite of what `shared_ptr_base.h:643` reads like at a glance), but it needs no RTTI: libstdc++ compares an identity token by address. Shipped. |
| `<stdexcept>` | **No, and low value** | the classes construct fine; you cannot `throw` them under `-fno-exceptions`, so what you get is a string-carrying struct we already have better answers for |
| real `try`/`catch` | **Yes** | genuinely needs level 1 AND level 2 of the Itanium ABI |

---

## 2. The decision that comes before the project

`axl-cxx.hpp` states:

> `-fno-exceptions` is not negotiable here, so a fallible operation returns
> #axl::result.

Building the unwinder is only worth doing to enable `try`/`catch`. That
reverses the sentence above, and the whole error model (`axl::result`,
`axl::string::bad()`, the stream fail state, `arena_allocator`) was designed
around it. **This was Mike's call, not an implementation detail**, and U0 below
was worth doing either way — so the decision never blocked anything.
**Answered in §2-RESULT below; the points that follow are what it weighed.**

Worth weighing:

- Exceptions under UEFI have no OS to fall back on. An uncaught throw with a
  working unwinder still ends at `std::terminate`; the difference from today
  is a stack unwind first.
- Firmware runs at raised TPL in places, where an unwind crossing a
  `RaiseTPL`/`RestoreTPL` pair without restoring it wedges the machine.
  Nothing in the ABI knows about TPL.
- The gain is real for deeply nested parse/IO code, which is exactly what
  AXL has a lot of.

### 2-RESULT — decided YES, 2026-08-10, and the TPL hazard is contained

Spiked before deciding. Every claim below is measured, not argued.

**The TPL hazard is worse than this section says, and then it is solved.**
Returning to the shell with an unrestored raise wedges the machine at
**every** raised level, `TPL_CALLBACK` as much as `TPL_NOTIFY` — and on x64
release firmware, silently (AArch64 at least asserts `Image->Tpl ==
gEfiCurrentTpl` first). At `TPL_HIGH_LEVEL`, `AllocatePool`,
`ConOut->OutputString`, `CreateEvent`, `SetTimer` and `CloseEvent` all HANG
rather than failing; only `Stall` and `CheckEvent` work.

But the mechanism is broader than TPL: an unwind through `-fno-exceptions` or
plain-C frames runs **no cleanup at all**, because those frames carry
`.eh_frame` (so the unwind walks) but no `.gcc_except_table` (so nothing
runs). Measured:

| middle frames built as | TPL after the catch |
|---|---|
| today's flags | leaked 0 -> 16 |
| C++ TU rebuilt `-fexceptions` | leaked 0 -> 8 |
| + C frame uses `__attribute__((cleanup))`, gcc default | leaked 0 -> 8 |
| + **`gcc -fexceptions` as well** | **restored to 0** |

`__attribute__((cleanup))` alone does nothing without `-fexceptions`. So the
answer is not to make libaxl unwind-safe — that is 440 allocation sites, 59%
of which transfer ownership out of the frame — but to **stop exceptions at
the C boundary**. That invariant now has compiler enforcement: `5bb6af38` put
`AXL_CB_NOEXCEPT` on 146 public callback declarations, so a throwing callback
is a compile error. See §6b of `AXL-Cxx-Design.md`.

**Proof it works.** A `try`/`catch` demo over `axl::exception` ran under QEMU
on x64: **9 passed, 0 failed** — catch-by-base three frames down, derived
handler precedence with state preserved, non-match fall-through, rethrow, all
destructors running in reverse order, and an RAII TPL guard's destructor
firing during the unwind. That last line is the hazard being handled by
ordinary C++, which is the whole argument.

**setjmp/longjmp cannot substitute.** `throw` is a compile error under
`-fno-exceptions`; under `-fexceptions` the compiler hard-wires
`__cxa_throw`/`__gxx_personality_v0` and DWARF tables, and the personality
routine walks real frames — it cannot be backed by a shadow stack the
compiler never pushes to. Redirecting that lowering is
`--enable-sjlj-exceptions`, a GCC *build-time* option neither toolchain is
configured with — and, since §U1-RESULT, one AXL could technically set, as it
now builds its own GCC. There is no reason to: DWARF unwinding works on both
arches.
A chained per-frame `setjmp` registry DOES run cleanup correctly (measured,
1.58 ns and 64 bytes of stack per protected frame with `__builtin_setjmp`),
but it yields macros rather than keywords and **runs no C++ destructors** —
which is most of the value. Useful for C error propagation; not an exception
substitute.

---

## 3. Phase U0 — finish the "no unwinder needed" path (do this regardless)

Same pattern, same file naming, same clean-room footing as the RB tree.

**U0.1 `src/runtime/axl-cxx-list.cpp`** — `_List_node_base::_M_hook`,
`_M_unhook`, `_M_transfer`, `_M_reverse`, `swap`. Clean-room from the declared
layout in `<bits/stl_list.h>`; no GPL source consulted.

**U0.2 `_Sp_make_shared_tag::_S_eq`** — DONE. It IS referenced under
`-fno-rtti`, and it does NOT need `-frtti`; see the U0 result below.

**U0.3 Tests.** The `std::list` case belongs in
`test/integration/cxx-hosted-selftest.cpp` beside the RB-tree differential
work: randomized insert/erase/splice/sort/reverse against a sorted reference,
plus a structural check that the ring stays a ring (`_M_next`/`_M_prev`
consistent, `n->next->prev == n` for every node, list length matching the walk
in both directions). Reverse traversal is a distinct code path and gets its
own assertion — that is what caught the `_Rb_tree_decrement` sabotage.

**U0.4** Update `AXL-Cxx-Stdlib-Surface.md` §3b: `std::list` moves out of the
"cannot link" table. Correct the tier-2 entry in §4 so it stops claiming
`std::list::sort` and `shared_ptr` need the unwinder.

**Acceptance:** `std::list` sort/reverse/splice runs under QEMU on both
arches, with `libstdc++.a` absent from the link line, and a sabotage of any
one of the five functions turns the structural assertion red.

### U0 RESULT — done, with two findings

Shipped as `src/runtime/axl-cxx-list.cpp`: the five `_List_node_base`
members, `_Sp_make_shared_tag::_S_eq`, and `__libc_single_threaded` (a
seventh symbol the plan did not predict -- glibc's single-thread flag, which
`shared_ptr` reads to take the non-atomic refcount path; under UEFI it is
a deliberate policy -- see the `@warning` at its definition -- and `weak`, so
a consumer on an AP or inside a notify function can override it). 120 hosted
assertions across both arches.

**U0.2 landed better than predicted.** The plan expected `_S_eq` to need
`-frtti` and therefore to be consumer-side. It does not: with RTTI off
libstdc++ compares an *identity token* (`_S_ti()` returns a zero-filled
static used only for its address), so `_S_eq` is an address comparison and
needs no RTTI at all.

**Two sabotages were NOT detected, and both are correct.** `_S_eq` is
unreachable in a uniform image -- and for a narrower reason than the first
draft of this said: `std::get_deleter<D>` is itself
`#if __cpp_rtti ... #else return 0;`, so with RTTI off `_M_get_deleter` is
never ENTERED and neither disjunct runs. The symbol exists only because the
override lands in a vtable. Sabotaging it to `return true` passes too, which
is the stronger statement. `__libc_single_threaded` is a
performance flag: setting it to 0 selects the atomic refcount path, which is
still correct. Both are recorded at their definitions rather than left as
untested code with no explanation. A third sabotage (`_M_transfer`'s
self-splice guard) was NOT detected on the first pass and that one WAS a real
gap -- nothing exercised `this == last`; a self-splice case was added and it
now fires.

**A follow-up review found a bigger one of the same class.** `swap`'s
BOTH-POPULATED branch -- the largest in the file -- had no caller at all:
every swap in the suite, including the three inside `sort()`, has one side
empty. Five separate sabotages of its eight writes all passed. Fixed with a
populated/populated swap followed by `push_front` on both, which is
load-bearing: `push_front` is the only public operation that READS
`begin()->_M_prev`, the one link neither a forward nor a reverse walk can
reach (`rend()` stops AT `begin()`). With `push_back` instead, dropping a
sentinel fixup still leaves both lists answering every query correctly.

The same blind spot was in the test helper: `ring_intact()` claimed to check
that "every link must be reciprocal" and only compared lengths. It now walks
the nodes and asserts `n->_M_next->_M_prev == n` around the whole ring
including the sentinel.

---

## 4. Phases U1+ — the actual unwinder, only if §2 says yes

### U1. Prior art, because writing one is the wrong default

| implementation | licence | baremetal? | verdict here |
|---|---|---|---|
| **LLVM `libunwind`** | Apache-2.0 WITH LLVM-exception | **Yes** — `LIBUNWIND_IS_BAREMETAL`, `.eh_frame` via linker-provided `__eh_frame_start` | **-> SUPERSEDED, see §U1-RESULT.** Was: "CONFIRMED, level 1", built from source in the spike, needing two local patches. Vendored in `3a188240` and **removed again** — the toolchain's own `libgcc.a` already carries a complete unwinder on both arches |
| GCC `libgcc_eh.a` | GPL-3 + RLE | **No** | **The original verdict understated this.** The Linux-isms (`_dl_find_object`, `mmap`, `getpagesize`, `pthread_cond_*`) are all stubbable, and `__register_frame` removes the need for `dl_iterate_phdr` entirely. What actually kills it is **AVX**: `uw_frame_state_for`, its DWARF unwinder core, opens with `vpxor`/`vmovdqu %ymm0`, and UEFI boots with `CR4.OSXSAVE` clear, so it `#UD`s on the first throw. You can stub a symbol; you cannot stub an instruction encoding. **-> The AVX verdict is TRUE OF THE DISTRO BUILD ONLY; see §U1-RESULT.** Compiled for `x86_64-elf` with AXL's flags, the same source yields **zero** VEX |
| GCC `libsupc++.a` | GPL-3 + RLE | **No** | Same AVX failure, in `_GLOBAL__sub_I_eh_alloc.cc` — its emergency-pool global constructor, which runs before `main` and faults unconditionally |
| LLVM `libc++abi` | Apache-2.0 WITH LLVM-exception | prebuilt: yes / **from source: NO** | **Verdict REVERSED, level 2.** The prebuilt x86-64 archive works — it drove the 9/9 spike. But it **cannot be compiled against libstdc++ headers**: libstdc++'s `std::type_info` declares four virtuals (`__is_pointer_p`, `__is_function_p`, `__do_catch`, `__do_upcast`) that libc++'s does not, so libc++abi's `__class_type_info` hierarchy inherits a different vtable layout and demands `libsupc++`. There is no prebuilt for aa64, so "ship the archive" fails the both-arches requirement |
| Rust `unwinding` crate | MIT / Apache-2.0 | Yes, no_std | Not linkable from our C++ toolchain, but useful as a correctness reference — it is a readable DWARF CFI interpreter |
| EDK2 | — | — | Has none. Upstream UEFI has no C++ exception story to borrow |

**The complication the tier-2 entry does not mention:** the unwinder is only
*level 1*. `try`/`catch` also needs *level 2* — `__cxa_throw`,
`__cxa_begin_catch`, `__cxa_end_catch`, `__gxx_personality_v0` and the
type-matching machinery.

So the honest scope is **two libraries, not thirteen symbols** — and, per the
table, they come from two different places:

- **Level 1: vendor LLVM `libunwind`.** Permissive, builds clean, and a DWARF
  CFI interpreter is genuinely hard to justify rewriting.
- **Level 2: WRITE IT.** Measured against GCC 14's libsupc++, the surface is
  **1,993 non-comment lines** — 558 of type matching plus 1,435 of EH engine,
  and `eh_alloc.cc`'s 316 lines are mostly an emergency pool we can drop
  because AXL's allocator is the backing store. Writing it against libstdc++'s
  `type_info` layout (the headers our own code uses) removes the coupling that
  makes libc++abi unbuildable here, and leaves no third-party runtime in the
  C++ path at all.

`axl::exception` rather than `std::exception` is what makes level 2 that
small: measured, a pure custom hierarchy — base, derived-with-state, multiple
inheritance — throws and catches with **zero `std::` undefined symbols**, so
`<stdexcept>`, `std::bad_alloc` and `std::bad_cast` never enter the picture.

### U1-RESULT — neither vendored nor written; the toolchain ships both levels

**Both bullets above are superseded, and the table's framing was the error.**
Every row compares a *prebuilt or distro* artifact against a *hosted* GCC.
The option missing from the table is the one that wins: **build a bare-metal
toolchain**, whose `libgcc.a` and `libsupc++.a` are compiled `--with-newlib
--disable-threads --disable-tls` and are therefore already what this section
was trying to construct by hand. See `AXL-Cxx-Toolchain-Handoff.md`.

Measured on the two toolchains AXL now requires:

| | x64 (`toolchain/x86_64-elf`) | aa64 (ARM 14.3.rel1) |
|---|---|---|
| defined `_Unwind_*` in `libgcc.a` | **30** | **21** |
| `RaiseException` / `Resume` / `ForcedUnwind` | all present | all present |
| VEX/AVX instructions | **0**, across all 147 objects | n/a |
| unwinder objects | `unwind-dw2.o`, `unwind-dw2-fde.o`, `unwind-c.o` | — |

Two corrections this forces, because both were used as justification:

- **The AVX verdict belongs to the DISTRO build, not to GCC's unwinder.**
  `uw_frame_state_for` opens with `vpxor`/`vmovdqu %ymm0` *as Fedora ships
  it*. Compiled for `x86_64-elf` with AXL's flags the same source yields zero
  VEX. The instruction encoding was never inherent — the build was.
- **"The ARM bare-metal toolchain ships no unwinder at all" is false.** It was
  the stated reason for vendoring libunwind (commit `3a188240`); aa64's
  `libgcc.a` carries a complete level-1 unwinder and always did.

So `deps/libunwind` was removed rather than kept as a fallback: an unwinder
that nothing compiles is not a fallback, and the level-2 work it was staged
for is cancelled. `src/cxxabi/` (the force-include prelude) and
`check-cxxabi-oracle` (which pinned the semantics a hand-written level 2 would
have had to reproduce) went with it — the oracle could only ever have gone red
on a *host* libstdc++ change, which is news about someone else's library.

**What survives from U1's framing:** `axl::exception` keeping `<stdexcept>`
out of the picture was correct, but is no longer load-bearing — the toolchain
throws `std::runtime_error` through libstdc++ frames and catches it.

**U2, U3 and U4 remain as TASKS** — getting unwind tables into the image and
testing them is work any unwinder needs. But two of them change shape, because
they were written against libunwind's lookup behaviour: see the amendments
marked `(U1-RESULT)` in U2 and U3, and the correction in U4.

### U2. `.eh_frame` must reach the image

`axl-cc`'s `objcopy -j` list carries neither `.eh_frame` nor
`.gcc_except_table` today, so unwind tables never reach the `.efi` even when
the compiler emits them. Adding them touches all three build paths, and
`make check-flag-parity` already gates that `-j` list — extend it rather than
adding a fourth copy.

Note `--gc-sections`: `.eh_frame` is referenced by nothing, exactly like
`.init_array` was, so it needs `KEEP()` or it is collected silently. That bug
has already happened once in this tree.

**But `KEEP()` must NOT go in the shared linker script**, and the reason is a
measurement:

| change | cost to a **C-only** app |
|---|---|
| adding `-j .eh_frame -j .gcc_except_table` to objcopy | **0 bytes, 0.0%** |
| adding `KEEP(*(.eh_frame))` to the shared script | **+11.5–13.6 KB, +16.8%** |

The `-j` list is free precisely *because* `--gc-sections` collects `.eh_frame`
when nothing keeps it — a C-only `.so` carries zero bytes of it today.
`KEEP()` defeats that, and then every C image pays for tables it can never
use. So the design constraint is:

> **A C image must be byte-identical whether or not the SDK supports
> exceptions.**

which means `KEEP()` plus the extra `-j` entries live in a SEPARATE linker
script selected by `axl-c++ --exceptions`, and the unwinder and ABI layer ship
as a separate archive a C link never names. A gate should assert the
byte-identity rather than leave it to trust.

A third section is needed that the original plan missed: `.eh_frame_hdr`,
with `__eh_frame_hdr_start`/`__eh_frame_hdr_end` and `ld --eh-frame-hdr`.
libunwind's baremetal mode looks there first and only falls back to a linear
`.eh_frame` scan if the range is empty.

**(U1-RESULT) That premise is gone, and this paragraph is now an OPEN
QUESTION, not a requirement.** libunwind's lookup order was the only stated
reason for `.eh_frame_hdr`. GCC's unwinder is handed the table directly by
`__register_frame` (U3), and `.eh_frame_hdr`/`PT_GNU_EH_FRAME` is what the
`dl_iterate_phdr` path consumes — the path `__register_frame` makes
unreachable. The spike's linker script did emit the section, but nothing
established that it was load-bearing rather than defensive.

Default to **omitting it** and prove the need before adding it back. This
section's own constraint is that a C image must not carry bytes it cannot
use, and `make check-flag-parity` pins the `-j` list across all three build
paths — so an unnecessary entry here is not inert, it gets replicated three
times and then gated.

### U3. FDE registration at startup

The crt0 must hand the unwinder the frame table. This is the same shape as the
existing `DT_RELA` walk in `src/crt0/axl-crt0-native.c`, and it has the same
failure mode — a section split across two output sections gives a table that
walks off its end (`make check-reloc-coverage` exists because that already bit
us on aa64).

**Mechanism, updated for the toolchain unwinder (U1-RESULT).** libunwind's
baremetal mode reads linker-provided `__eh_frame_start`/`__eh_frame_end`;
GCC's does not. The spike registered the table explicitly with
**`__register_frame(__eh_frame_start)`**, and that call is what makes libgcc's
`dl_iterate_phdr`/`_dl_find_object` fallback path unreachable — which in turn
is why the Linux-only symbols in that path only have to *link*, not work.
Both halves were prototyped and ran 7/7, but neither is committed: the linker
script and the registration call still have to land in the build.

### U2/U3-RESULT — wired, both arches, 2026-08-13

**DONE.** `axl-c++ -fexceptions` produces a working exceptions image on x64 and
aa64; `test-cxx-exceptions-qemu.sh` is the committed test (36 assertions), and
the 7/7 demo is no longer hand-linked.

What landed, and the shape of each decision:

- **No `--exceptions` flag.** `-fexceptions` is a real gcc flag the caller
  already has to pass to get landing pads emitted, so axl-cc detects THAT --
  on the command line, or via an input object referencing
  `__gxx_personality_v0`, so a staged `-c`-then-link build works. A second
  AXL-specific spelling would only be a way for the two to get out of step.
  Same shape as the pre-existing `-frtti` detection, sharing one `nm -u` pass.
- **`elf_*_efi_eh.lds`**, selected by that flag: `KEEP(*(.eh_frame))` plus
  `__eh_frame_start`. Separate files exactly as §U2 requires — the KEEP is what
  costs a C image +16.8%.
- **The `objcopy -j` entries are UNCONDITIONAL**, which §U2 did not anticipate.
  Measured with `cmp`: adding `.eh_frame`/`.gcc_except_table` to a C image's
  `-j` list is byte-identical, because `--gc-sections` already collected them
  when nothing keeps them. Gating them was the first shape and
  `check-flag-parity` correctly rejected it — the Makefile and the CMake
  package would each have needed the same conditional. The byte-identity
  constraint is about IMAGE bytes and is met by the linker-script split alone.
- **`__register_frame` runs from `_axl_cxxabi_run_init_array`**, via a WEAK
  reference, not from `_axl_init`. A pure-C image never pulls that object at
  all, so the hook costs it nothing — not even a null check. A C++ image
  without exceptions resolves the weak reference to 0 and skips it. The
  fixture's first assertion is a global constructor that throws and catches
  BEFORE `main`, which is what proves the ordering rather than assuming it.
- **AXL owns the newlib syscall stubs** (`src/cxxrt/axl-cxxrt-stubs.c`) rather
  than linking `libnosys.a`. Functionally identical; libnosys's objects carry
  `.gnu.warning.<sym>` sections, so a SUCCEEDING link emitted ten "not
  implemented and will always fail" lines. Both spellings are defined
  (`close` and `_close`, …) because the two toolchains disagree — the same
  trap `sbrk`/`_sbrk` already documents, one family along, and defining one
  covers exactly one arch.
- **`_exit` routes to `axl_exit`.** It is reached from newlib's `abort()`,
  i.e. from `std::terminate`. A halt loop wedges the machine; `axl_exit`
  returns to the shell with a status.

**CLOSED — the CMake package builds an exceptions image.** It used to
re-implement axl-cc's pipeline (its own compile line, `ld`, `objcopy`,
`pe-set-debug`, ~200 lines) with no `-fexceptions` handling at all: no `_eh`
linker script, no glue objects, no toolchain libraries, so a CMake consumer
passing `-fexceptions` got an image that compiled, linked, and faulted at the
first throw. It now shells out to `axl-cc` (`COMMAND ${AXL_CC}` in the
generated `axl-config.cmake`), which is the option this section recommended.
`check-flag-parity` guards the delegation itself rather than the spellings —
reintroduce a hand-rolled compile or objcopy there and it fires again.

### U4. Tests

A throw across three frames with destructors, caught by type; a rethrow; a
`catch(...)`; an uncaught throw reaching `std::terminate`. Each asserted on
exact output under QEMU on **both** arches — aa64 cannot be assumed to follow
from x64 passing.

**Correction (U1-RESULT):** the reason given here — "AArch64 uses
`.ARM.exidx`-style vs DWARF CFI" — is wrong. `.ARM.exidx` is 32-bit ARM;
AArch64 uses DWARF CFI in `.eh_frame`, which is why one linker script shape
served both arches in the spike. Test both anyway: the arches differ in
toolchain provenance (ours vs ARM's) and in relocation handling, and
`check-reloc-coverage` exists because aa64 diverged there before.

## 4a. U5 — the terminate handler, and the 112 KB that printed nothing

**DONE**, `src/cxxrt/axl-cxxrt-terminate.cpp`.

U4 above lists "an uncaught throw reaching `std::terminate`" among the cases
to test. Doing that revealed the case was not merely untested but *mute*:
libstdc++'s `__gnu_cxx::__verbose_terminate_handler` writes to a newlib
`stderr` that no UEFI image wires up, so an uncaught throw printed **nothing**
— verified by booting one. It was not free silence. `eh_term_handler.o`
initialises `__cxxabiv1::__terminate_handler` to that function, so every
`-fexceptions` image linked `vterminate.o` and, behind it, `__cxa_demangle`
and newlib's stdio.

Measured on `cxx-exceptions-selftest.cpp`, `--release -fexceptions`, with and
without AXL's replacement object:

| arch | stock | ours | delta |
|---|---|---|---|
| x64 | 264,185 | 150,920 | **−113,265 (−42.9%)** |
| aa64 | 259,933 | 139,651 | **−120,282 (−46.3%)** |

Decisions, and why each is not the obvious one:

- **An OBJECT on the link line, not an archive member.** This is preemption:
  defining the symbol before `vterminate.o` is ever considered means that
  member is never pulled, so its `__cxa_demangle` reference never arrives to
  be satisfied. From an archive the link resolves either way and the demangler
  can still come in. Same mechanism `axl-cxxrt-alloc.o` uses for `malloc`, and
  the fourth object `axl-cc` names on the `-fexceptions` path. Safe because
  `vterminate.o` defines nothing else — only the handler's `.cold` half and
  its function-local static.
- **The type name is KEPT, and the expected tradeoff did not exist.** The open
  question was "lose the exception's identity, or keep the 112 KB demangler".
  Neither: `abi::__cxa_current_exception_type()->name()` gives the *mangled*
  name for free, and `try { throw; } catch (const std::exception &e)` recovers
  `what()` from a real handler. Measured against a bare "terminate called"
  variant, both together cost **+89 B** on x64 and **+655 B** on aa64. So the
  output names the exception:

      terminate: uncaught exception of type St13runtime_error
        what(): a deliberate uncaught error

  `St13runtime_error`, not `std::runtime_error` — demangling that string is
  the whole 112 KB, and it is the trade taken deliberately.
- **The one C++ TU compiled `-fexceptions`** (`CXXFLAGS_EH`, derived from
  `CXXFLAGS` by subtraction so a future ABI flag reaches it too). The `throw;`
  is not decoration: `what()` is virtual on `std::exception`, so reaching it
  needs a reference of that static type, which only a handler can produce.
  That is also why the file is `.cpp` where its three siblings are `.c`.
- **It exposed a latent link defect, which is fixed rather than dodged.**
  `libaxl.a` and newlib both define eleven libc names, and `axl-cc` links both
  inside one `--start-group` — so which won was decided by whichever reference
  happened to be outstanding when each archive was scanned. Adding this object
  changed that, and an uncaught `throw 42;` stopped linking:

      libstdc++(eh_alloc.o) -> getenv -> libc(getenv.o) -> getenv_r.o
                                                        -> libc(strncmp.o)
                                            -> impure.o -> findfp.o
                                                        -> libc(memset.o)
      libgcc(unwind-dw2-fde.o) -> memcpy -> libaxl(axl-intrinsics.o), which
                                            also defines memset  -> collision

  AXL's are now weak. That buys **coexistence, not precedence** — an archive
  member is still extracted for a weak definition and `libaxl.a` is scanned
  first, so which copy ships still depends on what drags each member in
  (measured: `std::runtime_error` → newlib's `memcpy`/`strlen`; a bare `int` →
  AXL's). What changes is that both members can be present without the link
  failing. The line falls where the *runtime dependency* falls: leaf functions
  have none, so either copy is correct; `__cxa_atexit` and the `__stack_chk_*`
  pair only work if AXL's init/teardown ran, so they stay strong — newlib's are
  inert under UEFI for the very same reason this section's handler was.
  `make check-libc-overlap` enforces both directions. Making newlib's the ones
  that actually RUN is a separate, larger change: put `libc.a` on the C link
  line too and delete AXL's stand-ins.
- **Tested two ways, because either alone is weak.** `test-cxx-exceptions-
  qemu.sh` asserts the SIZE half (`nm` on the ELF `.so`: no `__cxa_demangle`,
  no newlib stdio) and the SPEAKING half (an uncaught-throw fixture booted
  under QEMU, exact lines). The size half alone would pass for a handler that
  printed nothing; the speaking half alone would pass with 112 KB of dead
  demangler alongside. The `nm` reads the `.so` because `objcopy` does not
  carry `.symtab` into the PE — on the `.efi` every absence assertion would
  pass while blind — and a readability control fires on an unreadable,
  stripped or non-exceptions input rather than reporting a false zero.

---

## 5. Recommendation

**Do U0 now.** It is small, it is the same pattern already shipped twice, it
removes `std::list` from the unsupported list with proof rather than
argument, and it is correct whatever §2 decides.

**Do not start U1+ without answering §2 first.** The tier-2 entry undersells
it by roughly a library and a half, and the thing it buys contradicts a design
invariant the whole error model is built on. That is a conversation, not a
task.

**Both answered — see §2-RESULT and §U1-RESULT.** §2 decided YES; U1's
"vendor level 1, write level 2" plan was superseded by building a bare-metal
toolchain, which supplies both levels. What remains is U2/U3 (get `.eh_frame`
and `.gcc_except_table` into the image behind a separate linker script, and
call `__register_frame`) and U4 (the tests), plus the libstdc++ emergency-pool
leak noted in `AXL-Cxx-Toolchain-Handoff.md` §5.4 — `__gnu_cxx::__freeres()`
is exported from `eh_alloc.o` on both arches and is the first thing to try
against it.

## 6. Repro for everything above

```sh
# our own object needs no _Unwind_*
g++ -std=c++23 -fno-exceptions -fno-rtti -ffreestanding ... -c tier2.cpp
nm -u -C tier2.o

# what libstdc++'s list.o would drag in
ar x "$(g++ -print-file-name=libstdc++.a)" list.o && nm -u -C list.o

# libgcc_eh.a is the Linux build
nm -u "$(gcc -print-file-name=libgcc_eh.a)"
```


## U6 — REFUTED: rebuilding libstdc++/libsupc++ would save nothing

An earlier handoff proposed rebuilding the toolchain's `libstdc++`/`libsupc++`
with `-fno-asynchronous-unwind-tables`, on the theory that they are what a
default `_eh` image's residual `.eh_frame` is made of. **Measured 2026-08-17:
the premise is wrong and the mechanism does not work.** Recorded so nobody
re-opens it.

### The residual is not what the hypothesis said it was

`.eh_frame` in a default `_eh` C++ image (`cxx-json-selftest`, x64),
attributed per input object from the linker map:

| contributor | bytes | share |
|---|--:|--:|
| `libstdc++.a` | 4,624 | 42% |
| `libgcc.a` | 2,812 | 25% |
| **the consumer's own object** | **2,040** | **18%** |
| `libc.a` (newlib) | 1,528 | 14% |
| `axl-cxxrt-terminate.o` | 64 | <1% |
| `libsupc++.a` | **0** | 0% |
| total | 11,068 | |

`libsupc++` — named in the hypothesis — contributes **nothing**. A quarter is
`libgcc`, which a libstdc++ rebuild would not touch. And 18% is the
CONSUMER's own object, which needs no toolchain work at all.

### `-fexceptions` makes the flag a no-op, which is the whole answer

Same TU, same toolchain, only the flag changing:

| compile | `.eh_frame` |
|---|--:|
| `-fexceptions` | 2,152 |
| `-fexceptions -fno-asynchronous-unwind-tables` | **2,152 — identical** |
| `-fno-exceptions` | 1,992 |
| `-fno-exceptions -fno-asynchronous-unwind-tables` | **0** |

Confirmed on a second TU (1,112 → 1,112). The last row is the CONTROL and it
moves, so the flag is reaching the compiler; `gcc -Q --help=common` confirms
it flips both `-fasynchronous-unwind-tables` and `-funwind-tables` to
`[disabled]`. The tables survive anyway because `-fexceptions` requires them:
what the async form adds is CFI validity at every instruction rather than at
call sites, and on x86-64 gcc emits the same table either way.

`libstdc++.a` carries 35,612 bytes of `.gcc_except_table`, so it is
unambiguously exception-enabled. **Therefore rebuilding it with
`-fno-asynchronous-unwind-tables` saves exactly zero bytes.** Same for
`libsupc++`.

### What IS available, and why it is still not worth doing

Only the exception-FREE libraries could shrink: `libc.a` (newlib, no
`.gcc_except_table` at all) contributes 1,528 bytes, and `libgcc.a`'s 2,812
some fraction — and `libgcc` is the unwinder, which is the last thing to
strip unwind information from casually.

So the realistic ceiling is ~1.5-4.3 KB per image, against: a full toolchain
rebuild, a new pinned tarball and sha256, and every consumer re-running
`axl-install-toolchain`. `TARGET_FLAGS` in `toolchain/x86_64-elf/build-
toolchain.sh` is `-O2 -fPIC`, so x64 is ours to change — but aa64 uses ARM's
prebuilt binaries (their manifest sets no target CFLAGS), so aa64 could not
follow without abandoning ARM's toolchain, which was a deliberate choice.

And on a `--no-eh-frame` image the saving is **zero regardless**, because
`.eh_frame` is already absent.

**Recommendation: do not.** The one free win here is the consumer's own 18%:
a consumer on a default `_eh` link removes their own contribution by putting
`-fno-asynchronous-unwind-tables` on their own compiles, no toolchain work
required. That does nothing for a `--no-eh-frame` consumer, for the same
reason everything else here does nothing for them.
