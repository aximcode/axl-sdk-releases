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

- Both archs build clean against `BUILD=RELEASE`:

  ```sh
  make ARCH=x64 BUILD=RELEASE
  make ARCH=aa64 BUILD=RELEASE
  ```

- TLS-enabled build is green if you touched anything in `src/net/`
  (release.yml hardcodes `AXL_TLS=1` for the published packages):

  ```sh
  AXL_TLS=1 make ARCH=x64 BUILD=RELEASE
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

The tag push triggers two workflows on the same commit:

- **CI** (`.github/workflows/ci.yml`) — full unit + integration
  suite, ~3 min on hosted runners.
- **Release** (`.github/workflows/release.yml`) — builds .deb +
  .rpm via `fpm`, attaches them to a GitHub Release, ~1.5 min.

Watch both. Prefer `gh run watch`, which streams events and exits
the moment the run terminates — no polling lag:

```sh
# List the runs on this commit, grab the IDs:
gh run list --commit "$(git rev-parse HEAD)" \
    --json databaseId,workflowName,status

# Then live-stream each one to terminal:
gh run watch <ci-run-id>
gh run watch <release-run-id>
```

If you'd rather poll once and move on, `gh run list --commit ...`
gives a one-shot snapshot. Both must finish green before the
release is considered shipped.

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
