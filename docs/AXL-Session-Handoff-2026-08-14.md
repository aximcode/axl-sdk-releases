# Handoff — 2026-08-14: T2/T3/T4/T5 all closed, exceptions wired, three guards added

> Self-contained. Every number here was measured on this tree on this date.
> `main` is at `372f2d53` and pushed; working tree clean apart from Mike's
> untracked `SCRATCH.txt` and six `docs/AXL-*.md` drafts.
>
> **Supersedes `docs/AXL-Session-Handoff-2026-08-13-2.md`.** Its §0 named T2 as
> the next task; T2 through T5 are done, and so is the U2/U3 work its §6 listed
> as queued.

---

## 0. Where to start

**Nothing is half-finished and nothing is broken.** Nine commits landed, each
with `verify.sh` ALL GREEN before it, and every consumer rebuilt clean.

The most valuable open item is **§7.1, the `AXL_TLS` prefix collision** — it is
small, it is a real footgun, and it is costing a full rebuild on every
gate run. After that, §7.2 (the CMake migration Mike wants) is the big one.

---

## 1. What shipped

| | |
|---|---|
| `00d98f1a` | **toolchain: `--enable-initfini-array`** — without it NO global constructor runs |
| `dbd7d296` | **x64 C++ compiles with OUR g++** — the last host input is gone |
| `b2b10a8f` | **T3: one C++ mode** — the freestanding/hosted split retired |
| `e16d3b5a` | `--hosted` REMOVED (not tolerated), and the CMake package gets its first test |
| `1af166de` | **U2/U3: real `try`/`catch` under UEFI**, both arches |
| `2229abc0` | **the CMake package CALLS `axl-cc`** instead of reimplementing it; T5 too |
| `e27ee5b1` | reject images whose constructors would silently never run |
| `13afef8a` | reject host headers and host libraries, at the one choke point |
| `372f2d53` | `--depfile` REMOVED — forward gcc's `-MD`, which also fixes a staleness bug |
| **release** | `toolchain-x86_64-elf-14.3.0-axl2` on `aximcode/axl-sdk-releases`, 6 assets |

**Task table state** (`AXL-Cxx-Design.md` §6a-PLAN): T1 done, **T2 done**,
**T3 done**, **T4 MOOT** (`include/compat/` was deleted outright when C moved
to the bare-metal cross — the row outlived its subject), **T5 done**.

## 2. The `.ctors` bug — the thing that cost the most to find

T2's premise on record was "`libaxl-cxx.a` becomes a multiple-definition error,
so T2 must change the LINK". **Measured: it does not.** `libaxl-cxx.a` is named
FIRST and archive selection is lazy, so libstdc++'s 51 colliding members are
never pulled. Proven with `ld -y` on the one path naming both archives:

    _Znwm           <- libaxl-cxx.a(axl-cxxabi-ops.o)
    __dynamic_cast  <- libstdc++.a(dyncast.o)

**The real blocker was `.ctors`.** GCC's `x86_64-*-elf` target ships
`HAVE_INITFINI_ARRAY_SUPPORT 0`, so the compiler emits global constructors into
the legacy `.ctors`. AXL walks `.init_array` and only that
(`src/runtime/axl-cxxabi.c`); the linker scripts' `__CTOR_LIST__`/`__CTOR_END__`
are read by nothing. So **every constructor silently did not run** — the
application's, and the 26 objects' worth inside the toolchain's own
`libstdc++.a` (libsupc++'s emergency exception pool among them).

    -axl    __init_array_start == __init_array_end   (empty)
            __CTOR_LIST__      != __CTOR_END__       (8 bytes, unwalked)
    -axl2   the reverse

**aa64 cannot be used to read this off**: its `auto-host.h` carries the same
`0`; the aarch64 port forces `.init_array` at the target level.

Mike's call was to fix it in the toolchain (one init path, not two) rather than
teach AXL to walk `.ctors`. Hence `14.3.0-axl2`.
`toolchain/x86_64-elf/build-toolchain.sh` now asserts the BEHAVIOUR in two
independently-failing places — the compiler's output, and the shipped
`libstdc++.a` — because a flag silently dropped reproduces the identical silent
failure.

**`/opt/x86_64-elf-gcc-14.3.0-axl` is still installed on this machine.** That
is why `e27ee5b1` exists (see §4).

## 3. Exceptions (U2/U3) — what the design doc did not anticipate

`axl-c++ -fexceptions` gives real `try`/`catch` on both arches.
`test-cxx-exceptions-qemu.sh`, 36 assertions.

- **No `--exceptions` flag.** `-fexceptions` is a real gcc flag the caller must
  pass anyway to get landing pads; axl-cc detects THAT, or an input object
  referencing `__gxx_personality_v0` so a staged build works. Same shape as the
  pre-existing `-frtti` detection, sharing one `nm -u` pass.
- **The `objcopy -j` entries are UNCONDITIONAL**, which §U2 did not anticipate.
  `cmp` says adding `.eh_frame`/`.gcc_except_table` to a C image is
  BYTE-IDENTICAL — `--gc-sections` already collected them. Gating them was the
  first shape and `check-flag-parity` correctly rejected it: the Makefile and
  the CMake package would each have needed the same conditional. The
  byte-identity constraint is met by the linker-script split alone
  (`elf_*_efi_eh.lds` carries the `KEEP`, worth +16.8% to a C image).
- **`__register_frame` runs from `_axl_cxxabi_run_init_array` via a WEAK
  reference**, not `_axl_init`: a pure-C image never pulls that object, so the
  hook costs it nothing. The fixture's first assertion is a global constructor
  that throws and catches BEFORE `main`, and nothing in it calls
  `axl_cxxrt_init` — a broken hook fails there.
- **AXL owns the newlib syscall stubs** (`src/cxxrt/axl-cxxrt-stubs.c`) rather
  than linking `libnosys.a`, which emitted ten "not implemented" warnings on a
  SUCCEEDING link. Both spellings are defined (`close` AND `_close`, …): ours
  calls the plain names, ARM's the underscored ones — the same trap
  `sbrk`/`_sbrk` documents, one family along.
- **NOT MEASURED: leak-free.** `axl_cxxrt_fini` was never called (review caught
  it; `--gc-sections` collected it), and now is via `axl_atexit`. Verified at
  the MECHANISM level — the symbol is linked and reachable, and removing the
  one `axl_atexit` call makes `nm` lose it entirely. But an integration image
  prints no leak verdict, so the run that showed none measured nothing. The
  `3/15112 -> 0` figure in `axl-cxxrt-eh.c` is the original author's.

## 4. Three guards added, each for a silent failure

1. **Unwalked constructors** (`e27ee5b1`). `axl-cc` checks
   `__CTOR_LIST__ != __CTOR_END__` after every link (6 ms), and
   `make check-ctors` scans what the MAKEFILE produced. NOT in `LINT_GATES` —
   artifact-scanning gates run concurrently with both arch builds under
   `verify.sh`; it declares prerequisites and runs from CI beside
   `check-no-avx`. Diagnostic names the culprit OBJECT (objdumps each input)
   rather than guessing a cause: two earlier drafts blamed the wrong thing.
2. **Host headers / libraries** (`13afef8a`). Two checks, because neither
   covers the other: a FLAG scan before compiling (the only one that can see
   `-Wl,-L/usr/lib` or a host `.a` passed positionally), and a separate `-M`
   pass after each compile (the compiler's own record; catches an absolute
   `#include "/usr/..."`). **`-M` not `-MM`** — `-MM` omits system headers, so
   an `-isystem /usr/include` leak is invisible to it (0 hits vs 19).
   `--allow-host-paths` is the opt-out.
3. **Dependency flags on a link** (`372f2d53`). Both spellings fail
   differently: with `-MF` the file IS written and its target names a scratch
   object that is deleted (worse than absent — it looks fine); without `-MF`
   nothing is written. Warning, not error: `-MD` in a shared CFLAGS reaching
   both steps is an ordinary Makefile pattern.

**The `-M` pass is SEPARATE from the real compile, and that is forced.** gcc has
ONE dependency mechanism; attaching `-MD -MF` collides with a consumer's own
`-MD/-MMD/-MF` — measured, it took the `-MF` they asked for and broke 11
assertions. Costs ~14 ms against a ~27 ms compile.

## 5. The CMake package — it now calls `axl-cc`

203 lines of re-implementation became 110 of delegation. `check-flag-parity`
polices TWO paths and PROVES the third delegates (matching `COMMAND ${AXL_CC}`
— a bare `${AXL_CC}` match was too weak, the file names it in a `FATAL_ERROR`
string too; sabotage caught that).

Two gaps the duplication hid, neither visible to the gate:

- it could not build an exceptions image;
- it had **no way to pass a compile flag at all** — `axl_add_app` makes a
  custom target, so `target_compile_options()` errors with "non-compilable
  target type". Hence the new `OPTIONS` keyword, forwarded to BOTH steps
  (review caught it reaching only the compile, silently dropping `-Wl,…` and
  `--minimal-runtime`).

`test-cmake-package.sh` is new and now BUILDS: C, driver, EMBEDS, multi-source,
C++, exceptions, plus no-op / project-header / SDK-header rebuild tracking.
**Until 2026-08-13 nothing had ever executed the CMake package** — no test, no
CI step called `find_package(axl)`.

## 6. `--depfile` removed, and why it was worse than redundant

It existed because a RELATIVE source made gcc emit cwd-relative prerequisites
CMake resolved wrongly, and because an absolute source changed `__FILE__` and
broke Makefile↔CMake object bit-parity (+7909 B). Both died: the package passes
absolute sources (measured: 153 prerequisites, 0 relative), and bit-parity
between two build systems stops mattering once the Makefile becomes CMake.

**It was also tracking nothing.** It used `-MMD` internally, which omits
`-isystem` headers — so editing an SDK header did NOT rebuild a consumer's
object. 292 headers listed under `-MD` vs 0. Found by writing the tracking
assertions FIRST: they went RED against the existing code.

AGT had already worked this out — its Makefile uses plain `-MD -MP -MF` and
says so at line 322. It never adopted `--depfile`.

## 7. Open — in the order I would take them

### 7.1 `AXL_TLS` shares a PREFIX with the non-TLS build (small, high value)

`PREFIX` splits on `BUILD` (`Makefile:34-38`) but NOT on `AXL_TLS`, so:

| | command | `AXL_TLS` | prefix |
|---|---|---|---|
| `verify.sh` → `test-axl.sh:28` | `make ARCH=… all tests` | off | `out/native-x64` |
| `run-integration.sh:132` | `make ARCH=… AXL_TLS=1 all tests tools …` | **on** | `out/native-x64` |

`AXL_TLS` IS in the build-state signature, so **each runner wipes the other's
objects** — `verify.sh` → integration → `verify.sh` rebuilds ~300 objects every
time, in both directions. It is also the mechanism behind false failures when
the two overlap in time (same prefix, concurrent writers — this bit me twice).

Fix: fold `AXL_TLS` into the `PREFIX` rule beside `BUILD`. An
`out/native-x64-tls` directory already exists, so something has done this
deliberately before. **Touches the build-state machinery**, which CLAUDE.md
warns has produced four wrong readings — verify carefully.

### 7.2 CMake as THE build system (Mike's stated direction)

In `ROADMAP.md`'s open backlog with what a port must carry. The CMake package
calling `axl-cc` already took three build paths to two.

### 7.3 Smaller, all recorded in ROADMAP / design docs

- **`verify.sh` never covers TLS.** Its ALL GREEN is 10393 of **10529** — the
  `AXL_TLS`-gated tests in `axl-test-{auth,crypto,jose,net}.c`. Measured green
  this session. Adding a TLS unit job costs ~1 min build + 50 s run. **Mike's
  call, offered and not answered.**
- **The toolchain repo split.** Raised by Mike; I recommended deferring (no
  build cycle exists — a release asset is a static file — but the cadence
  mixing is real, and my publish took the "Latest" badge off `v3.2.0` until I
  put it back).
- **`axl::string`** now competes with an always-available `std::string`.
- `lint.sh`'s C++ pass reads the CROSS libstdc++ as of this session; nothing
  host-side remains except `pe-set-debug`, which runs on the build machine.

## 8. Consumers — all five verified against this tree

| consumer | language | how the pin was overridden | result |
|---|---|---|---|
| AGT | C++ (most exposed) | `AXL_SDK_SRC=` | x64 + aa64 clean |
| axl-utils (`~/work/dell/delldiags/source/src/axl-utils`) | C | `AXL_CC=… AXL_SKIP_SDK_CHECK=1` | x64 + aa64 clean |
| axl-webfs | C | `AXL_SDK_SRC=` | clean |
| softbmc | C | `AXL_SDK_SRC=` | clean |
| uefi-devkit | orchestrator | `AXL_SDK=` | clean |

**An SDK 3.1.0 is installed at `/usr`**, and axl-webfs/softbmc default to it —
a bare `make` gives a FALSE PASS. axl-utils pins 3.1.0 via `.axl-sdk-version`
with a `check-sdk` gate.

Nobody passed `--hosted`; nobody used `find_package(axl)`. **The one action
item: anyone building C++ for x64 must `install-toolchain.sh x64` to get
`14.3.0-axl2`.**

## 9. Traps hit this session

- **The full `verify.sh` + `run-integration.sh` pair is ~25 min.** Mike's
  feedback: match the gate to the change. Docs → `--only=docs` (1m54s); one
  integration script → just that script; `axl-cc`/`install.sh` → the affected
  tests + `check-flag-parity`; `src/`/flags/`LINT_GATES` → the full pair.
- **Never run the two runners concurrently** — same prefix (§7.1). Produced 4
  false failures once and an "empty .so" objcopy error another time.
- **The HOST objdump cannot read `pei-aarch64-little`.** My section assertions
  used it, so on aa64 the positive one failed on a correct image and both
  negatives passed while BLIND. Use the cross objdump, and give the helper a
  `.text` control so "cannot read" is distinguishable from "not present".
- **`[[ false ]]` is TRUE in bash.** A sabotage replacing a guard condition
  with `false` made it fire on every link; only the control assertion caught it.
- **`grep -F t.h` cannot fail** when 40-odd SDK headers contain that substring.
- **A gate exempting `$SDK_DIR/` whitelists everything when `SDK_DIR` is
  `/usr`** (packaged install). My first fix for a false positive disabled the
  check exactly where it ships. Exempt the include DIR, not the prefix.
- **Piping a sabotage command through `tail` swallows its exit code**, so
  `--expect-fail` reported "NOT detected" on a sabotage the suite HAD caught.
- **`ugrep` is the local `grep`**; `${VAR}` in a pattern is treated as an
  interval quantifier and silently matches nothing. Use Python for exactness.
