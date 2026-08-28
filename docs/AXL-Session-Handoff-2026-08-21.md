# Handoff — 2026-08-21: two releases, a 31.4% gate win, and AxlSsh scoped

> Self-contained. Everything below was measured or checked in-session; where a
> claim is inherited rather than verified, it says so.

## 0. START HERE — state, and what is actually open

**Tree:** HEAD `5386eafe`, VERSION **4.3.1**, on `main`, working tree clean,
**nothing unpushed**, CI green on main (all four jobs).
**Tags cut this session: `v4.3.0` and `v4.3.1`**, both published with 8 assets
to `aximcode/axl-sdk-releases`.

**Nothing is half-finished.** There is no in-flight task to resume. The open
items are choices, not work:

| open | where | note |
|---|---|---|
| **Implement AxlSsh P1** | `docs/superpowers/plans/2026-08-21-axl-ssh-p1-transport.md` | 6 TDD tasks, real code in every step. Nothing started |
| Make `AXL_SHELL_LAUNCHER=1` safe as the **consumer** default | ROADMAP "making boots cheaper" | needs the firmware that hung, which no longer exists here (§2) |
| CMake port | ROADMAP | PROPOSED, **NOT started** — branch has zero unmerged commits |
| Networking layering (`AxlSocket` as substrate) | ROADMAP | deferral CLEARED, still unscheduled, trigger has not fired |

## 1. Releases

**v4.3.0** needed `cut-release.sh 4.3.0 --allow-breaking` — two `### Breaking`
entries (`--minimal-runtime` no longer implying stdio/args;
`axl_pci_get_class_code` returning `AXL_ERR` for an absent function) under a
minor bump. Mike decided 4.3.0 anyway; the guard's documented escape is the
right mechanism and relabelling to dodge it would have been the failure the
guard exists to catch.

**v4.3.1** needed no flag — patch, Fixed/Changed only.

Both cuts used `--yes`, because the confirm prompt cannot be answered from a
non-interactive session. That is a real deviation to know about: the prompt is
skipped, so the **semver guard is the only automatic check** on that path.

## 2. The 31.4% local-gate win — and why the default is split

**Mike found this, not me.** He doubted the claim that boots were near their
floor. He was right.

Every guest boot was paying the EDK2 Shell's
`Press ESC in 5…1 seconds to skip startup.nsh` — five `gBS->Stall(1s)`
busy-waits. `run-integration.sh` now sets `AXL_SHELL_LAUNCHER=1`, staging a
chainloader that starts the Shell with `-delay 0`.

Back-to-back, uncached, one machine, every run green:

| arch | OFF | ON | saving |
|---|---|---|---|
| X64 (172 tests) | 489 s | 368 s | 121 s = **24.7%** |
| AARCH64 (73 tests) | 322 s | 188 s | 134 s = **41.6%** |
| **combined** | 811 s | **556 s** | **31.4%** |

~4.6 s per boot on both arches (X64 6.2→1.6 s, AARCH64 10.8→6.1 s).

**ROADMAP had parked this** as "not worth it — `run-qemu.sh` already skips the
Boot Manager countdown and ~7 s is close to what an OVMF boot costs", asking it
not be re-proposed "without a measurement". The measurement was never taken and
the premise **conflated two timers**: the Boot Manager countdown *is* skipped;
the Shell's `startup.nsh` prompt is not. The real floor is 1.6 s.

**The gating hang is NOT fixed, and the code was never the variable.** The
launcher works at `1ae66ccd` — the commit that disabled it — verified by
building that tree in a worktree and running its own
`test-shell-launcher-qemu.sh` (passes), plus three OVMF builds, both arches, and
a full uncached 172-test suite. My first hypothesis (that `24c6c529`'s
double-`FreePool` fix repaired it) is **wrong**: it works two days before that
fix. What changed is this box's firmware — the custom OVMF of that era is gone,
distro stock now. **Unreproducible ≠ repaired.**

So the default is **split on purpose**: the suite takes the win (`run-integration.sh`);
a standalone `run-qemu.sh` keeps the conservative default, because it ships to
consumers in host-tools on firmware we cannot test. `AXL_SHELL_LAUNCHER=0`
still bisects.

## 3. The release path was repaired, then exercised by a real cut

Both fixes landed *because* v4.3.0 exposed them, and v4.3.1 proved them:

- **The docs gate is now enforced.** `cut-release.sh` runs
  `build-docs.sh --ci-doxygen` as a **precondition, before the first push**.
  RELEASING.md called it "the one gate worth running that `verify.sh` cannot
  substitute for" (CI's doxygen is older and rejects markup a dev box accepts —
  it shipped broken docs at v3.2.0 and v4.2.0). Being documented did not make it
  happen; it was missed cutting 4.3.0 and came back clean only by luck.
  Escapable as `--no-docs-check`, which announces itself.
- **The watcher gates only on the tag's own workflows.** It was blocking on
  *any* run on the SHA — measured on v4.3.1: Release done at 4m02s, Docs at
  5m02s, script returned at **16m07s**, i.e. **11m05s waiting on CI**, which the
  tag never started and which re-runs the suite the local gate already
  certified. Same bug made v4.3.0 report `RELEASE_VERDICT: FAIL` for a CI
  container defect *after* publishing all 8 assets. Next cut should be ~5 min.
- It also expected `Docs` only on a MAJOR tag, so a minor release **never
  waited for Docs at all**. `docs.yml` fires on every `v*` tag.

## 4. Bugs fixed, and the class they shared

Six defects, one shape: **a detector that could not see, reporting silence as
an answer.** Now named in CLAUDE.md's hard rules ("Empty is not an answer unless
you know the question was asked" / "A detector's silence is worth nothing until
you have shown it can fail").

| what it reported | what was true |
|---|---|
| `check-cxx-entry`: "missing or name-mangled in C++" | `nm` could not start |
| `check-cxx-entry`: "registered NO `.init_array` entry" | `objdump` could not load (**failed the v4.3.0 tag run**) |
| `check-awk-portability`: "clean — 203 build files" | could not see `axl-cc` at all |
| `check-release-semver`: listed 1 breaking entry | there were 2 |
| `watch-release-runs`: would report success | never waited for Docs |
| hermeticity: "no distro toolchain" | one call site reached host `nm` |

Fixes: `libdebuginfod1t64` added to CI's clang-tidy job; both
`check-cxx-entry` readers now probe the tool first; awk gate discovers
extensionless files by shebang (+ `--list` + a test); semver listing spans the
whole section; `axl-cc`'s log check defaults to `${NM_BIN:-${CROSS}nm}`.

**Also:** `LINT_GATES` had two artifact-building gates racing the concurrent
build — split into `LINT_GATES_ARTIFACT`, run serially as a new `makeimg` job
in `verify.sh`. And `run-integration.sh`'s stderr noise (a shell redirect error
`2>/dev/null` cannot suppress) is gone — it was **two** sites, not the one
reported.

## 5. New tests worth knowing about

- `test-hermetic-toolchain.sh` — poisons every host toolchain name on `PATH`
  (record-and-exit-111), builds through `axl-cc`, asserts the build succeeds
  **and zero shims were invoked**. Consumer's method, made permanent.
- `test-release-watch-scope.sh` — stubs `gh` on `PATH`; asserts the watcher
  returns while a non-gating workflow runs, does not fail a published release on
  a red CI, and **still blocks/fails on a gating one** (the control).
- `test-awk-portability-gate.sh` — asserts the gate can *see* `axl-cc`.
- `test-stale-sdk-prefix.sh` — a stale in-tree install must not serve itself.

Suite is now **172 (X64) / 73 (AARCH64)**, unit **10497** both arches.

## 6. AxlSsh — decided, specced, planned, NOT started

Spec: `docs/superpowers/specs/2026-08-21-axl-ssh-design.md`.
P1 plan: `docs/superpowers/plans/2026-08-21-axl-ssh-p1-transport.md`.

**Built here, not ported — licence decided it before architecture.** wolfSSH is
the best architectural fit and is **GPLv3-or-commercial** (verified from its
`LICENSING` file), requiring wolfSSL on the same terms; axl-sdk is Apache-2.0
and every vendored dep is permissive. Dropbear is MIT and architecturally wrong:
`fork`/`forkpty`/`select` is its structure, and this tree has no
`sys/socket.h`, no `pty.h`, no `select(2)` call site.

**Why it is tractable:** `AxlCrypto` already carries the exact set a default
`ssh(1)` negotiates (`AXL_ECDH_X25519`, `AXL_PK_ED25519`,
`AXL_AEAD_CHACHA20_POLY1305`), so it is framing and a state machine, not
crypto. Precedent: **Axl9p = 4,512 lines, client + server, 3 days**, same
primitives. **OpenSSH is a conformance oracle** via `run-qemu.sh --hostfwd`.

Decisions not to relitigate: **three headers** (core/server/client, following
AxlHttp because SSH's transport is nearly symmetric — decided before P1 since
retrofitting is an API break); **on `AxlTcp`, never `AxlSocket`**; **scp/SFTP
out** (§4a — OpenSSH 9.0 moved `scp` to SFTP by default, so it is two projects;
file transfer already works via the Axl9p server); non-goals **are** the
security argument; security review is a **gate per phase**.

## 7. Docs reconciled — and one class to watch

Corrected this session: ROADMAP's CMake entry (said IN PROGRESS; branch has
**zero** unmerged commits), two handoffs saying "START HERE" at shipped work
(`8af4e530`, `7955ba2f`), CLAUDE.md's test counts, CLAUDE.md's 9P line (said
"still no server/mount" — `src/9p/axl-9p-server*.c` and `axl-9p-mount.c` exist),
and `AXL-Libc-Substrate-Design.md`'s header ("P4-P5 not started" with
`P4-RESULT` and a ✅ P5 row **~800 lines below in the same file**).

That last one had made ROADMAP's networking-layering item read as blocked for
four days after its blocker finished. **Check the artifact, not the summary.**

Also answered: `AXL-CI-Release-Speed-Design.md` §9's clang-tidy image-caching
question — **NO**, with data. The job is 2m38s not 7 min and third of four, not
the largest; apt is 25 s = 3.1% of an 813 s run; and a prebuilt image would put
the apt list in a second place, which is exactly what broke the v4.3.0 tag run.

## 8. Traps this session paid for

- **`pgrep -f "x.sh"` matches its own command line.** Cost a wrong "no processes
  running" reading. Escape the dot or check `ps -o etime=`.
- **A sabotage "detected" verdict can be the wrong assertion failing.** The
  docs-gate sabotage tripped `cut-release.sh`'s clean-tree precondition, not the
  gate. Had to test `build-docs.sh --ci-doxygen` directly.
- **A control can pollute what it proves.** The hermeticity test's section-1
  control invoked a shim, which then failed its own "zero invocations" assertion.
- **Profile records are `boot|` AND `qemu|`** (84 + 136 = 220). Counting one
  gave a bogus census.
- **Baselines from log mtimes are not A/B.** My first aa64 number (613 s) was
  derived that way and wrong; the clean run is 322 s.
- **The staged SDK goes stale after every cut** (the version bump changes
  `axl-version.h`). `cut-release.sh` now says so at the end.
