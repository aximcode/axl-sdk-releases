# Releasing AXL

Step-by-step for cutting an AXL release. The flow is opinionated and
the order matters — running it out of order, or skipping the version
helper, has burned us before.

## Prerequisites

- You're on `main`, working tree clean, `git log origin/main..HEAD`
  shows the commits to ship.
- The integration suite passes locally:

  ```sh
  ./test/integration/test-axl.sh
  ./test/integration/test-tools.sh
  ./test/integration/test-tcp-echo.sh
  ./test/integration/test-http.sh
  ./test/integration/test-cpu-idle.sh
  ```

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
