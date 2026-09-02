# AXL-Build-System-Design — replacing the Makefile with CMake

**Status: IN PROGRESS 2026-08-15.** Direction stated by Mike.
**Constraint, stated explicitly: _"we will no longer ship Makefiles."_** The
Makefile is **replaced**, not supplemented. Any plan that ends with both is not
this plan.

**Sequencing DECIDED 2026-08-15 (§5): ship 4.0.0 first, port after.** The work
lands on branch `worktree-cmake-build-system`
(`.claude/worktrees/cmake-build-system`) rather than `main`, so `main`'s
`## Unreleased` stays exactly 4.0.0's two `### Breaking` entries and the
release can be cut at any time without diffing what it contains. That
mechanism is the decision — "ship first" without it is how v3.2.3 swept up 43
unrelated commits.

Phasing is §9; the first slice is §9.1.

---


## Where this doc sits — three docs, one subject, different questions

| doc | answers | owns |
|---|---|---|
| [AXL-SDK-Design.md](AXL-SDK-Design.md) | what the SDK CONTAINS | toolchain requirement, C++ support, shipped layout |
| [AXL-Distribution-Design.md](AXL-Distribution-Design.md) | how it REACHES and is USED by a consumer | packaging, `find_package` discovery, version pinning, `out/` vs `stage/` (§4), **install layout and the `axl` dispatcher (§12–§13)**, P1–P7 |
| [AXL-Build-System-Design.md](AXL-Build-System-Design.md) | how WE build it | the CMake port, port-surface measurements (§8.2a), why `axl-cc` is excluded, `axl-config.cmake` extraction (§8.4) |

**One owner per shared fact, everyone else links** — see AXL-SDK-Design.md for
why that rule exists (two docs asserted the same stale premise for months
because each was internally consistent).

## 1. Why

Three reasons, in the order they actually justify the cost:

1. **The build-state signature is a hand-rolled re-configure.**
   `$(BUILDDIR)/.axl-build-state` hashes `AXL_TLS`, `CFLAGS`/`CXXFLAGS` and
   `CC`/`CXX`, and wipes objects, archives and every `.efi` when any changes.
   That exists because make cannot express "the flags are an input". CMake
   re-configures instead — the same idea, done by the tool rather than by us.
   This is the strongest technical argument and it is not the one ROADMAP
   leads with.
2. **`compile_commands.json` for free.** `scripts/lint.sh` needs one and gets
   it from `bear`, which forces a full 375-object rebuild every run —
   **26s of lint's 32s**, measured 2026-08-15. CMake emits the database
   natively, so that rebuild disappears.
3. **Fewer copies of the flags.** Today `check-flag-parity` guards the
   compile flags and the `objcopy -j` list across the build paths, because
   each carries its own copy. Fewer paths, less to keep in sync.

Reason 3 is **weaker than ROADMAP claims** — see §3.

## 2. The port surface, measured

Measured 2026-08-15. `docs/ROADMAP.md`'s backlog entry understates three of
these, which is why they are restated here.

| | ROADMAP said | measured |
|---|---|---|
| Makefile | "~2,000-line" | **3,579 lines**, 140 targets |
| `LINT_GATES` | 19 | **17** |
| build paths | "three" | **two** |
| `$(eval)` / `$(shell)` | — | 56 / 12 |
| test EFIs | 43 | 42 |
| Makefiles in tree | — | **2** (root + `test/fuzz/`), **both shipped** (§5.1) |
| files that INVOKE make | — | **149** (172 invocations) |

**What a port must carry** — none of it optional:

1. **The `.efi` pipeline**, per image: `gcc` with ~16 baked flags → `ld
   -shared -Bsymbolic --no-undefined --gc-sections` against a **per-arch
   linker script AND a version script** → `objcopy` with a **14-entry `-j`
   section list** plus `--subsystem` → `pe-set-debug`. Four commands, ~75
   arguments. Expressible as a custom command chain; the risk is not
   expressing it but keeping it identical to `axl-cc`'s copy (§3).
2. **The PREFIX rule**, which splits on **ARCH × BUILD × AXL_TLS** —
   `out/native-<arch>`, `-release`, `-tls`, `-release-tls`. `AXL_TLS` joined
   it on 2026-08-15 (`d8ab47ee`); before that a TLS build and a non-TLS build
   shared a tree and wiped each other, ~300 objects per alternation.
   **Consumers must ask, never compose** — `scripts/build-prefix.sh` is the
   single definition and 66 scripts depend on it.
3. **The build-state signature** (§1 item 1) — or CMake's re-configure standing in
   for it, which must be proven equivalent, not assumed.
4. **17 gates + `NONCLEAN_GOALS`.** The gates are already standalone scripts
   (`scripts/check-*.py|sh`), so their *logic* ports for free. The exclusion
   machinery does not: an unexcluded gate WIPES objects mid-build, and
   `verify.sh` runs the gate job concurrently with both arch builds.
5. **The `AXL_TLS` source-set toggle** — which sources compile at all.
6. **~180 images per arch**, each with its own link rule — 42 `TESTS` and 33
   `TOOL_NAMES` pattern-generated, plus **104 distinct explicit
   `$(PREFIX)/….efi:` rules** for demos, selftests, fixtures and drivers. An
   earlier revision said "42 + 34" and missed the 104 entirely; see §9.0.
7. **Both arches, always.** `verify.sh` cross-checks that x64 and aa64 run
   equal test counts; an x64-only port is not a port.
8. **The hermetic rule** (`AXL-Libc-Substrate-Design.md` §4.1d): nothing from
   the host — not headers, not libraries, not compilers. A port that reaches
   for a system compiler is wrong by construction. Toolchain paths come from
   `scripts/axl-toolchains.conf`, which is already the single source shared by
   the Makefile, `axl-cc` and the generated CMake package.
9. **`test/fuzz/Makefile`** (146 lines) — the *second* Makefile. Easy to miss
   because every other count in this doc is about the root one, and a port
   that leaves it standing does not satisfy the constraint. It is a separate,
   much smaller job (libFuzzer on the HOST compiler, not a cross), so it can
   land last; but it must land.
10. **149 files that invoke `make`** — see below. This is the item most likely
    to be under-scoped, and §6's first trap is exactly about it.
11. **The `make print-*` QUERY interface**, which is separate from building
    and separate from §2.1's callers: `print-prefix` (via
    `scripts/build-prefix.sh`, which **66 scripts** depend on),
    `print-lint-gates` (`verify.sh` refuses to run if fewer than 5 come back),
    `print-cc-libc-include` (`lint.sh` and `check-clang-warnings.py`),
    `print-cxx-include-dirs` (`lint.sh`). Because `build-prefix.sh` is itself
    a make caller, **every one of the 129 integration scripts is a
    transitive make caller even where it never types the word.** Slice 3's
    shim abstracts *build* invocations and does not cover this; the query
    interface needs its own plan (§9, slice 5).
12. **`CROSS=<prefix>-`** — **SETTLED 2026-08-16, and the answer is "nothing to
    port"** (§5.1). It had selected binutils only since `0bf6ed51`, and the
    glibc-targeted cross the README named cannot build the tree at all now
    `include/compat/` is gone. The README path is removed and `AXL_TOOLCHAIN`
    replaces it as a variant beside the locators, so this port inherits no
    prefix convention. Kept in this list because "reproduce or remove" was an
    open item here for a day and a reader of an older revision will look for
    the resolution.

### 2.1 The 149 callers — measured, because §6 says to

`grep -w make` reports 383 files; almost all are prose ("make sure", "make
this fail"). Enumerated by CONSTRUCT instead — a `make`/`$(MAKE)` token at a
command position — **with comments stripped first**, in files that can
execute:

| | count |
|---|---|
| invocations | **172** |
| files | **149** |
| …in `test/integration/` | **129** |
| …in `scripts/` | 15 |
| …elsewhere (CI ×2, root, `test/`, `toolchain/`) | 5 |

**An earlier revision of this table said 157 files / 215 invocations.** That
census grepped raw text without stripping comments, so roughly a quarter of
it was prose — including 25 hits in the root Makefile, which contains only
three real `$(MAKE)` calls, and a hit inside an ipxe licence file. It also
MISSED live constructs by anchoring too tightly: `if ! make`, `<(make …)`
(process substitution, which is how `verify.sh` reads `print-lint-gates`),
`bear -- make` (the very line slice 1 deletes), `@$(MAKE)` (the dominant
recipe form), `run make make …`, and `subprocess.run(["make", …])` in two
Python gates. Wrong in both directions at once. Two independent re-derivations
now agree on 149/172.

**124 of the invocations are the single shape `make -C "$PROJECT_DIR"`**
(129 repo-wide). That uniformity is a trap, not a comfort — it is the "71
uniform assignments looked like the whole surface" shape from §6, and the
residue is what will bite.

**The decisive measurement — of the 129 integration scripts that call `make`,
43 source nothing at all.** (80 source `common-test.sh`; 6 source other shared
infrastructure — `axl-common.sh` ×4, `axl-toolchains.conf`, `lib/discover.sh`.
43+80+6 = 129.) So the indirection CANNOT be a shell function; it must be a
SCRIPT on disk, for the same reason and by the same evidence as
`scripts/build-prefix.sh`. Call it `scripts/axl-build.sh`, and switch the
callers to it BEFORE CMake exists — then the make→cmake swap is one file, not
a 149-file sweep performed under a half-ported build.

*(§6 separately says "51 integration scripts source nothing at all". That
counts the whole integration population, not just make-callers, and it too is
stale — the figure today is 59. Different questions; both re-measure.)*

**For scale**: the `AXL_TLS` prefix split was 98 references across 66 scripts
and cost 48 failing integration suites when measured by the tidy shape. This
surface is **larger**, and that remains true at 149 as it was at the inflated
157.

## 3. `axl-cc` is NOT part of this port

The appealing version of this project is "CMake replaces the Makefile *and*
`axl-cc`, one path, done." That was measured and it fails:

- **Direct-PE linking dies on aa64.** ARM's `aarch64-none-elf-ld` lists no PE
  emulation at all (`aarch64elf`, `armelf`, `aarch64linux`). x64's binutils
  does carry `i386pep`, so x64 alone could — two pipelines to save one step on
  one arch.
- **Specs files have no post-link hook**, so `pe-set-debug` cannot fold into
  the compiler driver.
- `axl-cc` is the **consumer** driver — what an installed-SDK user runs. It must keep
  working regardless of how the SDK builds itself.

**Consequence for the pitch.** ROADMAP calls "three build paths → one" the
strongest argument FOR the port. It is already **two**: `2229abc0` made the
generated CMake package CALL `axl-cc` instead of reimplementing it, and
`check-flag-parity` reports *"across 2 build path(s); scripts/install.sh
delegates to axl-cc"*. The realistic end state is **CMake (in-tree) + `axl-cc`
(consumer)** — still two, and `check-flag-parity` still has a job. Plan for
that rather than discovering it mid-port.

## 4. CMake, and why not Meson

**Decision: CMake.** Recorded with the counter-argument, because it is close.

For CMake:

- **The consumer API is already CMake.** `scripts/install.sh` generates
  `axl-config.cmake`; `find_package(axl)` + `axl_add_app()` is the documented
  consumer entry point and `test-cmake-package.sh` gates it. A Meson in-tree
  build would leave that CMake package to maintain anyway — meaning **three**
  build technologies in the repo (Meson, the generated CMake package,
  `axl-cc`) to remove one. That cuts against the point.
- Consumers already have CMake in their toolbox; several build with it.

For Meson, honestly stated:

- Its **cross-file** model expresses "two fixed bare-metal toolchains" more
  directly than CMake toolchain files do.
- Native `compile_commands.json` and a native test harness — though CMake
  gives the first too, so this is a smaller edge than it looks.
- "The consumer package is CMake" does not *oblige* the in-tree build to be.

The deciding factor is technology count, not elegance: CMake removes a
technology, Meson adds one.

## 5. Sequencing against 4.0.0 — DECIDED 2026-08-15

**Ship 4.0.0 first; port after.** 4.0.0's content is finished, validated and
green today (its two `### Breaking` entries are the `axl-cc --depfile` and
`axl-c++ --hosted` removals, and `check-release-semver.sh` already refuses
anything below a major). The port is unbounded until it starts. Holding a
finished release behind an unstarted one is how a release grows the 43
unrelated commits that produced the v3.2.3 incident.

### 5.1 We DO ship both Makefiles — corrected 2026-08-15 after review

**An earlier revision of this section claimed the opposite, and it was
wrong.** It is preserved as a correction rather than deleted, because the
mistake is the one §6 warns about and it nearly changed a release decision.

The claim was: `git ls-files` matches exactly two Makefiles, `install.sh`
installs neither, `sdk/` ships only `CMakeLists.txt`, therefore *"we have
never SHIPPED a Makefile — we use one"*, therefore the port is not
consumer-breaking and needs no major.

Every one of those measurements is accurate. **The conclusion does not
follow, because they measure the shipped SDK-tarball payload and the shipped
surface is larger than the payload.** Counting one shape and treating it as
the thing — §6's first trap, committed by the doc that teaches it.

What was missed:

- **`.github/workflows/release.yml` publishes the whole source tree on every
  tag.** It runs `git archive --format=tar HEAD`, deletes only `.github/`,
  and force-pushes the result to the PUBLIC `aximcode/axl-sdk-releases`
  repository plus a tag. **There is no `.gitattributes` anywhere in the tree**,
  so nothing is `export-ignore`d and both Makefiles go with it verbatim. The
  step's own comment says the intent is that
  `git clone …/axl-sdk-releases.git` yields the latest release's source tree,
  and GitHub's automatic "Source code (tar.gz/zip)" links on every release
  page are that tree.
- **`README.md` documents building with it, and README.md ships inside the
  SDK tarball** (`make-sdk-tarball.sh` stages it to
  `share/doc/axl-sdk/`; it used to be the `.deb`/`.rpm`, which retired with
  D2). For **macOS** and **native Windows/MSYS2** — platforms with *no
  published build at all* — it is the ONLY documented build path:

      git clone https://github.com/aximcode/axl-sdk-releases.git
      cd axl-sdk-releases
      make            CROSS=x86_64-unknown-linux-gnu-
      make ARCH=aa64  CROSS=aarch64-unknown-linux-gnu-

**Consequences, and they are not cosmetic:**

1. **Deleting the Makefile removes the only documented build path for macOS
   and native-Windows users.** That is a breaking change by any reading, so
   the port DOES need a `### Breaking` entry and a README rewrite, and it
   plausibly needs a major of its own.
2. **`CROSS=<prefix>-` is a supported consumer-facing feature** (Makefile:76,
   90, 113-115, 987, 1046, 1053, 1525) that lets a consumer build with a
   *system* cross-toolchain from brew or MSYS2. It appears nowhere in §2's
   port-surface list. **CMake must reproduce it, or the README path must be
   removed deliberately and with Mike's agreement** — not dropped by accident
   mid-port. Note this does not breach the hermetic rule, which governs how
   *we* build, not what a consumer may point the build at.

   **ANSWERED 2026-08-16 (`00206e7a`, on `main`): the README path is removed,
   and the port reproduces nothing.** Two measured reasons, neither of which is
   "we chose not to support it":

   - `CROSS=` has selected **binutils only** since `0bf6ed51` replaced
     `CC = $(CROSS)gcc` with an `AXL_*_GCC` lookup. The documented command left
     the compiler pointing at `/opt`, so it could not work on a host without
     that toolchain — and it shipped that way in 4.0.0.
   - A **glibc-targeted** cross cannot build the tree at all now that
     `include/compat/` is gone: `deps/` needs genuine bare-metal newlib
     headers and dies on the first `<stdlib.h>`. The brew tap the README named
     is exactly that kind of cross.

   What replaces it is `AXL_TOOLCHAIN`, a **variant** beside the existing
   locators — the pairing Zephyr (`ZEPHYR_TOOLCHAIN_VARIANT`), EDK2
   (`TOOL_CHAIN_TAG`) and the Linux kernel (`LLVM=1`) all use, and which AXL
   lacked. **For this port that is a simplification**: there is no prefix
   convention to carry into CMake, and the eventual toolchain file (Distribution
   §6.1 / P3) has a variant seam to slot into rather than one to invent.

   It also removes a reason to hurry: the kernel had to REINTERPRET
   `CROSS_COMPILE` as a `--target=` triple under `LLVM=1`, because clang is one
   binary carrying every backend. Having no prefix convention means the port
   never inherits that debt.

### 5.1a The sequencing decision survives — on one support instead of two

Ship-4.0.0-first rested on two arguments. §5.1 has just destroyed the second
one. The first is untouched and was always the stronger:

**4.0.0's content is finished, validated and green today; the port is
unbounded until it starts.** Holding a finished release behind unstarted work
is how a release grows the 43 unrelated commits that produced the v3.2.3
incident. That reasoning never depended on whether the port was breaking.

The *revised* conclusion is not "the port needs no major" but **"the port
probably needs a major of ITS OWN"** — i.e. 4.0.0 ships now, and the port
lands in 5.0.0 rather than being squeezed under 4.x. Majors are cheap; a
release held hostage is not. Note the falsified claim would have pointed the
same way anyway, which is luck, not vindication.

`AXL-Distribution-Design.md` §10 says the in-tree build system is invisible to
consumers. **That is now also suspect** — see §8.1, which is corrected to
match.

### 5.2 "Ship first" needs a mechanism, or it is only a sentiment

`cut-release.sh` dates whatever sits under `## Unreleased`. Port commits
landing on `main` before 4.0.0 is cut would therefore ship inside it, which is
the v3.2.3 shape exactly — the ordering would have been agreed and then
violated by the tooling.

**Mechanism: the port lands on `worktree-cmake-build-system`, not `main`.**
`main` stays at 4.0.0 content, cuttable on any day Mike chooses, and the
branch merges afterwards. Cost is periodic rebases, which is the cheap side of
this trade.

## 6. Traps this tree has already paid for

Each cost real time in 2026-08 and each is build-system-shaped:

- **Counting one SHAPE of a thing is not counting the thing.** The `AXL_TLS`
  prefix split needed 98 references across 66 scripts to stop composing
  `out/native-$arch` by hand. Measuring only the 71 uniform `VAR=` assignments
  looked complete, passed `verify.sh`, and failed **48 integration suites** on
  inline literals like `EFI="$PROJECT_DIR/out/native-x64/tools/ata.efi"`.
  `grep -c 'out/native-'` was the honest number. **A port will hit this class
  again**: enumerate by construct, not by tidiest instance.
- **Ask the build system for paths; never compose them.** That is why
  `scripts/build-prefix.sh` exists, and why it is a SCRIPT rather than a shell
  function — 51 integration scripts source nothing at all.
- **Scan for the SHAPE after the first instance fails.** The same split
  exposed three latent bugs; the scans found the second and third, the test
  failures found only the first.
- **A gate that cannot see is worse than none.** `bear` needs a full rebuild
  or the compile database is partial and clang-tidy silently skips exactly
  what changed. Whatever replaces `bear`, prove it sees everything.
- **`verify.sh` runs its jobs concurrently** — wall clock is the slowest job,
  not the sum (74s today). Do not serialize it.

## 7. Open

**Closed since PROPOSED:**

- ~~**Order against 4.0.0**~~ — DECIDED, §5: ship 4.0.0 first, port on a
  branch. Evidence in §5.1 that the port is not consumer-breaking at all.
- ~~**Whether the port owns `lint.sh`'s `bear` removal**~~ — YES, it is slice
  1. Spec in §9.1.

**Still open:**

- **Whether `verify.sh` stays a shell script** driving CMake, or becomes
  `ctest`. The gates are standalone scripts either way; this is about the
  runner, and `verify.sh`'s concurrency + per-job `--only` filtering is
  load-bearing. Lean: keep the shell script — `ctest` buys little here and
  `--only` has no clean `ctest` equivalent — but decide it after slice 2, when
  we know what the CMake side actually looks like.
- **Whether the port absorbs `AXL-Distribution-Design.md`'s P2**
  (build directory vs install prefix). **No longer forced** — the shared
  surface this hinged on was measured at SEVEN files, not 149, and P2's own
  half landed separately (§8.2a). What remains of P2 is Distribution's O1:
  does the default move from `out/` to `stage/`? **Still Mike's call, and
  still worth taking before slice 2**, because it decides the CMake
  `BUILDDIR`/`PREFIX` layout and retrofitting that is the expensive order —
  but it is now a one-script change either way, not a sweep, so answering it
  late is cheap rather than costly.
- **`test/fuzz/Makefile`** (§2 item 9) — CMake, or a plain script? It builds
  libFuzzer binaries with the HOST compiler, so it shares almost nothing with
  the cross build. A 146-line Makefile replaced by a 60-line script may be the
  honest answer; that is not a cop-out as long as no file named `Makefile`
  survives.

## 8. Relationship to AXL-Distribution-Design.md

Read that doc before slice 2. It covers the *consumer* half of the same
territory and three of its findings bear directly on this port.

### 8.1 Its "invisible to consumers" premise is WRONG, and so was ours

Its §10 lists **"switching axl-sdk's own build from GNU Make to Meson/ninja"**
as *deliberately out of scope*, on the grounds that it "would not fix a single
item in §2 — those are all consumer-facing."

An earlier revision of this section cited that as independent corroboration
that the in-tree build system is invisible to consumers. **Both docs were
wrong in the same way** — §5.1 has the evidence: the release workflow
publishes the full source tree, README ships in the package, and `make
CROSS=…` is the documented macOS/Windows build path.

The scoping conclusion in Distribution §10 still holds, but for a weaker
reason than stated: the port does not fix any item in *its* §2 list of
consumer pain. It is not true that consumers cannot see the in-tree build.

Two things follow. First, this port must justify itself on *internal* grounds
— §1's three reasons — because it delivers no consumer-facing benefit; §1 is
honest about that and should stay that way. Second, it now carries a
consumer-facing **cost** (§5.1's consequence 1), which §1 does not yet weigh.
That asymmetry is the honest summary of the project: internal benefit, some
external cost, and the external cost is a README and a `CROSS=` equivalent
rather than anything structural.

### 8.2 P2 and this port were thought to share a surface — WITHDRAWN, see §8.2a

Distribution §4 proposes separating the build directory from the install
prefix (`out/native-<arch>` → `build/<arch>-<mode>/`, `out/{bin,lib,…}` →
`stage/`), phased as **P2**, and describes it as *"touches every doc, test path
and CI reference to `out/`. Mechanical but wide; do it in one sweep, not
piecemeal."*

That is the identical surface this doc measures in §2.1 — the same integration
scripts, the same 50 that source nothing, the same `build-prefix.sh`
indirection. Executed separately they are two wide, risky, mechanical sweeps
over one set of files. Executed together they are one.

**Recommendation: the port absorbs P2**, on the argument that a build-system
replacement is the *only* good moment to change where build output lands —
CMake wants an out-of-tree build directory natively, which is P2's goal
arriving for free rather than as a sweep. Recorded as open in §7 because the
scope call is Mike's.

#### 8.2a WITHDRAWN 2026-08-16 — measured, the shared surface is SEVEN files

**The premise above is false and this section is kept as the correction.** It
asserts "the identical surface" from this side; `AXL-Distribution-Design.md`
§8.2 asserted it from the other; neither had measured it since `d8ab47ee`.
Counted with comments stripped — the same pollution that made §2.1's caller
count read 157 when it is 149:

| | files |
|---|---|
| invoke `make` | 155 |
| …already ask `build-prefix.sh` | **139** |
| reference an `out/` path at all | 23 |
| **both — the real overlap** | **7** |

Seven: this `Makefile`, `scripts/axl-common.sh`, `build.sh`, `install.sh`,
`lint.sh`, and two integration tests. **`d8ab47ee` already paid for the wide
sweep** — the `AXL_TLS` prefix split pushed 139 callers through
`build-prefix.sh` for an unrelated reason, and it is the same 98-refs /
66-scripts sweep §6 records as the trap that cost 48 failing suites.

What actually remained was not a caller sweep. The STAGED SDK had no accessor,
so ~12 integration tests hand-composed `$PROJECT_DIR/out/bin/axl-cc`. That is
fixed on `main` (`52f5f369`): `scripts/sdk-prefix.sh` mirrors
`build-prefix.sh`, `common-test.sh` gains `test_sdk_dir`, and `AXL_SDK_PREFIX`
relocates the staged SDK — verified by staging elsewhere and by the control of
pointing it at an empty directory.

**So the recommendation is withdrawn: the port need NOT absorb P2, and neither
blocks the other.** The entire argument for pairing them was the shared
surface. What is left of P2 is the *decision* in Distribution O1 — whether the
default moves from `out/` to `stage/` — which is now one edit to one script,
and is orthogonal to this port.

The half of the original argument that survives is narrower and still true:
CMake wants an out-of-tree build directory natively, so if O1 is answered YES,
doing it during the port is cheaper than doing it before. That is a
convenience, not a coupling.

### 8.3 P3 does NOT come free with this port — say so before anyone assumes it

Distribution §6.1 wants a **consumer CMake toolchain file** that makes
`axl-cc` play the role of `CMAKE_C_COMPILER`, so a consumer gets
`compile_commands.json`, `target_link_libraries` and a working clangd. It is
that doc's highest-leverage item.

**This port answers none of it.** The in-tree build drives the bare-metal
crosses (`x86_64-elf-gcc`, `aarch64-none-elf-gcc`) DIRECTLY, with the `.efi`
pipeline as explicit custom commands — it never puts `axl-cc` in a compiler
slot, which is the entire difficulty of P3 (§6.1's six open questions are all
about `axl-cc`'s CLI, not about CMake). Slice 1 producing a
`compile_commands.json` for *our* tree is not evidence that a *consumer* can
get one.

The overlap is real but narrow: both need `CMAKE_SYSTEM_NAME Generic`,
`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` and a per-arch toolchain file,
so the in-tree toolchain files are a working reference for P3's shape. That is
worth something and it is not the same as doing it.

### 8.4 IN SCOPE: extract `axl-config.cmake` from `install.sh`'s heredoc

**Decided 2026-08-16 (Mike): this belongs to the port, not to a standalone
cleanup.** Recorded here so it is not rediscovered as a surprise mid-slice.

`scripts/install.sh` generates the consumer CMake package from a quoted
heredoc: **334 of its 936 lines are CMake**, a third of the file in another
language. The stated reason (its comment at the `sed` block) is placeholder
substitution — the heredoc is quoted so CMake's own `${...}` survives, and
toolchain paths arrive as `@AXL_AA64_GXX@` markers, "so this third build path
reads from `axl-toolchains.conf` rather than carrying its own copy". That
argument justifies **templating**, not **embedding**: a tracked
`cmake/axl-config.cmake.in` with the same markers satisfies every word of it.

Measured 2026-08-16:

- This repo has **no `cmake/` directory** and **no tracked `.cmake` or
  `.cmake.in` file at all** — the only tracked CMake is
  `sdk/examples/CMakeLists.txt`. Our own consumer `axl-utils` does follow the
  convention (`cmake/AxlUtilsApp.cmake`).
- The INSTALL side is already conventional and should not move:
  `<prefix>/lib/cmake/axl/axl-config.cmake` is where `find_package(axl)`
  looks.
- **`axl-config-version.cmake` does not exist.** The consequence — that *any*
  versioned `find_package` is rejected outright, and that it is why `axl-utils`
  hand-rolled `.axl-sdk-version` — is **owned by `AXL-Distribution-Design.md`**
  (§6's discovery table and P1); not restated here. What is this doc's business
  is that the file gets built by the same extraction below, for about ten more
  lines.

**Why it belongs to the port specifically**, rather than being done first or
separately:

1. It is the file that already DRIFTED. When `axl-c++` gained `-fexceptions`
   the CMake copy did not, and `check-flag-parity` stayed green because both
   named the same `objcopy` sections — a consumer got an image that compiled,
   linked, and died at the first throw (§2229abc0). The gate has to parse a
   heredoc's interior to police it; against a real file it would not.
2. The port touches this file anyway. Anything that changes the `.efi`
   pipeline or the flag set has to change the generated package in the same
   commit, and doing that inside a heredoc is how the drift happened.
3. It converges with P1 rather than competing: the same extraction that gives
   `cmake/axl-config.cmake.in` gives `cmake/axl-config-version.cmake.in` for
   about ten more lines.

Scope when it happens: move the heredoc to `cmake/*.cmake.in`, keep the `sed`
substitution exactly as-is (including `sed_escape_repl` — an unescaped `&` in
a path silently corrupts the output), add the version file, and point
`check-flag-parity` at the real files.

## 9. Phasing

Ordered so that each slice is independently green, independently revertable,
and pays for itself before the next one starts. The riskiest item (the `.efi`
link pipeline) is deliberately not first.

| slice | scope | done when |
|---|---|---|
| **1** | Compile-only CMake for x64 → `compile_commands.json`; `lint.sh` drops `bear` | §9.1's gates pass, lint is ~6s not ~32s |
| **2** | The `.efi` pipeline as custom commands; one tool image builds identically | `cmp` says byte-identical to the Makefile's output |
| **3** | `scripts/axl-build.sh` indirection; switch all 149 callers (§2.1) | full integration 150/150, still on make underneath |
| **4** | Both arches, all **~180 images**, `AXL_TLS` | `verify.sh` cross-arch counts equal |
| **5** | Gates + `NONCLEAN_GOALS` equivalent; `verify.sh` decision (§7) | all 17 gates run, none wipes a concurrent build |
| **6** | `test/fuzz/Makefile` **first**, then the root Makefile — see §9.0 | `git ls-files` matches no Makefile |

Slice 3 is deliberately *before* the Makefile dies and *while make still
works* — that is the whole point of it. Swapping the callers and the build
system in one step means debugging two things at once across 149 files.

### 9.0 Five corrections the review forced on this table

Each was verified against the tree, not reasoned about.

1. **Slice 4 was undercounted by a factor of five.** It said "42 test EFIs
   and 34 tools/examples/drivers". `TESTS` is 42 and `TOOL_NAMES` is 33 (asked
   of make, not `sed`), but those are the *pattern-generated* images only —
   the Makefile additionally carries **104 distinct explicit
   `$(PREFIX)/….efi:` rules** for demos, selftests, fixtures and drivers.
   Total ≈ **179 images per arch**, not 34. Slice 4 is therefore the largest
   slice in the plan, not a mopping-up step, and the phasing should be read
   with that in mind. §6's trap once more: `TOOL_NAMES` is one shape of
   "image", and counting it is not counting images.

2. **Slice 6's deletion order was backwards, and it would have broken a
   gate.** `check-fuzz-link` is a member of `LINT_GATES` *and* its recipe runs
   `$(MAKE) -s -C test/fuzz` — both verified. Deleting the root Makefile while
   `test/fuzz/Makefile` survives leaves a gate whose driver is gone; deleting
   `test/fuzz/Makefile` first, with `check-fuzz-link` ported alongside it,
   orders the dependency correctly. The table said "`test/fuzz/Makefile`
   last", which is precisely wrong.

3. **The `print-*` query interface is assigned to no slice.** Four targets
   exist — `print-lint-gates`, `print-prefix`, `print-cc-libc-include`,
   `print-cxx-include-dirs` — and they are load-bearing infrastructure, not
   conveniences: `verify.sh` reads `LINT_GATES` back through one, and
   `build-prefix.sh:46` shells out to `make -s … print-prefix`. **That last
   one makes every script using `build-prefix.sh` a *transitive* make caller**,
   which is a second population on top of §2.1's direct callers and is not in
   any slice's "done when". Whichever slice retires `print-prefix` owns
   `build-prefix.sh` too.

4. **`check-dep-tracking` becomes vacuous under CMake, silently.** It asserts
   every object is compiled with `-MD -MP`. CMake generates its own dependency
   tracking by construction, so the gate can neither fail nor mean anything —
   and a gate that cannot fail is worse than no gate, which this tree has paid
   for more than once. It must be either deliberately retired with that
   reasoning recorded, or re-pointed at something it *can* see. Do not let it
   pass by default.

5. **Slice 2 is permanently invisible to the compile database.**
   `add_custom_command` output never appears in `compile_commands.json` —
   only real CMake targets do. Since slice 2 implements the `.efi` pipeline as
   custom commands, slice 1's coverage oracle cannot verify it, and the
   temptation will be to read slice 1's green DB as covering slice 2's work.
   It does not. Slice 2 needs its own verification (`cmp` against the
   Makefile's bytes, which its "done when" already says) and slice 1's oracle
   must not be extended to claim it.

### 9.1 Slice 1 — the compile database, specified

**Why this first.** It is the only slice with a measured payoff that lands
before anything else changes, and it forces the three foundations (toolchain
file, flag set, source sets) while deferring the `.efi` pipeline entirely.

**Baseline re-measured 2026-08-15 in a clean worktree**, since the numbers
above were taken elsewhere:

| | seconds |
|---|---|
| `lint.sh`, warm cache | **31.67** |
| …of which the `bear` step | **27.13** |
| `lint.sh`, cold cache (fresh worktree) | 68.88 |

So the earlier "26s of lint's 32s" holds. Slice 1 should land `lint.sh` at
**~5s warm**. Note the cold number: in a fresh worktree the clang-tidy result
cache is empty and lint costs 69s, of which `bear` is still only ~27s — so
slice 1 improves the warm case dramatically and the cold case by ~40%.

**The win is bigger than "drop bear".** `CMAKE_EXPORT_COMPILE_COMMANDS` writes
the database at **generate** time, not build time — so the replacement
compiles *nothing at all*. The 26s does not shrink; it disappears.

**Spiked and confirmed 2026-08-15**, because this claim carries the slice and
a bare-metal cross is exactly where CMake usually falls over. A two-target
spike (one C, one C++) against `/opt/x86_64-elf-gcc-14.3.0-axl2` with
`CMAKE_SYSTEM_NAME Generic`:

- configure+generate: **0.2s**; `compile_commands.json` written, **0 project
  objects compiled** (the only two `.o` are CMake's own compiler-ID probes).
- `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` is **required and
  sufficient**: with it, CMake reports *"Check for working C compiler —
  skipped"* and never tries to link. Without it, detection links a test
  executable, and there is no linker script — it would fail.
- The DB records the **cross** compiler path, so `clang-tidy`'s target
  inference stays correct (property 1 below).
- Object extension is `.obj` under `Generic`, not `.o`. Cosmetic for the DB;
  worth knowing before it surprises someone in slice 2.

One integration detail: CMake writes the DB into the **build** directory,
while `lint.sh` runs `clang-tidy -p .` from the repo root. Slice 1 either
passes `-p <builddir>` or symlinks; do not "fix" it by copying the file,
which reintroduces a second copy that can go stale.

**Scope — the compile half only, x64 only, no link step.**
Out of scope: aa64, `AXL_TLS`, linking, the gates, the 149 callers.

**Measured from the oracle, not from the filesystem** —
`bear -- make tests tools AXL_CPP=1`, 2026-08-15: **376 entries, 374 unique
files, 368 C/C++ TUs** plus 6 assembly (not linted).

| directory | TUs | note |
|---|---|---|
| `src/` | **265** | 252 `LIB_SOURCES` + 3 crt0 + 3 cxxrt + 7 `.cpp` |
| `tools/` | **44** | 45 on disk; `tools/axl.c` needs `make axl-busybox` |
| `test/` | **43** | 42 in `test/unit/` + `test/integration/axl-shell-launcher.c` |
| `deps/` | **12** | libvterm ×8, lzma ×4 |
| `drivers/` | **3** | `drivers/crashhandler/` |
| `scripts/` | **1** | `pe-set-debug.c` — **host-compiled, see below** |

**`find src -name '*.c'` is NOT the source set, and the first draft of this
section proved it** — §6's trap, in the doc that warns about it. That draft
counted files on disk, and the oracle then found three things it had missed
entirely (`drivers/`, the `test/integration/` source, `pe-set-debug.c`), one
it over-counted (`tools/axl.c` is on disk but never built here), and one it
got right for the right reason (the crt0 and cxxrt files ARE compiled by
separate rules and DO belong in the DB — `LIB_SOURCES` is a strict subset of
the DB, nothing in it is missing).

**Three shapes slice 1 must handle, all found by the oracle:**

1. **One TU is compiled by the HOST compiler.** `scripts/pe-set-debug.c`
   builds with `/usr/bin/gcc`; the other 375 use
   `/opt/x86_64-elf-gcc-14.3.0-axl2/bin/x86_64-elf-{gcc,g++}`. A CMake project
   configured with a cross toolchain file cannot naturally emit a host-compiled
   entry into the same database — that needs a separate host project (a
   superbuild / `ExternalProject`) or a deliberate exclusion. **Decide which
   before writing the CMakeLists**, because retrofitting it means re-running
   the toolchain decision. Note this does not breach the hermetic rule (§2
   item 8): `pe-set-debug` runs on the build machine and never enters an image.
2. **Two TUs are compiled twice, with different flags** —
   `tools/9p-{serve,mount}-svc.c`, the `--service` double-compile that produces
   a launcher and a driver from one source. A *set*-based coverage check
   collapses them to one entry, so it cannot see if one of the two is lost.
3. **Some `.s` inputs are generated into the build tree** (`EMBED_BLOB`'s
   `embed-blob-*.s`). They are assembly, so lint ignores them — but they are
   why the build tree must exist before the DB is complete under `bear`, and
   why CMake's generate-time DB is not merely faster but structurally
   different.

**Do not hard-code the resulting total.** Derive it, per the next paragraph.

**Three properties that are easy to lose and each turn lint into a gate that
cannot see:**

1. **The recorded compiler must stay `x86_64-elf-gcc`.** `clang-tidy` infers
   the freestanding target from the compiler's NAME in the database. Record
   the host `cc` and every TU is analyzed as linux-gnu against the host libc —
   the failure `lint.sh`'s `CT_LIBC` comment already documents at length.
2. **The flags must match what the Makefile actually builds with.** A DB that
   disagrees means lint analyzes a fiction that compiles nowhere.

   **DECIDED: CMake READS the flags and the source list back from the
   Makefile during the transition; it does not restate them.** New
   `print-lib-sources` / `print-cflags` / `print-cxxflags` targets, in the
   idiom this tree already uses for `print-lint-gates`,
   `print-cc-libc-include` and `print-cxx-include-dirs` — and `NONCLEAN_GOALS`
   already filters `print-%`, so a print target cannot trip the build-state
   wipe.

   The alternative — restating the flags in `CMakeLists.txt` and adding a
   fourth path to `check-flag-parity.py` — was the first plan and it is worse.
   A gate that detects drift is strictly weaker than a structure in which
   drift cannot occur, and the correctness requirement here is not "the flags
   are similar" but "the DB describes the very command the object was built
   with." Reading them back gets that by construction and needs no gate.

   It looks backwards for the new build system to read the old one. It is the
   correct shape for a strangler-fig migration: CMake is a *view* over the
   Makefile until it takes over, and the direction reverses in slice 6 when
   the print targets are deleted along with their owner.
3. **Coverage must be asserted as a SET, not a count** — and measured against
   the *incumbent*, not against the filesystem. A DB missing a TU makes
   `clang-tidy` skip it silently ("Compile command not found") and report
   clean.

   **Use `bear`'s own output as the oracle while it is still there.** Capture
   `compile_commands.json` from today's `bear -- make tests tools AXL_CPP=1`
   once and assert the CMake DB covers it. That is a far stronger check than
   counting files on disk — it compares against what the build *demonstrably*
   compiles, which is the only definition that matters, and it sidesteps every
   judgement call in the table above. **It already paid for itself**: it is
   what found `drivers/`, `pe-set-debug.c` and the `test/integration/` source
   that the disk-based estimate had missed. The oracle exists only while
   `bear` does, so capture it in the slice's first commit.

   **Compare as a MULTISET, not a set.** Two TUs are compiled twice with
   different flags (shape 2 above); set-equality collapses those to one entry
   and cannot see one of the pair being lost. Compare `(file, count)` pairs, or
   better `(file, flags-hash)`.

   It must fail on a MISSING entry, not merely on a count mismatch — a correct
   count over a truncated list reads as healthy.

   **Two distinct checks, and conflating them is a trap.** The oracle is a
   *migration* check with a natural end: it answers "does CMake cover what make
   covered?" and becomes meaningless once make is gone. The *permanent* gate is
   a different question — "does the DB cover everything lint should see?" —
   and must compare the DB against the sources on disk with an explicit,
   justified exclusion list (today that list is exactly `tools/axl.c`,
   `src/net/axl-mbedtls-platform.c`, and the `AXL_TLS` set). A frozen oracle
   as a permanent gate goes stale the first time someone adds a source, and
   goes stale SILENTLY, which is the failure mode §6 names.

**Positive control, required before believing any of it** (§6, and
`feedback_a_gate_that_cannot_see_is_worse_than_none`): delete one source from
the CMake source list and confirm the coverage check goes RED. A coverage
check that has never failed is an assertion, not a gate. Use
`scripts/sabotage.sh --expect-fail`.

**What slice 1 deliberately does NOT do.** `lint.sh` reads
`make -s print-cc-libc-include` and `make -s print-cxx-include-dirs`. Those
stay on make for now — they are derived from the compiler, CMake can produce
them identically, and moving them is slice 5's business. Noting it here so the
residual make dependency in `lint.sh` is a recorded choice rather than an
oversight.

**Gates for the slice:** `./scripts/verify.sh` ALL GREEN (74s baseline) and
`./test/integration/run-integration.sh` 150/150, plus the positive control
above and a before/after timing of `lint.sh` to confirm the 26s claim.
