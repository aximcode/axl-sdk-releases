# AXL-Build-System-Design — replacing the Makefile with CMake

**Status: PROPOSED 2026-08-15.** Direction stated by Mike; not started.
**Constraint, stated explicitly: _"we will no longer ship Makefiles."_** The
Makefile is **replaced**, not supplemented. Any plan that ends with both is not
this plan.

Target: part of, or immediately after, **4.0.0**. See §6 for sequencing.

---

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
3. **The build-state signature** (§1.1) — or CMake's re-configure standing in
   for it, which must be proven equivalent, not assumed.
4. **17 gates + `NONCLEAN_GOALS`.** The gates are already standalone scripts
   (`scripts/check-*.py|sh`), so their *logic* ports for free. The exclusion
   machinery does not: an unexcluded gate WIPES objects mid-build, and
   `verify.sh` runs the gate job concurrently with both arch builds.
5. **The `AXL_TLS` source-set toggle** — which sources compile at all.
6. **42 test EFIs + 34 tools/examples/drivers**, each with its own link rule.
7. **Both arches, always.** `verify.sh` cross-checks that x64 and aa64 run
   equal test counts; an x64-only port is not a port.
8. **The hermetic rule** (`AXL-Libc-Substrate-Design.md` §4.1d): nothing from
   the host — not headers, not libraries, not compilers. A port that reaches
   for a system compiler is wrong by construction. Toolchain paths come from
   `scripts/axl-toolchains.conf`, which is already the single source shared by
   the Makefile, `axl-cc` and the generated CMake package.

## 3. `axl-cc` is NOT part of this port

The appealing version of this project is "CMake replaces the Makefile *and*
`axl-cc`, one path, done." That was measured and it fails:

- **Direct-PE linking dies on aa64.** ARM's `aarch64-none-elf-ld` lists no PE
  emulation at all (`aarch64elf`, `armelf`, `aarch64linux`). x64's binutils
  does carry `i386pep`, so x64 alone could — two pipelines to save one step on
  one arch.
- **Specs files have no post-link hook**, so `pe-set-debug` cannot fold into
  the compiler driver.
- `axl-cc` is the **consumer** driver — what a `.deb` user runs. It must keep
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

## 5. Sequencing against 4.0.0

**Ship 4.0.0 first; port after.** 4.0.0's content is finished, validated and
green today (its two `### Breaking` entries are the `axl-cc --depfile` and
`axl-c++ --hosted` removals, and `check-release-semver.sh` already refuses
anything below a major). The port is unbounded until it starts. Holding a
finished release behind an unstarted one is how a release grows the 43
unrelated commits that produced the v3.2.3 incident.

The port then lands in its own release. Consumers should barely notice: they
consume `axl-cc` and the CMake package, not the Makefile — which is itself
evidence the "no more Makefiles" change is smaller for *them* than for us.

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

- **Order against 4.0.0** — §5 recommends ship-first; Mike's call.
- **Whether the port also owns `scripts/lint.sh`'s `bear` removal** (§1.2). It
  is the cleanest early win and a good first slice.
- **Whether `verify.sh` stays a shell script** driving CMake, or becomes
  `ctest`. The gates are standalone scripts either way; this is about the
  runner, and `verify.sh`'s concurrency + per-job `--only` filtering is
  load-bearing.
