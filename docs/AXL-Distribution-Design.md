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
sudo dnf install ./axl-sdk.rpm        # or: apt install ./axl-sdk.deb
axl-cc hello.c                        # → hello.efi
```

```sh
tar xf axl-sdk-3.1.0-linux-x86_64.tar.gz -C ~/opt
export PATH=~/opt/axl-sdk-3.1.0/bin:$PATH
axl-cc hello.c                        # → hello.efi   (no root, any distro)
```

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

`test-pkg-deps-minimal.sh` (debian:stable-slim, no toolchain) is the gate for
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

