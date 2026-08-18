# Handoff — 2026-08-16: two releases shipped, staging fixed, CMake design landed

> Self-contained. Every number was measured on this tree on this date.
> `main` is at `3d021758`, pushed, working tree clean apart from Mike's
> untracked `SCRATCH.txt` and six `docs/AXL-*.md` drafts — **do not commit
> those, and never `git add -A`.**
>
> **Supersedes `docs/AXL-Session-Handoff-2026-08-14.md`.** Its §7.1 (the
> `AXL_TLS` prefix collision) is done, and its §7.2 (the CMake migration) has
> its design merged to `main`.

---

## 0. Where to start

**Nothing is half-finished. One task is queued and fully specified: §6.**

Two releases shipped (`v4.0.0`, `v4.1.0`), both unblocking the AGT consumer.
Suite is **153/153**, `verify.sh` ALL GREEN both arches (10405 unit tests),
docs zero warnings.

The queued task is the **terminate-handler shrink (§6)**. Its one open design
question was answered by a spike this session, and the answer is better than
expected — see §6 before assuming a tradeoff exists.

---

## 1. What shipped

| tag | contains |
|---|---|
| **v4.0.0** | 64 commits since v3.2.3 — `--hosted` removed (containers unconditional), real `try`/`catch` under UEFI, host-path rejection, `include/compat/` deleted, hermetic toolchain |
| **v4.1.0** | `axl::unique_handle`, `AXL_TOOLCHAIN`, `sdk-prefix.sh`, jose staging fix |

Both published on `aximcode/axl-sdk-releases`, 8 assets each, Release +
Docs green. **Verified, not assumed:** `6118b2d0` is an ancestor of `v4.1.0`,
and `libaxl-cxx.a` is present for both arches inside the `.deb`.

AGT was blocked on each and is unblocked on both. Its `unique_handle`
migration is already done and proven against a throwaway build: 16 files,
one extra include, both arches 0 warnings, `make test` 9598/0.

## 2. `AXL_TOOLCHAIN` — a variant beside the locators (`00206e7a`)

The README told macOS and native-Windows users to build with
`make CROSS=<prefix>-`. **That could not work**, for two independent measured
reasons: `CROSS=` has selected *binutils only* since `0bf6ed51` replaced
`CC = $(CROSS)gcc`, and a glibc-targeted cross cannot build the tree at all
now `include/compat/` is gone (`deps/` dies on the first `<stdlib.h>`).

Prior art decided the shape. Zephyr (`ZEPHYR_TOOLCHAIN_VARIANT`), EDK2
(`TOOL_CHAIN_TAG`) and the Linux kernel (`LLVM=1`) all pair a **named
variant** with a **locator**; none selects a toolchain by bare prefix. AXL had
locators and no variant.

    AXL_TOOLCHAIN=axl     default; what axl-install-toolchain installs
    AXL_TOOLCHAIN=cross   yours, via AXL_<ARCH>_GCC / _GXX / _BINUTILS_PREFIX
                          (defaults NOT consulted — an unset locator is
                          refused BY NAME, which is the feature)

`llvm` is deliberately absent rather than accepted-and-ignored: clang carries
every backend in one binary and is selected by `--target=`, which is why the
kernel *reinterprets* `CROSS_COMPILE` as a triple under `LLVM=1`. **When clang
work starts, that is the seam** — and `axl-cc` is the reason it stays cheap:
all five consumers enter through it, so a clang toolchain is a change inside
`axl-cc` + `axl-toolchains.conf`, touching no consumer.

`scripts/lint.sh` already runs `clang -fsyntax-only -Wall -Wextra` over every
TU against the *cross* libc, so clang already parses the whole tree. Untested
is codegen and link.

## 3. Staging: the real defect behind a "flaky" test (`dc387444`)

`test-jose-cc-qemu` passed twice and failed once in one evening on
`undefined reference to axl_crypto_rng`. Reported cause: "concurrent installs
race". That was the smaller half.

**`install.sh` was defeating `d8ab47ee`.** It hardcoded
`PREFIX=out/native-$arch-release` and passed it on make's command line, which
**overrides** the Makefile's rule — so a TLS install and a non-TLS install
both built there. `AXL_TLS` is in the build-state SIGNATURE, so each
alternation *wiped* the other's objects; run concurrently, a wipe mid-build is
corruption (partial archives, objcopy's "the input file … is empty").

Fixed by ASKING: `make -s print-prefix` with the real `BUILD`/`AXL_TLS` state.
Then `flock` per build tree for two installs of the *same* configuration.
`--print-build-lock` reports the path so callers never re-derive it — that flag
earned itself immediately, since its first draft was already wrong by one
suffix against the build ten lines below.

**Also: a stale staged SDK is now a precondition, not a test result.**
`run-integration.sh` checks it once up front and names the remedy. Previously
one unstaged header edit surfaced as two unrelated-looking C++ failures at the
END of a seven-minute run — it cost a re-run twice in one evening.

## 4. `out/` → `stage/` (O1, `6cbcfe88`), and the collision that wasn't

Distribution §4 is titled "a build directory is not an install prefix" and was
describing this tree. Now:

    stage/    the staged SDK (install.sh --prefix default)
    out/      build trees and Sphinx output

**The measured finding that mattered more than the move:** both design docs
claimed Distribution's P2 and the CMake port's slice 3 sweep the same ~149
callers. Measured with comments stripped:

| | files |
|---|---|
| invoke `make` | 155 |
| …already ask `build-prefix.sh` | **139** |
| reference an `out/` path at all | 23 |
| **both — the real overlap** | **7** |

`d8ab47ee` had already paid for the wide sweep. What actually remained was
that the **staged SDK had no accessor**, so ~12 tests hand-composed it —
`scripts/sdk-prefix.sh` + `test_sdk_dir` fix that, and `AXL_SDK_PREFIX`
relocates it (verified by relocating, plus the control of pointing it at an
empty directory).

`install.sh` WARNS when a staged SDK remains at the old `out/` default —
every existing checkout has one, and it goes stale silently.

## 5. CMake port: design merged, implementation not started (`b4aef0f1`)

`main` now carries the design. **Zero implementation** — no `CMakeLists.txt`,
no toolchain file, none of the six slices. The branch
`worktree-cmake-build-system` still exists and is **6 behind main**; keep using
it, because `cut-release.sh` dates whatever sits under `## Unreleased` and port
commits on `main` would ship inside a release.

Corrections made to the design this session, all in the doc:

- The phasing table was wrong in **five** places (§9.0). Slice 4 was
  undercounted five-fold (~180 images per arch, not 34); slice 6's deletion
  order would have broken `check-fuzz-link`.
- "We have never SHIPPED a Makefile" is **retracted** — `release.yml` publishes
  the tree via `git archive HEAD`, no `.gitattributes` exists. The port is
  breaking and wants **5.0.0**.
- §8.2 (shared surface with P2) **withdrawn**, §5.1's `CROSS=` question
  **answered**.
- **§8.4 is new scope Mike assigned:** extract `axl-config.cmake` from
  `install.sh`'s heredoc. **334 of install.sh's 936 lines are CMake.** The repo
  has no `cmake/` directory and no tracked `.cmake`/`.cmake.in` at all, while
  our own consumer `axl-utils` has `cmake/AxlUtilsApp.cmake`. It converges with
  P1's missing `axl-config-version.cmake`.

**Doc rule established (`3d021758`): one owner per shared fact, everyone else
links.** The three docs (`AXL-SDK-Design`, `AXL-Distribution-Design`,
`AXL-Build-System-Design`) each carry the ownership table. Not combined —
different audiences, and Distribution §10 rules the build-system switch out of
its own scope. The rule exists because both docs asserted the same stale
premise for months while each stayed internally consistent.

## 6. QUEUED — the terminate-handler shrink

**Spiked and measured this session; only the implementation remains.**

libstdc++'s verbose terminate handler drags `__cxa_demangle` and newlib stdio
into every `-fexceptions` image. Preempting it with
`__gnu_cxx::__verbose_terminate_handler`:

| variant | x64 | aa64 |
|---|---|---|
| stock | 279,070 | 274,342 |
| bare "terminate called" | 166,632 | 153,195 |
| **type name + `what()`** | **166,721** | **153,850** |

**The open question — lose the type name, or keep the demangler — is moot.**
Keeping the exception's identity costs **+89 B** (x64) / **+655 B** (aa64) over
bare: `abi::__cxa_current_exception_type()->name()` gives the mangled name free,
and `try { throw; } catch (const std::exception &e)` recovers `what()`.

**And the stock handler prints NOTHING under UEFI** — verified by booting an
uncaught throw. Its output goes to a newlib stderr no UEFI image wires up. So
112 KB currently buys negative value; ours is smaller *and* the only one that
speaks:

    terminate: uncaught exception of type St13runtime_error
      what(): a deliberate uncaught error

**Implementation plan.** A new `.cpp` in `src/cxxrt/` beside
`axl-cxxrt-eh.c` — `axl-cc` links those objects ONLY on the `-fexceptions`
path (`scripts/axl-cc` ~line 1274), so a non-exceptions image pays nothing. It
must be `.cpp`, not `.c`, because recovering `what()` needs a catch. Touches
all three build paths, so `check-flag-parity` is in play. Working spike
fixtures are in this session's scratchpad pattern — re-create from the table
above; `all.cpp` (four containers) and an uncaught-throw fixture are enough.

Suggested tests: assert the terminate output names the type and `what()` (add
to `test-cxx-exceptions-qemu.sh`), and assert `__cxa_demangle` is ABSENT from
an exceptions image (`nm`) — the second is the cheap strong one.

## 7. Other open items, in no particular order

- **Distribution P1** is untouched: SDK tarball from `install.sh --prefix`,
  `axl-cc --print-prefix` / `--print-version`. (`axl-config-version.cmake` is
  now port scope, §5.)
- **`cut-release.sh --from <ref>`** — the tag-based flow is still hand-rolled.
- **AGT field report:** `AxlPageCache` is a borrowed-owner hazard of the same
  class as `AxlSurface` — borrowed by every piece tree, safe only because a
  handle is declared before a scope. Prose and declaration order, nothing the
  type system expresses. Evidence the class is real beyond the one type we
  excluded.
- The `AxlSurface` poison **never fired** in AGT's migration — the audit found
  the borrowed cases first. Insurance, not a crutch.

## 8. Traps this session paid for (read before repeating them)

1. **`until ! pgrep -f "x.sh"` matches ITSELF** and never exits. Cost 4h39m of
   a loop spinning while the suite never started. Background the real command
   and poll its LOG; check `ps -o etime=` before saying "still running" twice.
2. **Never mutate tracked files while a suite or build runs.** A `sabotage.sh`
   31 s into a `-j8` run produced an empty `.so` and a fake FAIL.
3. **Counting one SHAPE is not counting the thing** — twice. A caller census
   that grepped raw text was 25% prose; a path sweep that matched
   `$PROJECT_DIR/out/...` missed `$DIR/out` and a test's own `AXL_STAGE_DIR`.
4. **A pipeline's exit status is the last command's.** `install.sh | tail -1`
   swallowed a real failure; a `grep -c` returning 0 reported the docs gate as
   failed when it passed.
5. **Consumer surveys use all four roots** — `~/projects ~/work ~/bin ~/dell`,
   no depth limit, no invented variant. axl-utils lives in the delldiags tree
   and has now been missed three times.
