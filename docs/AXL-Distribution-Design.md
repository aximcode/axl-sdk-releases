# AXL SDK — Distribution & Consumption Design

**Status:** DRAFT v1, for iteration. No implementation decision taken.
**Date:** 2026-07-29.

How axl-sdk is packaged, installed, discovered, version-pinned, and consumed
— from a distro package, from a tarball, and from an unreleased working tree.

Companion to `AXL-SDK-Design.md` (which covers what the SDK *contains*); this
doc covers how it *reaches and is used by* a consumer.

## Where this doc sits — three docs, one subject, different questions

| doc | answers | owns |
|---|---|---|
| [AXL-SDK-Design.md](AXL-SDK-Design.md) | what the SDK CONTAINS | toolchain requirement, C++ support, shipped layout |
| [AXL-Distribution-Design.md](AXL-Distribution-Design.md) | how it REACHES and is USED by a consumer | packaging, `find_package` discovery, version pinning, `out/` vs `stage/` (§4), **install layout and the `axl` dispatcher (§12–§13)**, P1–P7 |
| [AXL-Build-System-Design.md](AXL-Build-System-Design.md) | how WE build it | the CMake port, port-surface measurements (§8.2a), why `axl-cc` is excluded, `axl-config.cmake` extraction (§8.4) |

**One owner per shared fact, everyone else links** — see AXL-SDK-Design.md for
why that rule exists (two docs asserted the same stale premise for months
because each was internally consistent).

---

## 1. The target experience

The whole design serves one sentence, in three flavours:

```sh
curl -fsSLO .../releases/latest/download/install.sh
sh install.sh --toolchain x64
axl-cc hello.c                        # → hello.efi   (no root, any distro)
```

```sh
tar xf axl-sdk-linux-4.4.0-x86_64.tar.gz -C ~/opt
export PATH=~/opt/axl-sdk-4.4.0/bin:$PATH
axl-cc hello.c                        # → hello.efi   (no installer at all)
```

> **The `.deb`/`.rpm` flavour that stood here first is GONE (D2, §17).** The
> packages retired once `install.sh` proved out; §5.1's artifact matrix, §12.6's
> weak-dependency argument and everything else below that discusses them is the
> RECORD of why they existed and why they stopped, not a description of what
> ships. The published asset set is §14.1a's six.

```cmake
# CMake consumer — plain, idiomatic CMake
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$(axl-cc --print-prefix)/lib/axl/cmake/axl-toolchain.cmake
```
```cmake
find_package(axl 3.1 REQUIRED)
add_executable(hello hello.c)
target_link_libraries(hello PRIVATE axl::axl)
```

**Flavour 1 already works today** and is verified (as of the 3.1.0 era when
this was written; the tree is on 4.3.4 as of 2026-08-29): this machine had
`/usr/bin/axl-cc 3.1.0` from the .rpm, and a bare `axl-cc hello.c` in an empty
directory produces `hello.efi`. That is worth stating up front so we do not
re-solve it. Flavours 2 and 3 do not exist.

---

## 2. What consuming axl-sdk actually costs today

The honest measure of an SDK is what a downstream project has to build in order
to use it. Two real consumers, independently, built the same four workarounds.

**axl-utils** (`~/work/dell/delldiags/source/src/axl-utils`, uses BOTH CMake and
Make and keeps them bit-identical):

| Workaround | Root cause |
|---|---|
| `scripts/install-axl-sdk.sh` — detects apt vs dnf, curls the release repo, verifies SHA256, `sudo` installs | No tarball; no root-free path; no universal installer |
| `.axl-sdk-version` + `.axl-sdk-checksums` + a `check-sdk` target + `SDK_STAMP` wired in as a real make prerequisite of every object | No way to declare "this project needs 3.1.0", and no way for a build to notice `/usr` changed underneath it |
| `AXL_UTILS_INTELLISENSE` + phantom `<name>-ide` OBJECT libraries mirroring **every source file** | axl-cc runs via `add_custom_command`, so there is no usable `compile_commands.json`. An entire shadow build exists only to feed clangd / VS |
| `project(axl-utils LANGUAGES NONE)`; Visual Studio generator hard-rejected | CMake cannot be told "axl-cc is the compiler" |
| `AXL_SDK_INCLUDE_DIR` cache variable defaulting to a hardcoded `/usr/include/axl-sdk` | Nothing exposes the SDK prefix to a build script |
| A Makefile *and* a CMakeLists that must stay bit-identical, with `parity` / `script-parity` targets to enforce it | Neither can be derived from the other |

**AGT** independently built a parallel set: `sdk-sync` (reinstall on every
build), a content-hash `sdk-guard` (because installed mtimes were untrustworthy
— fixed in `7ce842e1`), and hand-tuned depflag logic.

Zero of that is about UEFI. It is all about consuming the SDK. A proper SDK
absorbs these costs once so consumers do not pay them repeatedly and
differently.

---

## 3. What already works — do not re-solve

- `.deb` / `.rpm` install to `/usr` with a clean FHS layout.
- Everything is **relocatable**: `axl.pc` uses `${pcfiledir}`, `axl-config.cmake`
  computes paths relative to itself, `axl-cc` resolves `SDK_DIR` from `$0`
  (with `cd -P`, so the usrmerge `/bin`→`/usr/bin` symlink case is handled).
- `pkg-config` version checks work: `pkg-config --atleast-version=3.1.0 axl`
  passes.
- Single package with `Provides: axl-sdk-devel`, so `dnf install axl-sdk-devel`
  resolves for muscle memory. (Already implemented — good call.)
- `axl-cc hello.c` with no `-o` already defaults to `hello.efi`.
- Reinstalling an unchanged SDK is now a filesystem no-op (`install -C`), so
  stage-then-consume against a checkout is cheap.

---

## 4. Layout: a build directory is not an install prefix

> **THE HEADLINE OF THIS SECTION SHIPPED (re-measured 2026-08-29).**
> `scripts/install.sh:18` is now `PREFIX="$SDK_DIR/stage"` — the install
> prefix left `out/` in 4.1.0, which is the one change this section leads
> with and the one its "Proposed" block asks for. What remains in `out/` is
> object trees plus `out/docs/`, a materially weaker complaint than the four-
> way overloading described below. **O1 is also partly answered by fact:**
> there IS a default `--prefix` and it is `stage/`. Read the rest as history
> plus the still-open `build/<arch>-<mode>/` half.

`out/` held four unrelated things when this was written:

```
out/native-x64/          Make's object tree (BUILD=DEBUG)
out/native-x64-release/  install.sh's object tree (BUILD=RELEASE)
out/{bin,lib,include,share}/   an INSTALL PREFIX
out/docs/                Sphinx output
```

A build tree and an install prefix have different lifetimes, different
gitignore semantics, and different audiences. Every other toolchain separates
them (`_build/` vs `--prefix`; builddir vs `DESTDIR`). Because `--prefix`
*defaults* to the build root, every doc says `./out/bin/axl-cc`, which teaches
consumers the wrong mental model.

Evidence of the damage: `~/axl-sdk-2.2.0`, `~/axl-sdk-2.2.1`, `~/axl-sdk-2.9.0`
are **full source checkouts**, not unpacked prefixes. With no tarball, the only
way to pin a version is to keep a copy of the whole source tree, build
directory and all.

Note `out/` as a *build* directory is a legitimate convention — axl-utils uses
`out/build/<preset>`, which is the Microsoft/VS default. The problem is
exclusively the overloading.

**Proposed:**

```
build/<arch>-<mode>/     all object trees        (gitignored)
build/docs/              Sphinx output           (gitignored)
stage/                   default local install prefix, if we keep a default
```

Open question O1 (below): keep a default `--prefix` at all, or require it?

### 4.1 The accessor landed 2026-08-16 — P2 is no longer a sweep

**`scripts/sdk-prefix.sh` answers "where is the staged SDK", as
`scripts/build-prefix.sh` already answers "where is the build directory".**
Callers ask instead of composing, so relocating the staged SDK is now one
environment variable (`AXL_SDK_PREFIX`) rather than an edit to every caller.
Verified end to end: staged to a scratch directory, the suite follows it, and
pointing it at an empty directory makes a test fail with "staged SDK missing"
— the control, without which "it followed" proves nothing.

Two helpers exist for the two questions, deliberately not one with a mode
flag: `test_build_dir` varies with ARCH x BUILD x AXL_TLS, `test_sdk_dir`
varies with nothing.

**This also retires a premise BOTH design docs carried** — that P2 and the
CMake port's slice 3 sweep the same ~149 make callers, so running them apart
pays for one wide sweep twice. Measured, the real overlap is **seven files**.

> **The measurement is owned by `AXL-Build-System-Design.md` §8.2a** — the
> per-category counts, the method, and why the earlier figure was wrong live
> there and are not repeated here. This doc states only what follows for P2.

What remained on this side was not a caller sweep at all: the staged SDK had
no accessor, so ~12 tests hand-composed it. That is what §4.1 above fixes.

**Consequence for sequencing:** P2 and the port no longer need to be run
together, and neither blocks the other. The argument for pairing them was
entirely the shared surface, and the surface is gone.

## 5. Artifact matrix

### 5.1 What to ship

| Artifact | Status | Rationale |
|---|---|---|
| `axl-sdk.deb`, `axl-sdk.rpm` | **have** | Distro-native path; installs to `/usr`; `Provides: axl-sdk-devel` |
| **`axl-sdk-<version>-linux-<host>.tar.gz`** | **SHIPPED 2026-08-29** | Was "the gap": root-free, distro-agnostic, and the only path for Arch / Alpine / NixOS / SUSE / CI containers / locked-down corp hosts. Built by `scripts/make-sdk-tarball.sh` (the same code the release and the test both run), one archive for both target arches, named for the HOST since the bundled crosses are x86_64-hosted. Its single top-level directory is `axl-sdk-<version>/` — §12.2's versioned root, so `tar xf -C /opt` needs no rename. Turns `~/axl-sdk-3.1.0/` into a real prefix instead of a source copy |
| `axl-sdk-host-tools.{deb,rpm,tar.gz}` | have | run-qemu.sh + helpers. **"Correctly separate" was SUPERSEDED 2026-08-29** — §12's D3 folds it into the SDK package. The objection recorded here (different deps) is real and is answered in §12.6 by demoting QEMU/OVMF to weak deps; without that answer the merge is a regression |
| `axl-sdk-tools-{x64,aa64}.tar.gz` | have | Pre-built `.efi` utilities for USB-stick use. Correctly separate — these are *target* binaries, not SDK material |
| `SHA256SUMS` | have | |
| `axl-sdk-doc` | absent | See §5.4 |

The design doc `AXL-SDK-Design.md` currently documents a tarball workflow
(`tar xf axl-sdk-x64-linux.tar.gz`) that **has never existed**. That must
either be built or the doc corrected; today it is a false promise.

### 5.2 Do we need a `-devel` split? — No.

Standard distro practice splits `foo` (runtime `libfoo.so.N`) from `foo-devel`
(headers, `.pc`, cmake configs, static libs). **That split does not apply to
us, because there is no runtime half.** axl-sdk ships:

- host executables that are *build tools* (`axl-cc`, `axl-c++`, `pe-set-debug`),
- **target-architecture** static libs (UEFI x64/aa64 — not host-loadable at all),
- headers, linker scripts, `.pc`, cmake config, JSON5 sidecars.

100% of the payload is development material. A `-devel` split would leave the
base package empty or holding only the compiler driver. This is why the peer
group — `gcc-arm-none-eabi`, the Android NDK, Emscripten, Zig — all ship as
single toolchain packages.

**Recommendation: keep one package.** The existing `Provides: axl-sdk-devel`
already satisfies muscle memory and `dnf install axl-sdk-devel`. Nothing to do.

### 5.3 The split that *is* worth considering: per target arch

Measured payload:

| Component | Size |
|---|---|
| `lib/axl/x64/` | 16 MB |
| `lib/axl/aa64/` | 16 MB |
| `include/` | 2.5 MB |
| `share/axl/` + `bin/` | ~130 KB |
| **installed total** | ~40 MB |

> **Re-measured 2026-08-29 (v4.3.4): 42 MB staged — aa64 20 MB, x64 19 MB,
> include 3.0 MB, bin 132 KB, share 84 KB.** So it is now **39 of 42 MB** in
> per-arch libraries and the saving for a single-arch consumer is ~20 MB, not
> 16. The conclusion below is unchanged; only its magnitude moved.

**32 of 33 MB staged is per-arch libraries.** A consumer targeting only x64
carries 16 MB of AArch64 archives they will never link. So the meaningful axis
is target arch, not runtime/devel:

```
axl-sdk         common: axl-cc, axl-c++, pe-set-debug, headers,
                cmake, pkg-config, linker scripts, sidecars   (~2.6 MB)
axl-sdk-x64     lib/axl/x64/                                   (16 MB)
axl-sdk-aa64    lib/axl/aa64/                                  (16 MB)
```

Honest cost/benefit: saves ~16 MB for single-arch users and makes the
dependency graph express something true. Costs three fpm invocations instead of
one, three smoke tests, cross-package `Requires`, and a decision about what a
bare `dnf install axl-sdk` pulls. **My lean: not worth doing on its own for
16 MB, but worth doing at the same time as §5.5 if we ever write a real spec.**
Flagging rather than recommending.

### 5.4 Docs package — blocked on a real problem

> **Re-measured 2026-08-29: 139 MB of HTML and 67 MB of man pages** — 62% and
> 79% smaller than the figures below. The argument still stands; it is now
> roughly a third the size it was stated at. Do not quote the original numbers.

Sphinx emitted **372 MB of HTML and 314 MB of man pages** when this was written. That is not
a packaging decision, that is a bug — almost certainly per-symbol man page
explosion and duplicated assets. Packaging docs is cheap and desirable, but it
is gated on finding out why the output is three orders of magnitude larger than
the SDK itself. Separate investigation.

### 5.5 Source RPM / distro inclusion — not now, but know the blocker

We build packages with `fpm -s dir -t rpm`, which wraps a pre-built directory.
That means: **no `.spec` with `%prep`/`%build`/`%install`, no `.src.rpm`, no
`-debuginfo` subpackage, and no way for anyone to rebuild the package from
source.** fpm output cannot go into Fedora, EPEL, or Debian proper.

That is fine for direct-download distribution, which is what we do. But it is
the hard blocker if distro inclusion is ever a goal, and it is worth being
explicit that we have chosen the easy path. Writing a real `.spec` +
`debian/rules` is a meaningful project, not a tweak.

**Recommendation: stay on fpm. Revisit only if distro inclusion becomes a goal.**

---

## 6. Discovery — how a build finds and uses the SDK

This is where the real gap is, and where the consumer workarounds come from.

| Mechanism | Status | Notes |
|---|---|---|
| `pkg-config` (`axl.pc`, `axl-x64.pc`, `axl-aa64.pc`) | **have, works** | Version checks work today |
| `find_package(axl)` | **have** | But see below |
| `find_package(axl 3.1 REQUIRED)` | **BROKEN** | No `axl-config-version.cmake` exists, so *any* version request makes CMake reject the package. This one missing file is why axl-utils hand-rolled `.axl-sdk-version` |
| **CMake toolchain file** | **MISSING** | The big one — §6.1 |
| **Meson cross-file** | **MISSING** | ~30 lines; same idea |
| `axl-cc --print-prefix` | **MISSING** | Why axl-utils hardcodes `/usr/include/axl-sdk` |

### 6.1 The CMake toolchain file — the highest-leverage single change

Today `axl-config.cmake` drives everything through `add_custom_command` calling
axl-cc. It works, and it produces bit-identical output to the Makefile path,
which was the design goal. But CMake never learns that a compiler is involved,
so consumers lose everything CMake would otherwise give them:

- no `target_link_libraries` / usage requirements — the dependency graph is
  hand-wired through custom commands,
- no `compile_commands.json` for the real targets → clangd and Visual Studio
  are blind → **axl-utils' entire phantom `-ide` shadow build exists for this
  single reason**,
- no `target_compile_options` / `target_compile_definitions` per target
  (**partly addressed 2026-08-14**: the package grew an `OPTIONS` keyword,
  because `axl_add_app` makes a custom target and `target_compile_options`
  rejects it outright — so before that there was no way to pass a compile flag
  at all),
- CMake cannot do its own header dependency scanning → this is why
  `axl-cc --depfile` was invented. **REMOVED 2026-08-14**: the package now
  passes ABSOLUTE sources, so plain `-MD` already emits an all-absolute
  depfile and the post-processing had nothing left to do. The flag also used
  `-MMD` internally, which omits `-isystem` headers — so it tracked NO SDK
  header, and editing one did not rebuild a consumer's object,
- `project(LANGUAGES NONE)` and no Visual Studio generator.

The idiomatic answer for a cross-compilation target is a **toolchain file**:

```cmake
# lib/axl/cmake/axl-toolchain.cmake  (sketch)
set(CMAKE_SYSTEM_NAME       Generic)      # or UEFI, with a custom platform module
set(CMAKE_SYSTEM_PROCESSOR  x86_64)
set(CMAKE_C_COMPILER        ${AXL_PREFIX}/bin/axl-cc)
set(CMAKE_CXX_COMPILER      ${AXL_PREFIX}/bin/axl-c++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)   # no host-runnable output
```

Prior art to follow closely: Emscripten's `Emscripten.cmake`, the Android NDK's
`android.toolchain.cmake`, ESP-IDF. All ship a wrapper compiler *and* a
toolchain file; the wrapper is for the CLI case, the toolchain file is for the
build-system case. We ship only the wrapper, so every CMake consumer bridges
the gap itself, badly, and differently.

**Non-trivial design questions this raises** (which is why it deserves its own
pass, not a bullet in this doc):

1. axl-cc is not a drop-in `cc`. It consumes `--arch`, `--type`, `--embed`,
   `--service`, and it *links + objcopies + patches PE debug info* in one step.
   CMake will drive it as `<CC> -c` for compiles and `<CC> ... -o exe` for
   links. Does axl-cc's existing CLI already satisfy CMake's compiler ABI, or
   does it need a stricter gcc-compatible mode?
2. `CMAKE_SYSTEM_NAME Generic` vs a real `Platform/UEFI.cmake` module.
3. Arch selection: toolchain file per arch, or one file parameterised by
   `-DAXL_ARCH=`?
4. Does `axl_add_app` / `axl_add_driver` survive as sugar on top, or get
   replaced by plain `add_executable` + properties?
5. Bit-parity: axl-utils depends on Make and CMake producing **identical**
   binaries. A native CMake link path must preserve that or the parity target
   breaks.
6. `--service` compiles one source twice into two images. That has no
   `add_executable` equivalent; it likely stays a custom command.

### 6.2 Cheap wins in the same area

- `axl-config-version.cmake` — a few lines, unblocks `find_package(axl 3.1)`,
  retires hand-rolled version stamps. Should ship regardless of §6.1.
- `axl-cc --print-prefix` (gcc spells it `--print-sysroot`) — lets a plain
  Makefile stop hardcoding paths.
- Meson cross-file — small, and makes us legible to the Meson world.

---

## 7. Version pinning

Today a consumer that needs "3.1.0 exactly" writes its own `.axl-sdk-version`
plus checksums plus a `check-sdk` target plus a stamp wired in as a make
prerequisite. Everything needed to retire that already exists except one file:

- `pkg-config --atleast-version=3.1.0 axl` — **works today**.
- `find_package(axl 3.1 REQUIRED)` — needs `axl-config-version.cmake`.
- Plain Make — needs `axl-cc --print-prefix` / a version query. `axl-cc
  --version` already prints `axl-cc 3.1.0 (gcc, built …)`, which is greppable
  but not a stable machine interface. Consider `axl-cc --print-version` emitting
  a bare `3.1.0`.

The "did the SDK change under me?" problem (axl-utils' `SDK_STAMP`, AGT's
`sdk-guard`) is now largely solved by `install -C`: an unchanged reinstall
touches nothing, so ordinary mtime dependency tracking works. Worth telling
both consumers they can likely simplify.

---

## 8. Consuming an unreleased working tree

This is the part that already has the right shape and should be preserved.

AGT does `AXL_SDK_SRC=../axl-sdk` → runs `install.sh --arch $(ARCH)` → consumes
the resulting prefix. That is **stage-then-consume**, and its virtue is that
there is exactly *one* consumption model whether the SDK came from a package,
a tarball, or a checkout. The alternative — pointing a consumer directly at the
source tree — would be worse: headers in `include/`, libs in
`out/native-x64/lib/`, no unified prefix, no `.pc`, no cmake config.

Keep it. Two refinements:

1. Stage to an explicit prefix outside the source tree (§4), so `AXL_SDK_SRC`
   consumers get a real prefix rather than something that looks like build
   scratch.
2. Document it as a **supported, first-class workflow** with a name. It is
   currently folklore that each consumer reinvented.

One thing this does *not* fix, and should not pretend to: switching
`AXL_SDK_SRC` between two different trees. Depfiles record absolute paths, so
after a switch the old tree's headers are still present and unchanged. A
content fingerprint (AGT's `sdk-guard`) remains the right tool there. The two
mechanisms are complementary.

---

## 9. Smaller UX items found while surveying

- `axl-cc hello.c` emits **`hello.efi` *and* `hello.so`**. The `.so` is
  deliberate and load-bearing (ELF with DWARF, for `addr2line` and the RSOD
  decoder; `pe-set-debug` patches the PE debug directory to match). But it is
  **undocumented in `--help`**, so it reads as litter in the consumer's source
  directory. Document it; consider `--no-debug-sidecar`.
- `AXL-SDK-Design.md`'s "Distribution Model" section documents the
  non-existent tarball as the consumer workflow, and describes the layout as
  `out/lib/libaxl.a`, which is not where it goes. Needs rewriting against
  reality.
- README leads with `./out/bin/axl-cc`, reinforcing the wrong mental model.

---

## 10. Phasing (proposed, not decided)

**P1 — Tarball + discovery cheap wins.** Mostly mechanical, deletes real
consumer code, no design risk.
- Ship `axl-sdk-<ver>-linux-<arch>.tar.gz` from `install.sh --prefix`.
- `axl-config-version.cmake`.
- `axl-cc --print-prefix` / `--print-version`.
- Correct `AXL-SDK-Design.md` + README.

**P2 — Separate build dir from install prefix.** **Half done 2026-08-16, and
it was never wide** — see §4.1. `scripts/sdk-prefix.sh` + `test_sdk_dir` exist,
the ~12 hand-composed callers are converted, and `AXL_SDK_PREFIX` relocates
the staged SDK (verified with a control). The measured overlap with the CMake
port's slice 3 is **7 files, not 149**, so this no longer has to be paired
with the port or done in one sweep.

What remains is the part that IS a decision rather than a mechanism: whether
the default moves from `out/` to `stage/` (and `out/native-*` to
`build/<arch>-<mode>/`) — that is O1, and it breaks every existing invocation
and doc line, which is why the accessor deliberately kept `out` as its
default. The accessor makes that change one edit whenever you want it.

**P1's `axl-config-version.cmake` is folded into the CMake PORT** (decided
2026-08-16), because the package it belongs to is generated from a 334-line
heredoc inside `install.sh`, and extracting that to `cmake/*.cmake.in` is
port scope — see `AXL-Build-System-Design.md` §8.4. Until it exists,
`find_package(axl 4.1 REQUIRED)` cannot enforce a version, which is the call
§1 of this doc advertises.

**P3 — CMake toolchain file.** Its own design pass, per §6.1's open questions.
Success criterion: axl-utils can delete `AXL_UTILS_INTELLISENSE`, the phantom
`-ide` targets, `project(LANGUAGES NONE)`, and the VS-generator rejection —
while keeping Make/CMake bit-parity.

**P4 — Meson cross-file.** Small, follows P3's shape.

**P5 — the `axl` dispatcher (§13).** `scripts/axl`, `libexec/axl/` staging in
`install.sh`, `--print-prefix` / `--print-version`. A shell script plus
staging; touches no build system. **Do this first of the new phases** — it
fixes a defect live today for every package user, it is purely additive so no
existing invocation changes, and Build-System §3 puts `axl-cc` and its
siblings outside the port, so it need not wait for one.

**P6 — versioned install root + pruning (§12.2, §12.4).** **Partly SHIPPED
2026-08-29; and its first sentence was wrong.** It read *"install.sh's default
prefix becomes /opt/axl-sdk-<ver> (root) or ~/.local (not root)"* — but
`stage/` is the RIGHT default for a source checkout, is what §4's shipped
change made it, and `verify.sh`, the integration runner and dozens of tests
depend on it. Changing it would have broken the tree to serve a case that has
another answer.

The versioned root does not need a new default: **the tarball's top-level
directory already is one**, so `tar xf -C /opt` yields `/opt/axl-sdk-<ver>`.
What was actually missing is the part that keeps it bounded, and that shipped
as **`axl prune`** — see §12.4. Still open: whether anything should manage the
`current` symlink for the user, or whether creating it stays theirs (`axl
prune` already honours it either way). `install.sh`'s
default prefix becomes `/opt/axl-sdk-<ver>` (root) or `~/.local` (not root),
plus the `current` symlink and an install manifest for §12.4's pruning.
**P1's tarball bullet is its prerequisite**, not a parallel item: until
`axl-sdk-<ver>-linux-<arch>.tar.gz` exists there is no artifact a non-package
user can install, so a `~/.local` default serves nobody. Sequence the tarball
*after* P5 so the first one ever published already contains `bin/axl` and
`libexec/`, rather than changing shape between two releases.

**P7 — package merge + old-location cleanup (§12.5–12.6). SHIPPED
2026-08-29.** Most of it turned out to be **already done**: P5's `install.sh`
change stages the host tools to `libexec/axl/` with the dispatcher in `bin/`,
and the `.deb`/`.rpm` are built from that same `install.sh --prefix`, so the
CONTENT merged as a side effect of shipping `axl`. What remained was metadata
— weak deps and the retirement of the old package — plus deleting 92 lines of
release workflow that built a layout nothing consumes any more.

**P8 — root-free toolchain install (§12.7).** `_DEFAULT` twins for the two
`*_TOOLCHAIN_DIR` keys, and `install-toolchain.sh --prefix` requiring root
only when the prefix does. **Prerequisite for P6's `~/.local` half**: without
it that root gets a compiler driver that cannot compile. Independent of the
port — see §12.7's correction.

**Rides with the CMake port, not before it:** `axl-config-version.cmake`, which
AXL-Build-System-Design.md §8.4 already assigns to the port and which is the
actual blocker on `find_package(axl <ver>)` — a versioned root lets a consumer
*hold* two versions, not *request* one.

**Deliberately out of scope:** switching axl-sdk's own build from GNU Make to
Meson/ninja. It is a legitimate question, but it would not fix a single item in
§2 — those are all consumer-facing. Decide it separately, on its own merits.

> **Decided separately, and it is CMake** —
> [AXL-Build-System-Design.md](AXL-Build-System-Design.md), in progress since
> 2026-08-15. That doc reaches this paragraph's conclusion from the other
> direction and cites it: the in-tree build system is invisible to consumers,
> so the port must justify itself on internal grounds alone (it does — the
> hand-rolled build-state signature IS a re-configure) and it needs no major
> version. **Two items here interact with it and are NOT independent:**
>
> - ~~**P2 touches the same surface.** 157 files invoke `make` … so the port
>   proposes absorbing P2 (its §8.2). Open; scope call not yet taken.~~
>   **WITHDRAWN — and this bullet contradicted P2's own paragraph three above,
>   which already said "never wide … 7 files, not 149 … no longer has to be
>   paired with the port."** AXL-Build-System-Design.md **§8.2a** formally
>   withdrew it on 2026-08-16: the measured shared surface is **seven files**.
>   The scope call is not open. (Caught 2026-08-29 while auditing this doc.)
> - **P3 does not come free with it.** The in-tree build drives the bare-metal
>   crosses directly and never puts `axl-cc` in a compiler slot, which is the
>   entire difficulty of §6.1. An in-tree `compile_commands.json` is not
>   evidence a *consumer* can get one. Its §8.3 spells this out.

---

## 11. Open questions

- **O1.** ~~Keep a default `--prefix`, or require it explicitly?~~ **PARTLY
  ANSWERED BY FACT, then decided 2026-08-29.** A default already exists and is
  `stage/` (`install.sh:18`), so the "require it" option was foreclosed
  without being chosen. §12's D1 settles the rest: keep a default, and make it
  root-dependent — `/opt/axl-sdk-<ver>` or `~/.local` — chosen by a flag with
  a sensible default, never guessed.
- **O2.** Per-target-arch package split (§5.3) — worth 16 MB, or not yet?
- **O3.** Tarball granularity: one per host+target combination, or a single
  fat tarball with both target arches (~40 MB)?
- **O4.** ~~Does the tarball need a `bin/axl-env.sh`?~~ **ANSWERED 2026-08-29:
  no** — §13's dispatcher puts one binary on `PATH`, which beats an env script
  the user must remember to source.
- **O5.** Windows/WSL: axl-utils builds under "VS 2022 CMake → WSL". Is a
  native-Windows story ever in scope, or is WSL the permanent answer?
- **O6.** Do we care about distro inclusion (Fedora/EPEL/Debian)? Only that
  would justify the real-`.spec` work in §5.5.

---

## 12. Install layout — one root per method

**Decided 2026-08-29.** This section owns *where the bytes go*; §13 owns
*what is on `PATH`*. Together they answer **O1** and **O4**.

| | decision |
|---|---|
| **D1** | One root per install method. Packages keep `/usr`; the installer defaults to a **versioned** `/opt/axl-sdk-<ver>`, or `~/.local` when not root. |
| **D2** | **Versioned root** with a `current` symlink, so a pinned consumer can hold two versions at once. |
| **D3** | **Host-tools folds into the SDK package** (superseding §5.1's "correctly separate"), with old locations cleaned up subject to §12.5. |
| **D4** | A host-side **`axl` dispatcher** — §13. |

The direction is **not** "detect sudo and fork the layout". It is one shape
varying only the root, which is what rustup, Homebrew, ESP-IDF and ARM's GNU
toolchain all do. The root is **chosen by a flag with a sensible default,
never guessed**: a layout that varies by an invisible condition gives every
doc line, error message and consumer script two possible answers. This tree
has paid that twice — the crash-handler hint prints a command that is not on
`PATH` for a package user, and the no-root path runs through
`~/axl-sdk-host-tools/`, which **`README.md` itself prescribes**
(`mkdir -p ~/axl-sdk-host-tools && tar xf … -C ~/axl-sdk-host-tools`) and
axl-utils' `install-axl-sdk.sh` then implements. *That is ours, not a
consumer's invention* — an earlier draft of this section said axl-utils
invented it, which was wrong and let the fix look like someone else's problem.
It exists because no root-free SDK root was ever offered, so the README had to
name one.

### 12.1 Two measurements that removed the main objections

- **`find_package(axl)` already works from `/opt/axl-sdk*`.** *Measured
  2026-08-29:* `/opt` is in CMake's default `CMAKE_SYSTEM_PREFIX_PATH`
  (with `/usr/local`, `/usr`, `/`, `/usr/X11R6`, `/usr/pkg`), and CMake's
  `<prefix>/<name>*/` rule matches a directory named `axl-sdk*`. A real
  `find_package(axl REQUIRED)` resolved against a simulated
  `<prefix>/axl-sdk/lib/cmake/axl/`. **The standing objection to `/opt` —
  that it breaks discovery — is false.**
- **A user-prefix install already works end to end.** *Measured:*
  `install.sh --arch x64 --prefix <scratch>` produced `{bin,include,lib,share}`
  and the relocated `axl-cc` compiled a 37,376-byte PE32+ EFI application.

**Consequence: D1/D2 are defaults-and-packaging work, not relocation work.**
§3 already recorded that everything is relocatable; these confirm it reaches
the two roots we actually want.

**Documents that change when this lands**, listed so they are not rediscovered
mid-phase: [RELEASING.md](RELEASING.md) (its artifact list and the
`SHA256SUMS` set — P1's tarball and P7's merge both alter it),
[AXL-SDK-Design.md](AXL-SDK-Design.md) (owns *what* ships, and its
Distribution Model section already carries the unbuilt-tarball promise), and
`README.md`, whose "Any distro (tarball)" block is the origin of
`~/axl-sdk-host-tools/` and is what P5/P6 replace.

### 12.2 Target layout

```
/opt/axl-sdk-4.3.5/          versioned root (root install)
/opt/axl-sdk  -> axl-sdk-4.3.5   `current` symlink
    bin/      axl, axl-cc, axl-c++, axl-install-toolchain, pe-set-debug
    include/  axl/ ...
    lib/      libaxl.a, cmake/axl/, pkgconfig/axl.pc, axl/*.lds
    libexec/  axl/   <- host-tools scripts, reached via `axl <cmd>`
    share/    axl/, doc/

~/.local/                    non-root install; identical shape
/usr/                        .deb / .rpm only; unchanged
```

`libexec/axl/` rather than `share/axl/scripts/`: these are executables the
dispatcher invokes, not architecture-independent data. The `current` symlink
is what makes D2 useful — a consumer pins `/opt/axl-sdk-4.3.2` while `current`
moves on, and both are present.

**Not in scope: moving the `.deb`/`.rpm` off `/usr`.** It gains nothing now
that both roots work for CMake, and would discard the usrmerge handling
`axl-cc` was specifically patched for (§3).

### 12.3 A prefix is self-contained — the contract every prune rests on

**Stated as a contract, and asserted, since 2026-08-29** (`test-sdk-selfcontained.sh`).

> **An SDK prefix writes nothing outside itself, so `rm -rf <prefix>` is a
> complete uninstall.** The single exception is the bare-metal cross
> toolchains, which live outside on purpose: 739 MB shared across every SDK
> version, installed separately by `axl-install-toolchain`.

Everything below depends on it — a versioned root you cannot safely delete is
worse than no versioning at all — and it was believed true while asserted
nowhere, which is exactly how it would stop being true: one
`install ... "$HOME/.config/..."` and the guarantee is silently gone with every
other test still green. The test installs with a HOME of its own and checks it
stays empty, and checks that the generated path-encoding files
(`axl-config.cmake`, `axl.pc`, the staged manifest) name nothing absolute
beyond the prefix, the manifest's toolchain roots, and ordinary system
directories. Both halves are sabotage-verified.

### 12.4 Bounding what accumulates — `axl prune`

**SHIPPED 2026-08-29.** Policy: **current + one previous**, on both axes.

The numbers that set it, measured 2026-08-29: **7.1 GB across eight pinned SDK
versions** in one `$HOME`, every one a source checkout whose `out/` is ~85% of
it. The tarball drops a pinned version to **42 MB extracted / 7.2 MB
downloaded** — a 35x cut — so SDK roots are now cheap. **The toolchains are
not**: 500 MB for ARM's and 239 MB for ours, **739 MB per generation**, and
`--prefix` (P8) just made them installable in more places. At 12 tags in 17
days, "it will not add up" is not an argument that survives contact.

Why one previous rather than none: the value of an old root is a **fast
rollback** when a version bump breaks a build. Beyond one, a pinned checksum
plus a 7.2 MB download is cheaper than storage.

**One previous *toolchain* generation is kept too, and that is deliberate** —
an SDK root kept for rollback was built against the toolchain of its day, so
discarding that toolchain would break the rollback the policy exists to
provide.

Three things are never removed, and they are the whole design:

1. **The running prefix.** The one removal that cannot be undone by
   re-downloading, because it takes the tool that would do the re-download.
2. **Whatever `current` resolves to.** Something else on the machine points at
   it.
3. **Anything not identifiably ours.** The root is shared — `/opt` has other
   people's software in it — so a candidate must match a name we generate,
   anchored, *with a version after it*: `^axl-sdk-[0-9]`. `^axl-sdk-` alone
   would match a consumer's `axl-sdk-workspace`. Toolchain families are read
   from the staged manifest rather than hardcoded to `/opt`, so a relocated
   toolchain is still pruned and an unrelated directory still is not.

`--dry-run` prints what it would remove and removes nothing. All of it rests
on §12.3: deleting a prefix is a complete uninstall.

### 12.5 Cleanup of old locations — two of three must NOT be deleted

| location | owner | action |
|---|---|---|
| `/usr/share/axl-sdk-host-tools/`, `/usr/bin/{run-qemu,axl-emulate}` | **dpkg / rpm** | **Package metadata, never `rm`.** deb `Replaces:`+`Breaks:`, rpm `Obsoletes:` (fpm: `--replaces`/`--conflicts`). Deleting files another package owns leaves the package database describing files that are gone. |
| `~/axl-sdk-host-tools/` | **the consumer** — axl-utils' own `install-axl-sdk.sh` creates it and tracks it with its own `.installed-version` | **Do not touch.** Ours removing it breaks their idempotency check and reaches into a user's home. They migrate when they bump their pin. |
| `/opt/axl-sdk-<older>/`, stale `/opt/<toolchain>-<older>/` | **us** | Prune, `--keep N` (default: keep one previous), never the version `current` points at, and **only roots recorded in a manifest we wrote**. A glob over `/opt` that grows a character deletes someone's IDE. |

### 12.6 The merge, and the dependency objection it has to answer

§5.1 called host-tools "correctly separate — different audience, different
deps (QEMU, OVMF, virtiofsd)". That objection is real: `axl-sdk-host-tools`
hard-depends on `qemu-system-x86`, `qemu-system-arm`, `ovmf`,
`qemu-efi-aarch64`, `virtiofsd`, `mtools`, `dosfstools`, and dragging those
onto every `axl-cc` user is a regression. It is **why axl-utils bypasses that
package today** in favour of the tarball.

**Resolution: demote them to weak dependencies** — deb `Recommends:`, rpm
`Recommends:` (RPM >= 4.12). `axl-sdk` keeps its hard `curl`/`xz`, which
`axl-install-toolchain` genuinely needs.

**VERIFIED 2026-08-29, by building throwaway packages and reading them back
rather than from `fpm --help`:**

| | flag | result |
|---|---|---|
| deb | `--deb-recommends` | `Recommends: qemu-system-x86, ovmf` in the control file |
| rpm | `--rpm-tag 'Recommends: X'` | real weak dep — `rpm -qp --recommends` lists both |
| both | `--replaces` (+ `--conflicts` on deb) | deb `Replaces:`+`Conflicts:`, rpm `Obsoletes:` |

fpm 1.17.0 has no `--rpm-recommends`, but `--rpm-tag` writes a spec tag
verbatim and RPM >= 4.12 honours `Recommends:` as weak.

A trap worth recording: `dpkg-deb -f` printed **nothing** on this AlmaLinux
box, which reads identically to "the package has no such fields". `dpkg-deb`
was simply not installed. The control file had to be pulled out with `ar` +
`tar` to see the fields — "the tool could not run" and "the tool ran and found
nothing" are the same empty output and opposite facts.

**SHIPPED.** `axl-sdk` now carries `Replaces`/`Conflicts`/`Obsoletes` for
`axl-sdk-host-tools` and the QEMU stack as `Recommends`; the standalone
host-tools `.deb`/`.rpm` are retired. **The tarball is kept** — `README.md`
names a real audience for it (people who want `run-qemu` without the
build-side SDK), and a tarball serves them with no package metadata, no
dependency graph and no root. Retiring the packages removed a duplicate
install path; it was not a reason to strand that audience.

`test-host-deps-minimal.sh` (debian:stable-slim, no toolchain) is the gate for
this. The release smoke test **cannot** be: it runs on a runner that already
ships gcc, g++ and binutils, which is exactly the blindness that let a missing
`g++` dependency ship.

### 12.7 The toolchain is the real blocker on a root-free install

Mechanism owned by [AXL-Build-System-Design.md](AXL-Build-System-Design.md);
recorded here only as the consumer-facing consequence.

`install-toolchain.sh` **hard-fails without root** ("extracting to /opt needs
root, and this is not root and has no sudo") and has no `--prefix`. So a
`~/.local` SDK today yields a compiler driver that cannot compile.

**SHIPPED 2026-08-29 as P8, and it was smaller than two earlier drafts of this
paragraph claimed.** Recording both wrong turns, because each was a plausible
reading of the same file:

1. *"Rides with the CMake port"* — on the grounds that `axl-toolchains.conf`
   cannot express `${VAR:-default}` (true; its `KEY=VALUE` must parse as both
   `sh` and `make`) and that the port deletes the `make` half. **Wrong**: the
   conf never had to express it. The override resolves in the *consumer*, a
   convention already in production for six keys — `$(or $(AXL_X64_GCC),
   $(AXL_X64_GCC_DEFAULT))` in make, `"${AXL_X64_GCC:-$AXL_X64_GCC_DEFAULT}"`
   in `install.sh`, and the documented `AXL_<ARCH>_GCC` / `_GXX` /
   `_BINUTILS_PREFIX` in `axl-cc`.
2. *"Add `_DEFAULT` twins for the two `*_TOOLCHAIN_DIR` keys"* — **also
   unnecessary.** Those keys are read by `install-toolchain.sh` (as its
   install target), `.github/workflows/ci.yml` (as cache keys) and two
   integration tests. **No build path reads them**: `axl-cc` and the `Makefile`
   locate compilers through the `GCC`/`GXX`/`BINUTILS_PREFIX` locators, which
   already relocate. The conf's own header says exactly that — "set them to
   build against a toolchain installed somewhere other than /opt — a per-user
   prefix, a CI cache, or a locally built tree".

So **using** a relocated toolchain already worked. Only **installing** one did
not: `install-toolchain.sh` wrote `/opt` into the extract, the usage banner and
the refusal, and hard-failed with *"extracting to /opt needs root"*.

What shipped:

- **`--prefix DIR`**. The per-arch directory keeps its manifest name under the
  chosen root, and each tool path is the manifest's own with that directory's
  prefix rewritten — so "the directory prefixes the compiler", which
  `check-toolchain-conf` asserts of the manifest, stays true of what is
  installed.
- **Privilege follows the destination, not `/opt`.** A prefix the user owns
  needs no `sudo` at all; the refusal names the directory that could not be
  written and suggests `--prefix $HOME/.local/opt`.
- **The run prints the `AXL_<ARCH>_*` exports** for a non-default prefix. The
  manifest's defaults still name `/opt`, and the installer is the only party
  that knows which prefix was chosen — succeeding silently would leave a user
  with ~96 MB on disk and a build that still cannot find a compiler.

`test-toolchain-prefix.sh` shims `curl`/`sha256sum`/`tar`/`sudo` on `PATH` so
the real control flow runs with no 739 MB download, with the control
`test-hermetic-toolchain.sh` established: prove the shims shadow the real tools
before asserting anything, then clear the call log so the control does not
pollute the "never escalated" assertion it exists to make trustworthy.

---

## 13. The `axl` dispatcher — answers O4

One name on `PATH`; everything else is a subcommand. The same idiom the
project already uses for `axl.efi` (the UEFI busybox), so it introduces no new
concept — only a host-side sibling.

```sh
axl rsod-decode --syms app.map --rsod putty.txt
axl run-qemu -i --mount . app.efi
axl emulate <fixture> app.efi
axl --help                      # the discovery surface
axl --print-prefix              # §6 lists this as MISSING
axl --print-version             # §7 wants this for plain-Make consumers
```

**Why this is the answer to the actual problem.** The install *location* stops
mattering to every doc line, README and printed hint. Ten of the twelve
host-tools scripts are on no `PATH` at all today — only `run-qemu` and
`axl-emulate` have `/usr/bin` wrappers — which is why a crash report tells its
reader to run `rsod-decode.py`, a command that does not exist for a package
user.

### 13.1 What `axl` does NOT front, and why that is stated out loud

**"Tool" is overloaded four ways in this project**, and a flat command list
invites a reader to assume one program fronts all four:

| | what it is | where |
|---|---|---|
| **host commands** | the helper scripts — run-qemu, rsod-decode, emulate | `libexec/axl/`, via `axl <cmd>` |
| **the host's own toolchain** | the distro's gcc / binutils | **deliberately not used for target code** — AXL-Libc-Substrate-Design.md §4.1d: "no compiler, no assembler, no linker from the distro" |
| **AXL's bare-metal cross toolchain** | what actually compiles a `.efi` — `aarch64-none-elf-*`, `x86_64-elf-*` | `/opt/…`, per `axl-toolchains.conf`; driven by `axl-cc` / `axl-c++`, installed by `axl-install-toolchain` |
| **target tools** | the `.efi` utilities (lspci, dmidecode, …) | `axl-sdk-tools-<arch>.tar.gz`, and `axl.efi` — the UEFI busybox this dispatcher is named after |

So **`axl-cc`, `axl-c++` and `axl-install-toolchain` stay their own `bin/`
commands and are deliberately NOT subcommands.** They belong to the third row,
not the first: putting `axl install-toolchain` beside `axl run-qemu` would
imply the dispatcher fronts the compiler as well, which is the exact
conflation this table exists to prevent. `axl --help` says so in three lines
rather than leaving it to be inferred.

*(An earlier draft of this section listed `axl install-toolchain aa64` as an
example. It was never implemented, and on this reasoning it should not be —
the same "documents a thing that does not exist" defect as §5.1's tarball
promise, caught 2026-08-29.)*

**This answers O4 with *no*:** one binary on `PATH` beats a `bin/axl-env.sh`
the user must remember to source.

`axl` resolves `libexec/axl/<cmd>` from its own `$0` with `cd -P`, exactly as
`axl-cc` already resolves `SDK_DIR` — the relocatability §3 records and §12.1
re-measured. No env var, no compiled-in prefix.

When this lands, `README.md`'s host-tools block and
[RELEASING.md](RELEASING.md)'s artifact list both need the `axl <cmd>` form,
and the crash handler's printed hint (`drivers/crashhandler/report.c`) can
drop its full path.

**`axl-cc` and `axl-c++` keep their own names in `bin/`.** They are the two
commands consumers have wired into build systems; breaking them buys nothing.
Per §3 of the Build-System doc, `axl-cc` is the consumer driver and is
explicitly outside the CMake port — the dispatcher is its sibling and sits
outside the port for the same reason.


## 14. Release assets — naming, contents, and the lists that drift

Measured against **v4.4.0**, by downloading all six assets and listing them.
Everything below is what is in the files, not what the names suggest.

| asset | size | payload | runs on |
|---|---|---|---|
| `axl-sdk.deb` / `axl-sdk.rpm` | 13.7 / 13.8 MB | 315 files into `/usr` — 182 headers, 27 libs, 9 `libexec/axl` scripts, 5 `bin`, 75 doc | host |
| `axl-sdk-4.4.0-linux-x86_64.tar.gz` | 13.5 MB | the same tree under a versioned root `axl-sdk-4.4.0/` | host |
| `axl-sdk-host-tools.tar.gz` | 0.36 MB | 13 files under `scripts/` | host |
| `axl-sdk-tools-{x64,aa64}.tar.gz` | 2.6 MB each | **38 `.efi`** + 6 third-party `drivers/` + `pci-ids.json5` + licenses | **target** |
| `SHA256SUMS` | — | pins the six above; does not list itself | — |

### 14.1 The naming rule: name by where the payload RUNS

Four assets begin `axl-sdk-`, and **two of them contain no SDK**.
`axl-sdk-tools-<arch>` is 38 UEFI binaries that execute on the machine under
test; `axl-sdk-host-tools` is eight host scripts. The prefix that is supposed
to identify the product is instead the one thing every asset shares, so it
carries no information, while the distinction that actually matters — host
versus target — is nowhere in the name.

**Decision: name by execution target, and version every asset.**

(The concrete list is below, after the versioning question is settled.)

`axl-uefi-tools` is the load-bearing rename: it is the only asset whose
contents do not run on the machine that downloaded them, and the old name said
the opposite.

**Versions in filenames: RETRACTED.** An earlier draft of this section wanted
one in every asset name, on the grounds that a downloaded `axl-sdk.deb` is
anonymous. Reading `release.yml` says why it is anonymous, and the reason is
load-bearing:

> "Drop the version out of filenames so the GitHub
> `/releases/latest/download/<file>` redirect produces a stable URL across
> releases."

That redirect requires an exact, unchanging asset name, and **nine documented
install commands depend on it** — six in `README.md`, two in
`getting-started.rst`, one more for `SHA256SUMS`. Versioning the names would
break every one of them with nothing to replace them, because no `latest` URL
can exist for a name that changes each release. The version is also not
actually lost: `dpkg -l` / `rpm -qi` carry it, and both tarballs ship a
`VERSION` file. Meanwhile the consumer that pins a specific release
(`install-axl-sdk.sh`) uses the **versioned tag** URL
(`releases/download/v<ver>/<asset>`), so it never needed the filename to say
so.

**The real defect is the one asset that already breaks the convention.**
`axl-sdk-<ver>-linux-x86_64.tar.gz` carries its version, therefore has no
stable URL, therefore **cannot be documented as a `latest` download — and is
not mentioned in `README.md` at all.** v4.4.0's headline artifact, the one that
cut a pinned version from 7.1 GB of source checkouts to 42 MB, is absent from
the install instructions because its own name locked it out.

Fix it the same way the layout already works: name it
`axl-sdk-linux-x86_64.tar.gz` and let the version live where it already lives
— in the `axl-sdk-<ver>/` directory the archive extracts to, which §12.2's
versioned-root layout and `axl prune` both key on. The filename gets a stable
URL; the payload keeps the version.

**How other projects resolve the same tension.** A first survey here sampled
ripgrep, bat, gh and Neovim and concluded that stable names are a legitimate
choice. That was true but badly weighted: two of the four are tarball-first, so
the sample under-represented the case that is actually ours. Re-run against
**GitHub projects that ship `.deb`/`.rpm` from their releases** (live asset
lists, 2026-09-01):

| project | packages | versioned | example |
|---|---|---|---|
| goreleaser | 14 | 14 | `goreleaser-2.18.0-1.aarch64.rpm` |
| cosign | 12 | 12 | `cosign-3.1.3-1.aarch64.rpm` |
| syft | 10 | 10 | `syft_1.51.1_linux_amd64.deb` |
| caddy | 8 | 8 | `caddy_2.11.4_linux_amd64.deb` |
| dive | 6 | 6 | `dive_0.13.1_linux_amd64.deb` |
| pandoc | 2 | 2 | `pandoc-3.11-1-amd64.deb` |
| **k9s** | 10 | **0** | `k9s_linux_amd64.deb` |

**Six of seven version the filename.** The convention is near-universal among
our actual peers, not merely a repo-tooling artifact — and the earlier claim
that the tag namespace makes it unnecessary explains why it is *possible* to
skip, not why almost nobody does.

What the versioned six give up, and how they buy it back, varies: **caddy**
runs an apt/dnf repo; **syft** publishes a stable `install.sh` (verified live)
so the one-liner survives as `curl … | sh`; the rest expect the reader to
substitute a version. **k9s** is the one that keeps a raw stable URL, and it
does work — `releases/latest/download/k9s_linux_amd64.deb` returns 200 — so the
status quo is defensible, just unusual.

**The trade, stated plainly.** Versioned names cost us nine documented
one-liners; they buy a file that says what it is in `~/Downloads`, two versions
that can coexist, and the shape every `.deb` user expects. Two mechanisms make
the cost small, both borrowable and neither large:

```sh
# kubectl's: a stable one-line asset naming the current version.
V=$(curl -fsSL .../releases/latest/download/VERSION)
curl -LO ".../releases/download/v$V/axl-sdk_${V}-1_amd64.deb"

# syft's: a stable installer that resolves and fetches. One line again.
curl -sSfL .../releases/latest/download/install.sh | sh
```

**Recommendation: version the `.deb`/`.rpm`, keep stable names for the
tarballs, and publish a `VERSION` asset.** The packages are where the
convention is strongest and where a self-identifying filename actually helps a
human. The tarballs are fetched by scripts that already pin a versioned tag URL
(`install-axl-sdk.sh`, `test-sed-corpus.sh`), so their names buy nothing by
carrying a version — and stable names keep the `latest` convenience for the
one audience that types these by hand. That also leaves
`axl-sdk-linux-x86_64.tar.gz` documentable, which is the defect that started
this.

### 14.0a `AXL_INSECURE_FETCH` — when the hash is the anchor, and when it is not

§14 and §17.4 both say the pinned SHA256 is what makes fetching over a hostile
network defensible. `AXL_INSECURE_FETCH=1` acts on that: it drops TLS
verification from the SDK's own downloads, for a host behind a corporate MITM
proxy whose CA is not installed. Opt-in, default off.

It exists because a real coworker on a fresh WSL image could install the SDK
(the consumer fetches assets with `curl -k` against hashes pinned in its own
repo) and then had `axl-install-toolchain` die at `curl: (60) SSL certificate
problem: self-signed certificate in certificate chain`. Dell intercepts HTTPS
org-wide, so this reaches every consumer behind the proxy, not one machine.

**The two sites it affects are NOT equally safe, and conflating them would be
the mistake:**

| site | expected hash comes from | with `-k` |
|---|---|---|
| `scripts/install-toolchain.sh` | `scripts/axl-toolchains.conf`, **shipped in the SDK** | **costs nothing.** Pre-shared and out-of-band; no interceptor can forge a tarball matching it |
| `packaging/install.sh` | `SHA256SUMS`, fetched from the **same base URL** as the assets | **weaker.** Whoever can substitute an asset can substitute the sums vouching for it |

So for `install.sh` the guarantee drops from *authenticated* to
*corruption-resistant* — still worth having, and genuinely sound for a caller
that pinned hashes out of band, which the flagship consumer does in
`.axl-sdk-checksums`. It is **not** "the hash is the trust anchor", and
`install.sh` prints exactly that whenever the flag is on rather than letting
the weaker guarantee be read as the stronger one.

Measured, not assumed: against a store that does not trust the presented
certificate, plain `curl` returns `http=000` and `curl -k` returns `302`, and
the proxy relays the file bytes unchanged so the pinned hash still matches —
and still fails if the bytes change.

A failed toolchain fetch now names both ways out (install the CA, or set the
flag), including before the x64 path falls back to a ~30-minute source build.

**It is dormant for anyone who already has the toolchain**, which is most
existing users. `install-toolchain.sh` short-circuits when the binary exists
*and* reports the pinned version — measured with a stub `curl` on `PATH`:
**zero** fetches for either arch on a machine that has them. So the TLS failure
only reaches a host that does not yet have the toolchain, which is exactly the
fresh-WSL case that prompted this.

**Dormant is not gone.** The gate is a VERSION match, so the next toolchain
bump in `axl-toolchains.conf` correctly makes every user download again — and
every user behind the proxy would hit the TLS failure at once, on the same day.
That is the argument for landing the flag before the next bump rather than
after it. `test-insecure-fetch.sh` pins the no-fetch path, because if it
regressed the cost is silent: every run re-downloading 239 MB or 500 MB.

### 14.1a The settled scheme

`install.sh` (§17) changes the constraint that drove every earlier draft of
this section. Once the installer is the front door, **only two assets need a
stable name** — `install.sh` itself and the `VERSION` it resolves against.
Everything else is fetched by a script that already knows the version and can
use the versioned tag URL. The nine hand-typed `latest/download` commands that
forced stable names are the thing being replaced.

With that constraint gone, versioned filenames win uncontested: a download that
says what it is, two versions that coexist, and the convention six of seven
peers follow.

```
install.sh                               stable   the front door
VERSION                                  stable   one line
SHA256SUMS                               stable
axl-sdk-linux-<ver>-x86_64.tar.gz        host binaries + headers + libs
axl-sdk-host-tools-<ver>.tar.gz          scripts only
axl-sdk-uefi-tools-<ver>-<arch>.tar.gz   target firmware binaries
```

Three things about that shape are deliberate:

**One format string builds all of them.**
`axl-sdk-${component}-${ver}${arch:+-$arch}.tar.gz` — no special case for the
base tarball. `install.sh` constructs these names, `release.yml` emits them and
`SHA256SUMS` lists them, so a single expression in three places beats a
special case in each. The cost, recorded rather than hidden: it splits
`linux-x86_64`, which is a recognisable platform token elsewhere
(`gh_2.98.0_linux_amd64`). Uniformity wins because `uefi-tools` has no OS
component, so there is no platform token consistent across the family to
preserve.

**`<arch>` means the HOST in one name and the TARGET FIRMWARE in the others.**
`x86_64` on the SDK tarball is the machine that runs `axl-cc`; `x64`/`aa64` on
the uefi-tools tarballs are the firmware the `.efi` files boot on. They now sit
in the same field position, so this is written down: `release.yml` already
carries a comment defending the boundary, and it would be easy to "fix"
wrongly later.

**host-tools carries no `<arch>` because it has none.** Its payload is shell
and Python — verified, zero compiled artifacts. An arch field there would
either be a lie or force two byte-identical uploads.

The SDK tarball keeps `x86_64` for the opposite reason: `bin/pe-set-debug` is
an `ELF 64-bit x86-64` binary, so the archive genuinely is host-specific, and
dropping the label would leave no name for an aarch64-Linux build.

### 14.1b The toolchain is not one of these, and should not be

The cross toolchain ships as its own release — tag
`toolchain-x86_64-elf-<ver>`, one 55.5 MB `x86_64-elf-gcc-<ver>.tar.xz` — and
the AArch64 one is not ours at all: `install-toolchain.sh` fetches it from
`developer.arm.com`. Three reasons that stays true:

- **Cadence.** 18 SDK releases against 4 toolchain releases. Bundling
  republishes 55 MB on every patch release for a payload that changed 4 times.
- **Size.** ~739 MB installed per generation against 42 MB for the SDK.
- **Licensing.** Already recorded: bundling ARM's tarball "would mean owning
  the toolchain's release cycle and license redistribution."

Its asset name stays as it is. `x86_64-elf-gcc-<ver>.tar.xz` is self-describing,
it is a GCC build rather than an AXL artifact, and it sits on its own tag so it
never competes for sort order with the SDK assets.

**One external single point of failure is worth naming**: the AArch64
toolchain URL is ARM's. The pinned SHA256 protects integrity, not
availability — if ARM reorganises that path, `axl-install-toolchain aa64`
breaks and nothing on our side fixes it short of re-hosting.

### 14.1c Two of three tarballs are tarbombs

Measured on the v4.4.0 assets — top-level entries after extraction:

| tarball | top-level entries | |
|---|---|---|
| `axl-sdk-4.4.0-linux-x86_64.tar.gz` | **1** (`axl-sdk-4.4.0/`) | correct |
| `axl-sdk-host-tools.tar.gz` | **6** | scatters |
| `axl-sdk-tools-x64.tar.gz` | **43** | scatters |

The SDK tarball, being newest, got it right. The other two extract their
contents straight into the working directory, which is why the consumer's
README has to say `mkdir -p ~/axl-sdk-host-tools && tar xf … -C` — the caller
creates the directory because the archive will not.

Each should extract to a single versioned directory matching its own name:
`axl-sdk-host-tools-<ver>/`, `axl-sdk-uefi-tools-<ver>-<arch>/`. That makes an
extracted tree self-identifying and lets two versions coexist, the same way
`axl-sdk-<ver>/` already does — which is what §12.2's versioned roots and
`axl prune` are built on.

Tarballs create no symlinks. `<root>/axl-sdk -> axl-sdk-<ver>` is written by
`install.sh`, which is the only thing that knows which version should be
current.

### 14.2 `axl-host-tools` — keep it, but fix what is in it

Two defects, both verified against the tarball:

- **Four files no host tool uses.** `pe-set-debug.c` (C *source*),
  `elf_x86_64_efi.lds`, `elf_aarch64_efi.lds`, `uefi-manifest.json5`. Grepping
  the bundle's own scripts for each returns **zero references**. They are SDK
  build internals — linker scripts for `axl-cc`, input to the header generator
  — shipped in a bundle that contains no compiler.
- **No `bin/axl`.** Eight of its scripts are the same files the SDK tarball
  installs to `libexec/axl/`; the SDK tarball additionally carries
  `axl-prune.sh` and the dispatcher. So the one channel whose users *cannot*
  reach `axl` is the channel whose users are therefore forced to hardcode
  `~/axl-sdk-host-tools/scripts/...` — which is exactly what §13 exists to
  stop, and exactly what the flagship consumer still does in 28 places.

**Keep the asset.** 0.36 MB against 13.5 MB is a 37x difference, and a CI job
that only needs `run-qemu` should not pull 182 headers to get it. But ship
`bin/axl` + `axl-prune.sh` and drop the four build inputs, so the bundle is a
coherent "host side, no compiler" set rather than a subset plus debris.

### 14.3 The tool list has four owners, and two of them have drifted

The set of shipped UEFI tools is stated in four places. Two are derived or
gated and are correct for free; two are hand-maintained and are wrong:

| owner | documents | status |
|---|---|---|
| `release.yml` sanity list | derived from `make -s print-TOOL_NAMES` | correct |
| `devkit.conf` | gated by `make check-devkit-conf` | correct |
| **`README.md` tool table** | **21 of 38** | **17 missing** |
| **`axl-uefi-tools` `README.txt`** | **13 of 38** | **25 missing** |

Missing from `README.md`, of which fourteen are plain `TOOL_NAMES` tools:
`ata`, `axbench`, `fwtool`, `i2c`, `kbtune`, `lsacpi`, `mkfixture`, `netload`,
`nvme`, `rndisfix`, `scsi`, `smart`, `tar`, `timetest`. (The other three —
`crashtest`, `fbcon`, `kbtune-drv` — are a fault fixture, a console app and a
driver, and their omission is defensible.) `lsacpi` shipped in 4.3.3 and is
absent from both prose lists.

**`README.md` is the releases-site README.** The `aximcode/axl-sdk-releases`
README is byte-identical to this repo's, so it is published from here and one
fix covers both — and one omission is visible in both.

**Decision: derive both.** `devkit.conf` already carries a `desc:` line per
tool and `check-devkit-conf` already requires one, so the description text has
an owner with a gate on it. Generate the `README.md` table and the tarball
`README.txt` from `TOOL_NAMES` + those descriptions. This is the same fix
v4.4.0 applied to `release.yml`'s list, applied to the two lists that were
missed; the pattern is established, not new.

**DONE 2026-09-01, and the two halves went opposite ways — on purpose.**
Deriving assumed both lists want the same content, and they do not:

- **`README.md`'s table is NOT derived.** Its rows are hand-written prose up
  to ~320 characters describing real flags and behaviour; `devkit.conf`'s
  descriptions are ~40-character labels for an on-screen menu. Generating from
  those would have replaced documentation with labels. What is checkable there
  is COMPLETENESS, so `make check-tool-docs` checks that instead — a shipped
  tool with no row, a row for a tool that no longer ships, and a duplicate row.
  Same goal, better mechanism.
- **The tarball `README.txt` IS derived**, by
  `scripts/make-uefi-tools-readme.py`, because its entries *are* flat labels —
  precisely what `devkit.conf` holds. It had drifted furthest (13 of 38) and,
  being an inline heredoc in `release.yml`, could not be looked at without
  cutting a release. `check-tool-docs` also asserts the workflow still
  delegates to the generator, so the heredoc cannot grow back.

One thing that move had to be careful about: those 92 lines carry mbedTLS's
Apache-2.0 election, EDK2's BSD-2-Clause-Patent notice, iPXE's
GPL-2.0-or-later notice and a **GPL-2.0 §3(b) written offer**. They are
obligations, so `test-uefi-tools-readme.sh` asserts each survives
regeneration; a refactor that silently drops one is a licence violation, not
a typo.

### 14.4 Sequencing — renames break consumers that pin filenames

Consumers pin asset names literally. axl-utils' `.axl-sdk-checksums` names
`axl-sdk-host-tools.tar.gz` and `axl-sdk-tools-x64.tar.gz`, and its
`test/test-sed-corpus.sh` builds a download URL from that string. A rename
breaks them at their next pin bump.

So the renames, the `axl-host-tools` content fix, and the consumer's migration
onto `axl <cmd>` want to be **one coordinated release**, not three. Shipping
the rename separately makes a consumer bump twice and take the breakage alone;
shipping it with the migration means they change the pin once and land on both.


## 15. What the public snapshot publishes — and the gate it needs

`aximcode/axl-sdk-releases` is public; this repo is private. Each release
pushes a squashed **source snapshot** there (`Release vX.Y.Z — source
snapshot`), so the released source is public by design while development
history is not. That model is deliberate and is not in question here.

**What is in question is the selection rule.** `release.yml`'s publish step is:

```sh
git archive --format=tar HEAD | tar -xC "$SRC"
rm -rf "$SRC/.github"
```

So the rule is **everything tracked, minus `.github/`**. There is exactly one
exclusion, and it exists for a mechanical reason (a fine-grained PAT without
`workflow` scope cannot push workflow files), not an editorial one.

That rule is why `test/fixtures/` is correctly absent — it is gitignored, so
`git archive` never sees it, which is the same mechanism that keeps a captured
MSDM's Windows product key out of a public repo. It is also why **every tracked
document is public**, including ones written as internal working notes.

### 15.1 Two categories that should not be publishing

**Personal infrastructure.** The house rule is that home-server hostnames, LAN
IPs and personal machine names never appear in committed code or docs — every
repo, every subtree. Two tracked files break it and are public today:

- `docs/HW-Testing-Workflow.md` — names the dev box repeatedly and diagrams the
  reverse-tunnel topology between it, the tunnel host and the laptop, including
  the port and the `~/.ssh/config` block.
- `docs/AXL-Session-Handoff-2026-08-28.md` — one line identifying the dev box
  and its virtualization.

Two other pattern matches were checked and are **not** violations: an
RFC1918 literal inside an `axl_ipv4_parse_cidr()` test case, and the same
address range in a demo SVG. Example addresses are fine; naming the box is not.

> This section deliberately describes those files without reproducing the
> strings. A doc that explains the rule by quoting the thing the rule forbids
> publishes it again on the next snapshot.

**Internal working notes.** 15 `AXL-Session-Handoff-*.md` and 37 files under
`docs/superpowers/{plans,specs}` are tracked, therefore public. They are
working documents: dead ends, design arguments settled and reversed, and
consumer specifics. Counting published docs, `delldiags` appears in 14, `Dell`
in 16, `iDRAC` in 10, and a named customer server model in 2.

Nobody decided to publish those; they are public because the selection rule has
no opinion. **The decision to make is which doc CLASSES are public**, roughly:

| class | example | publish? |
|---|---|---|
| API / guide | `AXL-Coding-Style.md`, `AXL-Driver-Authoring-Guide.md` | yes — consumers need them |
| design records | this file, `AXL-Design.md` | probably — but see below |
| session handoffs | `AXL-Session-Handoff-*.md` | no — working notes |
| plans / specs | `docs/superpowers/**` | no — working notes |
| infra runbooks | `HW-Testing-Workflow.md` | no — names personal infrastructure |

If design records stay public, they inherit the consumer-naming question: the
rule barring downstream consumer names covers code and comments, and §14.4 of
this very document names one. That is defensible when the document is
internal and needs revisiting if it is not — which is the point of deciding by
class rather than file by file.

### 15.2 The fix: an exclusion list is not enough on its own

Adding paths beside `rm -rf "$SRC/.github"` is one line each and is the right
mechanism — the machinery already exists.

**But an exclusion list is exactly the shape that has drifted three times in
this tree** (§14.3: the devkit conf, the release sanity list, two tool
READMEs). A list with no check is a list that is correct until someone adds a
file, and here the failure lands in a public repo and cannot be recalled.

**So the exclusion list gets a gate, and the gate runs on the ASSEMBLED
snapshot, not on the source tree.** After `$SRC` is built and pruned, grep it
for the forbidden patterns — the dev box and tunnel hostnames, RFC1918 and
CGNAT literals outside test and asset files, the admin account name — and fail
the release on a hit. Checking `$SRC` rather than the repo is what makes it a
real gate: it asserts the property that actually matters ("what we are about to
push is clean"), not a proxy for it.

Two properties it must have, both learned the hard way here:

- **It must be shown to fail.** A gate that has never caught anything is a gate
  nobody has proven can see. Add a control that plants a forbidden string in a
  scratch `$SRC` and asserts the check fires.
- **It must distinguish "found nothing" from "could not run."** A grep over a
  path that does not exist and a grep that matched nothing both print nothing.
  Capture the exit status and assert the snapshot was non-empty first.

### 15.3 Repo-metadata discrepancies

Cheap, and the first one is worth more than it sounds:

- ~~**Neither repo shows a license.**~~ **WRONG — checked 2026-09-02.**
  `gh api repos/<r>/license` returns **Apache-2.0 for both**. The claim came
  from GraphQL's `licenseInfo` being null, which is a different field and not
  what the sidebar reads. The two pieces of evidence offered for it do not
  hold either: the canonical text *also* opens with a blank line, and the
  19-byte difference is entirely the appendix's
  `[yyyy] [name of copyright owner]` being filled in — which is what that
  placeholder is for. `diff` against
  `https://www.apache.org/licenses/LICENSE-2.0.txt` shows one changed line, and
  the body through `END OF TERMS AND CONDITIONS` is byte-identical.

  The underlying fix had already been made (commit `c448187`, 2026-04-24, after
  two clauses had genuinely drifted). Nothing to do. **Use the REST `/license`
  endpoint to check this, not GraphQL `licenseInfo`.**
- ~~**Wiki is enabled on the public repo**~~ — **DONE 2026-09-02**,
  `has_wiki=false` on `axl-sdk-releases`. It was an empty tab on the public
  face.
- ~~**This repo has no homepage URL**~~ — **DONE 2026-09-02**, both repos now
  point at `https://axl.aximcode.com`. Description and all ten topics already
  matched.

**`AXL_SNAPSHOT_FORBIDDEN` is set** (2026-09-02, 7 patterns). Its value was
derived from the files D5 excludes — ssh-config identifiers from the infra
runbook, the tailnet address from the excluded handoff, and the lab IPs and
service tag redacted from the archived roadmap, which belong in it precisely
because they are still in the public repo's *history* and must never be
re-added.

Two things that exercise taught, worth keeping:

- **A candidate that appears in the surviving snapshot is not a secret.** The
  first extraction produced 9 strings; 4 were common words, one appearing in
  724 files (`sysinfo`-class noise from grepping prose for `ssh <word>`).
  Filtering to "appears ONLY in excluded files" is self-validating and left 5.
- **The gate's own test had planted the real lab IP as its control string**,
  putting it straight back into a tracked file and undoing the redaction it was
  testing. Controls must use synthetic values. (Which the gate then proved
  again from the other side: an earlier draft of THIS bullet quoted the
  replacement address literally, and the check failed the suite for a private
  address in prose. A doc explaining the rule by quoting what the rule forbids
  is the thing §15.1 already warned about.)


## 16. Nothing tests the artifacts the way a consumer receives them

Every check we have runs **before publication, on locally built files**:

| check | what it proves | what it cannot |
|---|---|---|
| `build-packages.sh` | `rpm2cpio \| cpio` the fresh `.rpm`, run `axl-cc --version`, compile `hello.c` to PE32+ | the package manager never runs |
| `release.yml` per-artifact smoke tests | each tarball extracts and its commands run | the built file, not the published one |
| a person, at v4.4.0 | the published SDK tarball downloaded, hash-checked, and compiled a `.efi` | not repeatable, not automatic |

Nothing in the tree fetches `releases/download/...`. The only such URL is a
link in the release notes.

### 16.1 What that leaves unchecked

- **The package manager never runs.** `rpm2cpio` bypasses `dnf`/`dpkg`
  entirely, so nothing exercises file conflicts, the `Replaces`/`Obsoletes`
  that retire the old host-tools package, scriptlets, or the weak QEMU
  dependency — all of which §12.6 added deliberately, and none of which cpio
  can see. "The files are in the archive" is a weaker claim than "the package
  installs."
- **The published bytes are never verified.** A truncated upload, a missing
  asset or a `SHA256SUMS` that does not match would be found by a consumer
  first. That exposure rises the moment §14.1's renames land, because the
  failure mode becomes "one asset kept its old name" — which every local check
  passes and no local check can see.
- **Nothing starts from a clean machine.** The builder already has the cross
  toolchain under `/opt` and the mbedTLS submodule checked out. A consumer
  begins with neither and has to get through `axl-install-toolchain` first.
- **aa64 consumption is never exercised end to end**, only aa64 *production*.

### 16.2 Two jobs, because one cannot do both

**A gate cannot run after publication, and an upload cannot be verified before
it.** So this is two things, and conflating them gets one of them wrong.

**1. Pre-publish, containerized — the gate.** In clean `debian:stable` and a
Fedora/RHEL-family image, install the built `.deb`/`.rpm` with the real package
manager, then assert the consumer's first hour works: `axl --version`,
`axl-cc hello.c -o hello.efi` produces a PE32+, every `axl <cmd> --version`
answers, and both tarballs extract and do the same. Catches everything above
except upload integrity, and can block the release.

**2. Post-publish, small.** Fetch `SHA256SUMS` and every asset from the tag
URL; verify each hash, and that the asset set is exactly what was meant to
ship. It cannot gate — the release is already out — but it turns "a consumer
finds it" into "we find it in minutes", and it is the only check that can see
a rename that missed one asset.

### 16.2a DONE 2026-09-02 — and job 1 had to be re-aimed

**Job 1 is `test/integration/test-consumer-install.sh`.** §16.2 wrote it as
"install the built `.deb`/`.rpm` with the real package manager". D2 retired the
packages, so the gate now `curl`s `install.sh` out of a release directory and
runs it, which is what README's three commands actually say.

**The two distributions survived the re-aim for a different reason.** Debian
and Fedora were chosen because one takes `.deb` and the other `.rpm`;
`install.sh` is distribution-agnostic, so that reason is gone. The replacement
is better: **Debian's `/bin/sh` is dash and Fedora's is bash**, and
`install.sh` is `#!/bin/sh`. A bashism there is invisible on one image and
fatal on the other, and it is the likeliest way that script breaks.

**It found a real defect on its first run, of exactly the class §16.1
predicted** ("nothing starts from a clean machine"): neither
`debian:stable-slim` nor `fedora:latest` ships `python3`, and **four shipped
commands are Python** — `axl rsod-decode`, `axl gdb-syms`,
`axl extract-fv-shell`, `axl emulate`. `install.sh` never mentioned it, so a
user following `axl-cc`'s own crash diagnostics to `rsod-decode` got
`python3: command not found` with nothing pointing at the cause. Fixed by
`advise_python()`, which detects and advises exactly as `advise_qemu()` does —
it must not *refuse*, because `axl-cc` needs no interpreter. The test asserts
both halves: the advice appears, and every command works once it is followed.

A second, smaller find: a mirror that REGENERATES its checksums writes `./name`
entries, and `install.sh` refused them with "this release publishes none of:
…" about a directory that plainly held the asset. `sums_line()` now strips the
prefix.

**Job 2 is `scripts/check-published-release.sh`**, run from `release.yml`
after publication. It carries **no list of expected asset names** — a list here
would be a fifth copy of §14.1a's and would go stale exactly when it mattered.
It cross-checks the release against itself: every name `SHA256SUMS` lists is
attached, every attached asset is checksummed, every one downloads and matches,
`VERSION` agrees with the tag, and the two stable `latest/download/` URLs
resolve. A rename that missed one place is a set difference in whichever
direction it went. Being version-agnostic, it runs against a release cut
*before* the rename — verified live against v4.4.0, 6 assets, all hashes good.

`test-published-release-check.sh` injects each failure it exists for over
`file://` and requires it to be named: a corrupted asset, one listed but not
downloadable, one attached but unsummed, one summed but unattached, a wrong
`VERSION`, and no `SHA256SUMS` at all.

### 16.3 What it would not have caught, stated honestly

The host-tools tarball regression that shipped four Python tools with a
missing import would have been caught by job 1 — but it is *already* caught,
earlier and cheaper, by running each command in the tarball smoke test.
Container testing earns its keep on the **install path**: package-manager
behaviour, a clean machine, and the toolchain bootstrap. It is not a
replacement for testing artifact contents where they are built, and adding it
is not a reason to weaken those.


## 17. `install.sh` replaces the packages

**Decision: a stable `install.sh` becomes the install path, and the `.deb` and
`.rpm` retire once it is proven.** Not coexistence — the point is to stop
maintaining two package layouts, `fpm`, `rpmbuild`, `Replaces`/`Obsoletes`
metadata and a release job, for benefits a script can provide.

This is what Claude Code, uv and rustup do, and both of the first two install
to `~/.local/bin` (read from their live installers). The relevant thing is not
fashion: it is that **we already built the machinery** and the packages are now
the part that does not fit.

| piece | status |
|---|---|
| relocatable, self-contained prefix | §12.3, asserted by `test-sdk-selfcontained.sh` |
| versioned roots side by side | §12.2, and the tarball extracts to one |
| `current` symlink | created by `install.sh` since this cycle |
| `axl prune` — keep current + N | §12.4 |
| `axl --print-prefix` | a machine interface for "where am I installed" |
| removal | `rm -rf <prefix>`, already the contract |

### 17.1 What the packages actually did for us

Three things, and only one is hard to replace:

- **Weak dependencies.** §12.6 declares the QEMU/OVMF stack as
  `Recommends`/weak so `apt`/`dnf` install it by default and let a consumer
  decline. **A script cannot declare a dependency**; it can only detect and
  advise. This is the one genuine capability loss, and the honest replacement
  is: probe for `qemu-system-x86_64` and OVMF at install time and print the
  distro-appropriate command if absent. `run-qemu.sh` already has the
  three-tier discovery and the install hints — they exist as strings in
  `release.yml` today.
- **`Replaces`/`Obsoletes`.** These retired the standalone host-tools package.
  That migration is spent; nothing else needs it.
- **A version in a package database.** `dpkg -l` / `rpm -qi` answered "which
  version is this". So does `axl --version`, and so does the prefix's own name.

### 17.2 What it must do

**Download it, then run it — the pipe is the fallback, not the headline.**

```sh
curl -fsSLO https://.../releases/latest/download/install.sh
sh install.sh --help
sh install.sh --toolchain x64          # SDK + host tools + the x64 cross toolchain
sh install.sh --host-tools             # run-qemu and friends, no compiler
```

Two reasons, and the first is the stronger:

- **The install is options-driven, and the piped form makes options ugly.**
  `curl … | sh -s -- --host-tools --toolchain aa64` against
  `sh install.sh --host-tools --toolchain aa64`. Once a script takes flags,
  piping it stops being the simple option.
- **It can be read before it is run**, which matters more here than usual:
  installing a toolchain under `/opt` asks for `sudo`, and a good share of this
  audience works somewhere that forbids piping a URL into a shell outright.

The piped form keeps working for the zero-option default, because it is the
same script. It is documented second.


1. Resolve the version — from the stable `VERSION` asset (§14.1), or `--version X.Y.Z`.
2. Download the SDK tarball and verify it against `SHA256SUMS`. **Verifying is
   not optional**: a piped installer that skips the hash is strictly worse than
   the package manager it replaces, which checks signatures.
3. Extract to `<prefix>/axl-sdk-<ver>/`, defaulting to
   `${XDG_DATA_HOME:-$HOME/.local/share}` and honouring `--prefix`.
4. Point `<root>/axl-sdk` at it (§12.2) and link the entry points into
   `${XDG_BIN_HOME:-$HOME/.local/bin}`.
5. Probe for the QEMU stack; print the install command for the detected distro
   if it is missing. Never install it silently.
6. Say whether that bin directory is on `PATH`, and how to add it if not.
7. Offer the cross toolchain (`axl-install-toolchain`) rather than assuming it.

Upgrade is the same command: a new versioned root beside the old, the symlink
moves, `axl prune` bounds what accumulates. Removal is `rm -rf` plus the links,
which `--uninstall` should do so nobody has to know which links exist.

**Symlinking into `~/.local/bin` did not work until this cycle.** All three
entry points resolved their prefix with `cd -P "$(dirname "$0")"`, which never
follows a symlink on `$0`, so a link on `PATH` made the prefix resolve to
`~/.local` and `axl-cc --version` answer `unknown`. Fixed by resolving `$0`
through `readlink -f` first. An installer of this shape was impossible before
that, which is why this section could not have been written earlier.

### 17.3 Retiring the packages — the sequence that does not break a consumer

The flagship consumer's `install-axl-sdk.sh` installs the `.deb`/`.rpm` today,
and it has already absorbed one migration this week (onto the `axl`
dispatcher). §14.4's lesson applies: do not make it take two breaks in a row
without the second being ready.

1. Ship `install.sh`; keep publishing the packages. Document the script as the
   install path. Nothing breaks.
2. Move the consumer to `install.sh` — for it this is a simplification, because
   it currently detects deb-vs-rpm families and shells out to `apt`/`dnf` with
   `sudo`, all of which the script removes.
3. Drop the packages once nothing installs them. That deletes `build-packages.sh`,
   the `fpm` and `rpmbuild` dependencies, the `build-packages` release job and
   both package layouts.

**Step 3 needs one thing step 1 should already do:** detect an
existing package install (`dpkg -s axl-sdk` / `rpm -q axl-sdk`) and say so,
because a user who installed the `.deb` and then runs `install.sh` would
otherwise have two copies with `/usr/bin/axl` winning on `PATH`. Telling them
to remove the package is a two-line check, and it is the difference between a
migration and a confusing afternoon.

### 17.4 What this does not solve

- **System-wide, multi-user installs.** `install.sh --prefix /opt/...` works,
  but nothing makes the tools appear on every user's `PATH`. No one has asked.
- **Environments that mandate signed packages.** If a consumer's policy
  requires an `.rpm`, the answer is to keep building one for them specifically,
  not to keep it for everyone.
- **`curl | sh` as a trust model.** It is what the ecosystem does, and the
  hash check in step 2 is what makes it defensible rather than customary.


## 18. Prior art for the installer — what exists, and what we copy

§17 decided to write an installer. This section is the survey that should have
come first: what already exists, why none of it fits, and which specific
mechanisms to take from the ones that got it right.

### 18.1 There is no off-the-shelf installer that fits

Nine candidates, checked against their live repositories:

| tool | ⭐ | what it is | why not |
|---|---|---|---|
| `eget` | 2.1k | installs a prebuilt **binary** from a GitHub release | we are a 42 MB tree — 182 headers, target libs for two arches, a compiler driver, nine host scripts. There is no "the binary" |
| `ubi` | 591 | same idea, "Universal **Binary** Installer" | same |
| `godownloader` | 447 | goreleaser's `install.sh` **generator** — the closest fit | **archived, last pushed 2021-07-17.** No maintained successor |
| `mise` | 33k | dev-tool version manager | right shape, but the **consumer** must adopt it |
| `asdf` | 25k | the older same model; mise is plugin-compatible | same, and see §18.3 |
| `aqua` | 1.8k | declarative CLI version manager | same |
| `proto` | 1.4k | pluggable multi-language version manager | same |
| SDKMAN! | 6.8k | literally an SDK manager | JVM-centric; a non-JVM SDK in their registry is a stretch |
| webi | 3k | hosted installer service, per-package scripts | not a library we can vendor, but see §18.2 |

So: writing one is legitimate. **Inventing its structure is not** — the
generator that would have written it for us is dead, which means the
ecosystem's answer is "hand-write it, from a known-good template."

### 18.2 The three mechanisms worth taking

**From rustup — the structure, which is load-bearing.** `rustup-init.sh` is
930 lines and 25 functions and ends with `main "$@" || exit 1`. Nothing
executes until that last line. That is the guard against a truncated
`curl | sh`: a dropped connection leaves `sh` a *prefix* of the file, and a
prefix of a file that only defines functions does nothing.

Our first draft failed this exactly. It was linear, with
`rm -rf "$PREFIX_ROOT/$DIR"` at top level and the `tar` that refills it on the
next line — so a cut in between deleted an existing install and did not
replace it. Fixed by restructuring: 16 functions, zero top-level side effects,
`main "$@"` last.

Also from rustup: `need_cmd` (name the missing tool) and `ensure` (never let a
failed command pass for a completed step).

**From webi — the ordering, and the layout.** webi verifies a checksum before
touching the destination (`fn_checksum` against `WEBI_CHECKSUM`), and installs
into versioned directories with a `current` pointer. We arrived at the same
layout independently, which is worth recording as corroboration rather than
coincidence: `~/.local/share/axl-sdk-<ver>` plus `<root>/axl-sdk`.

**From the version managers — the idea that this is a solved category, and the
SHAPE of their verbs.** What was taken is the model, not a dependency: D1a's
`axl update` / `use <version>` / `list` / `uninstall` / `prune` are mise's verb
set, with `use` subsuming `install` for mise's reason. The plugin §18.3
proposed on top of that was built and declined — see the note there. So this
line reads "we implemented their design in `axl`", NOT "we integrate with
them", and the difference is the whole of §18.3's outcome.

### 18.3 Version management: we built the manager, and declined the plugin

The install/upgrade/switch/clean-up cycle is exactly what mise, asdf, aqua and
proto exist for, and rebuilding it would be the reinvention this survey was
meant to prevent. But they cannot be *required*: a consumer who does not use
mise still has to install AXL.

The cheap correct move is an **asdf-compatible plugin** — four small scripts
(`list-all`, `download`, `install`, `latest-stable`) — which mise consumes too.
Consumers who already run a version manager then get `mise use axl-sdk@4.4.0`
and per-project pinning for free, and everyone else uses `install.sh`.

> **BUILT, THEN DECLINED — 2026-09-02. Do not re-propose without new
> information.** The plugin was written, tested (18/18) and reverted the same
> day. Three reasons, and the middle one is the one this section missed:
>
> - **We already have the version manager.** D1a put `update`, `use <version>`,
>   `list`, `uninstall` and `prune` in `axl` — which is what "borrow from asdf"
>   was supposed to mean (§18.2 takes its *plugin contract* as a shape, not as
>   a dependency). The plugin is a SECOND manager over the same tree.
> - **Our own verbs cannot manage what it installs, and this was measured.**
>   In an asdf-managed prefix: `axl list` declines ("not placed by
>   install.sh"); `axl update` would write a versioned root INTO asdf's own
>   installs directory, so two managers own one tree; `axl prune` correctly
>   declines the SDK roots but still offers to delete the shared `/opt`
>   toolchains. Every one of those is a support conversation we would own.
> - **It cannot be used at all without a new repo.** asdf resolves a plugin
>   from a git root, so `packaging/asdf/` needed mirroring to
>   `aximcode/asdf-axl-sdk` — permanent maintenance surface for a consumer
>   nobody has named.
>
> What would change the answer: a real consumer who runs mise and asks for it.
> Then the first bullet still stands, so the work is not "ship the plugin" but
> "make `axl` detect an externally-managed prefix and decline cleanly" first.
>
> One real bug came out of it and was kept: `axl prune` told a non-versioned
> prefix "a distro package owns its files; upgrade with apt/dnf", which has
> been wrong since D2 retired the packages.

**`axl prune` still does not go away, and no version manager would replace
it.** Those tools manage per-user *tool* versions. Our cross toolchain is a
shared `/opt` resource — 739 MB per generation, used by every SDK version at
once. `axl prune` prunes both axes; a version manager models neither the
sharing nor the second axis.

### 18.4 Why `install.sh` does not absorb `axl prune`

`axl prune` is a **command in the SDK**: discoverable from `axl --help`,
runnable at any time, with `--dry-run` and `--keep N`. Folding its policy into
the installer would make it reachable only at install time and only with
whatever the installer hardcoded. So `install.sh --prune` *offers* it —
showing the dry run, and running it only with `--yes` — and the command stays
where a user can find it later.

### 18.5 The property that has to stay true

Structural safety is invisible: nothing about reading a correct script tells
you it is still correct after an edit. `test-installer-truncation.sh` feeds
`sh` **every prefix** of `install.sh` with a populated install present, and
requires the tree to be byte-identical afterwards at every one — 264 points
today.

It carries two controls, because a test that has never failed has not been
shown to see anything. A synthetic linear installer with the same destructive
step at top level *is* destroyed by truncation, proving the harness detects
that shape; and a sabotage that inserts one top-level `rm -rf` into the real
`install.sh` is caught and named.

### 18.6 CPack and the Qt Installer Framework, evaluated

Both are real installer frameworks and both were tried against this problem
rather than dismissed from memory.

**CPack (CMake).** Available here, works, and generates
`STGZ / DEB / RPM / TGZ / IFW / NSIS / …`. `STGZ` is the self-extracting shell
archive — the makeself equivalent, built in. Generated one and read it:

```
Usage: axl-sdk-4.4.0-Linux.sh [options]
  --help  --version  --prefix=dir  --include-subdir  --exclude-subdir  --skip-license
```

Grepping that script for `uninstall`, `upgrade`, `sha256`, `checksum` and
`verify` returns **zero** for all five. CPack STGZ is a *packer*: it extracts a
tree at a prefix, optionally into a versioned subdirectory, and shows a
licence. It has no upgrade path, no uninstall, no integrity check, no `current`
pointer, no dependency advice. Against `install.sh` it is strictly less.

It is also not reachable from here. CPack packages what **CMake installs**, and
this project has no top-level `CMakeLists.txt`, no `cmake/` directory, and no
tracked `.cmake` outside vendored gcc sources. Our install layout is 999 lines
of `scripts/install.sh` — 15 `install` invocations plus arch selection, the C++
variant, the toolchain manifest and symlink management. Expressing that in
CMake is the CMake port, which has zero implementation.

**Verdict: no.** It would replace `fpm` for `.deb`/`.rpm`, which §17 is
retiring anyway. *If* the CMake port ever lands, CPack becomes the natural way
to emit the tarballs and should replace `make-sdk-tarball.sh` then — worth
recording, not worth doing first.

**Qt Installer Framework.** None of its tooling (`binarycreator`, `repogen`,
`installerbase`, `archivegen`) is installed, and it is **not in the distro
repositories** — adopting it means vendoring or fetching Qt's own binaries, a
heavyweight new CI dependency. It is GUI-first by design; headless operation
exists but is the secondary path, and our users install on headless servers,
WSL, containers and CI runners. Licensing is Qt's LGPL/GPL-plus-commercial.

**Verdict: no** — but it contributes the best single idea in this survey.

**The MaintenanceTool pattern is worth stealing.** QtIFW installs an updater
*into the installation*: a tool that lives beside the product and knows how to
update, repair or remove it, so the user never needs the original installer
again. Ours currently requires re-fetching `install.sh` to uninstall, which is
backwards — the install knows its own version, prefix and links; the script on
a web server knows none of it.

So: **ship `install.sh` into the prefix** (`libexec/axl/`) and expose it as
`axl self-update` and `axl uninstall`. Both then become discoverable from
`axl --help` alongside `axl prune`, which already handles the versioned-root
half of maintenance. That costs a staging line and two dispatcher verbs, and it
is the same conclusion §18.3 reached from the version managers: the
maintenance surface belongs *in* the SDK, not in the thing that fetched it.

**Three surveys, one answer.** Binary installers, installer generators, version
managers, CPack and QtIFW have now all been checked. None fits a 42 MB tree
with a shared `/opt` toolchain and headless enterprise consumers. Every one of
them contributed a mechanism worth copying — rustup's structure, webi's
verify-before-touch, QtIFW's in-install maintenance tool, asdf's plugin
contract. That pattern is the finding: this class of tool is assembled from
known-good mechanisms rather than adopted whole.

### 18.7 Zero Install (0install.net) — the best candidate, blocked by a different problem

Raised because Windows users may matter one day, and 0install is the only
thing surveyed with a real cross-platform answer.

**It is alive and serious.** 2.29.3 released 2026-08-06, pushes within weeks;
an OCaml core (575 stars, ~20 years old) and a .NET implementation that is the
Windows one. Not a dormant project.

**What it would genuinely give us**, and this is the strongest list any
candidate has produced:

- **One feed serving Linux, Windows and macOS.** Exactly the future being asked
  about.
- **Content-addressed identity.** An implementation is named by its digest
  (`sha256new=RB425FJGG2VCK…`), so the hash *is* the identity rather than a
  `SHA256SUMS` side file that can drift from the assets it describes. That is
  strictly stronger than what we do.
- **Dependency solving.** The cross toolchain could be its own feed that
  `axl-sdk` depends on, instead of the SDK shelling out to
  `axl-install-toolchain`. Architecturally that is cleaner than what we have:
  a declared dependency rather than an imperative second step.
- **No root, versions side by side, a shared cache**, and GPG-signed feeds.
- **`0bootstrap`** produces a self-contained installer, so users do not need
  0install first — the obvious chicken-and-egg objection is already answered.

**What it costs.** A second tool between the user and the SDK; niche adoption
(575 stars after two decades — our firmware audience will not have it, while
everyone has `curl`); not in this box's distro repositories; and a third
channel to maintain beside the tarballs.

**But none of that is what blocks it.** The blocker is that **our host side is
POSIX shell**:

```
11,774 lines of bash and Python across the host tools
  axl-cc alone            1,940 lines of bash
  axl, axl-c++            bash
  run-qemu, profile-qemu, axl-prune, axl-common   bash
  rsod-decode, gdb-syms, extract-fv-shell, axl-emulate   Python
  pe-set-debug            an ELF x86-64 binary
```

0install can *deliver* that to a Windows machine. Nothing there would run.
Windows support today means WSL — which already works, unchanged, and is how
the flagship consumer's own laptop uses the SDK.

**So the sequencing matters more than the choice.** "What runs on Windows?" is
unanswered, and it is a much larger question than packaging: rewrite `axl-cc`
in something portable, ship a Windows `pe-set-debug`, decide whether the QEMU
harness is in scope at all. Picking a distribution mechanism now would be
answering the second question while the first is open — and the answer to the
first may make the second obvious (a native Windows port that produces `.exe`
tools has different options than a pile of bash).

**Verdict: not now, and revisit deliberately if Windows becomes real** — at
which point 0install is the first thing to re-examine, not the last. Two ideas
are worth carrying regardless: the digest *as* identity rather than beside it,
and the toolchain as a declared dependency rather than an imperative step.


## 19. Phasing for §14–§18

§10's P1–P8 predate this work and are unrelated to it. These are the phases for
the installer, the asset reshape and the verification that proves them.

**D1 — `axl self-update` / `axl uninstall`. SHIPPED 2026-09-01.** Stage `install.sh` into
`libexec/axl/` and expose two dispatcher verbs (§18.6). **First, because it
changes what D2 has to publish**, and because it is the piece that makes the
install maintain itself rather than depending on a script the user has to
re-fetch. Staged at mode 0644 so `axl`'s command list — executables in
`libexec/axl/` — does not offer it as a third way to reach the same thing; the
`axl_version.py` precedent. Small, self-contained, testable without a release.

**D1a — the five maintenance verbs. SHIPPED 2026-09-01.** `update`, `use <version>`, `list`,
`uninstall`, `prune`. Settled after asking what `install` and `update` each
mean:

| verb | meaning |
|---|---|
| `axl update` | move to the newest release. **Refuses `--version`** — that would be a downgrade wearing the word update |
| `axl use <ver>` | make that version current. Instant and offline when it is already on disk; downloads only if not |
| `axl list` | installed versions, `*` marking current |
| `axl uninstall` | remove a version, its links and its `current` marker |
| `axl prune` | bound what accumulates (§12.4) |

**Why `use` rather than a separate `install`:** they would differ only in
whether the version happens to be downloaded already, which is not a
distinction a user should have to make. One verb that downloads when it must is
`mise use`'s shape.

**Why a selection verb is not optional:** `axl prune` already keeps *current +
one previous*, deliberately, so a rollback target exists. Without a way to
activate it, that retention policy pays disk for nothing. The policy implied
the verb; we had simply never written it.

**Where this stops, against §18.3:** no per-project pinning, no shims, no
dependency solving, no multi-tool support. That is where mise begins and where
we defer. These five only expose the versioned-root layout `axl prune` already
requires.

**D2 — `release.yml` publishes the new shape. SHIPPED 2026-09-01.**
`install.sh` and `VERSION` as assets; the settled names from §14.1a; the two
tarbomb fixes from §14.1c; the `.deb`/`.rpm` build retired per §17. The largest
step and the one that cannot be split usefully — a half-renamed release is
worse than either end state.

**The §19/§17.3 fork, resolved.** This paragraph says the packages retire here;
§17.3 step 1 says "ship `install.sh`; keep publishing the packages", and D7 is
"retire the packages for real". The reading taken: **D2 stops PUBLISHING them**
(the fpm steps and both package smoke suites leave `release.yml`), and D7
deletes `scripts/build-packages.sh` and moves the flagship consumer. Keeping
them *built but unpublished* between the two would be worse than either end.
The consumer therefore takes the rename and the package removal in one bump,
which is exactly what §14.4 argues for — but it means **axl-utils must move to
`install.sh` before its next pin bump**, not after.

**Four things D2 turned up that the plan did not have:**

- **The SDK tarball carried no licences.** The `.deb`/`.rpm` staged 75 doc
  files including five third-party licence sets; the tarball staged none. Four
  of the five (mbedTLS, DejaVu, libvterm, edk2 — plus FreeType's credit clause)
  are obligations that attach to redistributing `libaxl.a`, so retiring the
  packages without moving them was a compliance regression, not a doc one.
  `make-sdk-tarball.sh` now stages the whole payload plus the examples.
- **`install.sh` had to stop guessing the asset name.** It now fetches
  `SHA256SUMS` first — stable-named across every release ever cut — and takes
  the first candidate that file lists. A 404 and an unreachable mirror are the
  same failed `curl`, so choosing by which download fails cannot distinguish
  "this release predates the rename" from "the network is down".
- **A legacy tarbomb had to be safe to extract.** `axl use <older>` reaches
  archives that unpack six or forty-three entries into the CWD. The installer
  now extracts into a staging directory inside the prefix root and renames it
  into place, which both contains that blast radius and makes the final step a
  same-filesystem rename.
- **`test-pkg-deps-minimal.sh` read its dependency list out of the fpm block.**
  Renamed to `test-host-deps-minimal.sh` and repointed at
  `packaging/install.sh`'s `need_cmd` calls — a better owner, because the
  installer enforces that list at run time. D4 is where it grows a Fedora row.

Anti-drift: `make check-asset-names` holds the names equal across
`install.sh`, the two producer scripts and `release.yml`. It compares
**computed** names — it asks the producers (`--print-name`) and runs
`asset_candidates()` — so it cannot be fooled by a renamed variable.

**D3 — Collapse the documentation. SHIPPED 2026-09-01.** The install path is
one `curl` (was eight), README's install prose is 185 lines (was 243), and
`getting-started.rst` no longer mirrors it — it links to README for the
variants and keeps the tutorial. `### Pinning, mirrors and air-gapped
installs` and `### Building axl-sdk from source` became their own sections;
both had been sitting *inside* `### Using your own toolchain`.

**What D3 turned up, and it was not length.** `getting-started.rst` told
readers to `apt install gcc-aarch64-linux-gnu` — the exact advice README says
is obsolete — and its C++ section listed exceptions, RTTI, `<string>`,
`<vector>`, `<unordered_map>`, `<stdexcept>`, `thread_local` and `<format>`
as **forbidden**. All of that stopped being true at P4, when every C++ link
started carrying libstdc++/libsupc++. Compiled and linked each to check:
containers on both arches with no flag, `throw`/`catch` under `-fexceptions`,
`typeid`/`dynamic_cast` under `-frtti`. `AXLMM-Design.md` repeats the same
list and is published, so it got correction banners in the two places a
reader would act on — it already used that convention for `libaxl-cxx.a`.
`--hosted` is likewise documented in three places as a "warned no-op"; it is
an **error** (exit 1).

The lesson for D4-D7: prose staleness is not found by counting lines. The
capability claims were wrong for two weeks in a file whose install commands
were the only thing anyone had looked at.

**D3's original brief.** README's install surface was 243 lines
across four sections with eight `curl` invocations; `getting-started.rst`
mirrors it. After D2 it is three commands. **Must follow D2**, because the
names it documents have to be the names that exist.

**D4 — Consumer-side verification (§16). SHIPPED 2026-09-02.** Pre-publish
containerized install-and-build on clean Debian and Fedora — the gate.
Post-publish hash and asset-set check — the thing that catches a rename which
missed one asset, and the only check that sees the published bytes. Proves D2
and D3. Job 1 had to be re-aimed from "install the package" to "run
`install.sh`", and the distro pair kept for dash-vs-bash rather than
deb-vs-rpm; it found an undeclared `python3` dependency on its first run. See
§16.2a.

**D5 — Snapshot exclusions and their gate (§15.2). SHIPPED 2026-09-02.**
Independent of D1–D4 and can land at any point. Kept separate because it is
about what the *public repo* carries, not about installation.

`scripts/make-source-snapshot.sh` assembles and prunes; `check-snapshot-clean.py`
gates the ASSEMBLED tree; `test-source-snapshot.sh` plants each forbidden class
and requires the gate to fire, and requires a missing or near-empty snapshot to
be REFUSED rather than reported clean.

**It found a leak §15.1's own inventory had missed.** That inventory named two
files. A third, `docs/ROADMAP-Archive.md`, carried two real lab IPs, a service
tag and an iDRAC credential line — and no exclusion class covered it, so
excluding working notes would not have helped. Redacted at source.

**Two design points worth keeping:**

- **The committed rules name nothing.** A tracked file listing the hostnames it
  forbids publishes them on the next snapshot — the exact failure it exists to
  prevent. So the committed patterns are structural (private/CGNAT ranges, ssh
  tunnel directives) and the specific strings arrive as
  `AXL_SNAPSHOT_FORBIDDEN`, a CI secret. The test asserts the secret is never
  echoed into the output, since a public build log would publish it just as
  well as the snapshot would.
- **Prose is scanned strictly; fixtures and vendored code are not.** A private
  address in mbedTLS's x509 test data or in `axl_ipv4_parse_cidr()`'s docstring
  is an input or an example — the first draft flagged 64 of those. A private
  address in *prose* is where a real machine gets written down, which is
  exactly where the missed leak was. This is not the file-allowlist §15.2 warns
  about: prose gets the strictest rule, not an exemption. 169.254/16 is out of
  scope entirely — link-local is auto-assigned and identifies no machine.

**D6 — asdf/mise plugin (§18.3). BUILT AND DECLINED 2026-09-02.** Written,
tested and reverted the same day; see the note in §18.3 for the reasoning and
for what would change the answer. The short version: D1a already put the
version manager in `axl`, and an asdf-managed install is a layout `axl update`
and `axl prune` cannot manage — measured, not assumed. D6 is **closed**, not
pending.

**D7 — Retire the packages for real (§17.3). THE ONLY PHASE LEFT, and now
unblocked.** v4.5.0 published the new asset shape on 2026-09-02, so the
consumer can finally pin it — that was the hard gate, not D4. The consumer's
own handoff is
`~/work/dell/delldiags/source/src/axl-utils/doc/axl-sdk-migration-handoff.md`;
the axl-sdk half is deleting `scripts/build-packages.sh` and the `fpm`/
`rpmbuild` mentions, and it goes LAST because that script is the only
remaining way to build a package for a consumer whose policy demands one
(§17.4).

**D7 — Retire the packages for real (§17.3).** Move the flagship consumer onto
`install.sh`, then delete `build-packages.sh`, the `fpm`/`rpmbuild`
dependencies and the `build-packages` job. **Last**, and gated on D4 passing:
the consumer has already absorbed one migration this week, and this is the step
that cannot be walked back by re-running a script.

Sequencing rule for the whole set: **D1 → D2 → D3 → D4 → D7**, with D5
free-floating and D6 closed. The only hard couplings are that D2 consumes D1's staging, D3
documents D2's names, and D7 must not precede the verification that would catch
its breakage.


## 19a. Shipped: v4.5.0, 2026-09-02

The first release carrying the shape §14–§18 designed. Verified beyond the
workflow's own verdict: `check-published-release.sh v4.5.0` is 6/6 against
published bytes, and a `curl`-and-run install from the live release yields
`axl 4.5.0` / `axl-cc 4.5.0`.

```
install.sh   VERSION   SHA256SUMS                    (stable names)
axl-sdk-linux-4.5.0-x86_64.tar.gz
axl-sdk-host-tools-4.5.0.tar.gz
axl-sdk-uefi-tools-4.5.0-{x64,aa64}.tar.gz
```

No `.deb`, no `.rpm`. D4's post-publish check and D5's snapshot gate both ran
inside a real release for the first time and both passed.

**It went out as a MINOR, not a major**, because the entries are filed under
`### Breaking (packaging)`: every asset was renamed and the packages retired,
and `axl.h` did not change — source and ABI compatibility were total. See
`RELEASING.md` §"Two kinds of breaking change"; the distinction was added for
this release and is the reason 4.5.0 was available at all.

**Two runbook defects surfaced while cutting it**, both now fixed: the
pre-release gate said to run at `-j$(nproc)`, which contradicts the runner's
deliberate `nproc-2` and produced a false red (`test-task-pool-mp-qemu.sh`,
twice, for three seconds of wall time); and the release commit re-ran the whole
suite on the same box, since `ci.yml` fires on the push that precedes the tag.

## 20. The manager is not the managed

**Decided 2026-09-02.** `axl` is currently installed *by* the SDK and *from
inside* the SDK, so the thing that manages versions is itself one of the
versioned things. That is the defect this section removes.

### 20.1 The defect, measured

`link_tree` symlinks every executable in `<prefix>/bin/` into the bin
directory — including `axl` itself. So switching versions switches the
manager:

```
after install:          axl -> …/axl-sdk-9.9.9/bin/axl
after `axl use 4.0.0`:  axl -> …/axl-sdk-4.0.0/bin/axl      ← moved too
```

Run against a prefix predating D1, which staged no `libexec/axl/install.sh`:

```
$ axl update      → axl: no installer at …/axl-sdk-4.0.0/libexec/axl/install.sh
$ axl use 9.9.9   → the same
```

**A rollback strands the user.** Neither verb works and the only escape is
re-fetching `install.sh` from a web page — which is exactly the dependency
D1 existed to remove. A rollback to a *post*-D1 version is milder but still
wrong: it runs that version's installer, which cannot know asset names
introduced later. And `axl prune` deliberately retains a previous version so
that rollback is possible (§12.4), so we ship the retention policy together
with the trap it walks into.

### 20.2 What "you only interact with axl" already means

The change is **not** justified by "after installing, the user only uses
`axl`". D1a already delivered that: every verb `exec`s the STAGED
`libexec/axl/install.sh` and re-fetches nothing. Selling the change on that
would be selling something we have. What it buys is the separation below.

### 20.3 Prior art: nobody lets the managed replace the manager

| | shape | mechanism taken |
|---|---|---|
| rustup | `rustup-init` installs `rustup`; `rustup` installs toolchains | `rustup self update` — the manager updates on its own axis |
| ghcup | closest in shape: large per-version toolchains | the manager root sits deliberately OUTSIDE the versioned tree |
| uv | one binary that also manages Python versions | a second managed axis distinct from the tool's own version — our `/opt` toolchains |
| pyenv | shims in the bin dir rather than symlinks into a version | a shim is what keeps the manager put while the managed thing moves |

### 20.4 The decision, in two steps

**M1 — stop the manager being downgraded. SHIPPED 2026-09-02** (`64f3cd9a`). `axl` is forward-only: linking
never replaces an `axl` in the bin directory with one from an older prefix,
and `axl`'s installer lookup falls back to the newest installed prefix that
carries a staged `install.sh`. Small, no new roots, and it removes the
stranding outright.

**M2 — separate the manager from the managed. SHIPPED 2026-09-02.** `install.sh` becomes a
bootstrap that installs the host-tools component — which *is* `axl` plus
`libexec/` — into a manager root that `axl prune` never touches; `axl` then
installs and switches SDK versions. rustup's shape, and it already fits the
second axis (`/opt` toolchains) we manage today.

### 20.4a How M2 landed, and the two things it turned up

**The manager IS the host-tools component**, and it needed no new root. `axl
prune` prunes `^axl-sdk-[0-9]`; `axl-sdk-host-tools-<ver>` is not that, so the
manager already sat outside what prune walks, and `axl use` only ever moves
the SDK marker. So M2 is: host-tools carries the installer (0644, like the SDK
prefix), `install.sh` installs it alongside the SDK, and `link_manager`
prefers it — through the `current` marker, so upgrading the manager relinks
nothing. `axl-cc`, `axl-c++` and `axl-install-toolchain` still come from the
SDK prefix, which is the point of switching versions.

**`--print-prefix` had to change first, and M1 had already broken it.** It
printed the prefix `axl` lives in — correct only while `axl` was always inside
the active SDK. M1 pins `axl` to the newest prefix carrying an installer, so
after `axl use <older>` it named the *newer* tree; M2 would have made it name
a manager root with no headers in it at all. Since every path a consumer wants
from it is SDK content — headers, libs, `share/axl/pci-ids.json5` — it now
resolves the `current` SDK marker, falling back to its own prefix for a
source-tree stage or a host-tools-only install. The flagship consumer is about
to use it for exactly that sidecar lookup.

**A release with no host-tools asset would have killed the SDK install.**
`ensure_manager` reached `fetch_and_verify`, which `die`s rather than
returning, so `if fetch_and_verify` could not catch it — `die` exits the
script. It now checks the SHA256SUMS already downloaded for the SDK (same
release, so it is the authority) and skips the manager with a note. Caught by
the `./`-prefixed-mirror fixture, which publishes no host-tools.

### 20.5 The bootstrap does not choose a target architecture

`--toolchain` selects the **target firmware** — `x64`, `aa64` or `all` — which
is a statement of what the user is building *for*. The host cannot infer it,
and guessing costs a 239 MB (x64) or 500 MB (aa64) download of the wrong
compiler.

So the bootstrap installs **no toolchain by default** — which is already
`install.sh`'s behaviour; `maybe_toolchain()` returns early when `--toolchain`
is absent, and `axl-cc` then fails naming `axl-install-toolchain`. What
changes is the DOCUMENTATION, which presents `--toolchain x64` as though it
were the norm. It is an example, and an x86-64-centric one: someone targeting
ARM firmware wants `aa64` and nothing else.

This also keeps M2 honest for a future non-x86-64 *host*. The asset format
string already carries the host arch (`axl-sdk-linux-<ver>-x86_64`, from
`uname -m`), so an aarch64-hosted SDK needs no naming change — only a build.
A bootstrap that hardcoded a target arch would have to be unpicked first.

### 20.6 No new verb

M2 leaves a machine with a manager and no SDK, so something must install the
first one. That is `axl update` — "move to the newest release" is exactly what
installing from nothing means — and `axl use <ver>` for a specific one. The
five verbs stand, and D1a's rule that `use` subsumes `install` is what makes
this fall out rather than needing a sixth name.

