# What the C++ standard library gives us under UEFI

> **Measured 2026-08-06, not recalled.** Every row below was produced by
> compiling AND LINKING a real *use* of the facility with `axl-c++`, and run
> under QEMU. A header-include check would have been wrong: several headers
> compile happily and fail at link, which is how this project reached three
> wrong conclusions in a row (see `AXL-Cxx-Design.md` §3).
>
> **Restructured 2026-08-13 (tasks T3 and U2/U3), and the two changes point in
> opposite directions.** This document was organised around a
> freestanding/hosted split that no longer exists — `--hosted` is removed and
> passing it is an error, so §1 and §2 are one list now. And its central "not
> supported" entry, the unwinder, is DONE: `axl-c++ -fexceptions` gives real
> `try`/`catch` on both arches. The measurements are unchanged and kept; the
> headings and the conclusions they led to are what moved.
>
> Regenerate with the probe harness described in §5.

---

## 1. Free — no flag, always available

Verified **running** in a 45 KB image, values checked:

`<array>` `<span>` `<string_view>` `<bitset>` `<tuple>` / `pair` `<optional>`
`<variant>` `<expected>` `<any>` `<algorithm>` `<ranges>` `<numeric>` `<bit>`
`<atomic>` `<iterator>` `<concepts>` `<type_traits>` `<limits>` `<compare>`
`<initializer_list>` — and `std::unique_ptr`.

**Trap:** `std::unique_ptr<int> u(new int(4))` works; `std::make_unique` does
NOT. `make_unique` is outside the C++23 freestanding subset. The error names
`make_unique`, so this one at least diagnoses itself.

## 2. Free — the containers, also no flag

These needed `axl-c++ --hosted` until T3 retired the freestanding C++ mode.
They cost nothing extra now and the flag is gone; §1 and §2 are separate lists
only because §1's are additionally in the C++23 *freestanding subset*, which
matters if you ever compile a TU outside AXL.

Containers verified **running** on both arches (`test-cxx-hosted-qemu.sh`);
the rest are link-verified:

`<vector>` `<string>` `<deque>` `<forward_list>` `<map>` / `<set>` (+ `multi`
variants) `<unordered_map>` / `<unordered_set>` `<stack>` `<queue>`
`<priority_queue>` `<valarray>` `std::make_unique` `std::function`
`<chrono>` `<complex>` `<random>` `<charconv>`

`libaxl.a` stays freestanding — C keeps `-ffreestanding`, C++ does not — and a
mixed image links; see `AXL-Cxx-Design.md` §2b.1 for what the link needs.

## 3. Opt-in — `-frtti`

`typeid` and `dynamic_cast` work with `axl-c++ -frtti`, verified running under
UEFI (positive AND negative cast). `libstdc++.a` supplies the
`__cxxabiv1::__class_type_info` vtables and `__dynamic_cast`, and this is the
ONE thing that puts a GCC runtime library on the link line — the default link
names none, which is the `AXL-Cxx-Design.md` §8 constraint. The suite reads
that off the link line rather than inferring it from success.

This section used to say "hosted only", and asserted that a freestanding
`-frtti` link FAILS. There is no freestanding mode to fail in since T3.

Off by default because `type_info` objects cost image size for every
polymorphic class, whether or not anything asks for them.

**Both arches**, since 2026-08-06. It was x64-only for a day, and the reason
is worth keeping: on AArch64 an RTTI link produced both a linker-synthesized
`.rela.dyn` and the script-placed `.rela` at non-contiguous addresses, while
`DT_RELA` pointed at the first and `DT_RELASZ` counted BOTH (`RELA=0x7d00`,
`RELASZ=0x798`, sections at `0x7d00` and `0xe000`). The crt0 walked
`DT_RELASZ` bytes from `DT_RELA`, ran past the end of `.rela.dyn`, and applied
the following bytes as relocations — the image faulted before `main` with
virtual calls working and only `type_info` access broken.

That was a linker-script defect latent in every aa64 image, not an RTTI bug;
RTTI was simply the first workload to split the table. Fixed by naming the
output section `.rela.dyn` so ld absorbs its own internal section.
`make check-reloc-coverage` now asserts, on both arches, that
`[DT_RELA, DT_RELA+DT_RELASZ)` lies inside ONE section and that the section
survives `objcopy` — whose `-j` takes exact names, which is the other half of
the same trap.

## 3b. What the containers still cannot link (2026-08-08)

`libstdc++.a` is no longer on the link line at all — `libaxl-cxx.a` supplies
the `_Rb_tree_*` helpers, `_Hash_bytes`, `_M_replace_cold` and the
`std::__throw_*` family. That is NOT the same as "everything links", and an
earlier commit message said "eleven functions were the whole gap", which was
wrong. Measured after the change:

| missing symbol | breaks |
|---|---|
| *(nothing currently known)* | — |

**`std::list` and `shared_ptr` moved OUT of this table on 2026-08-09.**
`src/runtime/axl-cxx-list.cpp` supplies `_List_node_base`'s five members,
`_Sp_make_shared_tag::_S_eq` and `__libc_single_threaded`; both now run under
QEMU on both arches with no `libstdc++.a`. See `AXL-Cxx-Unwinder-Design.md`
phase U0 — neither needed the unwinder, which is what that document exists to
say.

These are pre-existing, not regressions — the same code failed to link before,
with a worse diagnostic: pulling the member cascaded into `eh_personality.o`
→ `eh_throw.o` → `_Unwind_RaiseException`. Now it is one clean undefined
symbol. `-frtti` does not rescue them; it pulls the same `_Unwind_*` cascade.

Verified WORKING with no `libstdc++.a`: `vector`, `deque`, `string` (ctor,
append, `+=`, `+`, assign, substr, find, **replace**, **insert**), `map` /
`set` / `multiset` / `multimap` including C++17 `merge`/`extract`,
`unordered_map<string,int>`, `map::at`, `std::function`, `std::sort`,
`to_string`, `array`, virtual and pure-virtual dispatch.

## 4. Not supported — and what it would actually take

The blockers are three very different tiers. Lumping them together as
"exceptions and the heap" is the mistake this document exists to prevent.

### Tier 1 — done (2 symbols)

`__stack_chk_guard` + `__stack_chk_fail`, now in `libaxl.a`. This unlocked
stack-smashing detection and RTTI. Some prebuilt `libstdc++.a` members
reference `__stack_chk_fail` regardless of how we compile, so a hosted C++
link needs it even with the protector off.

### Tier 2 — the unwinder — **DONE 2026-08-13, and it is opt-in**

`axl-c++ -fexceptions` gives real `try`/`catch` on BOTH arches. Committed
test: `test-cxx-exceptions-qemu.sh`, 36 assertions — catch by exact type
across three frames, every destructor on the unwind path running exactly once,
rethrow preserving the exception object, `catch(...)`, a non-matching handler
declining, container elements destructing mid-unwind, and a global constructor
that throws and catches BEFORE `main`.

This entry is preserved below because its ANALYSIS was right and only its
verdict moved. The three things it says are needed were exactly the three that
had to be built:

| it said | what happened |
|---|---|
| a freestanding unwinder | each bare-metal toolchain's own `libgcc.a` already had one — 30 defined `_Unwind_*` on x64, 21 on aa64. Nothing was vendored or written |
| `.eh_frame` / `.gcc_except_table` into the PE | added to `objcopy -j`, UNCONDITIONALLY: measured byte-identical for an image that has none, because `--gc-sections` already collected them |
| register the FDE table at startup | `__register_frame`, called from `_axl_cxxabi_run_init_array` through a WEAK reference, so a pure-C image pulls nothing |

**What it cost a C image: nothing.** The `KEEP(*(.eh_frame))` that would have
cost +16.8% lives in a separate linker script (`elf_*_efi_eh.lds`) selected
only by `-fexceptions`. Asserted, not assumed.

**Still `-fno-exceptions` by DEFAULT**, and that is not inertia: exceptions
must never cross the C boundary (`AXL-Cxx-Design.md` §6b — AXL invokes
consumer callbacks from C frames that carry no landing pads, so an exception
unwinding through them runs no cleanup at all). Every public callback typedef
is `noexcept` and `make check-cb-noexcept` holds the line.

The original entry, as it stood:

> Blocks real `try`/`catch`, and nothing else.

**This entry used to also name `std::list::sort`, `shared_ptr` and
`<stdexcept>`, and that was wrong.** Measured 2026-08-09: a translation unit
using `std::list`, `list::sort` and `make_shared` under the SDK's own
`-fno-exceptions -fno-rtti` references ZERO `_Unwind_*` symbols. The cascade
appeared only because the link sourced five functions from libstdc++'s
`list.o`, which carries landing pads because IT was compiled with exceptions.
Supplying those five removed the member and the cascade with it; both
containers now run on both arches. `<stdexcept>`'s classes construct without
an unwinder too -- you simply cannot `throw` them under `-fno-exceptions`,
which is a different limitation. Full account and the remaining plan in
`AXL-Cxx-Unwinder-Design.md`.

> These libstdc++ members need the unwinder because *they* were compiled with
> exceptions — not because of how we compile. Reaching them needs three things,
> and none is a flag:
>
> 1. A freestanding unwinder. x86-64's `libgcc_eh.a` is the **Linux** build:
>    `mmap`, `munmap`, `_dl_find_object`, `getpagesize`, pthread TLS. The ARM
>    bare-metal toolchain ships **no** `libgcc_eh.a` at all.
> 2. `.eh_frame` and `.gcc_except_table` carried into the PE image. `axl-cc`'s
>    `objcopy -j` list has neither, so unwind tables never reach the `.efi`
>    today.
> 3. Registering the FDE table at startup.
>
> A real project, but a scoped one — not an impossibility.

Point 1 is the one worth reading twice: it was answered not by building an
unwinder but by building a TOOLCHAIN. `libgcc_eh.a` being the Linux build was
a fact about the HOST compiler, and it stopped mattering once x64 got its own
`x86_64-elf` cross (`AXL-Cxx-Design.md` §6a-T2).

### Tier 3 — glibc's locale subsystem (~60 symbols)

Blocks `<sstream>` `<fstream>` `<format>` `<regex>`.

**`<iostream>` is NOT in this tier — an earlier revision of this table put it
here and was wrong.** Measured directly, `std::cout << 42` and `std::cin >> x`
need **23** undefined symbols and **ZERO** of them are locale: 11 `_Unwind_*`
(tier 2) plus `fputc` `fputs` `fwrite` `sprintf` `stderr` `malloc` `free`
`realloc` `gettext` `__libc_single_threaded` `pthread_mutex_lock`/`unlock` —
every one of which AXL already has an equivalent for. See §6.

Measured residual after shimming malloc/free/stdio/assert/pthread/stack-guard:
`iconv` `iconv_open` `__newlocale` `__freelocale` `__uselocale` `mbsrtowcs`
`wcsnrtombs` `nl_langinfo` `__strcoll_l` `__strftime_l` `btowc` `wctob`
`bind_textdomain_codeset` `dgettext` `strfromf128` `syscall` `fegetround` …

This is a reimplementation of glibc's locale machinery, and it is real
functionality rather than error paths — there is nothing honest to stub.
Note `<format>` additionally pulls the `__float128` softfloat helpers
(`__eqtf2`, `__getf2`, `strfromf128`) for `long double` formatting.

**AXL already covers this ground better:** `axl_printf` / AxlStream instead of
iostreams, AxlRegex instead of `<regex>`, `axl_snprintf` instead of
`<format>`. If a `std::format`-shaped API is ever wanted, the cheap path is a
type-safe wrapper over `axl_vsnprintf`, not glibc's locale.

### Tier 4 — needs an OS

`<thread>` `<future>` `<condition_variable>` `<mutex>` need a scheduler;
`<filesystem>` needs a filesystem ABI. UEFI boot services are single-threaded
and non-preemptive on the BSP. AXL's MP services exist but are not a pthread
ABI.

## 5. Reproducing this table

Each probe writes a TU that USES the facility, then:

```sh
out/bin/axl-c++ --arch x64 --release               probe.cpp -o probe.efi
out/bin/axl-c++ --arch x64 --release -fexceptions  probe.cpp -o probe.efi
```

There used to be a second line here passing `--hosted`, and the harness
classified a failure by whether the log said `requires_hosted`. Both are gone
with the mode: a failure is now `undefined reference` (link) or neither
(compile). To
measure how *deep* a "no" is, link against a shim defining
`malloc`/`free`/`fputc`/`fputs`/`fprintf`/`__assert_fail`/`__stack_chk_fail`
and re-read the residual undefined set — that is what separates tier 2 from
tier 3, and it is the step that turns "can't" into a number.

## 6. `std::cout` / `std::cin` — how far it actually gets

Asked directly, and worth recording because the answer is *not* the locale
cascade and two intermediate guesses were wrong.

**Read the `_Unwind_*` row below as historical.** 11 of the 23 symbols were
the unwinder, which the toolchain now supplies (tier 2) — so the count that
matters today is the 12 plain-libc ones, all of which AXL has equivalents for.
Nobody has re-run this probe since; the number is from 2026-08-06. AXL ships
`axl::cout`/`cin`/`cerr` (`axl-ostream.hpp`, `axl-istream.hpp`,
`test-cxx-streams-qemu.sh`, 78 assertions), which is why re-running it has not
been worth anyone's afternoon.

| Step | Result |
|---|---|
| Symbols needed | **23, none of them locale** — 11 `_Unwind_*` + 12 plain libc |
| Shim those 23 over AXL | **links** (155 KB) |
| Run it | **`#PF`** in `std::ostream::sentry::sentry` |

The fault is a null dereference at offset −24: `std::cout` was never
constructed. `ios_base::Init` — the static object `<iostream>` declares to
build the stream objects — is not in the image at all (`nm` finds zero
`ios_base::Init` symbols), so nothing constructs them.

Chasing that found a **real and unrelated defect**, now fixed: `--gc-sections`
was collecting `.init_array` outright, because both linker scripts placed it
without `KEEP()` and nothing references an `.init_array` entry by design. No
C++ global constructor in any image had ever run. `KEEP()` fixes that
(`cxx-ctor-selftest.cpp` is the guard), but it does **not** make `std::cout`
work — `ios_base::Init` is still absent, for a reason not yet chased.

Two dead ends worth not repeating:

- **Supplying our own `std::streambuf` does not help.** The cost is above the
  sink, not below it: `std::ostream`'s constructor calls `basic_ios::init()`,
  which constructs a `std::locale`. A custom streambuf + `std::ostream` needs
  **65** symbols — *worse* than `std::cout`, which at least reuses
  libstdc++'s pre-instantiated objects.
- **`<iostream>` collides with `libaxl-cxx.a`.** It drags `functexcept.o`,
  `new_opv.o` and `new_handler.o`, which multiply-define our `operator new[]`,
  the `std::__throw_*` stubs and `std::nothrow`. Linking needs
  `--allow-multiple-definition`, which silences a genuinely useful error.

### The alternative, measured

A stream with `<<` chaining over `axl_printf` / `axl_printerr` measured at
**667 bytes** over a plain `axl_printf` program (44461 vs 43794), freestanding.
That is the ergonomics of `cout` without the machinery, and it is the
recommendation.

**Spelled `axl::cout` / `axl::cin` / `axl::cerr`**, mirroring `std::` exactly.
Not `axl::out` / `axl::in` / `axl::err`, for a reason that is a hard compile
error rather than a preference: **`axl::err` is already the error constructor**
in `axl-cxx.hpp` (`return axl::err(AXL_INVALID);`), and redeclaring it as a
stream object fails with "redeclared as different kind of entity". Mirroring
`std::` also makes porting hosted code an `s/std::/axl::/`.

Design work still open — see `AXL-Cxx-Streams-Handoff.md`. `<<` is trivial;
`>>` is not, because it must report failure while chaining wants to return the
stream.
