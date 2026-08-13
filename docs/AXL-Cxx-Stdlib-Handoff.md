# Handoff — use the standard containers, and the AVX trap that gates x64

> **Status: DONE 2026-08-06.** All three pieces shipped, on both arches, with
> `test/integration/test-cxx-hosted-qemu.sh` as the standing proof. What
> actually landed — including two symbols this plan did not predict and one
> defect it surfaced — is recorded in `docs/AXL-Cxx-Design.md` §2b.1 to §2b.3,
> which is the doc to read now. This one is kept for the traps in §3 and §8,
> which are still the traps.
>
> **Where it diverged, in brief:**
> - §3 answered with **option 1** — AXL defines `_Prime_rehash_policy`'s two
>   out-of-line members itself (`src/runtime/axl-cxx-rehash.cpp`). Both
>   members, never one: they share `_M_next_resize`.
> - x86-64 also needs **`ceil`**, which §4b's symbol list could not contain
>   because it was measured on aa64, where `__builtin_ceil` folds to `frintp`.
> - `operator new` returning NULL became a **defect** once containers were
>   reachable; it now halts. See §2b.3 of the design doc.
> - `check-no-avx` is **per-symbol with an allowlist**, not a blanket ban:
>   AXL ships dispatched AVX2 kernels on purpose and they must keep working.
> - It went to the **artifact gates** (beside `check-nx-compat`, run by CI),
>   not to `LINT_GATES` as §3 suggested — `LINT_GATES` members must build
>   nothing, and this one takes built artifacts as prerequisites.
>
> **Still open, and Mike's calls, not the implementer's:** both items in §5.
> They are restated in `AXL-Cxx-Design.md` §8 so they do not get lost with
> this file.

**For a fresh session.** Self-contained: the decision, what is proven, the one
thing that blocks x64, the remaining work, and exact repro commands.

Boilerplate: pure C library / C++ layer; TDD; both arches; `verify.sh` ALL
GREEN before commit; never `git add -A` (the tree carries pre-existing
untracked `SCRATCH.txt` and `docs/AXL-*.md`); push freely
(`feedback_push_freely_axl_sdk`).

---

## 1. The decision

**Do not write container classes.** Use `std::vector`, `std::string`,
`std::map`, `std::unordered_map`. An earlier plan (C1–C6 in
`AXL-Cxx-Design.md`) had us writing `axl::vector`, `axl::string`, `axl::map`
and `axl::owned`; Mike pushed back — *"if we can get things like std::vector
and std::map for free then I don't think we should write our own"* — and he was
right. Measurement backed him, not the plan.

What remains is **integration, not implementation**.

## 2. What is proven, and exactly how

Two gates, and only the first is about `-ffreestanding`:

1. `-ffreestanding` makes libstdc++ refuse the containers via
   `bits/requires_hosted.h`. Compiling a C++ TU `-fhosted` lifts it.
   `-D_GLIBCXX_HOSTED=1` does NOT.
2. Some containers then need out-of-line code from `libstdc++.a`.

| Header | Cost |
|---|---|
| `<vector>` `<deque>` `<array>` `<span>` `<string_view>` `<algorithm>` `<ranges>` | free |
| `<string>` | free to construct; 1–2 symbols for `+=` / `append` / `replace` / `insert` |
| `<list>` | 1 symbol |
| `<unordered_map>` | 1 symbol — `_Prime_rehash_policy::_M_need_rehash` (in `hashtable_c++0x.o`) |
| `<set>` / `<map>` | 2 / 3 symbols — the red-black tree core (in `tree.o`) |

Five distinct symbols, in **two archive members**. On the ARM bare-metal
toolchain both members have **zero undefined symbols** — no locale, no
iostreams, no unwinder. The feared cascade does not happen.

**Verified running, AARCH64, UEFI under QEMU:**

```
map size=3 ordered: apple=1 fig=2 pear=3
umap size=200 u[13]=169
```

`std::map<std::string,int>` ordering correctly and `std::unordered_map` with
200 entries. `-fno-exceptions`, five stubs, a 119 KB image.

Also verified earlier, x64: `std::vector<std::vector<int>>` + `std::sort`,
68 KB against a 44 KB C++ hello baseline.

## 3. THE TRAP: the distro's x64 `libstdc++.a` contains AVX

This is the thing that will waste a day if you do not know it.

**UEFI runs with `CR4.OSXSAVE` clear, so any AVX instruction is `#UD`.**
The prebuilt x86-64 `libstdc++.a` from `libstdc++-static` (installed
2026-08-04, `/usr/lib/gcc/x86_64-redhat-linux/14/libstdc++.a`) is built for
hosted Linux with a modern baseline:

| member | VEX instructions | consequence |
|---|--:|---|
| `tree.o` | **0** | `std::map` / `std::set` work on x64 |
| `hashtable_c++0x.o` | **49** (`vaddsd`, `vdivsd`, `vcvtsi2sd` — float math for the load factor) | `std::unordered_map` **faults on x64** |

Measured: x64 prints `map size=3 ordered: apple=1 fig=2 pear=3` and then
`#UD - Invalid Opcode`. aa64 runs both, because the ARM bare-metal toolchain's
archive is built for a freestanding target and has no AVX.

**The same mechanism bit this session's own harness**, separately: a spike
hand-compiled with `g++` omitting the Makefile's `$(GCC_ARCH)`
(`-mno-red-zone -march=x86-64`) emitted AVX and `#UD`'d, and the wrong
conclusion ("hosted containers are blocked") reached the design doc before it
was root-caused. **`#UD` with `CR2 = 0` is not a pointer bug** — disassemble
the RIP.

### Options for x64 `unordered_map`, none yet chosen

1. **Implement `_M_need_rehash` ourselves.** It is a prime-table lookup plus a
   float comparison — genuinely small. Licensing: libstdc++ is GPL-3 with the
   Runtime Library Exception, so this must be clean-room, not copied.
2. **Build libstdc++ from source with `-march=x86-64`.** Correct and general;
   a large build dependency.
3. **Find/ship a bare-metal x86-64 toolchain** whose libstdc++ is built for a
   freestanding baseline, mirroring the aa64 situation.
4. **Do not offer `std::unordered_map` on x64.** `std::map` works; `tree.o` is
   AVX-free.

### Strongly recommended regardless: a `check-no-avx` gate

Nothing in the tree currently detects an AVX instruction reaching a produced
image, and this class has now caused two separate wrong conclusions in one
session. A gate that disassembles each produced `.efi` (or its `.so`) and fails
on a VEX prefix would make it structural. Slot it beside `check-nx-compat` /
`check-bss-clear`, which are the existing produced-image gates, and add it to
`LINT_GATES` in the Makefile (one edit — `verify.sh` reads that list back via
`make -s print-lint-gates`).

## 4. Remaining work

### 4a. The five stubs

Under `-fno-exceptions` libstdc++ calls these instead of throwing. Put them in
`libaxl-cxx.a` beside `abort` (`src/runtime/axl-cxxabi-ops.cpp`, which already
defines `abort` as of `b4795ed9`):

```c++
namespace std {
void __throw_bad_alloc();
void __throw_bad_array_new_length();
void __throw_length_error(char const *);
void __throw_logic_error(char const *);
void __throw_out_of_range_fmt(char const *, ...);
}
```

**Declare them BEFORE including any container header**, or gcc reports
"declared here, later in the translation unit". Each must be `[[noreturn]]` /
end in `__builtin_unreachable()`.

**This is a policy decision, not plumbing**: putting them in `libaxl-cxx.a`
makes every consumer inherit halt-on-OOM. See §5.

### 4b. A hosted opt-in in `axl-c++`

The user-facing feature. For TUs that opt in:

- drop `-ffreestanding`
- drop `-Iinclude/compat` — those shims exist for the freestanding C build, and
  shadowing the real headers with a stub `typedef void FILE` is exactly what
  blocks `<string>` and `<memory>` (the conflict is against glibc's `stdio.h`,
  which hosted libstdc++ pulls in)
- keep `$(GCC_ARCH)` — see §3
- link the needed `libstdc++.a` members

`-fhosted` is a **per-TU** flag and costs nothing measurable. `libaxl.a` stays
`-ffreestanding`; a mixed image links and runs. The complete libc footprint of
a hosted TU using vector + string + map + unordered_map is `memcpy`, `memmove`,
`memset`, `memcmp`, `strlen` — **all five already in `libaxl.a`**.

Note `-fhosted` after `-ffreestanding` on the same command line does NOT
override it; `axl-c++` appends its own flags, so this needs a real code path,
not a passthrough flag.

### 4c. An arena allocator

For paths where halting on OOM is unacceptable (§5).
`std::vector<T, axl::arena_allocator<T>>` over a pre-sized `AxlArena` cannot
OOM at all — exhaustion becomes a caller-visible condition checked up front.
`AxlArena` already exists (`include/axl/axl-task.h`), is fixed-capacity, and is
**AP-safe (lock-free CAS)**, which matters because boot services are
unavailable on application processors.

## 5. Open questions — Mike's calls, not the implementer's

- **Distribution.** If `axl-cc` needs `libstdc++.a`, every SDK consumer does.
  aa64 gets it from the ARM toolchain `install-arm-toolchain.sh` already
  fetches; x64 needs the distro package. Vendoring the two `.o` files instead
  raises a licensing question worth answering properly rather than assuming —
  the GCC Runtime Library Exception is written for *linking*, and
  redistributing archive members inside an SDK is a different act. Mike's
  steer: treat this as an `AXL-Distribution-Design.md` item rather than solving
  it inside the C++ layer.
- **Whose policy the stubs encode.** Shipping them in `libaxl-cxx.a` makes
  halt-on-OOM the SDK's contract. AXL's C error model is the opposite:
  `axl_mem_fail_next_alloc()` is in a PUBLIC header, the suite carries 43 OOM
  assertions, and some are degradation contracts rather than error propagation
  ("failure at ANY allocation point -> valid lossy bbox superset"). Recoverable
  OOM here is exercised, not aspirational, and standard containers cannot
  participate in it. That is the one property we give up.

## 6. Acceptance test, stated up front

**AGT must shrink.** It hand-rolls, counted: **84** manual `axl_free` /
`axl_*_free` calls, **12** owning `char *` members (strdup + `if (x) free`),
**4** `AxlArray *` members used as typed containers with per-element free loops
in 3 files, and **1** hand-written ctor/dtor scope guard. If adopting the
standard containers does not remove most of that, the exercise did not fit the
real consumer.

AGT is at `~/projects/aximcode/agt`, builds with
`AXL_SDK_SRC=~/projects/aximcode/axl-sdk make test` (9578/0 both arches), and
needs a second `make` after an SDK tree switch — a known `sdk-guard` bug on
their side, already reported.

## 7. Repro commands

```sh
# x64: a hosted C++ TU. NOTE -march=x86-64 and the ABSENCE of include/compat.
g++ -std=c++23 -fno-exceptions -fno-rtti -fshort-wchar -fno-stack-protector \
    -fno-builtin -fno-omit-frame-pointer -fpic -mno-red-zone -march=x86-64 \
    -Iinclude -Iinclude/uefi -DAXL_BACKEND_NATIVE -c prog.cpp -o prog.o

# aa64
/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-g++ \
    -std=c++23 -fno-exceptions -fno-rtti -fshort-wchar -fno-stack-protector \
    -fno-builtin -fno-omit-frame-pointer -fpic -mno-outline-atomics -ffixed-x18 \
    -Iinclude -Iinclude/uefi -DAXL_BACKEND_NATIVE -c prog.cpp -o prog.o

# extract the two members (aa64 path shown; x64 is gcc -print-file-name=libstdc++.a)
ar x /opt/arm-gnu-toolchain-*/aarch64-none-elf/lib/libstdc++.a tree.o hashtable_c++0x.o

# link: axl-c++ only appends libaxl-cxx.a when it sees a .cpp, so pass a stub
echo 'namespace { struct P {}; }' > pull.cpp
out/bin/axl-c++ --arch aa64 --release pull.cpp prog.o tree.o hashtable_c++0x.o -o prog.efi

# run
./scripts/run-qemu.sh --arch AARCH64 --timeout 120 prog.efi

# check for the AVX trap before believing a green run
objdump -d prog.so | grep -cE '\bv[a-z0-9]+\s'      # must be 0
```

Spikes from the session are in the scratchpad (not committed):
`vec.hpp`, `assoc.hpp`, `boxed.hpp`, `avl.hpp`, `str.hpp` — the measurements
behind `AXL-Cxx-Design.md` §4.

## 8. Methodology, because it cost real time here

Three conclusions in this session were wrong, each from evidence that stopped
one step short:

- **Read a standard, never compiled** — "containers require exceptions".
  `-ffreestanding` was the actual gate.
- **Compiled and ran, with the wrong flags** — the `#UD`, §3.
- **Compiled, never linked** — "the containers are free". The associative ones
  need five symbols that only appear at link time.

**Compile is not link; link is not run.** And one more, from the benchmarking
half of the session: `axl_malloc` costs ~330 ns while the heap is GROWING and
~60 ns reusing freed blocks, so **whichever variant runs first in a benchmark
pays a 5.5× tax** — warm the heap and re-run with the order reversed, or the
numbers are fiction. That one inverted a conclusion mid-flight.
