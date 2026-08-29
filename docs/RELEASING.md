# Releasing AXL

Step-by-step for cutting an AXL release. The flow is opinionated and
the order matters — running it out of order, or skipping the version
helper, has burned us before.

## TL;DR — the fast path

```sh
scripts/cut-release.sh X.Y.Z            # do it (prompts before pushing)
scripts/cut-release.sh X.Y.Z --dry-run  # preview, change nothing
scripts/cut-release.sh X.Y.Z --yes      # skip the prompt (non-interactive)
```

**It stops and asks before it pushes anything.** After printing the commit
list it prompts `Cut and publish vX.Y.Z? [y/N]` and reads stdin. That is right
for a human at a terminal and wrong everywhere else: run from a script, a CI
step, an agent, or anything without a TTY, `read` sees EOF, the answer defaults
to **no**, and the script aborts having done nothing — correctly, and with exit
status 1. Pass `--yes` when there is nobody to answer.

> Check what actually happened before believing either outcome, and check the
> **script's** exit status rather than your shell's. `cut-release.sh X.Y.Z; echo $?`
> reports the script; `cut-release.sh X.Y.Z > log 2>&1; tail log` reports
> `tail`, which succeeds whether the release happened or not. The three facts
> that settle it, and none of them can be faked by a stray zero:
>
> ```sh
> cat VERSION                       # bumped?
> git tag -l vX.Y.Z                 # tag exists?
> git log --oneline @{u}..HEAD | wc -l   # 0 == main was pushed
> ```

`scripts/cut-release.sh` automates the cut: it bumps the version, dates the
CHANGELOG, commits + pushes `main`, tags, and watches Release/Docs to the
published release. By default it does **not** wait on CI (see the gate below).

**But first decide WHERE you are cutting from.** `cut-release.sh` releases the
tip of `main` and everything under `## Unreleased`. If `main` carries work that
does not belong in this release, that is the wrong flow and the script cannot
do the right one — see [Which flow](#which-flow-from-main-or-from-a-tag)
immediately below. Three of the last three releases used the other one.

### The gate is LOCAL — run the suite before you cut

**CI is no longer a per-push gate, and release tags do NOT trigger it.** `ci.yml`
runs **only** on a manual `workflow_dispatch` — a rare cross-OS backstop, to keep
Actions minutes for the runs that matter. (A release tag used to re-run CI on the
exact commit already validated on `main` — pure duplication, ~38 min for an
identical result — so that trigger was removed.) The **authoritative pre-release
gate is the full suite run locally**, which is fast in parallel:

```sh
make ARCH=x64 all tests tools axl-busybox   # one build for the whole gate
./test/integration/run-integration.sh --no-cache -j"$(nproc)"   # 0 failures required
scripts/lint.sh                                        # clang-tidy exactly as CI runs it
```

`run-integration.sh` discovers every `test-*.sh` (including the patched-QEMU /
real-pointer `local-only` tests, which your dev box CAN run) and runs each in
its own QEMU. Green here is the release gate. Then:

```sh
scripts/cut-release.sh X.Y.Z            # cut (no CI wait — local suite was the gate)
scripts/cut-release.sh X.Y.Z --dry-run  # preview, change nothing
scripts/cut-release.sh X.Y.Z --ci-gate  # want the CI backstop too (see the stamp, below)
scripts/cut-release.sh X.Y.Z --force-ci # --ci-gate, and dispatch even if the stamp covers it
```

**`--ci-gate` no longer always dispatches.** A clean, COMPLETE
**`--no-cache` is required for a release gate, not optional.** Caching is on by
default (it skips a test whose inputs are byte-identical to its last green run),
and a cached run deliberately refuses to write the stamp — it skipped tests on
the strength of their inputs looking unchanged, which is the right answer for an
inner loop and the wrong one for certifying a tree. `cut-release.sh` says so if
it finds no stamp.

`run-integration.sh` writes `test/integration/.last-run-stamp` (gitignored —
it describes one machine's run, not a property of the tree). If that stamp
covers the release commit, `--ci-gate` is satisfied without dispatching
anything, because it would be the same tests on the same code for an answer
you already hold. A run never dispatched is the only thing that improves both
turnaround and Actions spend — see `AXL-CI-Release-Speed-Design.md` §10.3.

"Covers" is deliberately narrow, since a false positive here tags code no
suite ever ran:

| situation | gate |
|---|---|
| stamp's SHA == the release commit | satisfied |
| stamp is the release commit's PARENT, and the only diff is `VERSION`, `include/axl/axl-version.h`, `CHANGELOG.md` | satisfied |
| anything else changed since the stamp | **dispatches** |
| stamp missing, malformed, or from a failing run | **dispatches** |
| stamp came from a `--shard` (partial) run | **dispatches** — it never wrote one |

The parent rule is not a convenience: `cut-release.sh` creates the version-bump
commit itself, so the stamp is always one commit behind by construction and a
bare SHA match could never fire at cut time.

`--force-ci` dispatches regardless. Use it when you want the fresh-OS backstop
specifically — a toolchain bump, a workflow change, anything where "it passed
on my box" is the thing in question.

Before any release, trigger CI by hand on `main` (Actions tab →
`workflow_dispatch`, or `gh workflow run ci.yml --ref main`) and watch it green —
that commit IS the one you will tag, so this is the fresh-OS backstop. The tag no
longer re-runs it. `--ci-gate` makes the cut dispatch CI on `main` and wait for it
before tagging. The `--ci` flag on the runner excludes the `local-only` tests CI
runners can't execute (patched-QEMU SMBus, usb-mouse pointer) — that's what CI runs.

### Before a tag: check docs at CI's doxygen version

```sh
scripts/build-docs.sh --ci-doxygen    # runs CI's doxygen in a container
```

`docs.yml` fires on **every** `v*` tag and a tag cannot be re-cut, so this is
the one gate worth running that the normal `verify.sh` cannot substitute for.
A dev box's doxygen is newer than the `ubuntu-latest` apt package CI installs,
and it ACCEPTS markup the older one rejects — so a locally-clean docs gate is
not evidence. That skew shipped broken docs twice: v3.2.0 and v4.2.0.

### Registering a self-hosted runner (any dev host)

Jobs run in a **container** (`ubuntu:24.04`, CI's own image), so a runner host
needs Docker and KVM — **not** the AXL toolchain. That is what makes this
portable: any machine you develop on can become the runner, and a second one
can join without displacing the first.

**Prerequisites**

```sh
sudo dnf config-manager --add-repo https://download.docker.com/linux/centos/docker-ce.repo
sudo dnf remove podman-docker            # it owns /usr/bin/docker; docker-ce-cli must
sudo dnf install docker-ce docker-ce-cli containerd.io docker-buildx-plugin
sudo systemctl enable --now docker
sudo usermod -aG docker "$(id -un)"      # log out/in, or use `sg docker`
```

**Docker, not podman**, and the reason is not preference. The Actions runner
drives the Docker CLI and API; podman's compat socket runs container jobs but
**will not create a missing bind-mount source**, and the runner only populates
`_work/_actions` for a job that uses an action — so any job without a `uses:`
step fails to start a container. Details in
`AXL-CI-Release-Speed-Design.md` §11.4.

**Register** (once per machine; the org's `Default` runner group is
`visibility=all`, so one registration serves every repo):

```sh
mkdir -p ~/actions-runner && cd ~/actions-runner
curl -fsSLO https://github.com/actions/runner/releases/latest/download/actions-runner-linux-x64.tar.gz
tar xzf actions-runner-linux-x64.tar.gz

./config.sh --url https://github.com/aximcode \
            --token "$(gh api -X POST /orgs/aximcode/actions/runners/registration-token --jq .token)" \
            --name "axl-qemu-$(hostname -s)" \
            --labels axl-qemu \
            --unattended --replace

sudo ./svc.sh install "$(id -un)" && sudo ./svc.sh start
```

The **label** `axl-qemu` is what the workflow selects on; the **name** only has
to be unique, which is why it carries the hostname. Register several hosts with
the same label and GitHub sends each job to whichever is idle.

**Confirm it is ONLINE, not merely registered** — a job targeting a label whose
runners are all offline queues rather than failing:

```sh
gh api /orgs/aximcode/actions/runners \
  --jq '.runners[] | {name, status, busy, labels: [.labels[].name]}'
```

**Decommissioning matters.** A retired machine left registered is a runner that
is permanently offline; if it is the only one, jobs queue forever. From the
host: `sudo ./svc.sh stop && sudo ./svc.sh uninstall && ./config.sh remove
--token <removal-token>`. Or delete it in the org's runner settings.

**Sanity check:** `.github/workflows/runner-probe.yml` dispatches a container
job that prints the distro and whether `/dev/kvm` is usable. Run it after
registering a new host.

### GitHub Actions trigger policy

**No workflow runs on a code push.** Push to `main` (or any branch) as much as
you like — zero Actions minutes. The triggers are baked into the workflow files:

| Workflow | What | Triggers on |
|---|---|---|
| **ci.yml** | build + QEMU integration + lint | **manual only** (`workflow_dispatch`) — dispatch on `main` before tagging |
| **docs.yml** | Doxygen/Sphinx → Cloudflare Pages | major tag `vX.0.0`, or manual |
| **release.yml** | build + publish `.deb`/`.rpm`/tarballs | **every** release tag `v*` (it's the publish step), or manual |

So: a normal push runs nothing; **no** release tag runs CI (it was validated on
`main` before the cut); a **patch/minor** release tag runs only `release.yml`
(publish); a **major** (`vX.0.0`) tag also runs `docs.yml`. `cut-release.sh` tells
its watcher which to expect (major → `Release Docs`, minor/patch → `Release`), so
a cut never hangs waiting for a workflow that won't run.

**Trigger a chain on demand** ("I specifically want it") — no push required:

```sh
scripts/run-ci.sh                 # dispatch + watch CI on origin/main
scripts/run-ci.sh <branch|tag>    # on a specific ref
scripts/run-ci.sh --docs          # dispatch the Docs deploy instead
# or directly:  gh workflow run ci.yml --ref main   (Actions tab → Run workflow)
```

To change the policy, edit the `on:` block at the top of each workflow (each has
a comment explaining its trigger).

## Prerequisites

- You've picked the flow — see
  [Which flow](#which-flow-from-main-or-from-a-tag). On the `main` flow:
  you're on `main`, working tree clean, and `git log origin/main..HEAD`
  shows the commits to ship. On the tag flow: you're on the release
  branch and `git log vPREV..HEAD` shows them — **read that range**, it
  is what the release will contain.
- The integration suite passes locally. The quick smoke set:

  ```sh
  ./test/integration/test-axl.sh
  ./test/integration/test-tools.sh
  ./test/integration/test-tcp-echo.sh
  ./test/integration/test-http.sh
  ./test/integration/test-cpu-idle.sh
  ```

  **But this list is a SUBSET of what CI runs** — `.github/workflows/ci.yml`
  is the source of truth, and it runs more (e.g. `test-input-keys-qemu.sh`,
  `test-yield-ctrlc.sh`, `test-axl-cc-service.sh`, the `clang-tidy` job, both
  arches). Running only the five above can leave a CI-only failure undetected
  until the tag. Either run every `*.sh` step listed in `ci.yml`, or — better
  — rely on the §4b "watch CI on `main` before tagging" gate, which validates
  the *exact* CI matrix. Some tests are deliberately local-only (they need a
  QMP-pointer-injection-capable host the GitHub runners don't provide, e.g.
  `test-input-modifiers-qemu.sh`); run those by hand before a release.

- Both archs build clean against `BUILD=RELEASE`. **Each `BUILD` now gets
  its own output tree automatically** (`out/native-<arch>-release`), so no
  `PREFIX` override is needed and the two can coexist.

  ```sh
  make ARCH=x64  BUILD=RELEASE
  make ARCH=aa64 BUILD=RELEASE
  ```

  This used to require a manual `PREFIX=out/native-<arch>-release`, because
  the `.o` cache key is the `.c` timestamp only, not the `BUILD` mode: a
  same-prefix RELEASE compile left `.o` files newer than their source, and a
  subsequent default `make` reused them with the wrong flags. The symptom was
  the `debug: alloc fill 0xDA` test failing (axl-mem.o built without
  `-DAXL_MEM_DEBUG`) — a *phantom* regression that cost real debugging time
  more than once, since `make tests` could not recover it either. Ask make
  where a configuration's artefacts landed rather than hardcoding the path:

  ```sh
  make -s ARCH=x64 BUILD=RELEASE print-prefix     # -> out/native-x64-release
  ```

  `make clean` removes only the current configuration's tree; `make clean-all`
  wipes every tree under `out/`.

- TLS-enabled build is green if you touched anything in `src/net/`
  (release.yml builds the published packages the same way):

  ```sh
  make ARCH=x64 BUILD=RELEASE
  ```

- **clang-tidy is clean locally.** The CI workflow's `lint` job
  runs `clang-tidy` with `WarningsAsErrors: '*'`; the unit /
  integration suites above don't exercise it. This step has been
  the post-tag failure mode TWICE in a row (v0.18.0 → v0.18.1,
  v0.19.0 → v0.19.1): Release published artifacts cleanly, then
  CI flagged a finding and required a follow-up patch release.
  Run it the same way CI does, before tagging, so any findings
  surface BEFORE the artifacts are public — just use the wrapper:

  ```sh
  scripts/lint.sh      # bear + clang-tidy, exactly as ci.yml's lint job
  ```

  Fix any errors and re-run. If the only findings are warnings
  (not errors), CI is fine — `WarningsAsErrors: '*'` in
  `.clang-tidy` only escalates the checks the config enables; the
  `2 warnings generated` lines per file are noise.

  **Version skew — local-clean does NOT imply CI-clean (unless versions
  match).** CI pins **`clang-tidy-21`** by running the `lint` job inside an
  **`ubuntu:26.04` container** (26.04 is the first Ubuntu whose apt ships
  clang-tidy-21 natively; 24.04 tops out at 20). `scripts/lint.sh` pins the
  same `CT_VERSION=21` — so a clean `scripts/lint.sh` *does* mean a clean CI
  lint when your local clang-tidy is 21. Three ways to get a matching 21:
  - **Ubuntu 26.04+:** `sudo apt-get install clang-tidy-21`.
  - **EL/Fedora/macOS dev boxes:** the distro's current `clang-tidy` is often
    already 21 — `scripts/lint.sh` detects that and uses it without warning
    (it only warns on a *real* mismatch).
  - **Any host:** run CI's exact lint in the same image —
    `podman run --rm -v "$PWD":/src:ro ubuntu:26.04 …` (the container `ci.yml`'s
    lint job uses), so local == CI byte-for-byte regardless of distro.

  This pinning exists because a *newer* local clang-tidy silently passed code an
  older CI clang-tidy flagged — v1.0.0 hit exactly that
  (`bugprone-sizeof-expression` on a correct array-of-pointers `sizeof`). When
  you intentionally move clang-tidy versions, bump it in both `ci.yml` (the
  `clang-tidy-NN` install + invocation, and the `ubuntu:NN.NN` container if the
  new version needs a newer base) and `scripts/lint.sh`'s `CT_VERSION` together.
  Either way, the §4b CI-on-`main` run remains the authoritative gate.

- **Doxygen has the same version-skew trap, with no pin available.**
  `docs.yml` installs whatever doxygen `ubuntu-latest`'s apt ships (1.9.8 at
  writing); a dev box is usually far newer, and reference resolution differs —
  1.13 resolved two link targets 1.9.8 could not, so `scripts/build-docs.sh`
  called the zero-warning gate clean while the v3.2.0 Docs run failed on both.
  Docs is best-effort for the release itself (artifacts come from Release), but
  the published site does not update until it is fixed. Before a release that
  touches public headers, reproduce CI's version:

  ```sh
  podman run --rm -v "$PWD":/src:z -w /src ubuntu:24.04 bash -c \
    'apt-get update -qq && apt-get install -y -qq doxygen && \
     cd docs/sphinx && doxygen Doxyfile'
  ```

  **Run clang-tidy one file per process (`-n1`).** Passing many
  TUs to a single `clang-tidy` invocation makes the path-sensitive
  `clang-analyzer-*` checks (notably `security.ArrayBound`)
  non-deterministic: the same clean tree intermittently reports
  spurious out-of-bounds findings that vanish when each file is
  analyzed alone with full budget. `-n1 -P"$(nproc)"` is
  deterministic, parallel, and still catches real bugs (they
  reproduce per-file). A bare `xargs -0 clang-tidy` (batched)
  is the flaky form — don't use it.

  **What `scripts/lint.sh` covers, and what it deliberately does not.**
  It runs three passes over one `bear`-generated compile database:

  1. `clang -fsyntax-only -Wall -Wextra` over every project TU
     (`scripts/check-clang-warnings.py`). The tree builds with **gcc**, and
     clang-*tidy* reports `.clang-tidy`'s checks, not the clang frontend's
     own `-W` diagnostics — so before this pass a clang-only compiler
     warning had nowhere to surface, and a `-Wformat` "zero field width"
     reached a commit that way. One category is suppressed:
     `-Wmissing-field-initializers` (60 of the 75 `-Wextra` findings; in C a
     partial initializer zero-fills the rest by language guarantee, so it is
     a style opinion, not a defect signal). Everything else `-Wextra` brings
     is enforced.
  2. clang-tidy over `src/` — the full `.clang-tidy` config.
  3. clang-tidy over `test/unit/` **and `tools/`** — **`bugprone-*` only**
     (`--checks='-clang-analyzer-*'`, which subtracts the analyzer from the
     shared config so the disabled-check list stays single-sourced).

  Pass 3 is narrowed on purpose, and the numbers are why. Both directories
  are **0** findings at `bugprone-*` and both are far from clean at the full
  config, for the same reason with different specifics:

  | Scope | `bugprone-*` | full config | what the difference is |
  |---|---|---|---|
  | `test/unit/` | 0 | ~40 | the analyzer objecting to what unit tests do deliberately — casting `99` to an enum to pin the bad-enum error path, indexing a buffer the test has already asserted non-NULL |
  | `tools/` | 0 | 10 | 4 × `security.PointerSub` on ordinary buffer arithmetic, 2 × garbage-value in `mkfixture.c`/`sysinfo.c`, and a `core.DivideZero` in `crashtest.c` — a tool whose entire purpose is to divide by zero |

  Enabling the analyzer over either would mean a red gate or ~50
  suppressions that bury the real findings.

  `tools/` was measured at **3** `bugprone-*` findings before it was turned
  on, and the three were resolved individually rather than waived as a
  group:

  - `tools/sysinfo.c` — `bugprone-switch-missing-default-case` on the SMBIOS
    memory-type decode. A real, if trivial, omission: **fixed** with an
    explicit `default: break;`, which is also the clearer code, since the
    switch is over a raw firmware byte and unlisted values are the expected
    case rather than an oversight.
  - `tools/sed.c` ×2 — `bugprone-branch-clone`, both *semantically distinct*
    cases that happen to share an implementation (`:` label-definition vs the
    `b`/`t`/`T` jumps; `A_ZERO` vs `default`). Merging them would delete
    intent, so each carries a **targeted** `NOLINTBEGIN/END(bugprone-branch-clone)`
    with the reason written out above it — the same shape `src/format/axl-format.c`
    already uses for its `long` / `long long` `va_arg` branches. The check
    stays enabled everywhere else.

  **Not covered (measured, deferred — not an oversight):**
  - `test/integration/`, `test/fuzz/` — absent from the compile database
    (`make tests tools` does not build them), so clang-tidy falls back to
    default flags and emits nonsense `'foo.h' file not found` errors rather
    than real analysis. Getting them into the DB is the prerequisite.

## Which flow: from `main`, or from a tag?

Two flows, and picking the wrong one is the most expensive mistake in this
document. Ask one question:

> **Does `main` contain anything that should NOT go out in this release?**

**No — release the tip of `main`.** Use `scripts/cut-release.sh X.Y.Z`. This is
the flow the script was written for and the rest of this document describes.

**Yes — cut from the previous TAG on a release branch.** The script cannot do
this: it hard-requires `main` (`git branch --show-current == main`) and
hardcodes `git push origin main`, so running it from a release branch would
push that branch's commits onto `main`. Follow "Cut the release" below by hand,
with the differences in the next section.

This is not an exotic case. **v3.2.1, v3.2.2 and v3.2.3 were all cut from tags**
— every release since v3.2.0 — because `main` was accumulating a toolchain and
C++ rework that had no business in a patch. The script models
"release = tip of `main`", and this project frequently does not work that way.

### Why this matters more than it looks

`## Unreleased` is **branch-wide state**; a release is a **commit range**. They
agree only when the release is "everything on this branch since the last tag".
`cut-release.sh` dates whatever sits under `## Unreleased` — so on a branch
where those two have diverged, it will happily stamp unrelated work into the
release notes and the tag.

That is not hypothetical. v3.2.3 was first cut from `main` and dated **43
commits** of in-progress work into a patch release, two of them under a
`### Breaking` heading, publishing eight assets before anyone read the version
number against the content. It was deleted (zero downloads) and re-cut from the
`v3.2.2` tag.

`scripts/check-release-semver.sh` now refuses that specific shape — a
`### Breaking` section under a non-major bump — from inside `cut-release.sh`.
It is a backstop, not a substitute for picking the right flow: it catches a
mislabelled *version*, not unrelated work that happens to be non-breaking.

### Cutting from a tag — what differs

Steps 1, 2, 3, 5, 6 and 7 below are **identical**. Only the branch handling
changes:

```sh
# instead of working on main:
git worktree add ~/axl-sdk-X.Y.Z -b release-X.Y.Z vPREV   # e.g. v3.2.2
cd ~/axl-sdk-X.Y.Z

# cherry-pick or apply ONLY the change this release is for, then
# steps 1-3: bump-version.sh, date the CHANGELOG, commit "release: vX.Y.Z"

# step 4 becomes: push the RELEASE BRANCH, not main
git push -u origin release-X.Y.Z

# steps 5-7 unchanged: tag, push the tag, watch, confirm
```

Two things to get right:

- **The `## Unreleased` section on the release branch is yours to write.** The
  tag's CHANGELOG will not have one (it was dated at that release), so add a
  heading carrying *only* this release's entries. Do not copy `main`'s
  accumulator across; that is the mistake this whole section exists to prevent.
- **Land it on `main` afterwards.** The release branch is not a long-lived
  fork. Cherry-pick the change onto `main` as one squashed commit with the
  version appended to the subject — the pattern `e98a4f6a` (v3.2.1) and
  `d64e15d1` (v3.2.2) set. Expect `CHANGELOG.md` to be the only conflict: keep
  `main`'s `## Unreleased` intact and place the newly-dated section beneath it.

## Cut the release

### 1. Bump the version

```sh
scripts/bump-version.sh X.Y.Z
```

This updates **both** `VERSION` and `include/axl/axl-version.h` in
one shot. Don't edit either file by hand — the Makefile's
`check-version` target compares them and refuses to build on
mismatch. If you've ever seen
`ERROR: VERSION (X.Y.Z) != axl-version.h (P.Q.R)` in CI, this is
why.

### 2. Date the CHANGELOG

`CHANGELOG.md` accumulates under a `## Unreleased` heading during
development. At release time, rename it:

```diff
- ## Unreleased
+ ## X.Y.Z — YYYY-MM-DD
```

Sweep `git log <prev-tag>..HEAD --oneline` and confirm every
user-visible change has an entry. Headlines worth catching:

- New public API (Added)
- Behavior changes that callers might trip on (Changed, Migration)
- Bug fixes — especially anything that was a UAF, leak, or
  flake (Fixed)
- Build/CI/Docs/Examples changes that affect contributors

### 3. One commit for the release metadata

```sh
git add VERSION include/axl/axl-version.h CHANGELOG.md
git commit -m "release: vX.Y.Z"
```

Keep this commit small. The release-cut commit is the canonical
"what changed in this release" reference; if it's noisy with code
changes, the diff-against-previous-tag becomes harder to read.

### 4. Push the release branch first

```sh
git push origin main                 # main flow
git push -u origin release-X.Y.Z     # tag flow
```

The branch must contain the release-metadata commit *before* the tag
points at it; if you tag first and then push the branch, the
release.yml workflow can race and check out the wrong commit.
`release.yml` triggers on any `v*` tag regardless of which branch
carries it, so the tag flow publishes exactly the same way.

### 4b. The gate is the LOCAL suite — CI no longer auto-runs on a push

> **Updated policy** (was "wait for CI green before tagging"). CI does **not**
> run on a push to `main` anymore (see "GitHub Actions trigger policy" above), so
> there's nothing to wait for on the branch. The authoritative gate is the
> **local** suite — run it before you cut:
>
> ```sh
> ./test/integration/run-integration.sh --no-cache -j"$(nproc)"   # 0 failures required
> scripts/lint.sh                                       # clang-tidy as CI runs it
> ```

Get the cross-OS CI backstop *before* tagging — run
`scripts/cut-release.sh X.Y.Z --ci-gate`, which dispatches CI on `main` (the
release commit) and waits for green before creating the tag. Without `--ci-gate`,
the cut tags immediately (the local suite was the gate). CI is **not** re-run by
the tag; a **major** (`vX.0.0`) tag additionally triggers `docs.yml` as a
post-publish step, and `cut-release.sh` watches Release (+ Docs on a major).

Why the change: the suite is run locally before every release anyway, so a
per-push CI gate was redundant cost. The two CI-only failures that burned v1.0.0
(a headless-runner pointer test, a clang-tidy version skew) are now handled
structurally: the pointer test is `local-only` (excluded from CI), and
`scripts/lint.sh` pins the exact CI clang-tidy version — run it locally and it
matches CI byte-for-byte.

### 5. Create and push the tag

```sh
git tag -a vX.Y.Z -m "vX.Y.Z

<headline paragraph from CHANGELOG>

  - bullet 1
  - bullet 2
  - bullet 3

See CHANGELOG.md for the full list."

git push origin vX.Y.Z
```

The annotated tag's body shows up in the GitHub Release page and
in `gh release view vX.Y.Z`. Worth a few minutes of polish.

### 6. Watch the workflows

The tag push triggers **two** workflows (and on a minor/patch, one):

> **CI is NOT one of them**, despite what this section said until 2026-08-18.
> `ci.yml` is `workflow_dispatch` only — see the trigger table above. A tag
> re-running CI on a commit already validated on `main` was pure duplication,
> and that trigger was removed. This paragraph outlived it.

- **Release** (`.github/workflows/release.yml`) — builds .deb +
  .rpm via `fpm` (both x64 and aa64), pulls iPXE from upstream
  for the host-tools tarball, attaches everything to a GitHub
  Release on `aximcode/axl-sdk-releases`.
- **Docs** (`.github/workflows/docs.yml`) — Doxygen + Sphinx
  build + Cloudflare Pages deploy.

**Realistic timing.** The TAG's own workflows are fast —
**~4–5 minutes wall-clock** for Release + Docs, which run in
parallel. Verified on v0.9.0: Release 2m57s, Docs 1m44s.
Release's Build-tools-aa64 (~3 min) is the longest of them,
slower than x64 because cross-tool execution goes through QEMU
user-mode emulation.

**The CI backstop is the slow one, and it is a separate wait.**
`--ci-gate` dispatches ci.yml on the release commit before
tagging **unless the local stamp already covers it** (see above),
and its QEMU job runs the WHOLE integration suite, each test in
its own QEMU. Measured **~50 minutes** on the v3.2.0 cut; the
other three jobs finished in under 6.

That wall time is not a repo defect: `run-integration.sh` picks
`nproc - 2` workers, and a hosted runner has 2 cores, so it
evaluates to one worker. The same unchanged line gives six on an
8-core box. `AXL-CI-Release-Speed-Design.md` §10 has the
measurement and what to do about it; the stamp above is the part
that is already built. `wait_for_ci` allows 75 minutes. If it ever times out on
a run that is still going, the tag was NOT created — wait for CI
yourself and finish with `--resume`, rather than re-cutting.

**Pathological case — bad-DNS days.** When GitHub Actions runner
DNS is flaky (azure.archive.ubuntu.com mirrors), `apt-get
install` retries can stall jobs at **30–40 minutes** before
giving up. The v0.9.0 first attempt hit this — five jobs across
CI/Release/Docs all failed at exit-code 100 (apt's "couldn't
fetch packages") after 18–40 minute stalls. The workflows now
write `Acquire::Retries=3` and `Acquire::http(s)::Timeout=15`
into `/etc/apt/apt.conf.d/99-axl-retry` as the first action of
every install-deps step — that bounds the worst case to ~5
minutes on bad days instead of 30+. Even with the bound, plan
for **up to ~15 minutes total** if a re-run is needed during a
runner network blip.

**Workflow notes:**

- AArch64 builds run on x86 hosted runners. The library + tests
  + tools cross-compile fine in 1-3 min each; QEMU emulation
  only kicks in when something needs to *execute* an aa64 binary
  (e.g. CI's QEMU integration tests).
- The `Docs` workflow occasionally hits transient apt-mirror
  failures even on otherwise-healthy days. Re-running just that
  workflow is the fix; it's not a release blocker since
  artifacts come from `Release`, not `Docs`.

**The default — and what to use unless you have a specific
reason to stream live output:**

```sh
scripts/watch-release-runs.sh
```

That's it. The script polls all three workflows via GraphQL
(separate quota pool from REST) at 60-second intervals, prints
status per workflow per tick, exits rc=0 only when all three
reach SUCCESS, rc=1 if any finish non-SUCCESS. Total cost:
~60 GraphQL calls/hour. **Use this. Don't reach for `gh run
watch`** unless you have a specific reason to stream
per-step output.

CI and Release **must** finish green for the release to be
considered shipped. Docs is best-effort — re-run later if it
flakes.

#### Why not `gh run watch`?

GitHub's REST API has a 5,000 req/hour cap per authenticated
user. `gh run watch --exit-status` is a polling loop disguised
as a stream — it makes ~1 request every 3 seconds. Three
parallel watchers (CI + Release + Docs) for a 30-minute
multi-arch workflow generates **~1,800 calls**; add ad-hoc
`gh run view` / `gh run list` during diagnosis and you'll
exhaust the quota mid-flight. When that happens the watcher
exits **rc=1** on the rate-limit error, which is
indistinguishable from "the run failed" by exit code alone.

GitHub does NOT sell a per-account rate-limit boost; the only
escape is Enterprise Cloud (15,000/hr) or a GitHub App on a
multi-installation org (up to 12,500/hr).

`scripts/watch-release-runs.sh` sidesteps the entire issue —
GraphQL has its own 5,000-points/hr quota that REST polling
doesn't touch, and a status query costs ~1 point. ~60 calls
per hour is well below the limit even if you forget to stop
the loop.

#### The `gh run watch` fallback

When you need streaming per-step output (e.g., debugging a
specific job's failure), use `gh run watch` for **one
workflow at a time** — never three in parallel. Pick the
slowest (Release):

```sh
gh run list --commit "$(git rev-parse HEAD)" \
    --json databaseId,workflowName,status --limit 5
gh run watch <release-id>
```

Critical caveat: **`gh run watch` follows ALL jobs in the
workflow**, not just the first one. Don't be misled by an
early `✓ Complete job` line in the output — multi-arch
workflows have several jobs, and the watcher keeps running
until the slowest one finishes.

Two corollaries:

1. **The streamed `✓ Complete job` lines are per-individual-job,
   NOT workflow completion.** A multi-arch workflow has several
   jobs (Build tools x64 → Build tools aa64 → Build axl-sdk →
   Build host-tools → publish); seeing `✓ Complete job` only
   means one of those finished. Don't conflate with workflow
   conclusion.
2. **Cross-check via GraphQL after rate-limit hits.** GraphQL
   has a separate 5000 req/h quota that REST polling doesn't
   touch:

   ```sh
   gh api graphql -f query='
   { repository(owner: "aximcode", name: "axl-sdk") {
       object(expression: "<sha>") {
         ... on Commit { checkSuites(first: 10) {
           nodes { workflowRun { workflow { name } } status conclusion }
   } } } } }'
   ```

   Returns `status=COMPLETED, conclusion=SUCCESS|FAILURE` for
   each workflow without consuming REST quota. Use this whenever
   `gh run watch` exits non-zero or `gh run view` 403s — it's
   the authoritative source while the REST limit recovers
   (typically 1 hour from the first rate-limit error).

### 7. Confirm

The Release is published on the public sibling repo
`aximcode/axl-sdk-releases`, not on the private upstream — pass
`--repo` to `gh release view`:

```sh
gh release view vX.Y.Z --repo aximcode/axl-sdk-releases
```

Should show the release page with `axl-sdk.deb`, `axl-sdk.rpm`,
`axl-sdk-tools-{x64,aa64}.tar.gz`,
`axl-sdk-host-tools.{tar.gz,deb}`, and `SHA256SUMS` attached.

The `.deb` / `.rpm` packages include the full C and C++ surface
when CI builds with BOTH bare-metal toolchains cached
(`scripts/install-toolchain.sh all` — C and C++ compile
bare-metal on both arches, so neither is optional any more).
Specifically, each package contains:

- C bits (always): `axl-cc` driver, `libaxl.a` per arch,
  `axl.h` + `axl/*.h` headers, CRT0 objects, linker scripts,
  CMake config, pkg-config, JSON5 sidecars.  The `axl-c++` driver
  is here too — it is a dependency-free `exec axl-cc -x c++`
  wrapper, so it ships unconditionally; without the C++ glue it
  simply reports which install step is missing.
- C++ bits (when toolchain present at build time): the four
  `axl-cxxrt-*.o` glue objects per arch, plus the C++ headers.
  The package conveys no libstdc++ — `axl-cc` names the
  consumer's installed copy — so there is no runtime-dependency
  escalation; pure-C consumers can ignore the extra files.

CI cache invalidation: both toolchain tarballs are keyed on
`hashFiles('scripts/axl-toolchains.conf')`, so ANY edit to that
manifest forces a re-fetch of both — which is also why a
toolchain version bump must be PUBLISHED before the commit that
bumps it lands, or every CI job downloads a URL that 404s.
Verify both packages contain the C++ glue after a release build.
`axl-cxxrt-terminate.o` is the one to grep for: it is the only
member that is C++-only, so its absence means exactly "no C++
half".

```sh
dpkg-deb -c axl-sdk_*_amd64.deb | grep axl-cxxrt
rpm -qpl axl-sdk-*.x86_64.rpm | grep axl-cxxrt
```

If `axl-cxxrt-terminate.o` is missing, the build host either didn't have
the bare-metal toolchains available or `install.sh`'s
auto-detect failed.  See
[`AXL-SDK-Design.md` §"C++ support"](AXL-SDK-Design.md) for the
toolchain story.

### 8. Get back on `main`

```sh
git checkout main      # if `git status` shows detached HEAD
git status             # confirm: "On branch main, working tree clean"
```

`git tag -a` and `git push origin <tag>` themselves don't detach
HEAD, but a typical release session involves enough sideband
operations (rebases, fix-ups during step 5's tag-message edit,
`git checkout vX.Y.Z` to spot-check the tagged tree before
pushing) that it's worth verifying. Subsequent commits made
while detached will be invisible to `git push origin main`,
which is the silent-data-loss case to avoid.

## Recovery: a failed release tag

If a workflow fails on a freshly-pushed tag and **no GitHub Release
has been created yet**, it's safe to re-cut:

1. Verify no release exists: `gh release view vX.Y.Z` should print
   `release not found`.
2. Land the fix on `main` as a normal commit.
3. Move the tag:

   ```sh
   git tag -d vX.Y.Z
   git push origin :refs/tags/vX.Y.Z
   git tag -a vX.Y.Z -m "<original tag message>"
   git push origin vX.Y.Z
   ```

4. Watch the new workflow runs (step 6) and confirm (step 7).

If a GitHub Release **does** exist for the tag (i.e. release.yml
ran past the `gh release create` step before failing), re-cutting
the same version is no longer clean: consumers may have already
downloaded the partial artifacts. Bump to the next patch version
instead.

## Why these steps

The order is a recovery from each pitfall we've actually hit:

- `bump-version.sh` exists because the Makefile's `check-version`
  target catches stale `axl-version.h` on every build. Cut a
  release without it and the first `make` invocation in CI fails;
  v0.2.6 was re-cut for exactly this reason.
- "Push main before the tag" exists because release.yml clones the
  *tag*, but the tag points to a commit that has to be reachable
  via the branch ref the runners pull from.
- "One commit for release metadata" exists because the tag-to-tag
  diff is the canonical changelog input on the next release.

## See also

- [`scripts/bump-version.sh`](../scripts/bump-version.sh) — the
  atomic version helper.
- [`.github/workflows/release.yml`](../.github/workflows/release.yml)
  — the deb/rpm build and publish workflow.
- [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) — the
  per-push integration suite that gates a release.
- [`CHANGELOG.md`](../CHANGELOG.md) — what's shipped, by tag.
