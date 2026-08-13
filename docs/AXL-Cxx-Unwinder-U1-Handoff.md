# Handoff — Tier 2 phase U1+ (the actual unwinder)

> **SUPERSEDED 2026-08-10 — do not act on this document's plan.**
>
> Its two headline claims are both now false. The decision it waits on was
> **answered YES** (`AXL-Cxx-Unwinder-Design.md` §2-RESULT), and its
> recommendation — vendor LLVM `libunwind` for level 1 — was **carried out and
> then reversed**: the vendored tree is deleted. Each toolchain's own
> `libgcc.a` already ships a complete level-1 unwinder (x64 **30** `_Unwind_*`,
> 0 VEX; aa64 **21**), and its `libsupc++` supplies level 2, so neither library
> is vendored nor written. In particular, this document's "the ARM bare-metal
> toolchain ships none at all" is **wrong** — that claim is what justified the
> vendoring.
>
> Current state: `AXL-Cxx-Toolchain-Handoff.md`. Current plan:
> `AXL-Cxx-Unwinder-Design.md` §U1-RESULT, and U2/U3/U4 as amended there.
> Kept for its measurements and traps, which remain valid.
>
> **Original status: U0 DONE and committed. U1 BLOCKED on one decision that
> is Mike's.**
>
> Self-contained. Everything below is measured on this tree, not estimated.
> The companion plan is `AXL-Cxx-Unwinder-Design.md`; this document is the
> session context around it — what shipped, what is unpushed, and the traps
> that cost real time.

Boilerplate: pure C library with a C++ layer; TDD; both arches; `verify.sh`
ALL GREEN before commit; never `git add -A` (the tree carries pre-existing
untracked `SCRATCH.txt` and seven `docs/AXL-*.md`); push freely
(`feedback_push_freely_axl_sdk`).

---

## 1. Repo state

**`main` has 9 commits that are NOT pushed.** Working tree clean,
`verify.sh` ALL GREEN (10360 unit assertions per arch, 14 gates), hosted C++
120 assertions across both arches, streams 78 per arch.

```
d5c078f6 cxx: std::list and shared_ptr, and tier 2 never blocked either
d47eb808 docs: plan Tier 2 — and measure that three quarters of it needs no unwinder
82b7401d ci: verify the release packages carry the C++ half, and link it HOSTED
482f3388 cxx: review fixes — a gate that deleted build trees, and "self-contained" was overclaimed
753a76a6 cxx: the SDK links no libstdc++.a — eleven functions were the whole gap
c71a6fd5 cxx: axl::string is not a skin — standalone with SSO, 6.9x/9.4x faster
f1c0099f build: gate the -MD -MP omission that made a sabotage read as UNDETECTED
788fcd7c docs(cxx): the stream size figures were wrong twice, and drift by design
8c47e205 cxx: axl::cout / axl::cin / axl::cerr, and the axl::string the plan forgot
```

**First action in a new session: decide whether to push.** They are green and
reviewed, but nine unpushed commits is not a state to accumulate on.

## 2. What shipped, in one paragraph each

**`axl::cout` / `axl::cin` / `axl::cerr`** (`include/axl/axl-{ostream,istream}.hpp`)
— freestanding, over `axl_printf`/`axl_readline`. `>>` matches libstdc++:
skip whitespace, newline is whitespace so extraction spans lines, a parse
failure leaves the cursor AT the offending character. Failure model is BOTH
paths over ONE sticky state — `if (!axl::cin)` and
`read<T>() -> axl::result<T>` — and `read<T>()` setting the same bit is
load-bearing, because `axl_str_to_u64` rewinds `endptr` on overflow so a
cursor-driven loop would never terminate. Globals are constant-initialised
(`constexpr` ctor, no destructor) so they emit no `.init_array` entry;
`cin`'s buffers are freed through `axl_atexit`.

**`axl::string`** (`include/axl/axl-string.hpp`) — 48 bytes, standalone, 23-byte
SSO sized so `\EFI\BOOT\BOOTX64.EFI` (21 chars) stays inline. NOT a skin over
`AxlString`: `AXL-Cxx-Design.md` §4.5 had already measured that shape and
rejected it, and re-measuring before/after on one box gave ctor 6.9x, copy
9.4x. Search family forwards to `std::string_view`, so `find`/`substr`/
`starts_with` are libstdc++'s own algorithms.

**The SDK links no `libstdc++.a`.** `--hosted` was pulling exactly `tree.o`,
`hash_bytes.o`, `list.o`, `shared_ptr.o` and `string-inst.o`'s
`_M_replace_cold`. All replaced clean-room in `src/runtime/axl-cxx-{rbtree,
hash,list,string-inst}.cpp`. This closes `AXL-Cxx-Design.md` §8: the one act
the GCC Runtime Library Exception does not cover — redistributing the runtime
library — no longer arises. **`-frtti` is the sole exception** and links the
consumer's own installed archive, which is not redistribution.

**Tier 2 reframed.** See §3.

## 3. Where U1 starts

`AXL-Cxx-Unwinder-Design.md` is the plan. The measured facts that matter:

- **Our own `-fno-exceptions` objects reference ZERO `_Unwind_*` symbols.**
  The cascade only ever came from sourcing functions out of libstdc++ members
  that carry landing pads. U0 removed the last of those.
- **So the unwinder now buys exactly one thing: real `try`/`catch`.**
  `std::list`, `shared_ptr` and `<stdexcept>`'s classes all work without it.
- **It is TWO libraries, not thirteen symbols.** Level 1 is `_Unwind_*`;
  level 2 is `__cxa_throw`, `__gxx_personality_v0`, `__cxa_begin_catch`, the
  exception hierarchy and type matching. GCC's level 2 is `libsupc++`
  (GPL-3 + RLE); LLVM's is `libc++abi` (permissive).
- **`libgcc_eh.a` is measurably the Linux build.** On this box it needs
  `_dl_find_object`, `mmap`, `munmap`, `getpagesize`,
  `pthread_cond_broadcast`/`wait`. The ARM bare-metal toolchain ships none at
  all. Using it would also reopen the redistribution question `753a76a6`
  just closed.
- **LLVM `libunwind` is the candidate** — Apache-2.0 WITH LLVM-exception,
  explicit `LIBUNWIND_IS_BAREMETAL`, reads `.eh_frame` via linker-provided
  `__eh_frame_start`/`__eh_frame_end`.

### THE BLOCKING DECISION

`axl-cxx.hpp` says, as a design invariant:

> `-fno-exceptions` is not negotiable here, so a fallible operation returns
> #axl::result.

Everything is built on that — `axl::result`, `axl::string::bad()`, the stream
fail state, `arena_allocator`. **Do not start U1 without Mike answering
this.** Points to weigh:

- An uncaught throw with a working unwinder still ends at `std::terminate`;
  the only difference from today is a stack unwind first.
- **TPL is the firmware-specific hazard.** Firmware runs at raised TPL in
  places, and an unwind that crosses a `RaiseTPL`/`RestoreTPL` pair without
  restoring it wedges the machine. Nothing in the Itanium ABI knows TPL
  exists. This wants a spike before any commitment.
- The gain is real for deeply nested parse/IO code, which AXL has a lot of.

### If the answer is yes, the remaining pieces

1. **`.eh_frame` + `.gcc_except_table` into the image.** `axl-cc`'s
   `objcopy -j` list carries neither, so unwind tables never reach the `.efi`
   even when emitted. Touches all three build paths; `make check-flag-parity`
   already gates that list — extend it rather than adding a fourth copy.
   `.eh_frame` is referenced by nothing, exactly like `.init_array` was, so
   it needs `KEEP()` in both linker scripts or `--gc-sections` collects it
   silently. That bug has already happened once in this tree.
2. **FDE registration in the crt0.** Same shape as the existing `DT_RELA`
   walk in `src/crt0/axl-crt0-native.c`, and the same failure mode — a table
   split across two output sections walks off its end
   (`make check-reloc-coverage` exists because that bit us on aa64).
3. **Tests on BOTH arches.** AArch64 uses a different unwind representation
   from x64 DWARF CFI; x64 passing proves nothing about it.

## 4. Traps from this session

**Review PER COMMIT, not per session.** The first commit got two reviewers and
they found 13 bugs against a fully green suite. I then committed three more
without review; a later pass found a build-tree-wiping gate, silent data
corruption in `assign()`, and a test pinned to broken behaviour. Green tests
do not substitute for review in this tree, and coverage does not carry
forward — the SSO rewrite *replaced* the code that had just been reviewed.

**A sabotage that is NOT detected means one of two things, and they need
different answers.** Either the test is missing (fix it) or the code is not
load-bearing in that way (record why, at the definition). Both occurred:
`_M_transfer`'s self-splice guard and `swap`'s both-populated branch were real
gaps; `_S_eq` and `__libc_single_threaded`'s value are genuinely
undetectable. Do not fake a test for the second kind.

**Make: five target lines sharing one recipe is FIVE RULES, and only the last
gets the recipe.** The others fall back to the pattern rule. It looked correct
and the build passed because the objects were already up to date; only
`lint.sh`'s build into a throwaway prefix — which forces every TU to compile
fresh — exposed it. Use target-specific variables to give several targets the
same flags.

**Assertion counts: "N per arch" vs "N total".** The hosted harness's summary
counter accumulates across BOTH arches. I wrote "120 per arch" in a doc and in
commit messages; it is 120 total, ~60 each.

**Byte-size claims drift and should be stated as orders of magnitude.**
`libaxl.a` is selectively linked, so a "cost" is the difference between two
different sets of pulled objects. The `axl::cout` figure went 667 → 1227 → 715
across one afternoon while the code only grew.

**Sweep for orphaned processes after an agent runs probes.** A mutation-testing
binary from a review subagent was left spinning at 100% CPU for 30 minutes,
reparented to init. Mutating a linked list can close the ring; the probe had
no iteration cap. `ps -eo pid,ppid,pcpu --sort=-pcpu | head` after any agent
that compiles and runs things.

**The staged SDK is a SECOND TREE.** `axl-c++` compiles against
`out/include/axl-sdk`. Re-run `scripts/install.sh --arch all --cpp` after any
`include/axl` edit or you are testing the previous build.

## 5. Repro commands

```sh
# stage first -- the staged SDK is what axl-c++ uses
./scripts/install.sh --arch all --cpp

# the gates (14, both arches)
./scripts/verify.sh

# the two C++ suites
./test/integration/test-cxx-hosted-qemu.sh both      # 120 across both arches
./test/integration/test-cxx-streams-qemu.sh --arch X64   # 78 per arch

# prove our own objects need no unwinder
g++ -std=c++23 -fno-exceptions -fno-rtti -O2 -I out/include/axl-sdk -c t.cpp && nm -u -C t.o

# libgcc_eh.a is the Linux build
nm -u "$(gcc -print-file-name=libgcc_eh.a)"
```

## 6. Related docs

- `AXL-Cxx-Unwinder-Design.md` — the plan, with the prior-art comparison and
  the U0 result.
- `AXL-Cxx-Stdlib-Surface.md` — §3b is the measured "what still cannot link"
  table (currently empty); §4 tier 2 is now corrected to say `try`/`catch`
  and nothing else.
- `AXL-Cxx-Design.md` — §4.5 is why `axl::string` is not a skin; §8 is the
  distribution question, now closed.
- `AXL-Cxx-Streams-Handoff.md` — §3 RESOLVED is the `>>` failure model; §6b
  and §6c are the bug accounts.
