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
