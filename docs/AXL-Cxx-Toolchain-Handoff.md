# Handoff — C++ exceptions are solved; the answer was the toolchain

> **Status: WORKING on both arches, 7/7 under QEMU. Nothing is wired into
> the build yet (§5).**
>
> **Point-in-time record — two items below have since been actioned.**
> §5.2/§9.2 (`deps/libunwind`) and §5.3 (the cxxabi oracle) are DONE: both
> were removed, along with `src/cxxabi/`, once measurement showed each
> toolchain's own `libgcc.a` already carries a complete unwinder (x64: 30
> `_Unwind_*`, 0 VEX across 147 objects; aa64: 21). §6's stale-docs item is
> also done. See `AXL-Cxx-Unwinder-Design.md` §U1-RESULT. The rest of §5 —
> the wiring and the leak — still stands.
>
> Self-contained. Every number below was measured on this tree during the
> session that produced it, not estimated. Where a conclusion was reached
> and then overturned, both are recorded — the overturned ones are the
> expensive part to rediscover.

Boilerplate: pure C library with a C++ layer; TDD; both arches;
`verify.sh` ALL GREEN before commit; never `git add -A` (the tree carries
a pre-existing untracked `SCRATCH.txt` and seven `docs/AXL-*.md`); push
freely.

---

## 1. The one-paragraph summary

Real C++ `try`/`catch` now works under UEFI on **x64 and aa64**, together
with `std::vector`/`string`/`map` and `<stdexcept>`, with **no host
packages, no vendored runtime, and no hand-written ABI layer**. The fix
was not a runtime at all: it was building an `x86_64-elf` **bare-metal
toolchain** so x64 matches what aa64 already had from ARM. Everything
that failed all session failed because x64 was borrowing the host's
**glibc-targeted** g++.

## 2. Repo state

**7 commits unpushed**, tree otherwise clean:

```
6a1b179b toolchain: build an x86_64-elf bare-metal toolchain, so both arches match
21a2c67f docs(cxx): plan the retirement of --hosted, and record why x64 blocks it
0071ffee docs: record newlib as a substrate worth investigating
e8684daf cxxabi: pin the level-2 type-matching semantics against libstdc++ first
3a188240 deps: vendor LLVM libunwind, building AVX-free on both arches
cd056197 docs(unwinder): the decision is taken, and two prior-art verdicts were wrong
f036e2e8 runtime: report the TPL repair without dragging printf into every C image
```

`verify.sh` was **ALL GREEN at 10384 assertions/arch** as of `f036e2e8`.
The commits after it are docs plus the inert `toolchain/` and
`deps/libunwind` trees, so nothing should have moved — but **verify.sh
has not been re-run since**, and that is the first thing to do.

## 3. What works, and exactly how it was proven

Same demo on both arches — containers **and** exceptions in one
translation unit, **no `-ffreestanding`, no `include/compat`**:

```
  PASS: std::vector push_back + index
  PASS: std::string concat + compare
  PASS: std::map insert + lookup
  PASS: std::runtime_error caught as std::exception
  PASS: vector::at out_of_range propagated        <- throws from INSIDE libstdc++
  PASS: destructor ran during unwind
=== 7 passed, 0 failed ===
```

- **aa64**: ARM's `aarch64-none-elf` toolchain, already installed by
  `scripts/install-arm-toolchain.sh`. Nothing to build.
- **x64**: `toolchain/x86_64-elf/build-toolchain.sh` (committed), ~40 min
  on 8 cores. Produces gcc/g++, newlib `libc.a`, and — the thing x64 has
  never had — a `libstdc++`/`libsupc++` configured for a freestanding
  target.

Both needed the same **four newlib stubs**: `getenv`, `strtoul`,
`_impure_ptr`, `__xpg_strerror_r`. None are on a path AXL exercises.

Spike artefacts live in the session scratchpad (`newlib-demo.cpp`,
`ehenv.c`, `aa64-stubs.c`) — **not committed**; §5 covers what to do
with them.

## 4. Why the hosted toolchain could never work — three blockers

All measured on `libsupc++.a` from the host g++:

| | hosted x86-64 | ARM bare-metal |
|---|---|---|
| `__thread` TLS symbols (`eh_globals.o`) | **1** | 0 |
| `%fs:0x28` reads — glibc's **stack canary** | **18**, across 11 objects | 0 |
| objects touching thread-pointer / `__stack_chk` | 11 of 65 | **0 of 65** |

`__cxa_get_globals()` reads through `%fs`, which UEFI never sets up, so
the first `throw` dereferences garbage **before reaching the unwinder**
— confirmed by instrumenting our own `_Unwind_RaiseException`, which was
never entered. The difference is entirely
`--disable-threads --disable-tls --with-newlib`.

Also measured, and worth not re-deriving: an unrestored `RaiseTPL` wedges
the machine at **every** raised level (`TPL_CALLBACK` as much as
`TPL_NOTIFY`), silently on x64. `f036e2e8` repairs it in `_axl_cleanup`.

## 5. What is NOT done

1. **Nothing is wired into the build.** `axl-cc`/`axl-c++` still use the
   host g++ for x64; the Makefile knows nothing about `toolchain/`. The
   7/7 runs were hand-linked.
2. **`deps/libunwind` (`3a188240`) is now redundant** — each toolchain's
   `libgcc.a` already carries a complete unwinder: **30** defined `_Unwind_*`
   on x64 (0 VEX across all 147 objects) and **21** on aa64. It should
   probably be reverted rather than left as dead weight. **DONE — removed;
   see `AXL-Cxx-Unwinder-Design.md` §U1-RESULT.**
3. **`e8684daf`'s type-match oracle** was built for a hand-written level 2
   that is no longer needed. It still passed (16/16 against libstdc++) and
   `check-cxxabi-oracle` was in `LINT_GATES` — but its stated purpose is
   obsolete. **DONE — removed**, with `test/cxxabi/`. It compiled with the
   *host* `$(CXX)` and asserted only against the *host* libstdc++, naming no
   AXL symbol, so its only possible failure mode was a host-toolchain change.
   `make check-cxxabi-oracle` no longer exists.
4. **The leak.** The aa64 container run leaked 3 allocations / 4640
   bytes, x64 2 / 4096 — libstdc++ init state nothing frees. **AXL's leak
   gate is a hard build gate**, so this must be understood before any of
   this ships.

   **Named suspect, not yet proven:** libsupc++'s **emergency exception
   pool**. `eh_alloc.o` defines an `emergency_pool` in `.bss` plus a
   static-init constructor that allocates it — through the spike's
   `posix_memalign`/`malloc` bridge onto `axl_malloc`, which is exactly
   what the leak report sees. Upstream frees it from
   **`__gnu_cxx::__freeres()`**, which is exported (`T`) from `eh_alloc.o`
   on **both** arches and which nothing calls under UEFI; glibc reaches it
   only via its valgrind-clean shutdown path. First thing to try is an
   `axl_atexit` hook calling it at teardown — AXL's atexit is already LIFO.
   Note the counts (2 and 3 allocations) suggest the pool is not the *only*
   contributor, so re-measure rather than assume one call clears it.
5. **Distribution.** The toolchain is built locally. If it ships it
   should mirror `install-arm-toolchain.sh` — a tarball fetched by a
   script — and that is likely when `toolchain/` becomes its own repo.

## 6. Docs that are now stale

`21a2c67f` records T2 (retire `--hosted` on x64) as **BLOCKED**, because
at the time it was. **The toolchain unblocks it.** `AXL-Cxx-Design.md`
§6a-PLAN needs T2 flipped and its "needs an x86_64-elf toolchain we would
have to build and host" note updated to "built; see `toolchain/`".

## 7. Dead ends — measured, so they are not re-tried

- **Write level 2 ourselves** (~1,993 lines measured against libsupc++).
  Unnecessary now.
- **libc++abi from source: impossible against libstdc++ headers.**
  libstdc++'s `std::type_info` declares four virtuals libc++'s does not
  (`__is_pointer_p`, `__is_function_p`, `__do_catch`, `__do_upcast`), so
  its `__class_type_info` inherits a different vtable layout and demands
  libsupc++. The PREBUILT archive works (drove an earlier 9/9 run) but no
  aa64 build of it exists anywhere.
- **Distro `libgcc_eh`/`libsupc++`: AVX**, in `uw_frame_state_for` and in
  libsupc++'s emergency-pool global constructor. `axl_cpu_enable_avx()`
  from a `constructor(101)` priority ctor DOES fix the `#UD`
  (`CR4=0x40668` confirmed in the guest) — but the TLS blocker remains,
  so it does not rescue the path.
- **Package dependencies.** `libstdc++-static` is in **CRB**, not enabled
  by default, so a hard `Requires:` makes `dnf install` fail outright.
  And LLVM libunwind/libc++abi are packaged on **neither** arch (EPEL's
  `libunwind` is the unrelated nongnu project).
- **setjmp/longjmp cannot implement `try`/`catch`** — the compiler
  hard-wires `__cxa_throw`/`__gxx_personality_v0` + DWARF tables, and the
  personality walks real frames. Redirecting that is
  `--enable-sjlj-exceptions`, a GCC *build-time* option. A chained
  per-frame `setjmp` registry DOES run cleanup correctly (**1.58 ns and
  64 bytes of stack per protected frame** with `__builtin_setjmp`) but
  yields macros rather than keywords and runs **no C++ destructors**.
- **Red zone was a false alarm.** Predicted distro archives would be
  unusable under UEFI because of it; measured **zero** red-zone writes in
  all four candidates (validated detector: 8 hits on a known red-zone
  build, 0 with `-mno-red-zone`).

## 8. Toolchain build traps, all now gated in the script

The script fails the build unless `libsupc++` shows **0 TLS symbols, 0
`%fs` accesses, 0 non-PIC relocations**. Each gate exists because
something bit:

- **`MAKEINFO=true` must be a make COMMAND-LINE variable, not an
  export.** Each sub-configure re-detects `makeinfo` and overwrites the
  exported value with the tree's `missing` wrapper, which exits 127 — a
  bare "Error 127" in `bfd/doc` that reads like a compiler failure.
  binutils treats the miss as fatal; GCC and newlib only warn.
- **Target libraries need `-fPIC`** because AXL links every image with
  `ld -shared`. Without it `libstdc++.a(eh_alloc.o)` carries an
  `R_X86_64_32` against `.bss` and the link dies. aa64 never hit this —
  AArch64 codegen is position-independent for these relocations.
- **`-fPIC -fno-PIE` silently CANCELS PIC** (measured: abs relocs 1,
  rip-relative 0; order-dependent). So the gate checks resulting
  relocations, not flag order. The `-fno-PIE` in a GCC build is host-only
  — GCC compiling itself.
- **The PIC check MUST exclude debug sections.** `.debug_info` and
  friends use absolute relocations legitimately; ARM's stock `libgcc.a`
  carries 53480 and links fine. A naive `grep -c R_X86_64_32` reported
  15491 against a `libgcc` whose allocatable sections were clean.

## 9. Suggested order for the next session

1. Re-run `./scripts/verify.sh` — nothing since `f036e2e8` should have
   moved it, but confirm before building on top.
2. Decide `deps/libunwind`: revert `3a188240`, or keep as a fallback.
3. Wire the toolchain into `axl-cc`/`axl-c++` and the Makefile for x64,
   behind whatever flag makes sense, and get the 7/7 demo running as a
   committed integration test on both arches.
4. Then T1–T5 in `AXL-Cxx-Design.md` §6a-PLAN — retire `--hosted`, retire
   `include/compat` for C++.
5. The leak (§5.4) before anything ships.

## 10. Related

- `docs/AXL-Cxx-Design.md` §6a (freestanding vs hosted, measured), §6a-PLAN
  (T1–T5), §6b (`AXL_CB_NOEXCEPT`)
- `docs/AXL-Cxx-Unwinder-Design.md` §2-RESULT, §U1 (two prior-art verdicts
  corrected)
- `docs/AXL-Newlib-Investigation.md` — newlib as a substrate under AXL,
  a separate and larger question
- `toolchain/README.md` — why AXL builds a toolchain and why no package works
