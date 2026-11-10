# AXL-Build-System-Design — replacing the Makefile with CMake

**Status: DONE 2026-11-09 -- the Makefile is retired; CMake + Ninja is the
only build (on `main` from 93b7e017, for 5.0.0; §10.12).** Slices 1-6 are
recorded in §10.3-§10.12; §10.13 makes every local entry point run CI's pinned
CMake. CPack was measured and not adopted (§10.9). What the
port left deliberately narrower is listed where it happened (§10.12: the
`AXL_PACK` toggle check runs on x64 only). Direction stated by Mike 2026-08-15;
scope widened 2026-11-02 (§10: Ninja, CTest, CPack, the output layout). The
paragraphs below are the history, kept as written.

*(Earlier status, superseded:)* SLICE 1 STARTED 2026-11-02 on branch
`worktree-cmake-build-system`. Re-checked 2026-09-03 and corrected -- it had
read "IN PROGRESS" for three weeks while nothing progressed.

**Since 2026-08-16 the Makefile grew from 3,579 to 5,948 lines** (measured
2026-11-02). The cost of waiting is not flat.

What "not started" means here, measured rather than asserted: the branch this
work was to land on, `worktree-cmake-build-system`, is **zero commits ahead of
`main`** — main has absorbed everything on it, and everything on it was docs.
Its last commit (`dc0832ba`, 2026-08-16) is a design note. The worktree at
`.claude/worktrees/cmake-build-system` is gone. **No `CMakeLists.txt` builds
any part of `libaxl`** — the three in the tree are `sdk/examples/` (consumer
projects exercising the GENERATED `axl-config.cmake` package, which is the
opposite direction: CMake consuming us, not building us) plus vendored
gcc/binutils sources under `toolchain/`. `main` still ships the Makefile this
document exists to replace. No phase of §9 has begun.

The DECISIONS below are still live and still correct — the constraint that the
Makefile is replaced rather than supplemented, and the §5 sequencing that put
4.0.0 first. Only the progress claim was wrong.
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

Phasing is §9; the first slice is §9.1. **§10 records the 2026-11-02
decisions that widen the scope: read it before §7 and §9, which it amends.**

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

**Still open:** *(closed -- `verify.sh` STAYED a shell script; since slice 5
it is a thin front end that selects CTest tests per job and keeps `--only`.)*

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

## 10. Decisions 2026-11-02 — CMake + Ninja in full, CTest, CPack, the layout

Mike, asked "sub-makefiles or the CMake port": the port. Sub-makefiles were
weighed and rejected — recursive make splits the dependency graph (`libaxl.a`
feeds ~180 images per arch from other directories) and makes `-j` worse, and an
`include`d-`.mk` reorganisation would tidy a file this plan deletes while fixing
none of §1's three reasons. `make -j` already works (`scripts/build.sh` passes
`-j$(nproc)`); a plain `make` is serial only because nothing defaults it.

Then, explicitly: **"take full advantage of CMake + Ninja and use CTest; CPack
too if it fits."** That reverses §7's lean (keep `verify.sh` a shell script) and
adds packaging to the scope. What it decides:

1. **Generator: Ninja.** Parallel by default; `make -j` stops being a question.
2. **Layout, the conventional CMake split** (this answers Distribution's O1
   for the build): `CMakePresets.json` with `x64-debug`, `x64-release`,
   `aa64-debug`, `aa64-release`; **build tree `build/<preset>`** (disposable,
   objects and built images); **install prefix `stage/`** (`cmake --install`,
   what consumers and the tests' SDK use). `out/`, which today is both build
   tree and output (`BUILDDIR = $(PREFIX)/build`), goes away with the Makefile.
3. **CTest replaces the schedulers** — `run-integration.sh`'s pool and
   `verify.sh`'s job fan-out:
   | today | CTest |
   |---|---|
   | worker pool | `ctest -j` |
   | `--only`, `arch=`, `needs=`, `local-only=` | LABELS, `-L` / `-LE` |
   | `est=` | TIMEOUT |
   | `builds=` pre-build | FIXTURES_SETUP |
   | the quiet `qloop` slot | RUN_SERIAL |
   | port allocation | RESOURCE_GROUPS |
   | balanced SKIP | SKIP_RETURN_CODE |
   | CI reporting | `--output-junit` |

   What CTest has no notion of and must be rebuilt ON it, not dropped: the
   content-hash CACHE (the local 900s -> 400s; a per-test wrapper), the unit
   RATCHET and the per-binary leak VERDICT (a post-run check), and UNJUDGED
   (exit 3 -- a SKIP with a reason, or a dedicated code; decide in that slice).
   The gates become tests too; each is already a standalone command.
4. **CPack builds the release tarballs** (the three versioned ones) and
   `SHA256SUMS` (`CPACK_PACKAGE_CHECKSUM`). Two things to settle in that slice,
   not assume: the SDK tarball holds BOTH arches, so both build trees install
   into one staging prefix that is packaged once; and the archives must stay
   reproducible (`SOURCE_DATE_EPOCH`, verified on the pinned CMake). `.deb` /
   `.rpm` stay retired (D2) unless Mike reverses that -- CPack would make them
   nearly free. `packaging/install.sh`, the end-user installer, is unchanged.

**Phasing, amended** (§9's slices 1-5 stand; 6 moves to the end):

| slice | scope |
|---|---|
| 1-5 | as §9 |
| **5a** | CTest: the unit binaries and the integration scripts as tests, labels/fixtures/resources; the cache, ratchet and leak verdict on top; `run-integration.sh` and `verify.sh` become thin `ctest` front ends, then go |
| **5b** | CPack: the tarballs + checksums from `stage/`; `cut-release.sh` / `release.yml` switch to it |
| **6** | delete `test/fuzz/Makefile`, then the root Makefile (§9.0 item 2) |

### 10.1 Amendments 2026-11-02 (later the same day) — supersede §9.1's mechanism

Measured at the start of slice 1 (fresh worktree, 2026-11-02): the oracle
(`bear -- make tests tools axl-busybox integration-fixtures AXL_CPP=1`) is
**657 compile commands over 610 files** -- not August's 376 -- in **18 distinct
compiler+flag shapes**, from **250 compile rules**, about 35 of which put extra
`-D`/`-I` flags inline in the recipe. `lint.sh` takes **~55s, cold and warm
alike** (it rebuilds into a throwaway prefix every run).

1. **CMake RESTATES the build, in its final form; a parity test guards the
   transition.** §9.1's "read the flags back from the Makefile" assumed a few
   `print-*` variables; at this size it means rewriting ~35 recipes into
   variables that slice 6 deletes, then reversing the direction. Instead the
   CMake targets are written as they will stay, and a CTest test compares
   CMake's compile commands against `make -n -B` (0.06s; 653 of the 657 --
   the rest are commands generated inside recipe shell loops, which the dry
   run prints unexpanded and the test must handle explicitly) as a MULTISET of
   (file, normalised flags), failing on a missing entry, an extra one, or a
   flag difference. It is retired with the Makefile. Accepted trade: drift is
   DETECTED, not made impossible -- the August reasoning, reversed by scale.
2. **`lint.sh` is replaced, not fed.** A `lint` preset compiles with clang
   (the `-Wall -Wextra` pass) and sets `<LANG>_CLANG_TIDY` per target (the full
   config for `src/`, bugprone-only for tests, tools and fixtures), so Ninja
   runs clang-tidy alongside each compile -- in parallel, and incrementally,
   re-checking only TUs whose sources or headers changed. That replaces the
   `bear` rebuild, the result cache, the scoped-file argument (build one
   target) and `db_check` (no database is consulted). The newlib include
   injection moves into the toolchain file; the "every source belongs to a
   target" coverage check becomes a CTest test.
3. **Scripts:** the goal is as few as possible doing build or test LOGIC.
   Thin wrappers over `cmake` / `ctest` / `cpack` calls are fine. Expected to
   go: `lint.sh`, `build-docs.sh` (a `docs` target), `install.sh`'s staging
   half (`cmake --install`), `verify.sh` and `run-integration.sh` (CTest and a
   workflow preset). Expected to stay: `run-qemu.sh` and `common-test.sh`
   (what a test runs), `sabotage.sh`, the release scripts, `axl-cc` (§3), and
   `which-gates.sh`'s change-to-test mapping (CTest labels select, they do not
   map).
4. **The Makefile stays, unchanged and building, until the CMake build is
   accepted** -- the comparison baseline: the parity test on every run, and
   from slice 2 a byte comparison of each `.efi` against the Makefile's.

### 10.2 What "retire the Makefiles" covers -- measured 2026-11-03

Mike: retire every make file WE wrote; third-party tools keep their own.
Scanned, not recalled (`git ls-files`, the scripts, the docs):

**Ours, to go:**
- `Makefile` (5,948 lines) -- slices 2-6.
- `test/fuzz/Makefile` -- host libFuzzer harnesses (clang, ASan), the
  `check-fuzz-link` gate's driver. Becomes a host CMake project or preset
  (a host compiler, not the cross toolchain), deleted BEFORE the root
  Makefile (§9.0 item 2).
- A make file GENERATED by a test: `test-runner-selftest.sh` writes a
  fixture project's `Makefile` to exercise the runner's cache and `builds=`
  handling. It moves with the runner (slice 5a).
- The ~280 of our files that RUN make: 234 integration tests and 8 of their
  helpers in `test/integration/lib/` (their `make -C ... <target>` lines and
  the `builds=` metadata `check-test-meta` holds them to), 37 scripts, 2
  workflows. August counted 149; slice 3's wrapper is for exactly this.
- 20 docs that describe `make` usage, the README's `make CROSS=` build for
  macOS and Windows among them (§5.1: the breaking change, 5.0.0).

**Third-party, staying:**
- `deps/mbedtls` (a submodule): its own Makefiles and CMakeLists.txt, used
  by neither build (§10 / `deps/CMakeLists.txt`).
- `toolchain/x86_64-elf/build-toolchain.sh` drives gcc's and binutils' own
  configure + make to build the cross toolchain -- their build systems, not
  ours.


### 10.3 Slice 2 result -- every image, byte-identical (2026-11-03)

CMake now links every `.efi` the Makefile can build from this tree, and each
is `cmp`-identical to make's: **320 of 320** in all four presets, **328 of
328** with `AXL_RTK_USB_UNDI` (a stand-in NIC driver). Compiles: 709/709 (714
with RTK), compared over every paired image's make goals rather than a fixed
goal list. Slice 4's "both arches, all images" fell out of this: aa64 is the
same preset with a different arch.

- **Inventory, not a list.** `cmake-image-parity.py` reads make's own database
  (`make -pn`) and fails on an image no CMake declaration pairs. Two kinds are
  excluded BY NAME and printed every run: the sshd-rtk images when no NIC
  driver is supplied, and `build/pack/pack-toggle*`, whose raw input
  `test-efi-pack-qemu.sh` plants to test the `AXL_PACK` toggle itself -- a
  build-behaviour test, ported with the tests (slice 3), not an image.
- **Where they are declared.** Fixtures in `test/integration/`, the kernel
  PoC in `experiments/axl-kernel/`, the release drivers in `tools/`. The SDK
  examples are declared in `sdk/CMakeLists.txt`, NOT `sdk/examples/`: that
  directory ships, and its CMakeLists.txt is the consumer's own project
  (`find_package(axl)`).
- **The shapes** are few: `axl_app_image` / `axl_driver_image` (make's
  `LINK_EFI_APP` / `LINK_EFI_DRIVER`, with `CXX` their `_CXX` forms),
  `axl_object_variant` (a source again with a variant's `-D`s),
  `axl_embed_blob` (a launcher's resident driver), `axl_wrap_efi` with
  `FORCE` / `STUB` (the test wraps and the retry-test stub).
- **Two defects the widening exposed in the comparison itself**, both "a
  check that could not see": `make -n` prints a backslash-continued recipe as
  two lines, and the half holding `-c` names no compiler -- the grace
  variants' compiles were silently skipped, not even counted unparsed (now
  joined first). And `enable_testing()` after `add_subdirectory()` silently
  drops every test a directory adds (it now comes first).
- **A make-time assertion became a gate test.** make fails `hello-minimal-cxx`
  when the object has a non-empty `.init_array` (a constructor no runtime
  would run); CMake runs the same check as `gate.hello-minimal-no-ctors`,
  sabotage-proven -- and also over the prioritised `.init_array.NNNNN` of
  `constructor(N)`, which make's awk never saw.
- **Review fixes before commit.** The image gate PRINTED images only make
  builds but passed anyway; it fails on them now (sabotage: one declaration
  removed -> `NOT IN CMAKE heap-rel-c.efi`, exit 1). And every variant of a
  linted source is linted: linting one leaves code the others `#ifdef` in
  unseen (svc-tdfail.c's ~150-line driver half had dropped out), and the lint
  coverage check counts files, not variants.
- **Makefile change, for the comparison only:** the grace variants computed
  `-DUPGRADE_SVC_IN_GRACE` in the SHELL (`$((9 - 7))`), which `make -n`
  prints unevaluated; it is computed by make now. Same value, same objects.

### 10.4 `test/fuzz/Makefile` retired (2026-11-03)

The first of our two Makefiles is gone (§9.0 item 2's order). The harnesses
are the `fuzz` preset: root `CMakeLists.txt` builds ONLY `test/fuzz/` as a
HOST project when `AXL_FUZZ` is on -- one CMake project has one C compiler,
and libFuzzer/ASan need clang, not the cross toolchain. The seed replays are
CTest tests (`fuzz.seeds.<h>`, labels `gate;fuzz`), failing also on a corpus
libFuzzer read ZERO files from. `make check-fuzz-link` drives the preset
until the root Makefile goes, then the CTest tests are the gate. It no longer
builds from clean: Ninja tracks headers, which the old Makefile did not.
Sabotage-proven: a dropped source (link), a parser that crashes on a seed, a
missing corpus, an empty one. The gate configures with `--fresh`, always: on
a compiler change CMake wipes the cache and re-runs, and that re-run LOST
`AXL_FUZZ` -- the UEFI tree generated into `build/fuzz` (review-found,
reproduced). It also holds the replay count to the committed corpora, so a
harness dropped from the list fails rather than reading "clean -- 3". Test presets now set `noTestsAction: error`,
so a preset whose tests never registered cannot report green.

### 10.5 Slice 3 -- callers build through `scripts/axl-build.sh` (2026-11-03)

Every test, the integration runner, `test-axl.sh` and the scripts that build
fixtures (`axl-common.sh`, `ssh-demo.sh`) now build through
`scripts/axl-build.sh`, which takes a make command line's arguments
(`ARCH=`, `BUILD=`, goals) and runs the build `AXL_BUILD_BACKEND` names:
`make` (exactly `make -C <repo> "$@"`) or `cmake` (the preset for ARCH x
BUILD). The cmake backend REFUSES a variable it has no meaning for rather than
dropping it. `build-prefix.sh` asks the same wrapper, so the prefix a test
reads follows the backend. `lib/discover.sh` reads the wrapper line as the
make line it stands for, so `builds=` stays held to it (discover selftest:
declared, undeclared, and a `--print-prefix` that builds nothing).

- **Goals.** `cmake/AxlGoals.cmake`: one target per make goal that reaches an
  image -- EVERY such target, not only the `.PHONY` ones (eleven are plain
  targets naming their image; the first full run died on one). Each names its
  images by prefix path; `axl_image_pair` records which target makes each.
  `build.make-image-parity` holds every goal to make's expansion of it. The
  non-image goals are targets of their own: `tool-sizes` (the report the test
  parses, identical to make's), `sshd-rtk` / `sshd-full-rtk` (copy to
  `$SSHD_RTK_OUT`, print make's `Built:` line; without a driver, make's "not
  built" line), `fwtool-host` and `axl-efi-pack` (host tools).
- **One layout.** Every image, host tool and runtime object is at the SAME
  path under `build/<preset>/` as under make's `out/native-*/` -- including
  `build/` (the embedded drivers, the pack stubs, the host tools, and the
  runtime objects a test links its own consumer from). The first full run's
  failures were all this: a file goal by prefix path, an embedded driver in
  `tools/build/`, a runtime object only CMake's object directory had. The
  published runtime objects equal make's except `DW_AT_comp_dir`: CMake's is
  the relative `build/<preset>`, make's the builder's absolute path.
- **Owed before the Makefile goes (slice 6), named so it cannot be lost:**
  the CMake wrap's build behaviour -- `AXL_PACK` on wraps, off copies raw, a
  failed wrap leaves the previous image, re-enabling re-wraps -- which
  `test-efi-pack-qemu.sh` asserts only of make's `WRAP_EFI` today (the review
  of this slice found the gap); and `test-log-link-granularity.sh`, pinned to
  make because it re-links with the Makefile's own link pieces
  (`print-LINK_LIBS` ...), needs those pieces from CMake.
- **Left on make, deliberately:** tests OF the Makefile (`test-build-state-race`,
  `test-toolchain-variant`, the `AXL_PACK` toggle in `test-efi-pack-qemu`), the
  `print-<VAR>` list queries (packaging, slice 5b), the gates (slice 5), and
  the discover selftest's make lines (parser fixtures). They go with the
  Makefile.
- **The gates are the bulk of what still COMPILES under make** (noted
  2026-11-05, when a CMake-default run still showed `make` processes):
  `gate.make.*` and verify.sh's make/makeimg jobs run `make check-*`, and
  `check-examples`, `check-fuzz-link` and the artifact gates build. Each gate
  needs a CMake (or script) home before slice 6 deletes the Makefile. The
  other `make` mentions in tests are hints in SKIP messages and `print-<VAR>`
  queries, which build nothing.

### 10.6 ccache (2026-11-03)

`cmake/AxlCcache.cmake`: `AXL_CCACHE=AUTO` (default) compiles C and C++
through ccache when `AXL_CCACHE_PROGRAM` resolves and builds without it when
not; `ON` fails the configure without it; `OFF` never, even when the
environment exports `CMAKE_<LANG>_COMPILER_LAUNCHER` (CMake seeds the launcher
from it). A launcher the user chose (sccache, distcc) is kept under `AUTO` and
refused under `ON`. It is the compiler LAUNCHER, so `compile_commands.json`
still names the compiler (clang-tidy and the parity checker read it). Make
never had a cache and does not get one; it goes in slice 6.

`test-cmake-ccache.sh` pins all of that on x64-debug: a rebuild after a clean
is 684 hits and 0 misses, and the cached build's 320 images are
byte-identical to the cold build's. Parity with make is NOT that test's
claim; `build.make-image-parity` was checked by hand against x64-debug and
aa64-debug trees compiled through ccache from a cold cache (2026-11-03), and
holds on any later run only because this host has ccache and AUTO is the
default.

- **Speed.** A full rebuild from a warm cache is ~3.6 s against ~10 s cold.
  The cache is effectively PER BUILD DIRECTORY: objects carry their directory
  (`-ffile-prefix-map`, ccache's `hash_dir`), so x64-debug, x64-lint, the
  `-rtk` trees and each worktree fill their own entries.
- **An existing build tree does not notice ccache arriving.**
  `scripts/axl-build.sh` configures only when `build.ninja` is missing; after
  installing ccache, re-run `cmake --preset <p>` (or delete the tree) to pick
  it up.
- **CI.** The integration job runs on the self-hosted box, which has ccache.
  Its hosted fallback installs neither ccache nor cmake/ninja, so every
  `test-cmake-*` test SKIPs there -- the slice 5 CI work, not this one.

### 10.7 Inputs from the test-guest pool work (2026-11-04)

The port paused while the pool (docs/AXL-Test-Guest-Pool-Design.md §6.5)
was built on this branch. It left three things the remaining slices must
carry, not rediscover:

- **Slice 5a: CTest allocates the pool's guests.** `python3 -m axlpool
  resource-spec` prints a CTest resource spec (format 1.0) with one resource
  type per guest class and arch, `guest_<class>_<arch>`, its slots the
  class's; a pooled test declares `RESOURCE_GROUPS "guest_standard_x64:1"`
  (test-pool-resource-spec.sh has CTest itself read it). **Plain `ctest`
  WITHOUT the spec runs every such test with no limit at all** -- measured --
  so the project must set `CTEST_RESOURCE_SPEC_FILE` (or the front end pass
  `--resource-spec-file`), and a test should show a plain `ctest` still
  bounded. The `--pool` runner semantics carry over: a pool failure is exit
  77 marked `AXLPOOL-MISS:` (CTest `SKIP_RETURN_CODE`), and is rerun booted,
  never left a quiet SKIP.
- **`scripts/axl-build.sh` skips what the runner pre-built**
  (`AXL_PREBUILT_MANIFEST`; its "PRE-BUILT BY THE RUNNER" comment): a test's
  identical build call -- same arch, command-line variables, targets and
  build fingerprint -- exits 0 without building. 11% off the uncached suite
  (wall 1101 -> 978 s). The fingerprint already covers both backends (the
  backend is part of it; the environment inputs are derived from the
  Makefile AND cmake/), but **the runner learns which goals leave no
  artifact by asking the MAKEFILE** (`make -s print-non-artifact-goals`,
  `NON_ARTIFACT_GOALS`: `tool-sizes` and the lint gates). When the Makefile
  retires, that list needs a CMake home (a cached variable a script can
  read, or `cmake/AxlGoals.cmake`), or every report and gate goal would be
  skipped as pre-built -- the runner refuses to run rather than let that
  happen silently, so the gap is loud, not latent.
- **CTest replaces the per-test build too.** Under CTest the "build once up
  front" step is a fixture (`FIXTURES_SETUP` on a build test, or
  `cmake --build` before `ctest`); the manifest is then the build's own
  record, and the skip stays in `axl-build.sh` for the tests' own calls.

### 10.8 Slice 5a -- the plan, and the default backend flipped (2026-11-04)

Mike approved this plan on 2026-11-04 ("Go"). It refines §10 item 3; each
step lands green.

**Mechanisms, each borrowed from a named implementation:**

- **Discovery at `ctest` time, not configure time**, the way GoogleTest's
  `gtest_discover_tests` does it: a `TEST_INCLUDE_FILES` script runs
  `lib/discover.sh`, which emits the `add_test` calls. The `test-meta`
  parser stays single (check-test-meta and the runner use the same one), and
  a new `test-*.sh` appears without a reconfigure.
- **The test-meta fields map to CTest properties:**
  - `arch=`, `needs=`, `local-only=` and `guest=` become LABELS;
  - `est=` becomes TIMEOUT;
  - `guest=` also becomes `RESOURCE_GROUPS` (the pool's resource spec,
    §10.7).

  Port blocks become a `ports` resource type: the slot id CTest hands a test
  (`CTEST_RESOURCE_GROUP_*`) is its port base, replacing the runner's
  worker-index arithmetic.
- **Retry: `--repeat until-pass:2` in the test presets**, replacing the
  runner's retry-once. ctest prints the retry itself. The per-test wrapper
  keeps the attempt-1 log. **REVERSED 2026-11-06** (step 6, H3 below):
  `--repeat` repeats every test, the unit ratchet and gate.qloop included,
  and their retries erased real failures. Integration tests retry inside
  `run-one.sh --retry`, as the runner's did, and nothing else retries.
- **A per-test wrapper** carries what CTest has no notion of:
  - the content-hash cache;
  - the pool-miss rerun (exit 77 marked `AXLPOOL-MISS:` is rerun booted,
    never left a quiet skip).
- **Exit codes follow the automake/Meson protocol.** 77 means SKIP.
  **UNJUDGED gets a dedicated code**, declared as that test's
  `SKIP_RETURN_CODE` with a reason; the front end maps it to exit 3, as
  verify.sh does today.
- **The unit binaries become one CTest test per binary per arch (~102)**, on
  pool guests with a booted fallback, replacing test-axl.sh's one boot per
  arch. A hang then costs one binary rather than every later one, and each
  binary gets its own verdict. The RATCHET and the cross-arch equality
  become a `FIXTURES_CLEANUP` test that sums every binary's count, and each
  binary's leak verdict stays in its own test.
- **The build-once step is a `FIXTURES_SETUP` test** per arch. It writes the
  prebuilt manifest that axl-build.sh's skip reads.

**Steps:**
1. Flip the default backend to cmake (below).
2. Register the integration scripts through discovery, with labels,
   TIMEOUT, resources and the build fixture.
3. The wrapper (cache, pool rerun), the resource spec for presets AND for a
   plain `ctest`, and a test that a plain `ctest` stays bounded.
4. The unit binaries per binary; the ratchet as a cleanup fixture.
5. verify.sh's jobs as labelled tests (`gate`, `lint`, `docs`; qloop
   RUN_SERIAL with UNJUDGED); check-test-est reads TIMEOUTs from
   `ctest --show-only=json-v1`.
6. run-integration.sh, verify.sh and which-gates' `--only` become thin
   `ctest -L/-R` front ends; CI reads `--output-junit`; the old engines are
   deleted.

**Step 1 result: cmake is the default.** `scripts/axl-build.sh` builds with
CMake unless `AXL_BUILD_BACKEND=make`.

- **release-suite.sh** builds through the wrapper. It used to REFUSE every
  backend but make, because its build step was make and the suite ran
  `--no-build` against that tree. Through the wrapper, the tree it builds is
  the tree the tests read, under either backend.
- **The prebuilt skip's fingerprint watches the CHOSEN backend's tree.** A
  CMake tree has no `.axl-build-state`, so under cmake the fingerprint hashes
  the tree's `build.ninja`, which a reconfigure rewrites and a wipe deletes.
  Reading make's file there would have left every skip standing over a wiped
  `build/` (test-axl-build-backend.sh).
- **A test that builds with make says so: `build-backend=make`.** The
  release-image test exports `AXL_BUILD_BACKEND=make` (its tarball is the one
  release.yml makes from make's release tree, until CPack). The fingerprint
  names the backend, so while the runner pre-built its `builds=` with the
  cmake default the test's own call never matched and it built its whole
  release tree again itself -- 82 s of setup on CI. The runner now pre-builds
  each test's `builds=` with its declared backend (lib/suite-prep.sh), and
  `check-test-meta` holds the field to the test's `export` both ways.
- **CI's `gcc x64` / `gcc aa64` jobs** build with `axl-build.sh ARCH=<a> all`
  in `ubuntu:26.04`, the lint job's image: CMake's minimum is 3.29, and 24.04
  ships 3.28. 26.04's `gcc` no longer pulls in `libc6-dev`, so the job lists
  it: without it the host builds (`pe-set-debug`, which every `.efi` link
  runs, and `axl-efi-pack`) found no `<stdint.h>`. Both legs were
  reproduced in podman from a clean copy of the tree. The old "Build with TLS" step was a `make clean` plus rebuild
  left over from the AXL_TLS toggle, which is gone. The hosted fallback of
  the integration job installs no cmake, and needs none: release-suite.sh
  refuses there (no DEBUG OVMF) before any build.
- **What a release ships is still make's,** until slice 5b:
  `release.yml` builds `make BUILD=RELEASE all tools`, and install.sh
  stages through make. With the suite and push CI on CMake, nothing else
  built that path from a clean checkout, so the build job keeps one step
  running exactly release.yml's command (the review of this step found the
  gap).
- **The transition gate now runs in CI.** `build.make-parity` and
  `build.make-image-parity` ran only when someone ran them (the review of
  this step caught the doc claiming otherwise). The build job runs both
  after the CMake build. Their FIRST run in a clean container failed --
  not on a CMake difference but on the MAKEFILE: 117 link rules (63
  drivers, 3 minimal apps, 51 unit tests through `BUILD_TEST`) did not
  list the crt0 and glue objects their link macro puts on the ld line, so
  any clean-tree goal that reached one before something else built them
  failed at `cannot find axl-cxxrt-alloc.o`. The application links had
  been fixed for the same class once (`LINK_CRT0`); the driver macro had
  not. Fixed with `LINK_CRT0_D` on every such rule, and
  `test-make-link-prereqs.sh` reads make's own database (`make -pRrq`) to
  hold every link rule to what its macro links, with a control.
- **The prebuilt fingerprint under cmake hashes `CMakeFiles/rules.ninja`
  too:** a compiler or launcher change rewrites only that file.
- **Still on make until later steps:** the make gates (`verify.sh`'s `make`
  and `makeimg` jobs) and install.sh's staging (slice 5b). The makeimg gates
  name their images as prerequisites, so they build make's tree themselves;
  they are no longer warm from the x64 job.

**Step 2 result (2026-11-04): the integration scripts are CTest tests.**

- **One copy of each half of the runner.** The runner's per-test logic (the
  cache, the pooled run and its booted fallback, the timeout, the verdict)
  moved verbatim into `test/integration/lib/run-one.sh`. Its build-once logic
  (the shared pre-build, the SDK restage, the fixture pre-build, the
  stale-stage refusal, the manifest) moved into `lib/suite-prep.sh`. Both
  run-integration.sh and the CTest registration call them. The independent
  review of the move found that `--no-build` no longer reached the child
  process; the variable is exported now, and the runner self-test has a
  real-tree check for it with a control.
- **Registration at ctest time.** `cmake/AxlIntegrationTests.cmake.in` is
  configured into each DEBUG tree (not the lint or RTK trees) and listed in
  `TEST_INCLUDE_FILES`. On every ctest invocation it runs
  `lib/discover.sh --emit-ctest`, so a new `test-*.sh` appears with no
  reconfigure. Each test is `integration.<script>`, run through `run-one.sh`
  from the source root (`WORKING_DIRECTORY`). ctest's default cwd is the build
  tree's `test/integration`, and the first run failed `test-cmake-ccache`
  there, whose `cmake --preset` found no presets.
- **Environment knobs, read at ctest time:**
  - `AXL_IT_TIMEOUT` sets every test's TIMEOUT (default 900);
  - `AXL_IT_CACHE=0` turns the cache off;
  - `AXL_IT_DIR` registers a stub directory instead of the real one (the
    registration's own test runs a pass, a skip and a fail through it).
- **Measured, the whole X64 suite through ctest** (`-j10 --repeat
  until-pass:2`): **846 s wall against the runner's 856 s**, 302 passed, one
  corpus skip. COST=est starts the costliest first, as the runner's
  longest-first order does.
- **What the independent review of the registration found, now fixed:**
  - the registration's own test ran a stub ctest INSIDE real runs, sharing
    and emptying the real manifest. A stub run (`AXL_IT_DIR`) now has its
    own log directory;
  - the generated test list was rewritten in place under a concurrent
    ctest. It is now written to a temporary name and renamed;
  - ctest's TIMEOUT equalled the test's own limit, so ctest's kill came
    first and `run-one.sh` / `run-qemu.sh` never reported or cleaned up.
    TIMEOUT is now the limit + 30 s (later 3 x limit + 36 s, once the
    retry moved inside run-one -- step 6, H3);
  - the runner's per-run temp base had no equivalent. An
    `integration.tmp-setup` / `-cleanup` fixture pair (`lib/ctest-tmpbase.sh`)
    now owns `/dev/shm/axl-ct-<hash of the log directory>`, so a stub run's
    base is never a real run's. The re-review found that the first version,
    keyed on the build tree, let the registration test's nested ctest
    `rm -rf` the base of the run it was part of;
  - **one ctest run per tree.** A run's tests share the manifest, the logs
    and the build tree, so the setup fixture records the owning ctest and
    refuses a second live run. A base whose owner has exited (an
    interrupted run) is taken over, and cleanup removes only its own;
  - freshness is judged against a stamp taken BEFORE discovery runs, so a
    script edited during the 5-8 s generation is not missed;
  - a discovery that died part-way registered a short list. Its status is
    now checked, and a failure registers `integration.discovery-failed`.
- **The registration is cached.** `discover.sh` forks per header field,
  which cost 5-8 s on every ctest call, `ctest -N` included. The output is
  cached per log directory and regenerated when the set of scripts, any
  script, or `discover.sh` changes: **10.5 s cold, 0.02 s cached**. The
  test proves that a new script and an edited header each appear on the next
  run.
- **`integration.prep` builds every registered test's fixtures**, not only
  the selected tests'. ctest cannot hand a fixture its selection, and the
  build must happen up front, because concurrent ninja runs on one tree are
  unsafe. On a warm tree that is a no-op build, plus a restage when a test
  reads the stage (~22 s measured). For one test on a warm tree:
  `ctest -R <name> -FS integration.prep`.
- **Up front is not enough on its own: one build per tree at a time.** CI
  (2026-11-09) failed `test-stderr-row-qemu.sh` with "subcommand failed" a
  few seconds after a green prep. Two defects together: `src/pack` wrote its
  empty payload with `file(WRITE)`, so every configure gave it a new mtime,
  and `test-ctest-integration.sh` ran `cmake --preset` on the shared trees
  while the suite built in them -- which made the pack stub and every
  wrapped image (62 edges of `all`) stale, and three tests then rebuilt the
  same wraps at once (one renamed another's `.efi.tmp` away; one read a `.so`
  still being linked). Reproduced 3 of 3 runs on an 8-test subset. Fixed by
  `file(CONFIGURE)` (`test-cmake-reconfigure-noop.sh`: a reconfigure rewrites
  no build input), by that test bringing the trees up to date through
  `axl-build.sh ... build.ninja` instead, and by a per-tree `flock` in
  `scripts/axl-build.sh`, the one place every build goes through, which
  replaced the sshd and console-device suites' own locks
  (`test-axl-build-lock.sh`).
- **Both trees, run whole through ctest:** X64 846 s (runner 856 s), and
  AARCH64 775 s (runner 735 s), 139/139.
- **Still the runner's alone, for step 3:**
  - pooled runs under ctest (`RESOURCE_GROUPS` and the resource spec);
  - cache key files named per arch. Both trees share
    `test/integration/.test-cache`, and the key FILE is named by test only;
    the arch is inside the key, so it is safe but thrashes when both trees
    run.

**Step 3 result (2026-11-04): pooled runs and the per-arch cache under ctest.**

- **The guests are a CTest resource.** Each integration tree binds
  `CTEST_RESOURCE_SPEC_FILE` in the root CMakeLists (ctest reads it from the
  top level), so a plain `ctest` with no `--resource-spec-file` still bounds
  them. Measured on a scratch project first: a spec written by a
  `TEST_INCLUDE_FILES` script at ctest time is the one ctest enforces. So the
  include script rewrites it from `axlpool resource-spec` on every run, and a
  slot change needs no reconfigure. A spec that cannot be had is written empty
  but valid: a missing one would fail every test in the tree.
- **`AXL_IT_POOL`** (`1` for the user service's socket, a path, or `auto` --
  pooled only when the user service answers for the arch; else
  `AXL_IT_POOL_DEFAULT`, which the plain and suite presets set to `auto`;
  `docs/AXL-Test-Guest-Pool-Design.md` §6.7), read at
  ctest time, makes every `guest=` test take one slot of
  `guest_<class>_<arch>`, run pooled through `run-one.sh` (a miss reruns it
  booted), and require `integration.pool`. That fixture is
  `lib/pool-check.sh`, now shared with run-integration.sh `--pool`; it refuses
  a pool that is not answering or has no agent for the arch.
- **Measured, a real pooled ctest run** of the eight `guest=standard` tests
  against a private service: 0 misses, ~1 s per test warm (2.5-6 s booted).
  The first attempt missed every lease, because the service's `--state` sat
  under a long path and its QMP socket outgrew `sun_path`. That was a setup
  error, but it exercised the miss-then-boot path for real, and all eight
  passed booted.
- **The cache's FILES carry the arch** (`<test>@<arch>[@pool].key`), so the
  two trees, or an X64 then an AARCH64 run, no longer overwrite each other's
  keys. `run-qemu.sh` built `<test>.inputs` by hand, which had already missed
  the `@pool` file and would have missed the arch too: such a test recorded
  into a file nothing read, and was never skipped. It now calls
  `test-cache.sh`'s own `cache_record_input`.
- **The independent review found the spec written too late.** Two early exits
  of the include script (a bad `AXL_IT_TIMEOUT`, a failed discovery) came
  before it. On a fresh tree ctest then found no spec and ran **no test in
  the tree**: the unit tests, the gates, and the test registered to explain
  the refusal. It is now written first, atomically, and the registration test
  checks a fresh tree with a bad timeout.

**Step 4 result (2026-11-04): the unit binaries, one CTest test each.**

- **`unit.<App>`** is `test-axl.sh` with `TEST_APPS_ONLY=<App>`, booted: the
  same staging, networking and per-binary leak verdict as the one-boot run.
  The **ratchet** is the `unit.counts` fixture's cleanup,
  `lib/unit-ratchet.sh`, which sums what each binary wrote to
  `TEST_COUNT_FILE`:
  - it judges only a whole run; a partial one (`ctest -R` one binary) is its
    SKIP, never a pass;
  - only the x64 tree raises the one baseline; aa64 judges against it.
  The inventory guard (every built `AxlTest*.efi` is run or deliberately
  skipped), which a one-binary run bypasses, is `unit.inventory`
  (`test-axl.sh --check-inventory`).
- **The ratchet caught a real difference on its first run.** The per-binary
  sum was 14901 against the one-boot run's 14908: run alone, `AxlTestUtil`
  could not load `AxlTestRuntime.efi`, which its image tests read off the
  volume, and 13 assertions became a 6-count SKIP. Every binary is now
  STAGED and only the selected ones RUN, so a subset sees the one-boot
  volume. Both arches now sum to exactly 14908.
- **Measured:** X64 90 s and AARCH64 107 s wall through `ctest -j10`, against
  ~111 s / ~101 s for the one boot.
- **Not carried over, deliberately:** the one-boot run's wall-clock BUDGET
  (110 s). It measures one boot running everything, which no BOOTED
  per-binary run is (each time includes its own boot). It stays with
  `test-axl.sh`'s one-boot path, which step 5 keeps as verify's arch jobs
  without the pool. (Per-binary runs on POOLED guests are judged against it
  since 2026-11-07, summed over their own run times:
  `docs/AXL-Test-Guest-Pool-Design.md` §6.7, steps 3b-4.)
- **The balancer gap behind it, closed.** `AxlTestUtil`'s missing-runtime
  path printed a bare `SKIP:` with no declared count, so that topology gate
  shifted the ratchet. It now declares its 7 (`test_skip_n`). Run with
  `AxlTestRuntime.efi` absent, the binary counts 2258, the same as with it
  present.
- **What the independent review found, now fixed:**
  - a RED run could raise the baseline. Each binary now writes
    `<count> green|red` AFTER its leak and assert verdicts, and the ratchet
    judges every count but raises the bar only when all are green;
  - counts left by an earlier run could be summed with this one's when the
    reset fixture was skipped (`-FS`). The ratchet now consumes them;
  - an unreadable baseline passed unjudged. It is now a failure, and the
    baseline is written atomically, for an aa64 ratchet reading it
    concurrently;
  - the unit runs inherited the suite's forced `-delay 0` launcher. They now
    boot as the one-boot run does.
- **Owed:** the unit binaries on pool guests. The standard class has no
  echo-port guestfwds, which `AxlTestNet` needs, so they boot for now.
  (Done since: the `unit` class, 2026-11-07 --
  `docs/AXL-Test-Guest-Pool-Design.md` §6.7, steps 2-4.)

**Step 5 result (2026-11-04): verify.sh's other jobs are CTest tests (the x64
tree).**

- **`gate.make.<goal>`:** one test per make gate. The lists are cached
  against the Makefile, so a ctest call pays no make parse (`ctest -N`:
  0.17 s).
- **`RESOURCE_LOCK make-tree`** goes to every test that builds make's output
  tree:
  - the ARTIFACT gates;
  - `build.make-image-parity`;
  - `integration.prep`;
  - every integration script whose own build lines use make (read off the
    script; a false positive costs only serialization).

  make's `libaxl.a` recipe starts with `rm -f`, so two makes on one prefix
  race it. verify.sh avoided that by ORDER (makeimg ran after the builds),
  and `ctest -j` has no order. The independent review found the first
  version locking only the gates, so they could still race image-parity or a
  make-pinned test. The pure lint gates stay unlocked: the Makefile defines
  them as safe beside a build.
- **`gate.lint`** runs `scripts/lint.sh` and **`gate.docs`** runs
  `scripts/build-docs.sh`. They and the make gates run under `nice -n 15`, as
  verify.sh runs them beside the guests: unniced, the box ran at load 19-21
  and a guest hit its own hang timeout. The unit runs carry verify's
  concurrent `TEST_TIMEOUT_SCALE=2`.
- **`gate.qloop`** runs AxlTestLoop's timing cases alone on the box
  (RUN_SERIAL), with its UNJUDGED (exit 3) as the test's SKIP. It first
  called `verify.sh --only=qloop`, the one copy of the qloop verdict (marker,
  unconfirmed misses, the starvation witness); step 6 moved that verdict to
  `scripts/qloop-verdict.sh`, which `gate.qloop` and verify.sh's qloop job
  both run. Since 2026-11-07 it runs on a leased `unit` guest when the pool
  serves -- `gate.qloop` takes a `guest_unit_x64` slot, after the pool has
  settled -- so a pooled verify boots nothing
  (docs/AXL-Test-Guest-Pool-Design.md §6.7 step 5).
- The ctest-time script now declares `cmake_policy(VERSION 3.29)`. ctest
  evaluates it under OLD policies otherwise, and a list kept its empty
  elements (CMP0007).
- A qloop UNJUDGED or FAIL under verify.sh keeps its log directory
  (`/tmp/axl-verify.*`, its keep-on-failure rule). Under ctest the run's
  files are in the tree's `qloop-logs/` (overwritten by the next run), and
  the verdict line is in `Testing/Temporary/LastTest.log`: ctest prints a
  SKIPPED test's output nowhere, even with `--output-on-failure`.
- **Measured:** `ctest -L gate -j10`, 37 tests (the 31 make gates, lint,
  docs, qloop, the parity and no-ctors gates), all green in 178 s; qloop
  judged.
- **Moved to step 6:** `check-test-est` reading TIMEOUTs from
  `ctest --show-only=json-v1`. It grades CI's invocation of the suite, and
  CI switches to ctest in step 6.

**The whole tree, together (2026-11-04).** One `ctest --test-dir
build/x64-debug -j10 --repeat until-pass:2` ran **397/397 in 1040 s**:
integration, the unit binaries and their ratchet, every gate, qloop, the
parity checks and the fixtures, with the locks and nice levels in play. That
coverage used to take a runner run (856 s) plus a verify.sh run (~180 s),
back to back. **Test presets:**
- `x64-suite` / `aa64-suite`: the whole tree (an integration test retries
  once, inside run-one.sh; nothing else retries -- step 6, H3);
- `x64-ci`: CI's selection -- the integration tests only (CI's unit run and
  lint are their own steps and jobs), no `local-only`, the 240 s limit
  (ctest's own backstop TIMEOUT 3 x 240 + 36 = 756), no cache, JUnit to
  `ctest-junit.xml`;
- `x64-integration` (step 6): every integration test, local-only included --
  a local release run's suite step (`AXL_IT_CACHE=0`).

The runner's `nproc-2` worker default is not expressible in a preset; a
front end passes `-j` (step 6).

**Step 6 -- the decisions (taken by Mike 2026-11-05, as recommended: 3, then
2, then 1):**
1. Whether run-integration.sh and verify.sh stay as front ends that keep
   their flags (`--only`, `--for`, `--apps`, the verify table, UNJUDGED),
   translating to `ctest -L/-R/-E` -- or are deleted, with `ctest --preset`
   as the interface. The flags are pinned by the runner and gate-scoping
   self-tests and documented in CLAUDE.md, so either way it is a large,
   user-facing change.
2. CI's integration job switching to `ctest --preset x64-ci`, which also
   moves `check-test-est` onto `ctest --show-only=json-v1`; and
   release-suite.sh's integration step with it.
3. Where the qloop verdict lives once verify.sh's engine goes (its own
   script). **Done:** `scripts/qloop-verdict.sh`.

Decided for 1: both scripts STAY as thin front ends that keep their flags,
and lose their engines (scheduler, cache decision, job runner) to CTest; a
whole-tree run is `ctest --preset x64-suite` directly. A SKIPPED `gate.qloop`
(UNJUDGED) must read as not green (verify's exit 3) in the front end, since
ctest's own exit is 0. Decided for 2: switch, after one dispatched CI run in
which the runner and the preset agree on every test.

**Step 6, decision 3 result (2026-11-05):** `scripts/qloop-verdict.sh` (commit
6a4f59b0). Under ctest its files, and its verdict line as
`qloop-verdict.txt`, are in the tree's `qloop-logs/`: a skipped test's output
otherwise reaches only `Testing/Temporary/LastTest.log`, which EVERY ctest
invocation on the tree rewrites -- measured: a `--show-only` listing does too,
and ctest has no flag against it.

**Step 6, decision 2 result (2026-11-06): CI's suite is `ctest --preset
x64-ci`.**
- `release-suite.sh --ci` runs `check-test-est.py && ctest --preset x64-ci`
  on nproc-2 workers. A local release run runs `ctest --preset
  x64-integration` (new: every integration test, local-only included) with
  `AXL_IT_CACHE=0`.
- `x64-ci` selects the integration tests only. CI's unit run and lint are
  their own steps and jobs.
- **`check-test-est` reads the selection and the limit from
  `ctest --preset x64-ci --show-only=json-v1`** (`scripts/check-test-est.py`,
  replacing `run-integration.sh --check-est`). It keeps no copy of either. It
  never configures CMake, since it is a lint gate run beside builds and a
  second configure of one tree races the first. It writes no JUnit file
  (`--output-junit /dev/null`), because inside a running ctest that would
  replace the outer run's. A ctest call to the preset must come after the
  gate on the same line: the runner's `--ci` refused an over-limit est at
  run time, and ctest refuses nothing.
- **The prep skips local-only fixtures under x64-ci** (`AXL_PREP_SKIP_LOCAL=1`
  in the preset). CTest hands the prep every registered test, so CI built the
  fixtures of tests it never runs, and one of them breaking would have failed
  the whole run.
- **A retry keeps the first attempt's log.** Under `--repeat until-pass` a
  retry is a second call writing the same log. `run-one.sh` kept a log newer
  than the run's temp base as `<name>.attempt1.log`. (Superseded by H3: the
  retry is run-one's own `--retry` again, which keeps that log itself.)
- **Scripts that register a stub directory take a lock** (`RESOURCE_LOCK
  it-stub`). Every stub run shares one registration cache, so two of them
  side by side rewrote each other's registration.
- **The comparison, on one tree back to back** (CI runs on this box): the
  runner's `--ci` passed 289/289 in 751 s. `ctest --preset x64-ci` passed
  291/292 in 764 s, plus the prep and the temp-base pair. The one failure
  was real: the preset's environment (`AXL_IT_CACHE=0`) reaches every test,
  and `test-release-suite.sh` recorded what it inherited. That test now
  unsets it. No other test differed.
- The runner kept its `--ci` and its own `CI_TIMEOUT` until decision 1 made
  it a ctest front end; it now reads the preset's `AXL_IT_TIMEOUT` and keeps
  no copy (test-runner-selftest.sh). test-check-test-est.sh's check that
  the copy equalled the preset outlived the copy, and failed the first
  full CI run after it (42312375209).

**Step 6, decision 1 -- the CONTRACT (2026-11-06, proposed; review before
code).** Both scripts stay and keep their flags. Each loses its engine to
CTest and becomes a translation to one `ctest` call per build tree, plus a
report read from that call's JUnit file.

*What depends on them* (a census, 2026-11-06): nothing in a workflow, the
Makefile or the release path runs either one any more -- release-suite.sh
and CI run ctest. Their callers are their own self-tests, one nested runner
call in `test-run-qemu-flags.sh`, and `profile-run.sh`, which reads the
runner's `logs:` line and per-test lines. That narrows the contract to: the
flags and their refusals, a few output lines, the log-directory layout, and
the exit codes.

`test/integration/run-integration.sh`:
- **Selection is unchanged**, still lib/discover.sh: `--arch`, `--only`,
  `--for` (which-gates), `--ci` (drops local-only), `--only-local`,
  `--shard i/K`. `--list` prints the selection as absolute paths and runs
  nothing; gate-scoping 3c2/3d depend on that format.
- **Execution is ONE `ctest --test-dir build/<arch>-debug -R
  '^integration\.(<name>|...)$' -j N --repeat until-pass:2`** (superseded by
  the review below: no `--repeat`, and `--tests-from-file`). CTest adds the
  prep and the temp-base fixtures. The flags map to:

  | runner flag | CTest |
  |---|---|
  | `-jN` (default nproc-2) | `-j N`, with `--test-load` per the measurement below (superseded 2026-11-07: `-j` is a CPU budget each test draws its `cpu=` from as `PROCESSORS`, no `--test-load` -- see below) |
  | `--timeout N` | `AXL_IT_TIMEOUT=N` |
  | `--ci` | the `x64-ci` preset's environment, READ from CMakePresets.json (jq), not copied: `CI_TIMEOUT` and its pinning check go |
  | `--no-cache` | `AXL_IT_CACHE=0` |
  | `--pool[=SOCK]` | `AXL_IT_POOL=1|SOCK`, after lib/pool-check.sh as now |
  | (no pool flag) | `AXL_IT_POOL=auto` when the user service serves the arch (added 2026-11-06) |
  | `--no-pool` | booted (added 2026-11-06; `--ci` booted too until 2026-11-07, when CI joined the pool) |
  | `--no-build` | `-FS integration.prep` |
  | `--logdir DIR` | the run's logs are COPIED there at the end. Default: none, `logs:` names the tree's `integration-logs/` |

  The `--ci` est refusal stays: the front end runs `check-test-est.py` first.
- **Output is kept**:
  - one `  <name>.sh PASS|FAIL|TIMEOUT|SKIP|CACHED <n>s` line per test, from
    the JUnit file (CACHED from `_cached.txt`);
  - the totals line `integration: N passed, M failed[, K SKIPPED] (<ARCH>)`;
  - `logs: <dir>`;
  - the `PARTIAL --` banners and the "does not certify a release" line.
- **Kept refusals**: everything that exits 2 today, by its exact message.
  This includes the too-long `AXL_QEMU_TMPDIR` check, which must still come
  first and print nothing else (`test-run-qemu-flags.sh`).
- **Retired with the engine**: the worker pool, cache decision, retry loop,
  SIGTERM reaper, port-base spread and tally files. Each runner self-test
  block that pins one is MOVED to a ctest-level test where CTest now owns the
  behaviour -- retry/attempt-1 logs and pool miss/rerun are already in
  test-ctest-integration -- or deleted with a reason recorded. One stays:
  SIGTERM to the front end must leave no QEMU behind (it reaches ctest, which
  kills its tests).
- **`--print-ctest`** (new): print the ctest command and environment, and
  run nothing. The self-tests pin the translation through it, exact strings,
  with no QEMU; the old stub-engine runs cost minutes.

`scripts/verify.sh`:
- **Jobs become selections over two trees**, one ctest per tree, the two run
  concurrently:

  | job | tree | selection |
  |---|---|---|
  | x64 | x64-debug | label `unit` (the binaries, ratchet, inventory) |
  | aa64 | aa64-debug | label `unit` |
  | lint | x64-debug | `gate.lint` |
  | make | x64-debug | the `gate.make.*` lint gates |
  | makeimg | x64-debug | the artifact gates |
  | docs | x64-debug | `gate.docs` |
  | qloop | x64-debug | `gate.qloop` (RUN_SERIAL: it runs alone) |

  `--only`, `--no-docs` and `--apps` (as `-R '^unit\.(A|B)$'`, validated
  against `test-axl.sh --list-apps` as now) narrow these.
- **The table is kept** (`JOB RC TIME DETAIL`):
  - RC: 0 when every selected test passed, UNJ when `gate.qloop` was skipped,
    1 otherwise.
  - TIME: the job's longest test -- the leg you waited for.
  - DETAIL, from the JUnit file and the tests' own output:
    - the unit ratchet's `unit ratchet: N assertions ...` line;
    - `lint: clean ...`;
    - `N gate(s) clean`;
    - qloop's `qloop-logs/qloop-verdict.txt`;
    - on a failure, the failing tests' names.
  - `ALL GREEN` / `NOT RUN:` / `NOT GREEN: UNJUDGED` / exit 3, as now.
- **Cross-arch equality**: compares the two trees' ratchet totals. Under
  `--apps` the ratchet reports PARTIAL (77), so unit-ratchet.sh gains a
  per-binary count line in that case, and the subset comparison keeps
  working.
- **What verify used to hand its child jobs is now in the registration**:
  `nice` on gates and `TEST_TIMEOUT_SCALE=2` on units are already properties
  pinned by test-ctest-integration. The gate-scoping blocks that pinned that
  environment (1b2, 1b3) are therefore retired, not moved.
- **`--print-ctest`**, as for the runner. The qloop table rows (1g-1j) are
  pinned by rendering fixture JUnit files and verdict files through the
  report helper -- the stub `test-axl.sh` they drove goes with the engine.
- **One report helper for both**: `scripts/ctest-report.py` renders the
  runner's per-test and totals lines and verify's table from a JUnit file.
  It is pure, so it is tested on fixtures.

**The sub-decision (DECIDED by Mike 2026-11-06: per-binary, budget dropped
in verify -- then REVERSED the same day on measurement, to one boot per
arch; see "decision 1 -- verify" below):** verify's x64/aa64 jobs become
per-binary CTest runs, as `x64-suite` already is. The one-boot run's
wall-clock BUDGET (`TEST_MAX_DURATION`, 110 s) then has nothing to measure in
verify; CI's release-suite step 1 still runs the one boot and keeps it.
Decided: verify drops the budget, and the unit ratchet cleanup reports the
summed per-binary time as information, not a gate. The alternative kept
verify's arch jobs on `test-axl.sh`'s one boot -- the budget stays, verify
keeps a second engine, and it is ~20 s slower on x64 (aa64 is the reverse:
107 s per-binary against 101 s for the one boot).

**Wall clock, measured as we go.** `-j16 --test-load 10` for x64-ci against
`-j10`'s 764 s: 654 s with no retries (2026-11-06; `-j24 --test-load 11`
643 s with two contention retries), so the front ends took `-j nproc+4
--test-load nproc-2`.

**Superseded 2026-11-07: a per-test CPU budget replaces the load gate**
(`docs/AXL-CI-Release-Speed-Design.md` §15 has the evidence and the sweep).
ctest's `--test-load` reads the one-minute load average, which lags a burst
by about a minute -- after each one the gate stayed shut while workers idled,
3,165-4,016 idle worker-seconds per CI run -- and it counts every process on
the box, so an unrelated build slowed CI. Now:
- a `cpu=<n>` test-meta field (default 1, 1-8, held by `check-test-meta`)
  becomes the registration's `PROCESSORS`, which ctest charges against `-j`;
  it is set from measured CPU-seconds over wall for the tests that need more
  than one CPU;
- `-j` is `suite_cpu_slots` (`lib/discover.sh`) in `release-suite.sh` and
  `run-integration.sh`, with no `--test-load`. verify keeps its gate (its two
  ctest processes cannot share a `PROCESSORS` budget; `scripts/verify.sh`);
- `COST` is est= only for a test the tree's `CTestCostData.txt` has not
  measured: ctest ignores its own average for a test with a `COST` property
  (and folds the `COST` into the average it writes back), so the
  registration reads the file at ctest time (`AXL_IT_COSTED`) and leaves
  measured tests to ctest. `release-suite.sh` keeps the file across CI's
  fresh checkouts, per preset, under `~/.cache/axl-ci/`, and seeds every
  run from that store (a developer tree's own file drifts low: ctest
  rewrites it after `-j1` runs and cache hits too).

**The contract's independent review (2026-11-06), and what it changed.**

The review found four HIGH defects:
- **H1. qloop is a THIRD ctest call, made after both trees' calls return.**
  RUN_SERIAL serialises within one ctest process only, so the aa64 tree
  would keep booting guests while qloop judges timing.
- **H2. `--no-build` is `RUN_NO_BUILD=1` in ctest's environment, not
  `-FS integration.prep`.** Excluding the prep leaves the previous run's
  `_prebuilt` manifest in place. axl-build.sh matches that manifest by
  fingerprint, not by source content, so a test would skip its own build of
  an edited source and pass against a stale binary. The same hazard applies
  to the "one test on a warm tree: `-FS integration.prep`" advice above.
- **H3. Retries move back into run-one.sh (DECIDED by Mike 2026-11-06,
  reversing `--repeat until-pass`).** CTest repeats EVERY test, the fixture
  cleanups and gates included:
  - the unit ratchet's attempt 1 consumes the counts and fails on a drop,
    and the retry then finds none, reports PARTIAL (77), and ctest exits 0,
    so the failure is erased;
  - a judged qloop FAIL can be retried into a pass.

  CTest has no per-test exemption. Integration tests now run through
  `run-one.sh --retry` (attempt-1 log, 3 s drain, the "(retry; attempt 1
  failed)" line), and the presets and front ends pass no `--repeat`. DONE.
  Its review found the consequence: ctest's TIMEOUT (limit + 30) then
  covered both attempts and killed a slow or hung test mid-retry. It is now
  a backstop at 3 x limit + 36 (a pooled attempt, its booted rerun, the
  retry, the drain). A retry can take up to twice what `check-test-est`
  grades -- the per-attempt est is still the bound that matters.
- **H4. Each tree's prep builds only what the run selected.** Two trees'
  preps running at once race make across processes (`RESOURCE_LOCK` is per
  ctest process; `libaxl.a`'s recipe starts with `rm -f`). A unit-only run
  also paid for every integration fixture plus a restage.

  So the front end hands the prep its selection through ctest's environment
  -- the earlier "ctest cannot hand a fixture its selection" is true of a
  bare ctest, not of a front end. The selection is `AXL_PREP_TESTS` (the
  selected scripts), and a run with no integration test builds none.

The MEDIUM and LOW findings, all adopted. They are the plan for the
front ends, and none is built yet unless marked done:
- **Per-test lines** come from run-one's own line in each test's output,
  with JUnit's status as a cross-check (a disagreement is a FAIL). JUnit
  cannot tell run-one's TIMEOUT from a FAIL, or carry a SKIP reason.
  `--test-output-size-passed/failed` stop ctest truncating a passing test's
  output to ~1 KB.
- **Classification:**
  - UNJ only when `gate.qloop` was skipped with its SKIP code AND
    `qloop-verdict.txt` is newer than this run's start;
  - every other not-run is a FAIL, with its reason named;
  - a failed prep or temp-base setup prints that cause, not
    "0 passed, 204 failed".
- **One run per tree:** the front end checks the temp base's owner first,
  and refuses (exit 2) instead of reporting every test Not Run.
- **SIGTERM:** spiked before it is claimed. The front end runs ctest in the
  background, forwards the signal, and reaps by the `axl-ct-*` base. The
  sweepers that match only `axl-itest.*` (run-selftests.sh, run-qemu.sh)
  learn `axl-ct-*`, each with a control showing it sees one.
- **Logs:** the include honours `AXL_IT_LOGDIR` for the tests' logs and the
  prep (the registration cache stays where it is). So a run's logs default
  to a fresh `mktemp -d` again, profile-run.sh's `logs:` / `--last` /
  per-test lines keep working, and `--logdir` names that directory instead
  of copying into it.
- **`--ci`** is `ctest --preset x64-ci` plus the selection, so CTest
  resolves the preset's inheritance; `--ci --arch AARCH64` is refused (there
  is no aa64-ci preset). The est refusal is `check-test-est.py`, as in CI.
- **`AXL_QEMU_TMPDIR`:** the include honours a caller's value as the run's
  temp base, keeping the too-long refusal (exit 1) meaningful.
- **Empty is not an answer:** `--no-tests=error`; refuse an unconfigured
  tree; and the JUnit test set must contain every requested name, which
  catches `discovery-failed` and a stale registration.
- **Selection by file:** `--tests-from-file` (CTest 3.29, the project's
  minimum) instead of a 10 KB `-R` alternation. Names need no escaping, and
  there is no regex-size ceiling.
- **verify:**
  - TIME is each ctest call's wall time plus the job's summed test-seconds,
    each labelled;
  - the two trees split `-j` (sum ≤ nproc-2, measured);
  - makeimg is selected by an `artifact` label;
  - the front end keeps its own JUnit and logs in a kept `mktemp` directory
    on failure;
  - progress is streamed, and the table rendered at the end.
- **Cross-arch:** unit-ratchet.sh prints one machine-readable `unit-counts:
  App=N ...` line in every branch, so the comparison names the binary that
  diverged, under `--apps` too. `--apps` keeps `unit.inventory`.
- **Tests:**
  - the front ends build ONE argv, printed by `--print-ctest` and executed
    otherwise;
  - the self-tests assert `ctest <that argv> --show-only=json-v1` selects
    exactly the intended set, fixtures included, with no QEMU;
  - JUnit fixtures are captured from real stub ctest runs;
  - one end-to-end stub run per front end through `AXL_IT_DIR`.

**Step 6, decision 1 -- the runner (2026-11-06).** `run-integration.sh` is a
front end: its selection and refusals are unchanged, and the run is one
`ctest` call rendered by `scripts/ctest-report.py`.

Departures from the contract above, each for a reason:
- **`--ci` is not `ctest --preset x64-ci`.** A preset's `environment`
  overrides the caller's, so `--ci --timeout N` could not win. The front end
  reads the preset's `AXL_IT_TIMEOUT` with jq (refusing if unreadable) and
  turns the cache off. The preset's `AXL_PREP_SKIP_LOCAL` is unnecessary
  here, because the selection already excludes local-only tests and
  `AXL_PREP_TESTS` confines the prep to it.
- **`scripts/run-qemu.sh`'s stale-base sweep does not learn `axl-ct-*`.** It
  `rmdir`s an EMPTY base older than ten minutes. A live ctest run's base can
  be empty between guests, and the sweep would then remove it from under
  the run. A dead run's base is bounded instead: it is one per tree, and
  that tree's next run takes it over (tmp-setup empties and reuses it).
- **The stray detector in `scripts/run-selftests.sh`** reports only what
  appeared DURING the self-tests: a snapshot by name and inode before, a
  difference after, covering both `axl-itest.*` and `axl-ct-*`. CI runs on
  the same box, so a plain glob would fail on another tree's base. It plants
  a probe base between two snapshots every run, and stops if it cannot see
  it.

What the runner's own review found and fixed:
- it now clears an inherited `AXL_IT_*` / `AXL_PREP_*` / `AXL_ONE_*` /
  `AXL_TEST_CACHE`, which overrode `--no-cache` or turned the pool on;
- it reaps and removes only when THIS run owned the tree, judged by the
  owner record or by its own setup fixture having run;
- it traps SIGHUP, plus an EXIT trap while ctest runs;
- the session kill (`pkill -s`) reaches tests that GNU `timeout` moved into
  groups of their own;
- the one-run check comes before a reconfigure;
- `--logdir` and the other paths are made absolute;
- a stale JUnit file is removed before the run;
- an empty selection runs nothing;
- a ctest failure with a clean report is said;
- a stub directory is discovered by the registration's rules
  (`AXL_DISCOVER_DIR`);
- failure excerpts are shown inline again;
- a configured registration is touched after a reconfigure, so a run does
  not reconfigure every time.

**Step 6, decision 1 -- verify (2026-11-06).** `scripts/verify.sh` is a front
end: the same flags (`--only`, `--no-docs`, `--apps`, `--lint-files`'s
refusal) and `--print-ctest`, one ctest call per tree -- x64 and aa64
concurrently, each on half of nproc+4 under `--test-load` nproc-2 -- then
qloop alone at `-j 1`, and the table rendered by `ctest-report.py verify`.

- **The per-binary sub-decision was measured and reversed.** A per-binary
  verify ran in **357 s** against the old verify's 162 s: x64 238 s, aa64
  308 s, because 49 guest boots per arch cost more than the one guest that
  runs all 49, and on aa64 every boot is TCG. Mike chose one boot per arch:
  - `unit.one-boot` is registered in each tree (label `unit-boot`):
    `test-axl.sh` with every binary in ONE guest. On x64 its ratchet (which
    writes `.last-pass-count`) and its wall-clock budget, as before; on aa64
    `TEST_SKIP_RATCHET=1`, which skips both there, as the old verify did --
    the cross-arch check is what compares aa64's count;
  - its scales are the FRONT END's, read at run time
    (`AXL_UNIT_TIMEOUT_SCALE` / `AXL_UNIT_BUDGET_SCALE`): strict by default,
    so a lone `ctest -R unit.one-boot` judges the real 110 s budget; verify
    declares them by the rules it always had (timeout x2 beside any other
    job, budget x1.40 only beside lint/make/docs);
  - verify's x64/aa64 jobs select it alone; `--apps` keeps the per-binary
    tests (plus `unit.inventory`, the ratchet and its reset as fixtures);
  - `x64-suite` / `aa64-suite` exclude it, so a whole-tree run does not run
    the unit binaries twice; `ctest -R unit.AxlTestFoo` still runs one.
- **The x64/aa64 DETAIL is the one-boot run's `Results:` line, and the
  cross-arch check compares its passed + skipped assertions** -- the old
  verify's figure and reading. Under `--apps` they are the per-binary rows and
  the ratchet's `unit-counts:` maps, as the contract said.
- **ctest keeps the HEAD of a test's output.** The one-boot run prints
  ~775 KB, its `Results:` line last; at verify's old 64 KiB limit the JUnit
  file held none of it. verify raises the limit to 8 MiB, and the report
  helper FAILS a passing one-boot run whose `Results:` line it cannot see
  -- a truncation must not read as green.
- **unit-ratchet.sh prints `unit-counts:` LAST** in every branch (PARTIAL,
  FAIL-below, green), which is what the `--apps` cross-arch reads.
- **The `artifact` label** marks makeimg's gates; make selects the rest.
- **The make gates are ordered after `integration.prep`** (`DEPENDS`: order
  only -- it pulls nothing in and needs no success). Unordered, ctest started
  eight gates at t=0, their compiles took the load past `--test-load`, and the
  1.5 s prep the one-boot run needs waited ~60 s for a start: the x64 tree
  took 195 s. Ordered, prep finishes second and the tree takes 116 s.
- **Each run's wall time ends when THAT run ends** (`cts_wait_any`, `wait -n
  -p`). Waiting in a fixed order timed aa64's 119 s run to x64's 195 s end.
- **Measured (2026-11-06, idle box): 173 s**, against 357 s per-binary and
  the old engine's 162 s. x64 116 s and aa64 123 s run side by side (aa64's
  120 s guest is the floor), then qloop alone, 38-48 s: each of its
  attempts is ~10 s, and right after the trees it can come out UNJUDGED
  (exit 3) where a quiet re-run judges it -- the quiet slot doing its job.
- **What the two reviews of the verify front end changed (2026-11-06):**
  - ctest's OWN exit status is cross-checked (`ctest-report.py verify
    --ctest-rc RUN=RC`): a fixture the table never shows -- the temp base's
    cleanup -- can fail with every row green, and a status never collected
    is not a pass. The runner already did this;
  - an interrupt is ONE library function for both front ends,
    `cts_take_down`: ownership asked of the owner record before the kill,
    then the kill, then -- for an owned run only -- the reap and the removal
    of the temp base (never a FOREIGN one, as the registration's tmp-setup
    says, `--foreign`) and the owner record. `cts_own` is called before
    `cts_start`, so a signal right after a start already knows what to
    clean. The post-run reap is shared too (`cts_reap_after`);
  - `cts_wait_any` collects a run that ENDED before the wait with a plain
    `wait`: once another job starts, bash drops an exited one from its job
    table and `wait -n` says "no such job" -- the run's status was lost, and
    verify died on an unset RC with no table;
  - the scrub is one list (`cts_inherited`): `AXL_IT_/PREP_/ONE_/UNIT_*`,
    `AXL_TEST_CACHE`, and `TEST_APPS_ONLY` / `TEST_SKIP_RATCHET`, which in a
    shell turned a "full" unit job into a subset;
  - `--apps` refuses a name the TREE does not register (a binary added since
    configure), and its ratchet never writes the baseline
    (`AXL_UNIT_NO_WRITE=1`) even when it names every binary;
  - makeimg counts in both scale rules: it now builds beside the guests;
  - the plain test presets exclude `unit-boot` (the `base` test preset), so
    one tree run never runs the unit suite twice; a bare `ctest --test-dir`
    with no label filter still does;
  - the qloop-verdict cases the old engine's self-test pinned are pinned on
    `qloop-verdict.sh` directly (gate-scoping 1k2) -- the first port had
    dropped them, leaving only the report's rendering tested;
  - `$OUT` is kept only once a run starts or a configure fails: 1630
    `/tmp/axl-verify.*` had piled up, almost all from the old engine.
- **`systemd-analyze --user verify` binds `$XDG_RUNTIME_DIR/systemd/private`.**
  Run against the real runtime directory (test-pool-units.sh before
  a186771a), it replaced the live user manager's private socket and exited,
  leaving `systemctl --user` refused host-wide until a daemon re-exec. The
  test points it at a directory of its own.

### 10.9 Slice 5b -- the release builds with CMake; CPack measured and NOT adopted (2026-11-08) -- REVERSED 2026-11-10

> **Reversed 2026-11-10, and why.** The "not adopted" below was a
> recommendation made without asking the owner, and it narrowed a plan he
> had set (§10 item 4: "CPack builds the release tarballs"). He asked for the
> plan to be followed, and the main blocker it recorded -- archives owned by
> the builder's uid/gid -- is gone in CMake 4.3 (`CPACK_ARCHIVE_UID`/`GID`,
> policy CMP0206), to which the pin moved (§10.13). What 4.3 did NOT remove,
> measured on 4.3.6: CPack still takes the user and group NAMES from the
> builder (`cmCPackArchiveGenerator` sets only the numeric owner, and
> `cmArchiveWrite` sets names only when given one; there is no
> `CPACK_ARCHIVE_UNAME`), so the same build as `mgosha` and as `runner` gave
> different archives -- and `tar x` as root restores ownership by name. The
> owner first chose to run `cpack` under **fakeroot** (superseded the same
> day by §10.18: the External generator) (every entry then reads
> `0/0 root/root` on any builder) and to propose the missing variables
> upstream (§10.17). The other points below were answered, not waived: entry
> order follows the install manifest, which is fixed for fixed rules, so it
> is deterministic if not sorted; `SHA256SUMS` stays ours
> (`scripts/release-sums.sh`), since `CPACK_PACKAGE_CHECKSUM` writes one file
> per archive; and the "LESS reproducible than today's" worry is now
> measured the other way -- `test-release-reproducible.sh` builds each
> archive as two uids under two umasks and requires the same bytes, which
> found that CPack also creates staged directories with the BUILDER's umask
> (fixed: the driver packs at 022). Where it landed: §10.16 (install rules)
> and §10.17 (CPack).

What a release ships is now built by CMake: `release.yml`'s tools job runs
`scripts/axl-build.sh ARCH=<a> BUILD=RELEASE tools`, and `install.sh` (the SDK
tarball's staging) builds the new `sdk` goal through the same wrapper and
copies from the tree `axl-build.sh --print-prefix` names. Nothing on the
release path runs `make` any more except `cut-release.sh`'s `make
check-version` (the gate port, slice 5c).

**The two open questions §10 item 4 left for this slice, settled by
measurement:**

1. *Reproducible archives from CPack on the pinned CMake* -- **no.** A TGZ
   from CPack 3.31.8 (this box; the minimum is 3.29) honours
   `SOURCE_DATE_EPOCH` for mtimes, but records the BUILDER's uid/gid and user
   names (`mgosha/mgosha` here, `runner` on CI): two builders, two archives.
   Owner 0/0 is the default only from CMake **4.3** (policy CMP0206). Entries
   follow the install manifest's order, not names (an `install(FILES b a)`
   and an `install(FILES a b)` gave different bytes). And
   `CPACK_PACKAGE_CHECKSUM` writes one `<pkg>.sha256` per archive, not the
   `SHA256SUMS` that `release.yml` publishes and `packaging/install.sh`
   verifies against. The three tarball scripts already write sorted, 0/0,
   commit-mtime archives -- byte-reproducible today.
2. *Both arches into one staging prefix* -- already the design: one
   `install.sh` run stages both arches' trees into one prefix, and the SDK
   tarball archives that once. Under CMake the two trees are the two RELEASE
   presets.

**Decision (mine to recommend, the owner's to confirm, since it narrows §10
item 4's "CPack builds the tarballs", which was "if it fits"):** the archives
stay on the three scripts' `tar` line, and CPack is not adopted. CPack would
also need `install()` rules for everything `install.sh` stages -- including
the generated `axl-config.cmake` / `.pc` files with their toolchain
substitutions and the `write_if_changed` mtime discipline -- to produce
archives that are LESS reproducible than today's. Revisit if the minimum
CMake rises to 4.3. For the same reason `install.sh` keeps its copy step
rather than becoming `cmake --install`: the copy is backend-neutral, which is
what lets the parity check below build the SAME staging from either tree
(§10's "or the documented equivalent").

**What CMake lacked, found by staging from it:** `libaxl-standin.a` (built by
no image, so never declared), `axl-cxxrt-nothrow.o` and the six pack-stub
objects `axl-cc --compress` links (`lib/axl/<arch>/pack/`) were not published
in the tree. All three now are (`src/CMakeLists.txt`,
`cmake/AxlImages.cmake`), and every goal depends on them, as every make goal
depends on `all`.

**The named lists moved out of the Makefile.** `TOOL_NAMES`,
`TOOL_EXTRA_APPS`, `SHIPPED_TOOL_NAMES` and `HOST_TOOL_FILES` live in
`scripts/axl-lists.conf`, read by `scripts/axl-list.sh NAME` (one line per
name, as `make -s print-NAME` printed), by `tools/CMakeLists.txt`
(`axl_list()`, at configure time) and by the Makefile (`include`) -- one copy
where CMake had a second. Its grammar is a strict subset all three read the
same way, and the reader refuses anything else rather than guessing
(`test-axl-lists.sh`, sabotage-checked). Every packaging script, gate and test
that asked make for a list asks it now.

**The parity check: `scripts/release-parity.sh`** builds each backend's
RELEASE tree, makes every release tarball from each with the scripts
`release.yml` runs, and compares each extracted pair with
`scripts/compare-release-trees.py`: the same files, modes and bytes, except
that an ELF relocatable object -- or an ar archive of them -- may differ in
DEBUG INFO and in member suffixes (`x.o` / `x.c.obj`), judged section by
section and member by member IN ORDER. CI's build job runs it per arch,
replacing the "build what release.yml ships (make)" step. Measured, both
arches: SDK 339 files, 307 identical,
32 differing only in debug info (every shipped object and archive: make
records the builder's absolute directory, CMake `build/<preset>`);
UEFI-tools 58 of 58 identical per arch; the host-tools archive
byte-identical (a determinism check, not backend parity: nothing in it is
built). ~90 s warm.
- **The two sides are proven to be two builds**, or the check would compare
  a tree with itself and pass: the backends must name different RELEASE
  trees, each UEFI-tools tarball must say it staged from its backend's
  (`make-uefi-tools-tarball: staging from <tree>`), and at least one SDK
  object must differ in debug info.
- **Review fixes (independent review, 2026-11-08):** the Makefile's
  `include` of the lists file broke `make -f <probe>` (check-dep-tracking) --
  the Makefile's own directory is now captured before any include; the
  release-image test's `size-gate-paths=` covers the lists file; the
  comparator compares every section-header field but `sh_offset` (a `.bss`
  size lives only there) and the ELF header; `axl-list.sh` refuses globs and
  trailing comments instead of expanding or keeping them; a `--keep`
  directory is emptied first; `install.sh` fails on a missing C++ glue object
  and prints a failed build in full; VERSION against `axl-version.h` is a
  CMake configure check now -- make's `all` enforced it, and the CMake
  release path had lost it. `test-efi-pack-release-qemu.sh` boots CMake's
  RELEASE tarball, as `release.yml` now makes it -- and its
  `size-gate-paths=` now names the CMake build files (`cmake/`,
  `CMakeLists.txt`, `CMakePresets.json`, `tools/CMakeLists.txt`), which
  which-gates routes a size change by. Owed with the gate port:
  `size-gate-inputs.sh` still derives the image's inputs from MAKE's graph,
  for an image CMake now builds; the two graphs share every source (parity),
  so only a build-definition file could slip, and those are now named.
- **Parity runs on CI's CMake (ubuntu:26.04: 4.2.3), the release on the
  runner image's (3.31.6)** -- recorded, not closed: the release step logs
  its versions, and pinning one CMake for both is the owner's call (it
  would mean installing CMake in release.yml rather than using the image's).
  **Closed in slice 6 (§10.12, decision 4):** one pinned 4.2.x for both.

- **Controls.** `test-compare-release-trees.sh` builds real objects with two
  `-fdebug-prefix-map`s (the latitude), then breaks one thing per case -- a
  file on one side, a mode, code in an object, code in an archive member, the
  member ORDER, a non-ELF byte, an empty comparison -- each asserted by its
  exact line. Sabotaging the comparator (debug latitude for every section,
  order ignored, the empty-tree refusal) fails it. One real sabotage: CMake's
  `libaxl-standin.a` with one member dropped fails `release-parity.sh`.
- **Consumer-visible, and judged harmless:** the SDK's objects now say
  `DW_AT_comp_dir build/<arch>-release` where make's carried the builder's
  absolute path, and `libaxl.a`'s members are named `*.c.obj`, visible to
  `ar t` and in a consumer's link map. No linked image changes: the parity
  check compares every section the linker reads. The full suite found the
  one place that read a member name: test-axl-cc-flags.sh's link-map grep
  for `libaxl.a(axl-log.o)`, which now accepts either spelling. Giving
  CMake's members make's names would mean renaming every object in the tree
  (`CMAKE_<LANG>_OUTPUT_EXTENSION_REPLACE`), which the parity and lint
  tooling key on -- not worth it for a map file's spelling.
- **release.yml used the runner image's CMake (3.31.6) and Ninja (1.13.2)**
  (superseded by §10.12, decision 4: the pinned CMake).
  apt's cmake on 24.04 is 3.28, under the minimum; the configure refuses an
  old one loudly. CI's build job runs in `ubuntu:26.04` (§10.8) and already
  installs both.

### 10.10 The two owed tests, ported (2026-11-08)

§10.5 named two things owed before the Makefile goes. Both are done.

- **`test-cmake-wrap.sh` -- the CMake wrap's build behaviour.** It reads
  the wrap step for `tools/sshd.efi` off the graph (`ninja -t commands`) and
  replays it with only RAW / OUT / STATE (and PE_SET_DEBUG for the failure
  case) pointed at private copies, so every other argument is the build's
  and the shared tree is never toggled. AXL_PACK on wraps; off copies the raw
  bytes; re-enabled re-wraps byte-identically; a pe-set-debug that fails
  fails the step, leaves the previous image untouched and older than its raw
  input, and the next run re-wraps it identically. The toggle reaching the
  step: a private tree configured with `-DAXL_PACK=OFF` (configure only,
  ~3 s) gives the same edge the same command but `-DMODE=off`, and Ninja
  reruns an edge whose command changed. 14 assertions, ~3 s. Sabotaged five
  ways, each failing the assertion it targets: the off branch wrapping, the
  final image written in place instead of by rename, pe-set-debug's failure
  ignored, `MODE` hard-wired on in the graph (and one sabotage that broke
  the build instead, discarded as proving nothing).
- **`test-log-link-granularity.sh` re-links with CMake's pieces.** The
  RELEASE tree now generates `axl-link-pieces.sh` (cmake/AxlImages.cmake):
  ld, its flags and script, the application and minimal runtime objects,
  the library group, the log-engine pull, objcopy's sections and strip --
  from the same variables and objects `axl_efi_image` links, which now share
  them through `_axl_link_libs`, `AXL_LOG_ENGINE_PULL` and
  `AXL_OBJCOPY_STRIP` instead of spelling them inline. The probe compiles
  with hello.c's own command from `compile_commands.json`. A new control
  proves the pieces ARE the shipped link: hello.c's object linked from them
  reproduces the tree's `hello.so` byte for byte (sabotage: a library group
  missing the porting objects fails it). The first version listed the
  minimal runtime with `foreach(IN LISTS a b)`, which reads `b` as a list
  VARIABLE -- the minimal crt0 dropped out and the probe failed to link.
  The minimal runtime is now one variable, `AXL_CRT0_MINIMAL`, which the
  minimal-log fixtures use too. 20 assertions, both arches.
- **Review fixes:** the wrap test `cd`s to the source root (`cmake --preset`
  reads the cwd); its path redirection uses quoted patterns and is CHECKED
  before the replay runs -- an unapplied substitution would have replayed an
  `off` wrap onto the shared tree, a raw image Ninja then ships as up to date
  (sabotage: one substitution dropped fails "the replay is redirected");
  the failure case proves it failed AT pe-set-debug (the fake records its
  call, and `OUT.tmp` exists); "ninja could not answer" is reported apart
  from "no wrap step"; the pieces file refuses a path holding a `'`.

### 10.11 Slice 5c -- the gates run without make (2026-11-08)

Every `make check-*` gate is a CTest test, `gate.check-<name>`, registered in
`cmake/AxlIntegrationTests.cmake.in` (`_axl_gate`) and run straight against a
CMake tree. That registration is now THE gate list: verify.sh's `make` job
selects the non-artifact gates and `makeimg` the `artifact`-labelled ones, in
BOTH trees, and CI's lint job runs `ctest -L check` on both trees after
building them -- one list where CI had a hand-picked dozen `make check-*`
steps and verify read the Makefile's `LINT_GATES`. The job names stay `make`
/ `makeimg` until slice 6 (they are pinned by the self-tests and CLAUDE.md).

**Where each gate's make-specific half went:**

| gate | was | now |
|---|---|---|
| ascii, docs, dogfood, nul, test-registered, json-dialect, toolchain-conf, uefi-scope, log-levels, awk-portability, devkit-conf, tool-version, tool-docs, asset-names, tools-sidecars, no-inner-map-refresh, test-est, test-meta | a one-line recipe | the same script, directly |
| tautology, version, fuzz-link, ctors | inline recipe shell | `scripts/check-<name>.sh`, the recipe moved verbatim |
| cxx-entry, cb-noexcept, handle-exclusions, examples | `$(CXX) $(CXXFLAGS_BASE)` | `scripts/compile-like.py`: the command the tree runs for an application TU (compile_commands.json), source and output swapped; examples drop what a consumer lacks (`AXL_ALLOW_UEFI`, the library's private TLS build). Exit 125 = no command to borrow, never a compile verdict |
| nx-compat, pe-stripped, log-linkage, reloc-coverage, bss-clear, no-avx | images as make prerequisites | the tree's images, the gate requiring the prep that built them |
| libc-overlap | `make -q` freshness, print-LIB_SOURCES | `--tree`: the tree's archives and glue, each judged against ninja's record of its inputs (`-t inputs`, `-t deps`) |
| size-gate-paths | make's database (`make -p`) | `size-gate-inputs.sh --tree`: ninja's inputs and deps plus the build definition, narrowed to directories that reach the goal |
| flag-parity | Makefile vs axl-cc vs the CMake package | the CMake build is a FOURTH compared path: AxlFlags/AxlImages flags and `-j` list, `AXL_LDFLAGS_EFI`, the wrap's stub section and packer arguments |
| dep-tracking, build-mode | tests OF the Makefile | still `make`, labelled `make-only`; they go with it (slice 6) |

**Per arch:** cxx-entry, ctors, libc-overlap, nx-compat, log-linkage,
reloc-coverage and bss-clear register in each arch's tree (CI ran cxx-entry
and ctors on aa64; verify ran libc-overlap on both). nx-compat,
log-linkage, reloc-coverage and bss-clear are NEW on aa64 and pass.
**pe-stripped stays x64-only, as it always ran:** on aa64 it reports images
whose COFF symbol-table pointer is set with ZERO symbols -- under make too
(`make ARCH=aa64 check-pe-stripped` fails identically). Whether that is the
gate being too strict or aa64 objcopy leaving a dangling pointer is an open
question, recorded, not a regression of this port. **Resolved in slice 6 (§10.12):**
the pointer was objcopy's COFF string table for the 9-byte `.rela.dyn`;
`pe-set-debug` now truncates the name and drops the table, as the PE spec
says an image must, and the gate runs on both arches.

**Detectors that could not see, found while porting:**
- `ninja -n` as a freshness check: in a tree with CONFIGURE_DEPENDS globs,
  every dry run plans the glob check and a CMake re-run and STOPS there --
  measured, it printed the same two steps over a touched source as over a
  clean tree. libc-overlap judges each artifact against `-t inputs` and
  `-t deps` instead (touching a source or a header is now seen).
- the size-gate derivation first demanded an object deps record for the
  published `build/*.o` copies (copy steps have no depfile), and then counted
  every directory's CMakeLists.txt as an input of sshd; both corrected, and
  the CMake derivation found two inputs the declaration missed
  (`scripts/axl-list.sh`, `VERSION`).
- a ctors self-test's `| grep -q` under pipefail failed ON a match.

**Controls:** each new script sabotaged with `scripts/sabotage.sh
--expect-fail` -- a planted `test_check(true...)`, a VERSION mismatch, the
noexcept macro emptied, a poison message changed, a library-private header
in an example, a lost allow-uefi marker, a harness that no longer links and
one dropped from the list, the size-gate paths narrowed twice, the CMake
flag/ld/`-j`/wrap legs of flag-parity (`test-flag-parity-gate.sh` cases
17-21), libc-overlap over a touched source and header, and
`test-check-ctors.sh`'s fixture images (the real trigger, a compiler without
`--enable-initfini-array`, cannot be planted). `test-ctest-integration.sh`
pins every gate niced and ordered after the prep, every artifact gate
requiring it, the exact artifact set, and that only the make-only gates run
make.

**Still on make, for slice 6:** `suite-prep.sh`'s `print-non-artifact-goals`
(the runner's pre-built skip list; it refuses loudly when it cannot ask), the
make-only gates, and the make derivation in `size-gate-inputs.sh` (used only
by `make check-size-gate-paths`).

**What the independent review of the port found, and what was done:**
- *which-gates routed a gate's own script nowhere* (a recipe moved from the
  Makefile into `scripts/check-*.sh` had routed to full verify), and CMake
  build files never to `makeimg`, where check-size-gate-paths now runs.
  Fixed by READING the registration: a `scripts/` file named on a gate line
  routes to `make,makeimg`, as do `compile-like.py`, the registration and
  every build file (gate-scoping 3a1, sabotage-checked).
- *`--only=x64,make --apps=X` refused*: the aa64 tree now runs for gates
  alone, and the `--apps` registration check treated it as a unit tree.
  Fixed, with a self-test case.
- *cut-release's version check could never stop a cut*
  (`check && note` under errexit skips, not stops); it dies now. No test
  reaches that line -- every cut-release test stops earlier -- so the fix is
  read, not run: a gap recorded for the release tests.
- *`compile-like.py --drop` was silent when it matched nothing*, so a
  spelling change would have quietly re-granted an example the library's
  private TLS includes; an unmatched drop is now a failure (sabotage: a
  misspelt drop).
- *libc-overlap took deps records unchecked*: every object must now have a
  VALID record, as the size-gate derivation requires.
- *a size-gate hang lost its message*; restored around the timeout.
- *the aa64 gate prefix was unpinned*: `test-ctest-report.sh` now renders an
  aa64 gate failure, exactly (`FAILED: aa64:check-c`).
- *Second pass:* the first which-gates lookup read the WHOLE registration,
  so `build-docs.sh` routed to every job and `qloop-verdict.sh` to makeimg;
  it now reads only `_axl_gate(...)` calls (self-test pins both, and that
  `lib/discover.sh`, gate.check-test-meta's implementation, routes to the
  gates). And check-reloc-coverage read an objdump that could not run as
  "no relocation table" -- clean over nothing, under make too; it now says
  CANNOT RUN and requires at least one graded table (check-bss-clear words
  a missing objdump as CANNOT RUN too). The objdump gates take the build's
  binutils (CROSS): their aa64 runs needed aarch64-linux-gnu-objdump, which
  CI's containers no longer install -- found in a fresh ubuntu:26.04, where
  the size-gate reader also met CMake 4.2's spelling of the regeneration
  edge (`build build.ninja <every cmake_install.cmake>: RERUN_CMAKE`).

Left as recorded, not changed: the size-gate derivation reads the x64 DEBUG
tree while the size test builds RELEASE (identical input sets, diffed); an
aa64 derivation would also name `scripts/elf_aarch64_efi.lds` and the aa64
crt0, which the declared paths do not cover (pre-existing: make's
derivation was x64-only too); libc-overlap's `--tree` mode still takes nm
and libc from axl-toolchains.conf rather than the tree's configured ones;
and `--only=make`/`makeimg` now need a configurable aa64 tree, its makeimg
leg building aa64 beside the x64 guests -- the 1.40 budget scale was
measured with only x64's makeimg there (a full verify measured 104.8 s of
pooled guest time against 190 s).

### 10.12 Slice 6 -- the Makefile retired (2026-11-09)

**The owner's decisions for this slice (2026-11-09), applied here:**

1. **CPack is NOT adopted.** §10.9's measurement stands: the three tarball
   scripts write sorted, 0/0, commit-mtime archives today, and CPack before
   CMake 4.3 (CMP0206) records the builder's owner. Revisit only when the
   minimum CMake reaches 4.3.
2. **`libaxl.a`'s members are named `*.c.obj`** (CMake's naming; §10.9
   judged renaming every object not worth a map file's spelling). Listed
   under Breaking in the 5.0.0 changelog.
3. **No two-guest one-boot split.** The plan (§10.8's notes: halve each
   arch's one boot across two guests) is dropped. Instead CI's unit step,
   `release-suite.sh`'s `run_unit`, runs the selection verify.sh already
   makes: `verify.sh --only=x64` -- every unit binary its own CTest test on
   a leased `unit` guest when the axlpool service serves x64, else the
   booted one-boot run. The step declares what verify would otherwise
   scrub or default: the tree's own agent on the leased guests
   (`AXL_VERIFY_POOL_AGENT=tree`, as the suite presets set
   `AXL_IT_POOL_AGENT`), CI's unit budget (`AXL_VERIFY_UNIT_BUDGET` from
   ci.yml's `TEST_MAX_DURATION`), and the failing tests' output inline
   (`AXL_VERIFY_PRINT_FAILURES=1`: CI's log is all anyone sees).
4. **One CMake, 4.2.x, for ci.yml and release.yml.** `scripts/ci-cmake.sh`
   pins 4.2.8 and Kitware's SHA-256 per machine and installs it into
   `$RUNNER_TEMP` in every job that configures the tree; no workflow takes
   cmake from apt or the runner image. `test-ci-cmake.sh` holds every job
   that checks out the tree to installing it first, except a named list of
   jobs that never configure (which it also checks).
5. **check-pe-stripped gates aa64.** The pointer it flagged was objcopy's
   COFF string table for the 9-byte `.rela.dyn`; `pe-set-debug` truncates
   the name to `.rela.dy` and drops the table, as PE/COFF spec §3.4 says an
   image must (`test-pe-set-debug-strtab.sh`). The gate was not loosened. The
   same holds for a C++ image's `.eh_frame`/`.gcc_except_table` on both arches
   (`.eh_fram`/`.gcc_exc` now): `test-cxx-exceptions-qemu.sh` looked them up
   by the long name through `objdump -h`, failed in the slice's
   run-integration pass, and matches the eight-byte prefix since.

**Found on the way, and fixed:** the host tools (pe-set-debug,
axl-efi-pack, fwtool-host) are built by custom commands with the host
compiler, so they were in no compile database and outside the lint gate,
and their multi-source compiles tracked no headers (gcc's `-MD` keeps one
TU's): an edit of `src/pack/axl-pack-format.h` left a stale packer.
`axl_host_tool()` builds them, `cmake/AxlHostDeps.cmake` writes one depfile
over every source, and the lint preset runs the bugprone pass over each
non-vendored host-tool source (`test-cmake-host-tools.sh`).


**The Makefile's `AXL_TOOLCHAIN` variant had no CMake counterpart**, so
deleting the Makefile would have silently dropped it: an environment
`AXL_TOOLCHAIN=cross` was ignored by the CMake build, which took /opt's
toolchain. `cmake/AxlToolchain.cmake` now implements it as the Makefile did
(`axl` default, `auto` = `axl`, `host` refused with the reason, an unknown
variant refused naming the real ones, `cross` requiring every locator and
never falling back to the conf). And a tree keeps the toolchain its first
configure resolved (`AXL_TC_*` in the cache): Ninja's own regenerate runs in
whatever environment the build started from, and re-reading the locators
there left a `cross` tree compiling with its own gcc and linking with /opt's
binutils. A reconfigure naming another toolchain, or a moved conf default
under a locator that came from it, is refused with the way out (`cmake
--fresh`). install.sh's early `cross` check covers all three locators, as
the CMake build needs them (`test-toolchain-variant.sh`). Not carried over:
`SMOKE_COLS`/`SMOKE_ROWS`/`SMOKE_EXTRA` (the wide console-device smoke's
ad-hoc geometry knobs; CMake builds the 142x44 the tests use), `PREFIX=`
(a CMake tree is `build/<preset>`) and `HOSTCC` (`-DAXL_HOST_CC=`).

**newlib's mallinfo probe had no CMake counterpart either** (found by the
comment sweep): the Makefile defined `AXL_NEWLIB_MALLINFO_INT` when the
toolchain's `__malloc_current_mallinfo` was 40 bytes (ten ints against a
header of ten size_t). The pinned toolchains are 80-byte, so no image changed,
but a `cross` toolchain built from the old recipe would have lost
`axl-cxxrt-alloc.c`'s mallinfo override silently. `cmake/AxlFlags.cmake`
probes it the same way, except that a probe which cannot see (no libc.a, a
failing nm, no sized symbol, a size other than 40 or 80) fails the configure
rather than reading as "80, fine" (`test-toolchain-variant.sh`, with stub
nm's).

**The result.** In deletion order (§9.0 item 2: consumers first, the file
last), each commit green and independently reviewed, with a second review of
each review-fix delta:

| commit | what |
|---|---|
| `de3d6155` | decision 5: pe-set-debug drops the COFF string table; check-pe-stripped on aa64 |
| `522e175e` | the host tools under clang-tidy; their multi-source builds track headers |
| `3626db7f` | decision 4: one pinned CMake (4.2.8, `scripts/ci-cmake.sh`) for CI and the release |
| `ec35b6ac` | decision 3: the release suite's unit step runs verify's pooled/booted selection |
| `4f309a1a` | the `AXL_TOOLCHAIN` variant, ported; a tree keeps its first toolchain |
| `038aae19` | the last consumers of the Makefile moved off it (non-artifact goals, flag parity, size-gate derivation, libc-overlap, cut-release's version check now tested) |
| `3f91162e` | the Makefile deleted, with the make backend, the transition gates, the Makefile's own tests and gates, `build-backend=`, the make-tree lock; a test that still runs make now fails check-test-meta |
| `2952a428` | verify's `make`/`makeimg` jobs are `gates`/`imggates` |
| `2eb3ead5` | the mallinfo probe |
| `e19431f6` | the comment and doc sweep; this section, the CHANGELOG, README, CLAUDE.md, ROADMAP |
| `9f0f2d3d` | fixes from the full suite: the 8-byte section names in test-cxx-exceptions and check-reloc-coverage, and the fifth axl list in test-axl-lists |
| `3431d385` | the published docs' present-tense guards named `gate.check-*`, not make goals |

What a deleted test guarded, and where it went:
- `test-make-link-prereqs.sh` (link rules not listing their glue objects):
  `axl_efi_image` declares every object and library a link takes, and CI's
  clean-checkout RELEASE build exercises it on every push.
- `test-build-state-race.sh` and `check-dep-tracking` (make's flag signature;
  objects without `-MD`): Ninja tracks commands and depfiles itself; the
  concurrent-build race is axl-build.sh's per-tree flock
  (`test-axl-build-lock.sh`).
- `test-compare-release-trees.sh`, `release-parity.sh`, the parity tests: one
  build has nothing to compare against; CI's build job makes all three release
  tarballs from a clean checkout instead.
- test-efi-pack-qemu's AXL_PACK toggle block: `test-cmake-wrap.sh` (x64 only;
  the old block ran on both arches -- a narrowing, recorded).
- `check-build-mode` (a lowercase `BUILD=release` aliasing trees): axl-build.sh
  refuses any BUILD but DEBUG or RELEASE.

**Consumers outside this tree** that run `make -C <axl-sdk>`: uefi-devkit's
`build.sh` (`tools ... AXL_TLS=1` and `sshd-rtk RTK_USB_UNDI=...`) and its
`Makefile` (`tools`, `clean-tools`). They must call `scripts/axl-build.sh
ARCH=<arch> tools` and `... sshd-rtk sshd-full-rtk RTK_USB_UNDI=<drv>` (whose
images land in `build/<arch>-debug-rtk/tools/`); `devkit.conf` already names
those paths. softbmc, agt and axl-webfs build against the staged SDK through
`install.sh`, which is unchanged.

### 10.13 Every local entry point runs the pinned CMake (2026-11-09)

**Why.** §10.12's decision 4 pinned one CMake (4.2.8, `scripts/ci-cmake.sh`)
for CI and the release, but local builds and tests kept taking whatever
`cmake` the box had (3.31.8 on the development host). A test then passed on
every local run and failed only on CI: CMake 4.2 writes
`compile_commands.json`'s `output` absolute where 3.31 writes it relative.
A local green has to be the green CI would give, so local runs use the
version that ships.

**The helper: `scripts/axl-cmake-pin.sh`** (sourced; never shipped in the
SDK). `axl_use_pinned_cmake` makes the pinned cmake and ctest the first on
PATH:
- the cmake on PATH AND the ctest beside it already report the pinned
  version (CI, after `ci-cmake.sh`; or a nested entry point): nothing
  happens, nothing is printed;
- otherwise from a per-user install shared by every checkout and worktree,
  `${XDG_CACHE_HOME:-$HOME/.cache}/axl-sdk/cmake/<version>/`, installed
  there on first use through `ci-cmake.sh install` (Kitware's tarball,
  SHA-256 verified; ~50 MB once) under a per-user `flock`, with ONE line
  saying so;
- a failed install (offline, a checksum mismatch, no HOME) returns 1 with
  the installer's reason, the cmake it refuses to fall back to, and the
  command to retry. It never falls back to the system cmake: a silent
  fall-back is the drift this removes. No opt-out knob: no caller needs
  one (an offline box with the pin already cached works; CI is a no-op).

**A tree another cmake configured is reconfigured `--fresh`.** PATH alone
moves nothing: a tree's `CMakeCache.txt` records `CMAKE_COMMAND`, and
ninja's regeneration edge runs exactly that binary. `axl_cmake_tree_foreign`
calls a tree foreign when that binary is neither the cmake now on PATH
(through symlinks) nor another binary reporting the pinned version -- the
second clause keeps a wrapper on PATH (pip's, a snap's) that records the
real binary behind it from reconfiguring every tree on every build; a
recorded binary that is gone (CI's `$RUNNER_TEMP`) is foreign. The
callers delete the tree's `build.ninja` and CTest registration before
`cmake --fresh --preset <p>`: a `--fresh` that fails still writes a cache
naming the pinned cmake, and the old `build.ninja` would otherwise read as
configured and current. Where:
- `axl-build.sh`, under its per-tree flock (every build goes through it, so
  this box's trees migrate on their next build with no manual step);
- `cts_tree_current` (`test/integration/lib/ctest-session.sh`), which
  verify.sh and run-integration.sh ask before any ctest reads a tree they
  did not build -- now under the same per-tree lock, asked again once the
  lock is held, for its ordinary reconfigure too;
- `release-suite.sh`'s integration step, through `cts_tree_current`, when
  the tree is foreign (`--step integration` skips the build step);
- `lint.sh` (the x64-lint tree); `check-fuzz-link.sh` configures `--fresh`
  every time already.

`check-test-est.py` lists with the tree's OWN ctest
(`CMAKE_CTEST_COMMAND` from its cache), whatever PATH says: it is a lint
gate that never configures, and run by hand from a shell with the box's
ctest first it would read a registration another ctest wrote.

**The census** (`git grep` over scripts/ and test/integration/, shell and
Python):

| entry point | how it gets the pinned cmake |
|---|---|
| `scripts/axl-build.sh` | calls the helper before configuring or building (not for `--print-prefix`/`--env-fingerprint`/a pre-built skip, which run no cmake); `--fresh` on a foreign tree |
| `scripts/install.sh`, `make-release-assets.sh`, every fixture build in a test | call the helper (they run `cmake --install` / `cpack`), and build through axl-build.sh |
| `scripts/verify.sh`, `test/integration/run-integration.sh` | call the helper; `cts_tree_current` per tree |
| `scripts/release-suite.sh` | calls the helper; `cts_tree_current` on a foreign tree before ctest |
| `scripts/lint.sh`, `scripts/check-fuzz-link.sh` | call the helper |
| `scripts/run-selftests.sh`, `qloop-verdict.sh`, `size-gate-inputs.sh` | run no cmake/ctest themselves (`ninja -t` queries only); their children pin |
| `scripts/check-test-est.py` | the tree's own `CMAKE_CTEST_COMMAND` |
| the tests and the fixture generator that run cmake/ctest directly (`test-cmake-*.sh`, `test-ctest-integration.sh`, `test-toolchain-variant.sh`, the `*-selftest.sh` that drive real trees, ...; `test-pinned-cmake.sh` lists them) | call the helper, so a test run alone sees what CI sees |

`test-pinned-cmake.sh` holds the census: every tracked shell script that
runs cmake or ctest as a command must call `axl_use_pinned_cmake` on a line
of code, unless named with its reason (sourced libraries, tests that drive
only their own stubs, matches inside strings). `test-ci-cmake.sh`'s workflow
scan derives its "this job builds" list from the same callers, so a CI job
that starts running a pinned entry point without installing the pin first
fails there rather than downloading into the runner's `$HOME`.

Tests that run a COPY of an entry point in a stub repository with stub
cmake/ctest (`test-axl-build-lock.sh`, `-prebuilt`, `test-release-suite.sh`)
use `test/integration/lib/cmake-stub.sh`: their stubs answer `--version` with
the pinned version (or the helper would put the REAL cmake first and the
stub would never run), and the copied `ci-cmake.sh` refuses every install
(exit 97), so nothing is ever downloaded on a stub's behalf.

**Scope.** This repo's dev and CI entry points only. The SDK consumers
install -- `axl-cc`, the CMake package `install.sh` generates, `sdk/examples`
-- builds with the consumer's own CMake; nothing there sources the helper,
and the tree's own 3.29 (CMakeLists.txt, CMakePresets.json) is unchanged.
The consumer minimum is separate, and is now 3.20 (below).

**The consumer floor is TESTED, not just declared: CMake 3.20.** Pinning
`test-cmake-package.sh` meant no test exercised the consumer package under a
pre-4.x CMake (local runs had done so by accident with the box's 3.31; CI
never did), so a command newer than the floor or a newer policy default
could reach what install.sh generates unseen. The test runs every case
TWICE -- under the pin and under **CMake 3.20.6**, the last release of the
minor sdk/examples declares (`ci-cmake.sh --floor-version`; the test fails if
the two minors drift apart) -- and both runs also configure and build
`sdk/examples` as shipped. The floor comes from the same code as the pin:
`ci-cmake.sh install --floor DIR` (Kitware's tarball for x86_64 or aarch64,
its SHA-256 from `cmake-3.20.6-SHA-256.txt`, ~45 MB) and `axl_floor_cmake_bin`
in `axl-cmake-pin.sh`, which installs it into the same per-user cache
(`~/.cache/axl-sdk/cmake/3.20.6/`) with the same lock and one-line notice but
leaves PATH alone. Each run is a child of the test with that cmake first on
PATH, and first asserts `cmake --version` IS that one.

**Below the floor, the package refuses by name.** A package config must not
call `cmake_minimum_required` (it would reset the consumer's policies), so
the generated `axl-config.cmake` opens with a `CMAKE_VERSION VERSION_LESS
3.20` check whose message is `axl: the AXL SDK's CMake package requires
CMake 3.20 or newer (this is CMake X.Y.Z).` Loaded by `find_package`, it
sets `axl_FOUND` false with that message as `axl_NOT_FOUND_MESSAGE` and
returns: REQUIRED stops configure and prints it as the "Reason given by
package", and an optional `find_package(axl)` is simply not found, so a
consumer can fall back. Loaded any other way it is a FATAL_ERROR (the idiom
CMake's own generated `*Targets.cmake` files use): a bare `include()` has
no not-found channel, and inside another package's `find_package`
`CMAKE_FIND_PACKAGE_NAME` names THAT package, so the not-found path is taken
only when it is `axl` (compared lowercased). The test checks all four under **CMake
3.16.9** (`ci-cmake.sh --below-floor-version` / `install --below-floor`,
`axl_below_floor_cmake_bin`, ~40 MB) from a project whose own minimum is
3.16, so the error can only be the package's -- and builds the expected
message from `ci-cmake.sh --floor-version`, so moving the floor without
moving the package's check fails there (as does an sdk/examples
CMakeLists whose minimum disagrees with the floor). `ci-cmake.sh name
[--floor|--below-floor]` knows those tarballs' names (`linux` from 3.20,
`Linux` before) and which machines Kitware built them for, from an explicit
per-version list; the helper asks it. (`axl_use_pinned_cmake` spells the
pin's `linux` name itself, keeping its fast path free of a subprocess.)

A CMake that cannot be installed is a FAIL naming it, never a skip; the only
skip (exit 77, after a green pinned run) is a machine Kitware built no such
release for -- ci-cmake.sh exits **3** for that, distinct from its usage
error (2), and only that becomes the helper's status 2, so a stale or broken
installer FAILS the test rather than skipping it. 3.20 exists for x86_64 and
aarch64; 3.16 for x86_64 only, so on aarch64 the refusal check is the part
that skips.

**Why 3.20, and what the floor found.** `add_custom_command(DEPFILE)` -- the
header tracking of every `axl_add_app`/`axl_add_driver` object -- works
under Ninja from CMake 3.7 but under the Makefile generators only from 3.20
(CMP0116). While the minimum was 3.16, the package gated the depfile on
`if(POLICY CMP0116)` and so tracked NO headers on 3.16-3.19 under either
generator, silently; running the test under 3.16.9 found that, and the fix
then needed a pre-CMP0116 `-MT` spelling for Ninja, a guard for binary dirs
outside the top build dir, and a configure WARNING for Makefiles, which had
no mechanism at all. Raising the minimum to 3.20 (owner decision, a
`### Breaking` change) deleted all three: the depfile is unconditional and
`cmake_policy(SET CMP0116 NEW)` always applies -- between
`cmake_policy(PUSH)` and `POP`, since a policy set inside a function
otherwise propagates to the consumer's directory.

Moving the floor to 3.20.6 then found the next defect: the package passed
gcc `-MP`, and CMake 3.20's depfile rewrite turns its phony header targets
into `header.h::`, so the first rebuild after any header edit failed under
BOTH generators (Ninja: "depfile mentions ... as an output"; make: "has both
: and :: entries"). 4.2.8 was unaffected, so only the floor run could see
it. `-MP` is gone: Ninja ignores a depfile input that has disappeared and
the Makefile generator writes its own phony entries into
`compiler_depend.make`, which the dependency cases now prove by deleting a
header. Those cases run under both generators, with two apps in
`add_subdirectory()`s -- one binary dir under the top build dir, one outside
it -- since CMP0116 resolves a depfile's relative paths against the
subdirectory's binary dir. Sabotage-measured: re-adding `-MP` fails the
floor run alone; dropping the DEPFILE for one generator fails that
generator's cases under both CMakes; removing the version check, or naming
another version in its message, fails the refusal check. The deleted-header
case is a regression guard: no package change can provoke a stale-rule
error today, and what was measured is that its check fires when the fixture
keeps the deleted header's #include.

**CI is unchanged**: every job that configures installs the pin first
(`ci-cmake.sh install "$RUNNER_TEMP/..."` onto `GITHUB_PATH`), so the helper
is a no-op in the containers and on the self-hosted host job alike.

**Bumping the pin** is one edit: `AXL_CI_CMAKE_VERSION` and both SHA-256s in
`scripts/ci-cmake.sh`. Every local tree then reads as foreign (its recorded
cmake reports the old version) and reconfigures `--fresh` on its next build;
the old version's cache directory can be deleted once nothing needs it.

**3.31 -> 4.2 differences met.** The `compile_commands.json` `output` path
(above, found on CI before this change). Migrating the development host's
trees (`--fresh` by axl-build.sh, lint.sh and verify.sh's
`cts_tree_current`), then verify.sh (every job), the X64 and AARCH64
integration suites and the self-tests, all under 4.2.8, found no other: the
two failures in the suites were timing bounds under host load
(test-timing-lines.sh, test-net-latency-qemu.sh's first-exchange bound), each
green run alone. A migrated tree rebuilds every object once (the regenerated
build.ninja), ~20 s per tree. A non-`--fresh` 4.2 configure over a 3.31 cache
also succeeds and rewrites `CMAKE_COMMAND`, but keeps 3.31's cached
detection results, which is why the migration is `--fresh`.

**Bumped to 4.3.6 (2026-11-10, owner decision).** The newest 4.3.x patch
(4.4.5 also exists; 4.3 is the release the decision named), checksums from
Kitware's `cmake-4.3.6-SHA-256.txt`, identical at github.com and
cmake.org/files/v4.3. Why 4.3: CMP0206 / `CPACK_ARCHIVE_UID`/`GID`, the
blocker §10.9 recorded against CPack (§10.9's reversal says what it does and
does not fix). The one-edit procedure held: the five trees (`x64-debug`,
`aa64-debug`, both RELEASE trees, `x64-lint`) each reconfigured `--fresh` on
their next build with axl-build.sh's one line naming the old cmake, and
built and linted clean.

**4.2 -> 4.3 differences met:** none in the build, the lint preset,
`test-pinned-cmake.sh`, `test-ci-cmake.sh` (now asserting a 4.3.x),
`test-cmake-package.sh` (pin and floor), `test-ctest-integration.sh` and
`test-toolchain-variant.sh`.

### 10.14 qloop: the quiet slot (reference)

The one description of qloop; the scripts' headers and the other docs point
here, and `scripts/qloop-verdict.sh`'s header is the authority on the verdict
itself.

**What it is.** AxlTestLoop's driver idle-budget cases time a resident
driver's loop (a tick finishes within its budget, an idle loop does not spin).
Those bounds need a vCPU that keeps running while it is measured, so under a
concurrent run's load they void and SKIP: the x64 and aa64 unit runs never
judge them. qloop is where they are judged: AxlTestLoop alone, on x64 (KVM),
after everything else has finished.

**Where it runs.**

| entry point | how |
|---|---|
| `scripts/verify.sh` | the seventh job: a third ctest call, made after both trees' calls return (RUN_SERIAL alone would serialise it within one ctest process only, and the aa64 tree is another) |
| `ctest -L gate` on `build/x64-debug` | `gate.qloop` (RUN_SERIAL), registered in `cmake/AxlIntegrationTests.cmake.in` |
| CI, and `scripts/release-suite.sh` | **not run.** CI selects `integration` and `check` labels only, and the release suite's unit step runs AxlTestLoop concurrently, where these bounds SKIP. Only a local verify judges them |

Both entry points run the one script, `scripts/qloop-verdict.sh`. When the
pool serves (`AXL_ONE_POOL_SOCK`), it asks the pool to settle first (bounded,
60 s: released guests still restoring are the very load the slot avoids) and
runs on a leased `unit` guest, so it boots nothing; on a pool miss it boots.
The row says which (`on a leased guest` / `booted (no pool)` / `booted after a
pool miss`). Pooled design: docs/AXL-Test-Guest-Pool-Design.md §6.7 step 5.

**The verdict.** Green needs POSITIVE evidence.

| verdict | when |
|---|---|
| PASS | a clean run AND the guest printed `LOOP-TIMING: judged` |
| FAIL | a bound missed in every judged attempt (at least two judged), or any functional failure, leak or stall |
| UNJUDGED (exit 3, never green) | every attempt voided; a bound missed in the only judged attempt (the row names each); or a FAIL made only of timing misses while the guest's QEMU waited over `AXL_STARVE_MAX_MS` (50 ms) for a CPU |

**The starvation witness.** The harness samples how long the guest's QEMU
threads were runnable but not running (schedstat run delay), per thread, over
the whole run (`vcpu_starve_sampler` in `scripts/axl-common.sh`). Every row
prints the figure (`the guest's QEMU waited N ms for a CPU`); read it before
believing a timing FAIL. It reads `vCPU starvation not measured` rather than a
number when it cannot compare two readings. `test-net-latency-qemu.sh` judges its round-trip
bounds against the same witness and limit; docs/AXL-CI-Release-Speed-Design.md
§16.1 has the measurements behind 50 ms.

**What nothing covers:** a sustained slowdown that does not preempt (a vCPU on
a slower core, a frequency or thermal drop). Neither the guest's clock-gap
probes nor the witness see it, and a bound stretched in every attempt FAILs.

**Logs.** Under verify.sh a qloop UNJUDGED or FAIL keeps its
`/tmp/axl-verify.*` directory. Under ctest the files are in the tree's
`qloop-logs/` (overwritten by the next run), with the verdict line in
`qloop-logs/qloop-verdict.txt`; ctest prints a SKIPPED test's output nowhere.

History: §10.8 (step 6 moved the verdict out of verify.sh into its own
script).

### 10.15 ctest's TIMEOUT backstop sends SIGTERM first (2026-11-10)

**What changed.** The integration tests and the unit runs (`integration.test-*`,
`unit.<App>`, `unit.one-boot`) carry `TIMEOUT_SIGNAL_NAME SIGTERM` and
`TIMEOUT_SIGNAL_GRACE_PERIOD 30` (`AXL_IT_TERM` in
`cmake/AxlIntegrationTests.cmake.in`; CMake 3.27+, the tree needs 3.29). When
ctest's TIMEOUT fires (the backstop, 3 x the test limit + 36 s, §10.8) it sends
a TERM, waits up to 30 s, and only then KILLs the process tree. The bare KILL
ran no cleanup: a podman container (conmon is not in the tree), a pool lease
and a guest started in a session of its own all outlived it.

**What ctest actually does** (measured, CMake 4.2.8 and 4.3.6):

- the TERM goes to the test's OWN process -- not its process group, not its
  children;
- the KILL at the end of the grace reaches that process's whole tree, but only
  while the process is alive: once it has exited, what it left behind has
  been reparented away and is not killed;
- ctest waits for the test's stdout to close, so a test that exits but leaves
  a child holding the pipe still takes the whole grace.

**So the signal goes only where the process acts on it.** For a bash script
with no TERM trap -- every gate, the prep, the fixtures -- a TERM is worse than
the KILL: the script dies on the spot and its children (ninja, clang-tidy, a
build) are orphaned outside the tree the KILL reaches. Those keep the KILL.
`test-ctest-integration.sh` pins both sides on every registered test.

| test | its process | on the TERM |
|---|---|---|
| `integration.test-*` | `lib/run-one.sh` | passes it to the attempt in flight (`lib/fwd-signal.sh`): `timeout` forwards it to the test's whole group, whose `common-test.sh` handlers clean up; `axlpool run` forwards it to its test and releases the lease. Then it TERMs, and after 10 s KILLs, whatever the attempt left reparented to it (it is their subreaper), prints `<name> FAIL Ns (stopped by SIGTERM from outside -- not its own Ns limit, and not a verdict)` with the output excerpt, and exits 143: no retry, no booted rerun, nothing cached |
| `unit.<App>` | `lib/pool-or-boot.sh` | pooled: as above, through the same `pool_attempt`; booted: it has exec'd `test-axl.sh` |
| `unit.one-boot` | `test-axl.sh` | its `common-test.sh` handler (measured: exits in 0.05 s, its guest ended) |

Why run-one had to change, not just the property: bash runs a trap only after
a FOREGROUND child returns, and with no trap it dies at once. Before the
change a TERM to run-one killed it in 1 ms and left the test's `timeout`, the
test and its QEMU running at ppid 1 -- worse than the KILL. `lib/fwd-signal.sh`
runs the child in the background and `wait`s, which a trapped signal
interrupts (the fix `lib/podman-run.sh` made for its podman client).

**The grace, measured** (TERM to the test's process mid-run, to its exit):

| case | took |
|---|---|
| booted QEMU test (`test-agent-qemu.sh`, a background guest mid-launch) | 0.26 s, nothing left |
| pooled test (`test-9p-tool-qemu.sh`, leased) | 0.26 s, lease released in 1 ms |
| `podman rm -f -t 0` of a running container (`podman_owned_cleanup`) | 0.03-0.3 s |
| `unit.one-boot` (`test-axl.sh` with a guest up) | 0.05 s |

30 s is set by the designed worst case, not these: a pooled test that ignores
the TERM is KILLed by the lease client after its own 10 s
(`CHILD_TERM_GRACE_S`, `test/pool/axlpool/client.py`) and then released, and
run-one gives what the attempt left behind up to 10 s more -- ~20 s, plus a
loaded host's margin. ctest allows up to 60.

`AXL_IT_BACKSTOP` (seconds) replaces the computed backstop; it is the seam
`test-ctest-integration.sh` uses to fire it on a stub in 3 s, not a knob.

**Not done: `ATTACHED_FILES_ON_FAIL`.** ctest's `--output-junit` writes no
trace of an attached file (CMake 4.2.8 and 4.3.6: an empty `<properties/>`,
with `-T Test` too); only the dashboard `Test.xml` carries it (base64 tar/gzip
`NamedMeasurement type="file"`), which nothing here produces or uploads.
`<CTestMeasurement>` tags in a test's output are likewise dropped from the
JUnit. A failing test's log excerpt reaches the JUnit only as run-one's
`--- <name> output (last 25 lines) ---` block in `<system-out>`.

### 10.16 The SDK prefix is `install()` rules; install.sh builds, installs, publishes (2026-11-10)

The owner's direction (§10 item 2: the install prefix comes from
`cmake --install`) had stopped at the build: `install.sh` still copied every
file itself (§10.9 kept the copy "backend-neutral" while two backends
existed; slice 6 removed the second). Now **what a prefix holds is
`cmake/AxlInstall.cmake`**, and `scripts/install.sh` keeps its CLI and does
three things: build each arch's RELEASE tree (`sdk` goal, under the tree's
build lock, as before), `cmake --install` it into a private scratch prefix,
and PUBLISH the scratch into `--prefix`.

**Four components**, so that one prefix holds both arches with every file
installed exactly once: `sdk_arch` and `sdk_cxxrt` from every arch's tree,
`sdk_host` (`pe-set-debug`, `axl-efi-pack`, the unsuffixed `axl.pc`) and
`sdk_common` (everything arch-neutral) from the LAST arch's tree -- the
`axl.pc` "reflects the last arch built" rule install.sh always had. The
generated files moved out of install.sh's heredocs into templates,
`cmake/axl-config.cmake.in` and `cmake/axl.pc.in` (`configure_file @ONLY`).
The package's compilers are substituted at INSTALL time, as install.sh's sed
did: the tree's own arch from its recorded `AXL_TC_*` (the compiler its
libraries were built with), the other arch from the installing environment
or the conf's default (none under `cross`). The host-tool staging rule (a
shebang makes a command, 755; anything else 644) is
`axl_install_host_tools()` in `cmake/AxlInstallHostTools.cmake`, decided at
INSTALL time from the file itself.

**Why install.sh publishes rather than `cmake --install --prefix` directly.**
`write_if_changed`'s discipline is a consumer contract, not tidiness:
consumers reinstall the SDK on every build and their depfiles name SDK
headers, so a no-op reinstall that refreshed mtimes recompiled 84 of 85
objects downstream (`test-install-idempotent.sh`). CMake's `file(INSTALL)`
compares MTIMES (`cmFileCopier::InstallFile`, 4.3.6) and stamps the
source's mtime on a copy, so an identical rebuild, or a branch switch that
touches headers, would refresh every one; and it writes in place
(`cmSystemTools::CopyAFile`), so a test linking from the prefix while
another install refreshes it could read half a file -- what `install_c`'s
rename exists to prevent. So the install goes to scratch and `install_c`
publishes it: content-compared, by rename, mode fixed in place. The
`current` link, the stale-prefix warning and removing a stale `compat/` act
on the destination and stay in install.sh. `write_if_changed` is gone (every
generated file is now a scratch file published like any other).

**Parity, measured** (`scripts/check-install-parity.sh`, which runs the last
copy-based install.sh, `d50df81d`, against the same sources and trees and
compares the two prefixes with `scripts/compare-trees.py`: file set, types,
modes, symlink targets and bytes; not mtimes or owners): RED first (the
rules not yet included: every file "only in A"), then one real difference
found -- `file(READ ... LIMIT 2)` returns the two bytes with a newline
appended, so every host tool read as having no shebang and installed 644 --
fixed by reading HEX; then **identical for `--arch all` (279 paths), `x64`
and `aa64`**. It is a migration check, not a suite test: the old script
carries its own copy of the generated files, so the first intended change to
a template would make it differ for a good reason.

The gates that read install.sh's text moved with it: `check-flag-parity`
proves the package template delegates to axl-cc
(`cmake/axl-config.cmake.in`), `check-tools-sidecars` finds the
install-time `install(DIRECTORY .../share/ ... PATTERN "*.json5")`,
`check-tool-version` finds the host-tool list read in
`AxlInstallHostTools.cmake`, and `check-toolchain-conf` scans the rules and
the template for a hardcoded toolchain path; each was sabotaged against its
new input. `test-sdk-selfcontained.sh` puts the pinned CMake on PATH before
replacing HOME: install.sh now runs `cmake --install`, so it takes the pin,
which would otherwise download into the test's empty HOME -- the build
tool's cache, not something the install leaves for the user.

**Independent review, and what it changed.** Two bugs on paths the parity
run never took: (1) the package's compilers were first substituted at
CONFIGURE time, once per tree, so an `AXL_AA64_GCC` override or a conf bump
on a later install left a prefix whose `axl-toolchains.conf` and
`axl-config.cmake` disagreed -- now install-time, and
`test-install-package-config.sh` pins it (sabotage: the configure-time rule
fails its override case); `axl-toolchains.conf` is also a configure
dependency now, so a bump reaches the tree's own toolchain-conflict check.
(2) A `DESTDIR` in the environment sent `cmake --install` to
`$DESTDIR$SCRATCH`, and the publish, reading its listing through a process
substitution, found nothing and reported success -- install.sh now unsets
DESTDIR, takes the listing to a file and checks it, and refuses an empty
scratch (the same test's DESTDIR case; sabotaged, it fails three ways). And
four risks: the publish is serialised by an `flock` on the prefix directory
itself (two runs could publish an older build over a newer one), the scratch
is removed on INT/TERM (`axl_trap_cleanup`), a file whose destination is a
directory is refused, and the presets' `installDir` (`stage/`) is gone, so a
hand-typed `cmake --install build/x64-debug` cannot write a DEBUG library
into `stage/` in place. The parity check gained `--no-cpp` and passes the
environment through; re-run identical for `--arch all`, `--arch all
--no-cpp`, and `--arch x64` under an `AXL_AA64_GCC` override. (Under an
override of the tree's OWN arch the two differ on purpose: the copy-based
script stamped the environment's compiler into the package even when the
tree had been built with another.)

### 10.17 CPack builds the release archives (2026-11-10)

> **The archiver changed the same day -- §10.18.** The first version ran
> `cpack -G TGZ` under fakeroot; an independent review then found CPack's
> archive order is the staging filesystem's readdir order, and the owner
> moved the tarballs to `cpack -G External` with a deterministic tar and no
> fakeroot. The fakeroot points below are that first version's record.

§10 item 4, as planned, after §10.9's reversal. **`scripts/make-release-assets.sh
sdk|uefi-tools|host-tools`** builds what it needs, then packs the archive
with `cpack` over install() rules; the three tarball scripts
(`make-sdk-tarball.sh`, `make-uefi-tools-tarball.sh`,
`make-host-tools-tarball.sh`) and `stage-host-tools.sh` are deleted, and
`release.yml`, `ci.yml`'s release-build step, `make-test-release.sh` and
every test that called them run it instead.

- **Where each archive's rules live.** SDK: `cmake/AxlInstall.cmake`'s
  `sdk_*` components (the same rules `install.sh` stages from) from BOTH
  RELEASE trees in one `CPACK_INSTALL_CMAKE_PROJECTS`, plus
  `cmake/AxlPackage.cmake`'s `sdk_doc` (the licence and documentation
  payload, `sdk/examples` as source, a root `VERSION`). UEFI-tools:
  `packaging/uefi-tools/CMakeLists.txt`, a project with NO languages that
  reads one arch's built images from the tree it is given
  (`-DAXL_IMAGE_TREE`) -- so the packing needs no toolchain and runs against
  any tree the build wrapper names, `test-make-test-release.sh`'s stub tree
  included. Host-tools: `packaging/host-tools/CMakeLists.txt`, likewise
  compiler-free, calling the same `axl_install_host_tools()` as the SDK.
  The QEMU dependency lists the host-tools README prints live once, in
  `cmake/AxlQemuDeps.cmake` (the packages' Recommends will read them).
- **The asset names are the driver's**, never CPack's: it passes
  `CPACK_PACKAGE_FILE_NAME`, and the versioned root as
  `CPACK_PACKAGING_INSTALL_PREFIX=/<root>` with
  `CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF` -- CPack's top-level directory
  would otherwise be the FILE name, and the SDK's root (`axl-sdk-<v>/`)
  differs from its file name (`axl-sdk-linux-<v>-<host>.tar.gz`). It checks
  each archive has exactly one root of that name. `test-release-asset-names.sh`
  pins the set against v4.7.5's (the seven published names, version
  substituted) and each archive's top level; sabotaged, a renamed host-tools
  file and a CPack-default root both fail it.
- **Reproducible, and tested** (`test-release-reproducible.sh`): each
  archive built as this user at umask 022 and as root in a user namespace
  (`unshare -r`; another HOME/USER where the kernel refuses one) at umask
  077 must be byte-identical, every entry `0/0` and `root/root`, every mtime
  the commit's. `cpack` runs under **fakeroot** (the names) with
  `SOURCE_DATE_EPOCH` from `git log -1 --format=%ct` and a fixed umask 022
  (CPack creates the staged directories with the builder's umask). Both
  sabotaged: without fakeroot the names are the builder's; without the
  umask the directories differ. `fakeroot` is a new build dependency of the
  release and CI jobs (apt), and of this host.
- **Compared with the scripts' output** before deleting them
  (`scripts/compare-trees.py`, names, types, modes, link targets AND bytes):
  host-tools and the SDK (371 entries) identical; each uefi-tools archive
  identical but for its ROOT directory, `drwx------` from the old script's
  `mktemp -d` (and in every published uefi-tools tarball, as was its owner
  `runner/runner` -- the old script ran plain `tar`), now `drwxr-xr-x`
  `root/root`. The tools inside keep the `0755` they always shipped with.
- **`SHA256SUMS` is `scripts/release-sums.sh DIR`**, the one writer (the
  `sha256sum -- *` line release.yml ran, C-locale order); `CPACK_PACKAGE_CHECKSUM`
  writes a file per archive, not the one list `packaging/install.sh`
  verifies. `test-consumer-install.sh` stages its release directory with it.
- **The gates follow the rules**: `check-tools-sidecars` finds both globs
  (`install(DIRECTORY .../share/ ... PATTERN "*.json5")` with no `EXCLUDE`,
  and the uefi-tools rules' `file(GLOB ... share/*.json5)`) and the
  workflow's `make-release-assets.sh uefi-tools`; `check-tool-docs` the
  README generator's `COMMAND`; `check-asset-names` asks the driver;
  `check-toolchain-conf` scans the rules; `test-ci-cmake.sh` counts the
  driver as a pinned entry point (and now matches pinned scripts by PATH:
  `scripts/install.sh` calls the pin, and a bare `install\.sh` matched the
  release job's copy of `packaging/install.sh`).

**Upstream.** The fakeroot wrapper stands in for two CPack variables that do
not exist: `docs/upstream/cmake-cpack-archive-uname.patch` adds
`CPACK_ARCHIVE_UNAME` / `CPACK_ARCHIVE_GNAME` (set both or neither; when set,
`cmArchiveWrite::SetUNAMEAndGNAME`, which the archive writer already has and
CPack never calls), with documentation and a release note, against v4.3.6.
**Not submitted** -- a public post is the owner's to make -- and not yet
compiled or run against CMake's own test suite (it would want a RunCMake
case asserting `tar tv` shows the names). Once a CMake release carries it,
the pin can move and fakeroot can go.

### 10.18 The tarballs through `cpack -G External`; the .deb/.rpm reproducible too (2026-11-10)

**What forced it.** §10.17's `cpack -G TGZ` under fakeroot fixed the owner
NAMES, and an independent review then found the second half: CPack's
archive writer lists entries in the staging directory's READDIR order.
Measured on CMake 4.3.6 with six files installed in one order: XFS and
tmpfs gave install order; two freshly made ext4 loop filesystems gave
`yankee charlie alpha bravo mike zeta` and `bravo alpha yankee charlie zeta
mike` -- each exactly that filesystem's `ls -U`. ext4 hashes its directories
with a per-filesystem seed, so the same commit packed on two GitHub runners
is two archives. No CPack variable sorts the entries.

**The owner's decision: `cpack -G External`** (CMake 3.13+), every
`CPACK_EXTERNAL_*` checked against the 4.3.6 documentation:
`CPACK_EXTERNAL_ENABLE_STAGING` (CPack installs the components into
`CPACK_TEMPORARY_DIRECTORY` under `CPACK_TOPLEVEL_DIRECTORY`),
`CPACK_EXTERNAL_PACKAGE_SCRIPT` (run after staging, with the config's
variables), `CPACK_EXTERNAL_BUILT_PACKAGES` (3.19+: CPack copies the listed
files out as its own) and `CPACK_EXTERNAL_REQUESTED_VERSIONS` (`1.0`, the only
format). The script, `cmake/AxlPackTar.cmake`, packs the ONE versioned root
with the line the release used before CPack:

    tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner
        --mtime=@$SOURCE_DATE_EPOCH -cf - <root> | gzip -n -9

So cpack stays the one packaging entry point and fakeroot is gone again
(release.yml, ci.yml, README). Entries carry no owner names at all now
(`tar tv` prints `0/0`), the same as the pre-CPack tarballs. The staged
directories still take the builder's umask, which tar records, so the driver
keeps packing at umask 022.

**The test sees the class it was blind to.** `test-release-reproducible.sh`
builds every archive three times: A and B as this user on two freshly made
ext4 filesystems (root or passwordless sudo makes them; without either, that
half says SKIP), C as root in a user namespace at umask 077 on the usual
TMPDIR -- all three byte-identical, every entry `0/0` without names, every
mtime the commit's. Against `CPACK_GENERATOR "TGZ"` (sabotage) A and B differ
and the names appear; against the External generator it passes.

**The .deb and .rpm, measured rather than assumed** (the owner's rule: keep
CPack's own generator where it is reproducible, else route the format through
the External hook to the distribution's tool). CPack DEB: identical on two
ext4 filesystems, but its `Installed-Size` sums `cmSystemTools::FileLength`
over every staged path, directories included -- 4096 per directory on ext4,
tens of bytes on XFS -- so ext4 against XFS differed. It goes through External
to **`dpkg-deb --root-owner-group`** (`packaging/system/AxlPackDeb.cmake`):
the control file is written from the `CPACK_DEBIAN_*` settings with an
Installed-Size from the files alone (dpkg-gencontrol's rule), and every staged
mtime is set to `SOURCE_DATE_EPOCH` first -- dpkg-deb only CLAMPS a newer one,
and the checkout's older mtimes survived (measured: `VERSION` kept its own;
sabotaged, the test's mtime check fails). CPack RPM: differed in
`BUILDTIME`, and `BUILDHOST` named the build machine; rpmbuild's own macros
fix it (`use_source_date_epoch_as_buildtime`, `clamp_mtime_to_source_date_epoch`,
a fixed `_buildhost`, and every file's mtime set in `%install`), so it stays
CPack's RPM generator. Both are in `test-release-reproducible.sh` -- A and B on two ext4
filesystems, C as another uid at umask 077 on tmpfs, a third filesystem type
(an independent review found the first version staged C on the same kind of
filesystem as A and B, which CPack's DEB writer would have passed). The .rpm's
reproducibility holds for ONE rpmbuild: another rpm version records its own
version and may default to another payload compressor, so a local rebuild on
a different distribution need not match the published file. dpkg-deb and
rpmbuild are build dependencies of the release job (dpkg is the runner
image's; `rpm` is installed) and of this host (both from EPEL).

**The published set is decided in one place**: `make-release-assets.sh
assets [--version V]` lists what a release of V publishes -- the four
archives, `install.sh`, `VERSION`, and the `.deb`/`.rpm` unless V is a
prerelease (an RPM Version cannot hold a `-`; Debian would sort `5.0.0-rc1`
after `5.0.0`). release.yml asks it whether to build the packages and stages
EXACTLY that set (a `diff` by name, not a count); `gate.check-asset-names`
(`--version V` is its seam) and `test-release-asset-names.sh` hold both a
release (eight assets) and a prerelease (six) to it, and the package tests
SKIP for a version it gives no packages. The `deb`/`rpm` kinds refuse a
prerelease by name.

**Upstream, optional.** We do not patch CMake. `docs/upstream/cmake-cpack-archive-reproducible.patch`
drafts what would let `cpack -G TGZ` do this itself: `CPACK_ARCHIVE_UNAME` /
`CPACK_ARCHIVE_GNAME` (CPack never calls the `SetUNAMEAndGNAME` its archive
writer already has) and `CPACK_ARCHIVE_SORT_ENTRIES` (sort the staged file
list before writing), with documentation and a release note, against v4.3.6.
Not compiled against CMake's test suite and **not submitted** -- that is the
owner's public action, if he wants it.
