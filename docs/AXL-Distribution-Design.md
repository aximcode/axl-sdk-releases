# AXL SDK — Distribution & Consumption Design

**Status:** DRAFT v1, for iteration. No implementation decision taken.
**Date:** 2026-07-29.

How axl-sdk is packaged, installed, discovered, version-pinned, and consumed
— from a distro package, from a tarball, and from an unreleased working tree.

Companion to `AXL-SDK-Design.md` (which covers what the SDK *contains*); this
doc covers how it *reaches and is used by* a consumer.

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

**Flavour 1 already works today** and is verified: this machine has
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

The single change that makes the SDK stop *reading* like a build-tree hack.

`out/` currently holds four unrelated things:

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

---

## 5. Artifact matrix

### 5.1 What to ship

| Artifact | Status | Rationale |
|---|---|---|
| `axl-sdk.deb`, `axl-sdk.rpm` | **have** | Distro-native path; installs to `/usr`; `Provides: axl-sdk-devel` |
| **`axl-sdk-<version>-linux-<arch>.tar.gz`** | **MISSING** | The gap. Root-free, distro-agnostic. Arch / Alpine / NixOS / SUSE / CI containers / locked-down corp hosts have no supported path today. Also what turns `~/axl-sdk-3.1.0/` into a real prefix instead of a source copy |
| `axl-sdk-host-tools.{deb,rpm,tar.gz}` | have | run-qemu.sh + helpers. Correctly separate — different audience, different deps (QEMU, OVMF, virtiofsd) |
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

Sphinx currently emits **372 MB of HTML and 314 MB of man pages.** That is not
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
- no `target_compile_options` / `target_compile_definitions` per target,
- CMake cannot do its own header dependency scanning → this is precisely why
  `axl-cc --depfile` had to be invented,
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

**P2 — Separate build dir from install prefix.** Touches every doc, test path,
and CI reference to `out/`. Mechanical but wide; do it in one sweep, not
piecemeal.

**P3 — CMake toolchain file.** Its own design pass, per §6.1's open questions.
Success criterion: axl-utils can delete `AXL_UTILS_INTELLISENSE`, the phantom
`-ide` targets, `project(LANGUAGES NONE)`, and the VS-generator rejection —
while keeping Make/CMake bit-parity.

**P4 — Meson cross-file.** Small, follows P3's shape.

**Deliberately out of scope:** switching axl-sdk's own build from GNU Make to
Meson/ninja. It is a legitimate question, but it would not fix a single item in
§2 — those are all consumer-facing. Decide it separately, on its own merits.

---

## 11. Open questions

- **O1.** Keep a default `--prefix`, or require it explicitly? Requiring it kills
  the wrong mental model permanently but breaks every existing invocation.
- **O2.** Per-target-arch package split (§5.3) — worth 16 MB, or not yet?
- **O3.** Tarball granularity: one per host+target combination, or a single
  fat tarball with both target arches (~40 MB)?
- **O4.** Does the tarball need a `bin/axl-env.sh` (prepend `PATH`, set
  `AXL_SDK`) the way most SDKs do, or is `PATH` alone enough given everything
  is `$0`-relative?
- **O5.** Windows/WSL: axl-utils builds under "VS 2022 CMake → WSL". Is a
  native-Windows story ever in scope, or is WSL the permanent answer?
- **O6.** Do we care about distro inclusion (Fedora/EPEL/Debian)? Only that
  would justify the real-`.spec` work in §5.5.
