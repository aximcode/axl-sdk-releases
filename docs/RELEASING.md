# Releasing AXL

Step-by-step for cutting an AXL release. The flow is opinionated and
the order matters — running it out of order, or skipping the version
helper, has burned us before.

## TL;DR — the fast path

```sh
scripts/cut-release.sh X.Y.Z            # do it
scripts/cut-release.sh X.Y.Z --dry-run  # preview, change nothing
```

`scripts/cut-release.sh` automates the cut: it bumps the version, dates the
CHANGELOG, commits + pushes `main`, tags, and watches Release/Docs to the
published release. By default it does **not** wait on CI (see the gate below).

### The gate is LOCAL — run the suite before you cut

**CI is no longer a per-push gate.** `ci.yml` runs only on a **major tag
(`vX.0.0`)** or a manual `workflow_dispatch` — a rare cross-OS backstop, to keep
Actions minutes for the runs that matter. The **authoritative pre-release gate
is the full suite run locally**, which is fast in parallel:

```sh
make ARCH=x64 AXL_TLS=1 all tests tools axl-busybox   # one consistent-flag build
./test/integration/run-integration.sh -j"$(nproc)"    # ~6-7x vs serial; 79/79 must pass
scripts/lint.sh                                        # clang-tidy exactly as CI runs it
```

`run-integration.sh` discovers every `test-*.sh` (including the patched-QEMU /
real-pointer `local-only` tests, which your dev box CAN run) and runs each in
its own QEMU. Green here is the release gate. Then:

```sh
scripts/cut-release.sh X.Y.Z            # cut (no CI wait — local suite was the gate)
scripts/cut-release.sh X.Y.Z --dry-run  # preview, change nothing
scripts/cut-release.sh X.Y.Z --ci-gate  # opt back in: wait for a (manually-triggered) CI run
```

For a **major** release, trigger CI by hand first (Actions tab →
`workflow_dispatch`, or it fires on the `vX.0.0` tag) for the fresh-OS backstop;
`--ci-gate` makes the cut wait on it. The `--ci` flag on the runner excludes the
`local-only` tests CI runners can't execute (patched-QEMU SMBus, usb-mouse
pointer) — that's what CI runs.

### GitHub Actions trigger policy

**No workflow runs on a code push.** Push to `main` (or any branch) as much as
you like — zero Actions minutes. The triggers are baked into the workflow files:

| Workflow | What | Triggers on |
|---|---|---|
| **ci.yml** | build + QEMU integration + lint | major tag `vX.0.0`, or manual (`workflow_dispatch`) |
| **docs.yml** | Doxygen/Sphinx → Cloudflare Pages | major tag `vX.0.0`, or manual |
| **release.yml** | build + publish `.deb`/`.rpm`/tarballs | **every** release tag `v*` (it's the publish step), or manual |

So: a normal push runs nothing; a **patch/minor** release tag runs only
`release.yml` (publish); a **major** (`vX.0.0`) tag also runs CI + Docs as a
cross-OS backstop. `cut-release.sh` tells its watcher which to expect, so a
minor/patch cut doesn't hang waiting for CI/Docs.

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

- You're on `main`, working tree clean, `git log origin/main..HEAD`
  shows the commits to ship.
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

- Both archs build clean against `BUILD=RELEASE`. Use a separate
  `PREFIX` so the RELEASE-flagged `.o` files don't shadow the
  in-tree DEBUG cache the integration tests above reuse — the
  `.o` cache key is the `.c` timestamp only, not the `BUILD`
  mode, so a same-prefix RELEASE compile leaves `.o` files newer
  than the `.c` source and a subsequent default `make` reuses
  them with the wrong flags. Symptom: the
  `debug: alloc fill 0xDA` test fails (axl-mem.o built without
  `-DAXL_MEM_DEBUG`). `scripts/install.sh` uses the `-release`
  prefix internally for the same reason.

  ```sh
  make ARCH=x64  BUILD=RELEASE PREFIX=out/native-x64-release
  make ARCH=aa64 BUILD=RELEASE PREFIX=out/native-aa64-release
  ```

- TLS-enabled build is green if you touched anything in `src/net/`
  (release.yml hardcodes `AXL_TLS=1` for the published packages):

  ```sh
  AXL_TLS=1 make ARCH=x64 BUILD=RELEASE PREFIX=out/native-x64-release
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

  **Run clang-tidy one file per process (`-n1`).** Passing many
  TUs to a single `clang-tidy` invocation makes the path-sensitive
  `clang-analyzer-*` checks (notably `security.ArrayBound`)
  non-deterministic: the same clean tree intermittently reports
  spurious out-of-bounds findings that vanish when each file is
  analyzed alone with full budget. `-n1 -P"$(nproc)"` is
  deterministic, parallel, and still catches real bugs (they
  reproduce per-file). A bare `xargs -0 clang-tidy` (batched)
  is the flaky form — don't use it.

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

### 4. Push `main` first

```sh
git push origin main
```

`main` must contain the release-metadata commit *before* the tag
points at it; if you tag first and then push the branch, the
release.yml workflow can race and check out the wrong commit.

### 4b. The gate is the LOCAL suite — CI no longer auto-runs on a push

> **Updated policy** (was "wait for CI green before tagging"). CI does **not**
> run on a push to `main` anymore (see "GitHub Actions trigger policy" above), so
> there's nothing to wait for on the branch. The authoritative gate is the
> **local** suite — run it before you cut:
>
> ```sh
> ./test/integration/run-integration.sh -j"$(nproc)"   # 79/79 must pass
> scripts/lint.sh                                       # clang-tidy as CI runs it
> ```

For a **major** (`vX.0.0`) release you may want the cross-OS CI backstop *before*
tagging — run `scripts/cut-release.sh X.Y.Z --ci-gate`, which dispatches CI on
the release commit and waits for green before creating the tag. Without
`--ci-gate`, the cut tags immediately (the local suite was the gate); for a major
tag, CI + Docs then run automatically *on the tag* as a post-publish backstop,
and `cut-release.sh` watches them.

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

The tag push triggers three workflows on the same commit:

- **CI** (`.github/workflows/ci.yml`) — full unit + integration
  suite across both architectures.
- **Release** (`.github/workflows/release.yml`) — builds .deb +
  .rpm via `fpm` (both x64 and aa64), pulls iPXE from upstream
  for the host-tools tarball, attaches everything to a GitHub
  Release on `aximcode/axl-sdk-releases`.
- **Docs** (`.github/workflows/docs.yml`) — Doxygen + Sphinx
  build + Cloudflare Pages deploy.

**Realistic timing.** On healthy GitHub-runner infrastructure
the whole flow is **~4–5 minutes wall-clock** (parallel across
the three workflows). Verified on v0.9.0's successful retry:
Release 2m57s, Docs 1m44s, CI 4m10s. The longest individual
jobs are CI's QEMU integration tests (~4 min) and Release's
Build-tools-aa64 (~3 min, slower than x64 due to QEMU user-mode
emulation of cross-tool execution). All other jobs finish in
under 2 minutes.

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
when CI builds with the ARM bare-metal toolchain cached
(`scripts/install-arm-toolchain.sh`).  Specifically, each
package contains:

- C bits (always): `axl-cc` driver, `libaxl.a` per arch,
  `axl.h` + `axl/*.h` headers, CRT0 objects, linker scripts,
  CMake config, pkg-config, JSON5 sidecars.
- C++ bits (when toolchain present at build time): `axl-c++`
  driver, `libaxl-cxx.a` per arch.  `libaxl-cxx.a` doesn't link
  libstdc++ so there's no runtime-dependency escalation on the
  package — pure-C consumers can ignore the extra files.

CI cache invalidation: the ARM toolchain tarball is keyed on
its pinned version (`14.3.rel1` at writing).  Bump the pin in
`scripts/install-arm-toolchain.sh` to force a re-fetch.  Verify
both packages contain `libaxl-cxx.a` after a release build:

```sh
dpkg-deb -c axl-sdk_*_amd64.deb | grep libaxl-cxx
rpm -qpl axl-sdk-*.x86_64.rpm | grep libaxl-cxx
```

If `libaxl-cxx.a` is missing, the build host either didn't have
the ARM bare-metal toolchain available or `install.sh`'s
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
