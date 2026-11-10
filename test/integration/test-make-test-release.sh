#!/bin/bash
# test-meta: arch=none needs=git,python3 est=20
# test-make-test-release.sh -- scripts/make-test-release.sh publishes a test
# build as a Pre-release on aximcode/axl-sdk-releases, and prunes old ones.
# Its two dangerous halves are the ones checked here, with no network:
#
#   tag-format     the tag is test-<YYYYMMDD>-<8-hex sha> of THIS checkout,
#                  never a v* tag that would fire a release workflow
#   prune          keeps the KEEP newest VALID test pre-releases and deletes
#                  the rest of THOSE -- never v4.7.3, test-foo, or a
#                  test-shaped tag that is a full release
#   prune-control  keep 6 deletes nothing (and `prune` proves the stub sees
#                  deletes at all, so this zero is not a blind one)
#   dirty-tree     a tracked modification refuses, with the exact message,
#                  which names --dirty
#   untracked      an untracked file under share/ or third_party/ -- which
#                  the build's globs would ship -- refuses, naming it; an
#                  untracked file elsewhere does not
#   pushed/local   HEAD on origin/main: a plain Commit line, no warning. HEAD
#                  not on it: a warning, and the Commit line says so
#   tag-exists     a published release on the tag refuses, saying to use
#                  --update or --delete; a leftover DRAFT (a create that died
#                  part way) is deleted and the build goes on
#   gate-fails     a failed release-image gate publishes nothing
#   publish-args   --dry-run prints the exact create line (pre-release, never
#                  latest), and the two POSTs that would make the tag's own
#                  commit, and runs neither create, delete nor any POST
#   own-commit     a real (stubbed) run: the exact gh calls, in order -- the
#                  releases repo's main read, a NEW commit of main's tree with
#                  main as its parent, the tag ref pointed at THAT commit, the
#                  release created on that tag (--verify-tag), then the prune.
#                  No call writes refs/heads (main is never moved); the read
#                  of refs/heads/main is the control that the grep sees one
#   create-fails   a failed release create deletes the tag ref it made, and
#                  prunes nothing; when that delete fails too it says so and
#                  prints the command to delete it by hand
#   bad-answer     a main ref that is not a 40-hex sha refuses, naming it,
#                  and POSTs nothing
#   dirty          --dirty: the tag is test-<date>-<sha8>-dirty-<hash8> of
#                  the patch, the patch ships as <tag>.patch (byte for byte
#                  `git diff --binary HEAD`) and in SHA256SUMS, the notes say
#                  so and list the changed files; untracked share/ still
#                  refuses
#   list           --list: the test pre-releases only, newest first, with
#                  their asset counts, from one gh api read
#   delete         --delete TAG: exactly one delete; a v* tag, a test-shaped
#                  full release and a missing tag each refuse, naming why;
#                  --delete-all deletes exactly the test pre-releases;
#                  --dry-run prints and runs none
#   update         --update [TAG]: the newest test pre-release by default;
#                  the exact gh calls when the source is unchanged (no tag
#                  move) and when it changed (a fresh commit, the tag PATCHed
#                  to it, the notes say so); a stale asset is deleted; a
#                  non-test tag refuses; --dirty ships the patch; --dry-run
#                  prints every write and runs none
#   modes          one mode at a time; --dirty only with a build
#   stale-tool     the REAL make-release-assets.sh uefi-tools over a fake tools/:
#                  a stale.efi left there (a tool since removed) is not
#                  shipped and is named; every shipped tool is; one missing
#                  shipped tool refuses
#
# A STUB `gh` on PATH logs its argv and serves a canned `release list` and
# canned git-data answers (GH_MAIN_SHA, GH_TREE_SHA, GH_NEW_SHA); build,
# staging and the QEMU gate are replaced by functions, so this needs only git.
# stale-tool runs the real staging script under a stub build wrapper
# (AXL_BUILD_WRAPPER) that answers --print-prefix with a scratch tree and
# fakes release-drivers and the RTK builds -- so it needs python3 too (the
# README generator).
#
# Usage: ./test/integration/test-make-test-release.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
TOOL="$PROJECT_DIR/scripts/make-test-release.sh"
REPO_SLUG=aximcode/axl-sdk-releases

WORK="$(mktemp -d -t axl-testrel.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() { local d="$1"; shift; if "$@"; then pass "$d"; else fail "$d"; fi; }

# --- the gh stub -----------------------------------------------------------
mkdir -p "$WORK/bin"
export GH_LOG="$WORK/gh.log" GH_LIST="$WORK/list.tsv"
export GH_MAIN_SHA=1111111111111111111111111111111111111111
export GH_TREE_SHA=2222222222222222222222222222222222222222
export GH_NEW_SHA=3333333333333333333333333333333333333333
export GH_RELEASES="$WORK/releases.tsv"
export GH_TAG_SHA=4444444444444444444444444444444444444444
export GH_TAG_MSG="" GH_ASSETS="" GH_IS_DRAFT=false GH_NOTES_BODY=""
export GH_TAG_DATES="$WORK/tag-dates.tsv"
: > "$GH_TAG_DATES"
# The exact tag-commit-date query (what GitHub orders releases by).
TAGDATESCALL="api graphql --paginate -f owner=aximcode -f name=axl-sdk-releases -f query=query(\$owner: String!, \$name: String!, \$endCursor: String) { repository(owner: \$owner, name: \$name) { refs(refPrefix: \"refs/tags/\", first: 100, after: \$endCursor) { nodes { name target { ... on Commit { committedDate } } } pageInfo { hasNextPage endCursor } } } } --jq .data.repository.refs.nodes[] | [.name, (.target.committedDate // \"\")] | @tsv"
cat > "$WORK/bin/gh" << 'STUB'
#!/bin/bash
printf '%s\n' "$*" >> "$GH_LOG"
case "$*" in
    "release list "*) cat "$GH_LIST" ;;
    "release create "*)
        # A created release is listed from then on, as on GitHub.
        [[ "${GH_CREATE_RC:-0}" == 0 ]] \
            && printf '%s\ttrue\t2026-12-31T00:00:00Z\n' "$3" >> "$GH_LIST"
        exit "${GH_CREATE_RC:-0}" ;;
    "api repos/"*"/git/ref/heads/main --jq .object.sha") echo "$GH_MAIN_SHA" ;;
    "api repos/"*"/git/commits/"*" --jq .tree.sha") echo "$GH_TREE_SHA" ;;
    "api -X POST repos/"*"/git/commits "*) echo "$GH_NEW_SHA" ;;
    "api repos/"*"/releases?per_page=100 --paginate --jq "*) cat "$GH_RELEASES" ;;
    "api repos/"*"/git/ref/tags/"*" --jq .object.sha") echo "$GH_TAG_SHA" ;;
    "api repos/"*"/git/commits/"*" --jq .message") printf '%s\n' "$GH_TAG_MSG" ;;
    "api graphql --paginate "*) cat "$GH_TAG_DATES" ;;
    "release view "*" --json isDraft --jq .isDraft") echo "$GH_IS_DRAFT" ;;
    "release view "*" --json body --jq .body") printf '%s\n' "$GH_NOTES_BODY" ;;
    "api -X DELETE repos/"*) exit "${GH_DELETE_RC:-0}" ;;
    "release view "*" --json assets --jq .assets[].name") printf '%s\n' $GH_ASSETS ;;
esac
exit 0
STUB
chmod +x "$WORK/bin/gh"
export PATH="$WORK/bin:$PATH"

# --- a scratch repo: what the script tags and refuses ----------------------
REPO="$WORK/repo"
git init -q -b main "$REPO"
echo 4.7.3 > "$REPO/VERSION"
# out/ is ignored, as in the real tree: the build writes there, and an
# unignored out/ would be an untracked top-level input to a --dirty patch.
printf 'out/\n' > "$REPO/.gitignore"
git -C "$REPO" add VERSION .gitignore
git -C "$REPO" -c user.email=t@example.invalid -c user.name=t \
    commit -q -m "fixture: the subject line"
SHORT=$(git -C "$REPO" rev-parse --short=8 HEAD)

if [[ ! -f "$TOOL" ]]; then
    echo "  FAIL: $TOOL does not exist"
    echo "make-test-release: 0 passed, 1 failed"
    exit 1
fi
# shellcheck source=scripts/make-test-release.sh
source "$TOOL"

echo "=== tag-format ==="
TAG=$(cd "$REPO" && test_release_tag)
check "the tag is test-<YYYYMMDD>-<8 hex> ($TAG)" \
    grep -qxE 'test-[0-9]{8}-[0-9a-f]{8}' <<< "$TAG"
check "the tag ends with this checkout's short sha ($SHORT)" \
    test "${TAG##*-}" = "$SHORT"
check "the tag carries today's UTC date" \
    test "${TAG:5:8}" = "$(date -u +%Y%m%d)"

echo "=== prune ==="
# Out of date order on purpose: the prune must sort, not trust the listing.
cat > "$GH_LIST" << 'LIST'
test-20261003-00000003	true	2026-10-03T10:00:00Z
v4.7.3	false	2026-09-01T10:00:00Z
test-20261006-00000006	true	2026-10-06T10:00:00Z
test-20261001-00000001	true	2026-10-01T10:00:00Z
test-foo	true	2026-09-02T10:00:00Z
test-20261005-00000005	true	2026-10-05T10:00:00Z
test-20260901-0000000a	false	2026-09-01T09:00:00Z
test-20261002-00000002	true	2026-10-02T10:00:00Z
test-20261004-00000004	true	2026-10-04T10:00:00Z
LIST
: > "$GH_LOG"
prune_test_releases 3 > "$WORK/prune.out" 2>&1
rc=$?
check "prune keep 3 exits 0 ($rc)" test "$rc" = 0
n=$(grep -c '^release list ' "$GH_LOG" || true)
check "prune lists the releases once ($n)" test "$n" = 1
check "prune lists with the brief's exact flags" \
    grep -qxF "release list --repo $REPO_SLUG --limit 200 --json tagName,isPrerelease,createdAt --template {{range .}}{{.tagName}}{{\"\\t\"}}{{.isPrerelease}}{{\"\\t\"}}{{.createdAt}}{{\"\\n\"}}{{end}}" "$GH_LOG"
grep '^release delete ' "$GH_LOG" | sort > "$WORK/deletes"
printf 'release delete %s --repo %s --cleanup-tag --yes\n' \
    test-20261001-00000001 "$REPO_SLUG" \
    test-20261002-00000002 "$REPO_SLUG" \
    test-20261003-00000003 "$REPO_SLUG" > "$WORK/want-deletes"
check "keep 3 deletes exactly the three oldest valid test pre-releases" \
    cmp -s "$WORK/deletes" "$WORK/want-deletes"
n=$(grep -c '^release delete ' "$GH_LOG" || true)
check "keep 3 runs exactly three deletes ($n)" test "$n" = 3
for t in v4.7.3 test-foo test-20260901-0000000a; do
    n=$(grep -c "^release delete $t " "$GH_LOG" || true)
    check "never deletes $t ($n)" test "$n" = 0
done

echo "=== prune-by-tag-date ==="
# A release refreshed with --update keeps its old createdAt but its tag now
# names a fresh commit: GitHub shows it by that commit's date, and so must
# the prune -- it is the NEWEST, not the first to go.
cat > "$GH_LIST" << 'LIST'
test-20261001-00000001	true	2026-10-01T10:00:00Z
test-20261002-00000002	true	2026-10-02T10:00:00Z
test-20261003-00000003	true	2026-10-03T10:00:00Z
test-20261004-00000004	true	2026-10-04T10:00:00Z
LIST
printf '%s\t%s\n' test-20261001-00000001 2026-10-07T10:00:00Z v4.7.3 2026-09-01T10:00:00Z \
    > "$GH_TAG_DATES"
: > "$GH_LOG"
prune_test_releases 3 > "$WORK/prunetd.out" 2>&1
rc=$?
check "prune by tag date exits 0 ($rc)" test "$rc" = 0
check "it reads the tag commits' dates, once" test "$(grep -cxF -- "$TAGDATESCALL" "$GH_LOG")" = 1
check "the refreshed release is kept; the oldest by tag date goes" \
    test "$(grep '^release delete' "$GH_LOG")" = \
    "release delete test-20261002-00000002 --repo $REPO_SLUG --cleanup-tag --yes"
# A GitTimestamp can carry an offset. 2026-10-04T11:00:00+02:00 is LEXICALLY
# after 2026-10-04T10:30:00Z but is 09:00 UTC -- half an hour earlier. By the
# instant, 00000004 (10:30Z, createdAt) is newer than 00000003 (09:00Z).
printf '%s\t%s\n' test-20261003-00000003 2026-10-04T11:00:00+02:00 > "$GH_TAG_DATES"
cat > "$GH_LIST" << 'LIST'
test-20261003-00000003	true	2026-10-01T10:00:00Z
test-20261004-00000004	true	2026-10-04T10:30:00Z
LIST
: > "$GH_LOG"
prune_test_releases 1 > "$WORK/prunetz.out" 2>&1
check "an offset date is ordered by its instant: the older one (09:00Z as +02:00) goes" \
    test "$(grep '^release delete' "$GH_LOG")" = \
    "release delete test-20261003-00000003 --repo $REPO_SLUG --cleanup-tag --yes"
check "test_prereleases shows every date in UTC" \
    test "$(test_prereleases "$(cat "$GH_TAG_DATES")" < "$GH_LIST" | cut -f1,3 | tr '\t' ' ')" = \
    "$(printf 'test-20261004-00000004 2026-10-04T10:30:00Z\ntest-20261003-00000003 2026-10-04T09:00:00Z')"
: > "$GH_TAG_DATES"

echo "=== prune-control ==="
: > "$GH_LOG"
prune_test_releases 6 > "$WORK/prune6.out" 2>&1
rc=$?
check "prune keep 6 exits 0 ($rc)" test "$rc" = 0
n=$(grep -c '^release delete ' "$GH_LOG" || true)
check "keep 6 deletes nothing ($n)" test "$n" = 0
n=$(grep -c '^release list ' "$GH_LOG" || true)
check "keep 6 still listed the releases ($n)" test "$n" = 1

# The heavy halves of main, replaced: what they WOULD have done is logged.
build_arch() { echo "build $1" >> "$WORK/steps"; }
stage_tarball() {   # arch version outdir
    local name
    name=$("$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools --arch "$1" \
        --version "$2" --print-name) || return 1
    echo "stage $1${MTR_RTK_DIR:+ rtk=$MTR_RTK_DIR}" >> "$WORK/steps"
    echo "fake $1" > "$3/$name"
}
gate_arch() { echo "gate $1 $(basename "$2")" >> "$WORK/steps"; return "${GATE_RC:-0}"; }

echo "=== dirty-tree ==="
echo 4.7.4 > "$REPO/VERSION"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dry-run) > "$WORK/dirty.out" 2> "$WORK/dirty.err"
rc=$?
check "a tracked modification exits 1 ($rc)" test "$rc" = 1
check "and says so, exactly" grep -qxF \
    "make-test-release: the tree has uncommitted changes; commit them first, or pass --dirty to build them as they are" "$WORK/dirty.err"
check "and built nothing" test ! -s "$WORK/steps"
echo 4.7.3 > "$REPO/VERSION"

echo "=== untracked ==="
for f in share/foo.json5 third_party/x/LICENSE; do
    mkdir -p "$REPO/$(dirname "$f")"
    echo x > "$REPO/$f"
    : > "$GH_LOG"; : > "$WORK/steps"
    (cd "$REPO" && main --dry-run) > "$WORK/untr.out" 2> "$WORK/untr.err"
    rc=$?
    check "an untracked $f exits 1 ($rc)" test "$rc" = 1
    check "and names it, exactly" grep -qxF \
        "make-test-release: untracked $f would ship in the build; commit or remove it first" \
        "$WORK/untr.err"
    check "and built nothing" test ! -s "$WORK/steps"
    rm -rf "${REPO:?}/${f%%/*}"
done

echo "=== tag-exists ==="
printf '%s\ttrue\t2026-10-20T10:00:00Z\n' "$TAG" > "$GH_LIST"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dry-run) > "$WORK/exists.out" 2> "$WORK/exists.err"
rc=$?
check "an existing tag exits 1 ($rc)" test "$rc" = 1
check "and names it, saying what to do, exactly" grep -qxF \
    "make-test-release: $TAG is already a release on $REPO_SLUG; rebuild it in place with --update $TAG, or --delete $TAG first" \
    "$WORK/exists.err"
check "and built nothing" test ! -s "$WORK/steps"
check "it asked whether that release is a draft" grep -qxF \
    "release view $TAG --repo $REPO_SLUG --json isDraft --jq .isDraft" "$GH_LOG"
# A leftover draft -- what a create that died uploading leaves -- is ours
# (test-shaped, unpublished): it is deleted, and the run goes on.
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && GH_IS_DRAFT=true main --dry-run) > "$WORK/draft.out" 2> "$WORK/draft.err"
rc=$?
check "a leftover draft on the tag does not stop the run ($rc)" test "$rc" = 0
check "it says so" grep -qxF \
    "make-test-release: $TAG has a leftover draft release (an earlier create that failed); deleting it" \
    "$WORK/draft.out"
check "and deletes it (printed under --dry-run)" grep -qxF \
    "make-test-release: dry-run: gh release delete $TAG --repo $REPO_SLUG --cleanup-tag --yes" \
    "$WORK/draft.out"
check "then builds" test -s "$WORK/steps"

: > "$GH_LIST"
VER="4.7.3-$TAG"
DIR="$REPO/out/test-release/$TAG"
T64="$DIR/axl-sdk-uefi-tools-$VER-x64.tar.gz"
TA64="$DIR/axl-sdk-uefi-tools-$VER-aa64.tar.gz"

echo "=== gate-fails ==="
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && GATE_RC=1 main) > "$WORK/gate.out" 2> "$WORK/gate.err"
rc=$?
check "a failed gate exits non-zero ($rc)" test "$rc" != 0
check "the gate did run" grep -qxF "gate x64 $(basename "$T64")" "$WORK/steps"
n=$(grep -c '^release create' "$GH_LOG" || true)
check "a failed gate publishes nothing ($n create)" test "$n" = 0
n=$(grep -c '^release delete' "$GH_LOG" || true)
check "a failed gate prunes nothing ($n delete)" test "$n" = 0

echo "=== publish-args (--dry-run) ==="
# HEAD is on origin/main here (a local ref; nothing fetches). An untracked
# file OUTSIDE the shipped globs must not refuse.
git -C "$REPO" update-ref refs/remotes/origin/main HEAD
echo notes > "$REPO/SCRATCH.txt"
# Three test releases already up: with the new one, keep 3 drops the oldest.
cat > "$GH_LIST" << 'LIST'
test-20261002-00000002	true	2026-10-02T10:00:00Z
test-20261001-00000001	true	2026-10-01T10:00:00Z
test-20261003-00000003	true	2026-10-03T10:00:00Z
LIST
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dry-run) > "$WORK/dry.out" 2> "$WORK/dry.err"
rc=$?
check "--dry-run exits 0 ($rc)" test "$rc" = 0
printf 'build x64\nbuild aa64\nstage x64\nstage aa64\ngate x64 %s\ngate aa64 %s\n' \
    "$(basename "$T64")" "$(basename "$TA64")" > "$WORK/want-steps"
check "--dry-run builds, stages and gates both arches, in order" \
    cmp -s "$WORK/steps" "$WORK/want-steps"
WANT_CREATE="make-test-release: dry-run: gh release create $TAG --repo $REPO_SLUG --verify-tag --prerelease --latest=false --title AXL\\ SDK\\ test\\ build\\ $TAG --notes-file $DIR/notes.md $T64 $TA64 $DIR/SHA256SUMS"
n=$(grep -cxF -- "$WANT_CREATE" "$WORK/dry.out" || true)
check "--dry-run prints the exact create line once ($n)" test "$n" = 1
FULL=$(git -C "$REPO" rev-parse HEAD)
WANT_COMMIT="make-test-release: dry-run: gh api -X POST repos/$REPO_SLUG/git/commits -f message=AXL\\ SDK\\ test\\ build\\ $TAG\\ \\(axl-sdk\\ $FULL\\) -f tree=$GH_TREE_SHA -f parents\\[\\]=$GH_MAIN_SHA --jq .sha"
n=$(grep -cxF -- "$WANT_COMMIT" "$WORK/dry.out" || true)
check "--dry-run prints the exact POST of the tag's own commit once ($n)" test "$n" = 1
WANT_REF="make-test-release: dry-run: gh api -X POST repos/$REPO_SLUG/git/refs -f ref=refs/tags/$TAG -f sha=NEW_COMMIT_SHA --silent"
n=$(grep -cxF -- "$WANT_REF" "$WORK/dry.out" || true)
check "--dry-run prints the exact POST of the tag ref once ($n)" test "$n" = 1
n=$(grep -c '^api -X' "$GH_LOG" || true)
check "--dry-run runs no POST/DELETE ($n)" test "$n" = 0
n=$(grep -c "^api repos/$REPO_SLUG/git/ref/heads/main --jq .object.sha\$" "$GH_LOG" || true)
check "--dry-run reads main (a GET) once ($n)" test "$n" = 1
n=$(grep '^make-test-release: dry-run: gh release create ' "$WORK/dry.out" \
    | tr ' ' '\n' | grep -cE '^v[0-9]' || true)
check "the printed create line carries no v-prefixed tag ($n)" test "$n" = 0
n=$(grep -c '^make-test-release: dry-run: gh release delete ' "$WORK/dry.out" || true)
check "--dry-run prints one delete: the new build is the newest of the 3 kept ($n)" test "$n" = 1
check "--dry-run prints the exact delete line for the oldest" grep -qxF \
    "make-test-release: dry-run: gh release delete test-20261001-00000001 --repo $REPO_SLUG --cleanup-tag --yes" \
    "$WORK/dry.out"
n=$(grep -c '^release create' "$GH_LOG" || true)
check "--dry-run runs no create ($n)" test "$n" = 0
n=$(grep -c '^release delete' "$GH_LOG" || true)
check "--dry-run runs no delete ($n)" test "$n" = 0
check "SHA256SUMS lists both tarballs, verifiably" \
    bash -c "cd '$DIR' && sha256sum -c --quiet SHA256SUMS && test \$(wc -l < SHA256SUMS) = 2"
check "the notes name the commit" \
    grep -qxF "Commit: $(git -C "$REPO" rev-parse HEAD) fixture: the subject line" "$DIR/notes.md"
n=$(grep -c '^make-test-release: warning' "$WORK/dry.err" || true)
check "HEAD on origin/main: no warning ($n)" test "$n" = 0
check "the notes say it is not a release" \
    grep -qxF "A test build for hands-on testing, not a release." "$DIR/notes.md"
check "the notes say the Source code links are not this build" \
    grep -qxF "The Source code links on this page are the last official snapshot, not this build." "$DIR/notes.md"

n=$(grep -c 'sshd-rtk' "$DIR/notes.md" || true)
check "without --with-rtk the notes do not mention sshd-rtk ($n)" test "$n" = 0

echo "=== with-rtk (--dry-run) ==="
RTK="$WORK/rtk"
mkdir -p "$RTK/x64" "$RTK/aa64"
echo rtk-x64 > "$RTK/x64/RtkUsbUndiDxe.efi"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dry-run --with-rtk "$RTK") > "$WORK/rtk.out" 2> "$WORK/rtk.err"
rc=$?
check "--with-rtk without an aa64 driver refuses ($rc)" test "$rc" = 2
check "...naming the missing file, exactly" grep -qxF \
    "make-test-release: --with-rtk: no $RTK/aa64/RtkUsbUndiDxe.efi" "$WORK/rtk.err"
check "...before building anything" test ! -s "$WORK/steps"
echo rtk-aa64 > "$RTK/aa64/RtkUsbUndiDxe.efi"
(cd "$REPO" && main --dry-run --with-rtk "$RTK") > "$WORK/rtk.out" 2> "$WORK/rtk.err"
rc=$?
check "--with-rtk --dry-run exits 0 ($rc)" test "$rc" = 0
printf 'stage x64 rtk=%s\nstage aa64 rtk=%s\n' "$RTK" "$RTK" > "$WORK/want-rtk"
check "each arch's tarball is staged with the driver directory" \
    cmp -s <(grep '^stage ' "$WORK/steps") "$WORK/want-rtk"
printf '%s\n' \
    "Each tarball also carries sshd-rtk.efi: sshd with Realtek's USB NIC driver" \
    "(RtkUsbUndiDxe.efi, Realtek's own binary, not covered by the SDK's licence)" \
    "embedded, for a box whose cabled NIC is a Realtek USB dongle the firmware has" \
    "no driver for. It is run, stopped and queried exactly as sshd.efi is." > "$WORK/want-rtk-notes"
check "the notes say sshd-rtk.efi embeds Realtek's driver, exactly" \
    bash -c "grep -A3 -xF \"Each tarball also carries sshd-rtk.efi: sshd with Realtek's USB NIC driver\" '$DIR/notes.md' | cmp -s - '$WORK/want-rtk-notes'"
(cd "$REPO" && main --with-rtk "$RTK" --list) > "$WORK/rtk2.out" 2> "$WORK/rtk2.err"
rc=$?
check "--with-rtk without a build refuses ($rc)" test "$rc" = 2
check "...exactly" grep -qxF \
    "make-test-release: --with-rtk applies to a build (the default mode or --update)" "$WORK/rtk2.err"

echo "=== local commit (not on origin/main) ==="
git -C "$REPO" -c user.email=t@example.invalid -c user.name=t \
    commit -q --allow-empty -m "fixture: a local commit"
TAG2=$(cd "$REPO" && test_release_tag)
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dry-run) > "$WORK/local.out" 2> "$WORK/local.err"
rc=$?
check "a local commit still builds (--dry-run exits 0, $rc)" test "$rc" = 0
check "it warns, exactly" grep -qxF \
    "make-test-release: warning: HEAD is not on origin/main; the notes name a commit the remote user cannot see (git push to publish it)" \
    "$WORK/local.err"
check "the notes label the commit local" grep -qxF \
    "Commit: $(git -C "$REPO" rev-parse HEAD) fixture: a local commit (local commit, not yet pushed)" \
    "$REPO/out/test-release/$TAG2/notes.md"
n=$(grep -c '^release create' "$GH_LOG" || true)
check "and it pushes nothing: no create ran ($n)" test "$n" = 0

echo "=== own-commit (a real run, stub gh) ==="
# The tag gets a commit of its own on the releases repo: main's tree, main as
# its parent, a fresh date (GitHub stamps it) -- so the release sorts by date
# at the top -- and main itself is never moved.
cat > "$GH_LIST" << 'LIST'
test-20261002-00000002	true	2026-10-02T10:00:00Z
test-20261001-00000001	true	2026-10-01T10:00:00Z
test-20261003-00000003	true	2026-10-03T10:00:00Z
LIST
FULL2=$(git -C "$REPO" rev-parse HEAD)
DIR2="$REPO/out/test-release/$TAG2"
V2="4.7.3-$TAG2"
LISTCALL="release list --repo $REPO_SLUG --limit 200 --json tagName,isPrerelease,createdAt --template {{range .}}{{.tagName}}{{\"\\t\"}}{{.isPrerelease}}{{\"\\t\"}}{{.createdAt}}{{\"\\n\"}}{{end}}"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main) > "$WORK/real.out" 2> "$WORK/real.err"
rc=$?
check "a real run exits 0 ($rc)" test "$rc" = 0
{
    echo "$LISTCALL"
    echo "api repos/$REPO_SLUG/git/ref/heads/main --jq .object.sha"
    echo "api repos/$REPO_SLUG/git/commits/$GH_MAIN_SHA --jq .tree.sha"
    echo "api -X POST repos/$REPO_SLUG/git/commits -f message=AXL SDK test build $TAG2 (axl-sdk $FULL2) -f tree=$GH_TREE_SHA -f parents[]=$GH_MAIN_SHA --jq .sha"
    echo "api -X POST repos/$REPO_SLUG/git/refs -f ref=refs/tags/$TAG2 -f sha=$GH_NEW_SHA --silent"
    echo "release create $TAG2 --repo $REPO_SLUG --verify-tag --prerelease --latest=false --title AXL SDK test build $TAG2 --notes-file $DIR2/notes.md $DIR2/axl-sdk-uefi-tools-$V2-x64.tar.gz $DIR2/axl-sdk-uefi-tools-$V2-aa64.tar.gz $DIR2/SHA256SUMS"
    echo "$LISTCALL"
    echo "$TAGDATESCALL"
    echo "release delete test-20261001-00000001 --repo $REPO_SLUG --cleanup-tag --yes"
} > "$WORK/want-real"
diff "$WORK/want-real" "$GH_LOG" >&2
check "the exact gh calls, in order: read main, new commit, tag ref, release on it, prune" \
    cmp -s "$WORK/want-real" "$GH_LOG"
n=$(grep -cE '^api -X [A-Z]+ repos/[^ ]+/git/refs/heads' "$GH_LOG" || true)
check "no call writes a branch ref: main is never moved ($n)" test "$n" = 0
n=$(grep -c 'git/ref/heads/main' "$GH_LOG" || true)
check "control: the log does carry the read of refs/heads/main ($n)" test "$n" = 1
n=$(grep -cE '^api -X (PATCH|PUT) ' "$GH_LOG" || true)
check "no PATCH or PUT at all ($n)" test "$n" = 0
check "it says which commit the tag names" grep -qxF \
    "make-test-release: $TAG2 -> $GH_NEW_SHA (main's tree, parent $GH_MAIN_SHA)" "$WORK/real.out"

echo "=== create-fails ==="
: > "$GH_LIST"   # the real run above listed its tag; start clean
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && GH_CREATE_RC=1 main) > "$WORK/cf.out" 2> "$WORK/cf.err"
rc=$?
check "a failed release create exits non-zero ($rc)" test "$rc" != 0
n=$(grep -cxF "api -X DELETE repos/$REPO_SLUG/git/refs/tags/$TAG2 --silent" "$GH_LOG" || true)
check "it deletes the tag ref it made ($n)" test "$n" = 1
n=$(grep -c '^release delete' "$GH_LOG" || true)
check "and prunes nothing ($n)" test "$n" = 0
check "and says so, exactly" grep -qxF \
    "make-test-release: release create failed; deleted the tag $TAG2 it made" "$WORK/cf.err"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && GH_CREATE_RC=1 GH_DELETE_RC=1 main) > "$WORK/cf2.out" 2> "$WORK/cf2.err"
rc=$?
check "a failed create AND a failed tag delete exits non-zero ($rc)" test "$rc" != 0
check "it reports the delete's real outcome, with the command to finish it" grep -qxF \
    "make-test-release: release create failed, and deleting the tag $TAG2 failed too; delete it by hand: gh api -X DELETE repos/$REPO_SLUG/git/refs/tags/$TAG2" \
    "$WORK/cf2.err"
n=$(grep -c 'deleted the tag' "$WORK/cf2.err" || true)
check "and does not claim it deleted the tag ($n)" test "$n" = 0

echo "=== bad-answer ==="
: > "$GH_LIST"   # the real run above listed its tag; start clean
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && GH_MAIN_SHA="" main) > "$WORK/bad.out" 2> "$WORK/bad.err"
rc=$?
check "an empty main sha exits non-zero ($rc)" test "$rc" != 0
check "and names it, exactly" grep -qxF \
    "make-test-release: $REPO_SLUG main resolved to '', not a commit sha; nothing published" \
    "$WORK/bad.err"
n=$(grep -c '^api -X' "$GH_LOG" || true)
check "and POSTs nothing ($n)" test "$n" = 0
n=$(grep -c '^release create' "$GH_LOG" || true)
check "and creates no release ($n)" test "$n" = 0

echo "=== dirty ==="
: > "$GH_LIST"
# A tracked source, committed; then a tracked edit that includes a NEW,
# untracked header -- the build reads both, so the patch must carry both.
# SCRATCH.txt (untracked, at the root, no build input) must not be in it.
mkdir -p "$REPO/src"
printf 'int a;\n' > "$REPO/src/a.c"
git -C "$REPO" add src/a.c
git -C "$REPO" -c user.email=t@example.invalid -c user.name=t commit -q -m "fixture: a source"
printf '#include "new.h"\n#include "../newlib/x.h"\nint a;\n' > "$REPO/src/a.c"
printf '#define NEW 1\n' > "$REPO/src/new.h"
# A brand-new top-level directory (in no commit yet) the edit also uses, and
# an untracked doc: docs/ is not built by a test release, so it stays out.
mkdir -p "$REPO/newlib" "$REPO/docs"
printf '#define X 1\n' > "$REPO/newlib/x.h"
printf 'notes\n' > "$REPO/docs/notes.md"
# Hidden top-level directories -- an agent's settings and worktrees, a
# secret -- are never inputs, untracked and unignored as they may be.
mkdir -p "$REPO/.claude" "$REPO/.secret"
printf '{}\n' > "$REPO/.claude/settings.json"
printf 'key\n' > "$REPO/.secret/k"
echo 4.7.4 > "$REPO/VERSION"
FULLD=$(git -C "$REPO" rev-parse HEAD)
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dirty) > "$WORK/dp.out" 2> "$WORK/dp.err"
rc=$?
check "--dirty publishes a dirty tree ($rc)" test "$rc" = 0
DTAG=$(sed -n 's/^make-test-release: published //p' "$WORK/dp.out")
DDIR="$REPO/out/test-release/$DTAG"
DV="4.7.4-$DTAG"
HASH=$(sha256sum < "$DDIR/$DTAG.patch" 2>/dev/null | cut -c1-8)
check "the tag is test-<date>-<sha8>-dirty-<sha256 of its patch, 8> ($DTAG)" \
    test "$DTAG" = "test-$(date -u +%Y%m%d)-${FULLD:0:8}-dirty-$HASH"
check "the dirty tag fits the test tag shape" grep -qxE "$TEST_TAG_RE" <<< "$DTAG"
check "the patch carries the untracked header the tracked edit uses" \
    grep -qxF "+++ b/src/new.h" "$DDIR/$DTAG.patch"
check "the patch carries a brand-new top-level directory the edit uses" \
    grep -qxF "+++ b/newlib/x.h" "$DDIR/$DTAG.patch"
n=$(grep -c 'docs/notes.md' "$DDIR/$DTAG.patch" || true)
check "the patch leaves out docs/ (no test-release build reads it) ($n)" test "$n" = 0
n=$(grep -cE '\.claude|\.secret' "$DDIR/$DTAG.patch" || true)
check "the patch leaves out hidden top-level directories (.claude/, .secret/) ($n)" test "$n" = 0
check "it says, before building, which untracked directories go in" grep -qxF \
    "make-test-release: --dirty: including untracked: newlib/" "$WORK/dp.out"
n=$(grep -c 'SCRATCH' "$DDIR/$DTAG.patch" || true)
check "the patch leaves out an untracked file outside the build inputs ($n)" test "$n" = 0
# The patch reproduces what was built: applied to a clean checkout of the
# commit, the build inputs come out identical to the tree that was built.
rm -rf "$WORK/repro"
git clone -q "$REPO" "$WORK/repro"
git -C "$WORK/repro" checkout -q "$FULLD"
git -C "$WORK/repro" apply "$DDIR/$DTAG.patch"
check "applied to a clean checkout, the patch reproduces the built src/" \
    diff -r "$REPO/src" "$WORK/repro/src"
check "...and VERSION" cmp -s "$REPO/VERSION" "$WORK/repro/VERSION"
check "...and the new top-level newlib/" diff -r "$REPO/newlib" "$WORK/repro/newlib"
n=$(git -C "$REPO" status --porcelain | grep -c '^?? src/new.h$' || true)
check "the real index is untouched: src/new.h is still untracked ($n)" test "$n" = 1
check "SHA256SUMS lists both tarballs and the patch, verifiably" \
    bash -c "cd '$DDIR' && sha256sum -c --quiet SHA256SUMS && test \$(wc -l < SHA256SUMS) = 3 && grep -q '  $DTAG.patch\$' SHA256SUMS"
check "the create line carries the patch, then SHA256SUMS" grep -qxF \
    "release create $DTAG --repo $REPO_SLUG --verify-tag --prerelease --latest=false --title AXL SDK test build $DTAG --notes-file $DDIR/notes.md $DDIR/axl-sdk-uefi-tools-$DV-x64.tar.gz $DDIR/axl-sdk-uefi-tools-$DV-aa64.tar.gz $DDIR/$DTAG.patch $DDIR/SHA256SUMS" \
    "$GH_LOG"
check "the tag's commit names the dirty source" grep -qxF \
    "api -X POST repos/$REPO_SLUG/git/commits -f message=AXL SDK test build $DTAG (axl-sdk $FULLD-dirty-$HASH) -f tree=$GH_TREE_SHA -f parents[]=$GH_MAIN_SHA --jq .sha" \
    "$GH_LOG"
check "the notes say it was built from uncommitted changes" grep -qxF \
    "Built from uncommitted changes on top of that commit; the exact patch" "$DDIR/notes.md"
check "...and name the patch" grep -qxF "is $DTAG.patch. Changed files:" "$DDIR/notes.md"
# A --stat row per changed file (column widths depend on the whole set, so
# each row is matched by its file name and counts, not byte for byte).
check "the notes list the untracked header among the changed files" \
    grep -qE '^ src/new\.h +\| +1 \+$' "$DDIR/notes.md"
check "...the edited source" grep -qE '^ src/a\.c +\| +2 \+\+$' "$DDIR/notes.md"
check "...and VERSION" grep -qE '^ VERSION +\| +2 \+-$' "$DDIR/notes.md"
check "...and the summary counts all four" \
    grep -qxF ' 4 files changed, 5 insertions(+), 1 deletion(-)' "$DDIR/notes.md"
check "the notes' asset line names the patch" grep -qxF \
    "directory to a USB stick), $DTAG.patch and SHA256SUMS." "$DDIR/notes.md"
# An untracked directory over the size bound is refused, never published.
NLSIZE=$(du -sb "$REPO/newlib" | cut -f1)
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && MTR_UNTRACKED_DIR_MAX=4 main --dirty --dry-run) > "$WORK/big.out" 2> "$WORK/big.err"
rc=$?
check "an untracked directory over the bound refuses ($rc)" test "$rc" = 1
check "...naming it and its size, exactly" grep -qxF \
    "make-test-release: --dirty: untracked newlib/ is $NLSIZE bytes, over the 4-byte bound; commit it, ignore it or remove it" \
    "$WORK/big.err"
check "...and builds nothing" test ! -s "$WORK/steps"
mkdir -p "$REPO/share"; echo x > "$REPO/share/foo.json5"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --dirty --dry-run) > "$WORK/du.out" 2> "$WORK/du.err"
rc=$?
check "--dirty still refuses an untracked share/ file ($rc)" test "$rc" = 1
check "naming it, exactly" grep -qxF \
    "make-test-release: untracked share/foo.json5 would ship in the build; commit or remove it first" \
    "$WORK/du.err"
check "and builds nothing" test ! -s "$WORK/steps"
rm -rf "${REPO:?}/share" "${REPO:?}/src/new.h" "${REPO:?}/newlib" "${REPO:?}/docs" \
    "${REPO:?}/.claude" "${REPO:?}/.secret"
printf 'int a;\n' > "$REPO/src/a.c"
echo 4.7.3 > "$REPO/VERSION"
n=$(cd "$REPO" && git status --porcelain | grep -vc '^?? ' || true)
check "control: the scratch repo is clean again ($n)" test "$n" = 0

echo "=== list ==="
printf '%s\t%s\t%s\t%s\n' \
    v4.7.3 false 2026-09-01T10:00:00Z 7 \
    test-20261003-00000003 true 2026-10-03T10:00:00Z 3 \
    test-20261005-00000005-dirty-0000000e true 2026-10-05T10:00:00Z 4 \
    test-foo true 2026-10-06T10:00:00Z 1 \
    test-20260901-0000000a false 2026-09-01T09:00:00Z 3 \
    test-20261004-00000004 true 2026-10-04T10:00:00Z 1 > "$GH_RELEASES"
printf 'test-20261003-00000003\t2026-10-09T10:00:00Z\n' > "$GH_TAG_DATES"
: > "$GH_LOG"
(cd "$REPO" && main --list) > "$WORK/list.out" 2> "$WORK/list.err"
rc=$?
check "--list exits 0 ($rc)" test "$rc" = 0
printf '%s\n' \
    "test-20261003-00000003  2026-10-09T10:00:00Z  3 assets" \
    "test-20261005-00000005-dirty-0000000e  2026-10-05T10:00:00Z  4 assets" \
    "test-20261004-00000004  2026-10-04T10:00:00Z  1 asset" > "$WORK/want-list"
check "--list prints the test pre-releases only, newest TAG COMMIT first, with asset counts" \
    cmp -s "$WORK/want-list" "$WORK/list.out"
printf '%s\n' \
    "api repos/$REPO_SLUG/releases?per_page=100 --paginate --jq .[] | [.tag_name, (.prerelease | tostring), .created_at, (.assets | length | tostring)] | @tsv" \
    "$TAGDATESCALL" > "$WORK/want-list-calls"
check "from two exact gh reads: the releases, the tag dates" cmp -s "$WORK/want-list-calls" "$GH_LOG"
: > "$GH_TAG_DATES"
printf 'v4.7.3\tfalse\t2026-09-01T10:00:00Z\t7\n' > "$GH_RELEASES"
(cd "$REPO" && main --list) > "$WORK/list0.out" 2>&1
check "--list with none says so" grep -qxF \
    "make-test-release: no test pre-releases on $REPO_SLUG" "$WORK/list0.out"

echo "=== delete ==="
printf '%s\t%s\t%s\n' \
    test-20261003-00000003 true 2026-10-03T10:00:00Z \
    test-20261004-00000004-dirty-0000000f true 2026-10-04T10:00:00Z \
    v4.7.3 false 2026-09-01T10:00:00Z \
    test-foo true 2026-09-02T10:00:00Z \
    test-20260901-0000000a false 2026-09-01T09:00:00Z > "$GH_LIST"
: > "$GH_LOG"
(cd "$REPO" && main --delete test-20261003-00000003) > "$WORK/del.out" 2> "$WORK/del.err"
rc=$?
check "--delete TAG exits 0 ($rc)" test "$rc" = 0
check "it deletes exactly that release and its tag" test "$(grep '^release delete' "$GH_LOG")" = \
    "release delete test-20261003-00000003 --repo $REPO_SLUG --cleanup-tag --yes"
check "and says so" grep -qxF "make-test-release: deleted test-20261003-00000003" "$WORK/del.out"
for case in "v4.7.3|refusing to delete 'v4.7.3': not a test-<YYYYMMDD>-<sha8> tag" \
            "test-foo|refusing to delete 'test-foo': not a test-<YYYYMMDD>-<sha8> tag" \
            "test-20260901-0000000a|refusing to delete test-20260901-0000000a: it is not a pre-release" \
            "test-20991231-deadbeef|refusing to delete test-20991231-deadbeef: no such release on $REPO_SLUG"; do
    t=${case%%|*}; want=${case#*|}
    : > "$GH_LOG"
    (cd "$REPO" && main --delete "$t") > "$WORK/dr.out" 2> "$WORK/dr.err"
    rc=$?
    check "--delete $t refuses ($rc)" test "$rc" = 1
    check "...naming why, exactly" grep -qxF "make-test-release: $want" "$WORK/dr.err"
    n=$(grep -c '^release delete' "$GH_LOG" || true)
    check "...and deletes nothing ($n)" test "$n" = 0
done
: > "$GH_LOG"
(cd "$REPO" && main --dry-run --delete test-20261003-00000003) > "$WORK/dd.out" 2>&1
check "--dry-run --delete prints the delete" grep -qxF \
    "make-test-release: dry-run: gh release delete test-20261003-00000003 --repo $REPO_SLUG --cleanup-tag --yes" \
    "$WORK/dd.out"
n=$(grep -c '^release delete' "$GH_LOG" || true)
check "...and runs none ($n)" test "$n" = 0
: > "$GH_LOG"
(cd "$REPO" && main --delete-all) > "$WORK/da.out" 2> "$WORK/da.err"
rc=$?
check "--delete-all exits 0 ($rc)" test "$rc" = 0
grep '^release delete' "$GH_LOG" > "$WORK/da-got"
printf 'release delete %s --repo %s --cleanup-tag --yes\n' \
    test-20261004-00000004-dirty-0000000f "$REPO_SLUG" \
    test-20261003-00000003 "$REPO_SLUG" > "$WORK/da-want"
check "--delete-all deletes exactly the test pre-releases, newest first" \
    cmp -s "$WORK/da-want" "$WORK/da-got"
: > "$GH_LOG"
(cd "$REPO" && main --dry-run --delete-all) > "$WORK/dad.out" 2>&1
n=$(grep -c '^make-test-release: dry-run: gh release delete ' "$WORK/dad.out" || true)
check "--dry-run --delete-all prints both deletes ($n)" test "$n" = 2
n=$(grep -c '^release delete' "$GH_LOG" || true)
check "...and runs none ($n)" test "$n" = 0

echo "=== modes ==="
(cd "$REPO" && main --list --delete-all) > "$WORK/m.out" 2> "$WORK/m.err"
rc=$?
check "two modes refuse ($rc)" test "$rc" = 2
check "...exactly" grep -qxF \
    "make-test-release: use only one of --list, --delete, --delete-all, --update" "$WORK/m.err"
(cd "$REPO" && main --dirty --list) > "$WORK/m2.out" 2> "$WORK/m2.err"
rc=$?
check "--dirty without a build refuses ($rc)" test "$rc" = 2
check "...exactly" grep -qxF \
    "make-test-release: --dirty applies to a build (the default mode or --update)" "$WORK/m2.err"

echo "=== update ==="
FULLU=$(git -C "$REPO" rev-parse HEAD)
UTAG=test-20261005-00000005
UDIR="$REPO/out/test-release/$UTAG"
UV="4.7.3-$UTAG"
U64="$UDIR/axl-sdk-uefi-tools-$UV-x64.tar.gz"
UA64="$UDIR/axl-sdk-uefi-tools-$UV-aa64.tar.gz"
printf '%s\t%s\t%s\n' \
    test-20261004-00000004 true 2026-10-04T10:00:00Z \
    $UTAG true 2026-10-05T10:00:00Z \
    test-20260901-0000000a false 2026-10-09T09:00:00Z \
    v4.7.3 false 2026-10-10T10:00:00Z > "$GH_LIST"
export GH_ASSETS="old-4.7.2-x64.tar.gz axl-sdk-uefi-tools-$UV-x64.tar.gz SHA256SUMS"
# The source unchanged: assets and notes replaced, the tag left where it is.
export GH_TAG_MSG="AXL SDK test build $UTAG (axl-sdk $FULLU)"
export GH_NOTES_BODY="$(printf 'A test build.\n\nCommit: %s x\nSource: axl-sdk %s\n' "$FULLU" "$FULLU")"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --update) > "$WORK/u1.out" 2> "$WORK/u1.err"
rc=$?
check "--update exits 0 ($rc)" test "$rc" = 0
{
    echo "$LISTCALL"
    echo "$TAGDATESCALL"
    echo "api repos/$REPO_SLUG/git/ref/tags/$UTAG --jq .object.sha"
    echo "api repos/$REPO_SLUG/git/commits/$GH_TAG_SHA --jq .message"
    echo "release view $UTAG --repo $REPO_SLUG --json body --jq .body"
    echo "release upload $UTAG --repo $REPO_SLUG --clobber $U64 $UA64 $UDIR/SHA256SUMS"
    echo "release view $UTAG --repo $REPO_SLUG --json assets --jq .assets[].name"
    echo "release delete-asset $UTAG old-4.7.2-x64.tar.gz --repo $REPO_SLUG --yes"
    echo "release edit $UTAG --repo $REPO_SLUG --notes-file $UDIR/notes.md"
} > "$WORK/want-u1"
diff "$WORK/want-u1" "$GH_LOG" >&2
check "same source: the newest test pre-release's assets and notes, replaced; the tag not moved" \
    cmp -s "$WORK/want-u1" "$GH_LOG"
printf 'build x64\nbuild aa64\nstage x64\nstage aa64\ngate x64 %s\ngate aa64 %s\n' \
    "$(basename "$U64")" "$(basename "$UA64")" > "$WORK/want-steps"
check "it rebuilt, restaged and re-gated both arches under the same tag" \
    cmp -s "$WORK/steps" "$WORK/want-steps"
check "the notes say it was updated in place" grep -qxF \
    "Updated in place on $(date -u +%Y-%m-%d) from axl-sdk $FULLU." "$UDIR/notes.md"
check "the notes' FIRST line states the build's actual source" \
    test "$(head -1 "$UDIR/notes.md")" = \
    "Test build of axl-sdk $FULLU, updated in place: the tag's name is from the first build published under it."
check "and record it for the next update" grep -qxF "Source: axl-sdk $FULLU" "$UDIR/notes.md"
n=$(grep -c 'The tag moved' "$UDIR/notes.md" || true)
check "...and not that the tag moved ($n)" test "$n" = 0
check "and says it is done" grep -qxF "make-test-release: updated $UTAG" "$WORK/u1.out"
# The source changed: the tag moves to a fresh commit of its own.
export GH_TAG_MSG="AXL SDK test build $UTAG (axl-sdk 5555555555555555555555555555555555555555)"
export GH_NOTES_BODY="$(printf 'Commit: 5555555555555555555555555555555555555555 x\nSource: axl-sdk 5555555555555555555555555555555555555555\n')"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --update $UTAG) > "$WORK/u2.out" 2> "$WORK/u2.err"
rc=$?
check "--update TAG with a changed source exits 0 ($rc)" test "$rc" = 0
{
    echo "$LISTCALL"
    echo "api repos/$REPO_SLUG/git/ref/tags/$UTAG --jq .object.sha"
    echo "api repos/$REPO_SLUG/git/commits/$GH_TAG_SHA --jq .message"
    echo "release view $UTAG --repo $REPO_SLUG --json body --jq .body"
    echo "api repos/$REPO_SLUG/git/ref/heads/main --jq .object.sha"
    echo "api repos/$REPO_SLUG/git/commits/$GH_MAIN_SHA --jq .tree.sha"
    echo "api -X POST repos/$REPO_SLUG/git/commits -f message=AXL SDK test build $UTAG (axl-sdk $FULLU) -f tree=$GH_TREE_SHA -f parents[]=$GH_MAIN_SHA --jq .sha"
    echo "api -X PATCH repos/$REPO_SLUG/git/refs/tags/$UTAG -f sha=$GH_NEW_SHA -F force=true --silent"
    echo "release upload $UTAG --repo $REPO_SLUG --clobber $U64 $UA64 $UDIR/SHA256SUMS"
    echo "release view $UTAG --repo $REPO_SLUG --json assets --jq .assets[].name"
    echo "release delete-asset $UTAG old-4.7.2-x64.tar.gz --repo $REPO_SLUG --yes"
    echo "release edit $UTAG --repo $REPO_SLUG --notes-file $UDIR/notes.md"
} > "$WORK/want-u2"
diff "$WORK/want-u2" "$GH_LOG" >&2
check "changed source: a fresh commit, the tag PATCHed to it, then assets and notes" \
    cmp -s "$WORK/want-u2" "$GH_LOG"
n=$(grep -cE '^api -X [A-Z]+ repos/[^ ]+/git/refs/heads' "$GH_LOG" || true)
check "no call writes a branch ref ($n)" test "$n" = 0
check "the notes say the tag moved, and from what" grep -qxF \
    "The tag moved to a fresh commit: this build's source changed from 5555555555555555555555555555555555555555 to $FULLU." \
    "$UDIR/notes.md"
check "it says it moved the tag" grep -qxF \
    "make-test-release: moved $UTAG -> $GH_NEW_SHA (main's tree, parent $GH_MAIN_SHA)" "$WORK/u2.out"
# A retry after a run that moved the tag and then failed: the tag already
# names this source, but the published notes still describe the old one --
# so no second move, and the notes still say the tag moved.
export GH_TAG_MSG="AXL SDK test build $UTAG (axl-sdk $FULLU)"
export GH_NOTES_BODY="$(printf 'Commit: 5555555555555555555555555555555555555555 x\nSource: axl-sdk 5555555555555555555555555555555555555555\n')"
: > "$GH_LOG"
(cd "$REPO" && main --update $UTAG) > "$WORK/ur1.out" 2> "$WORK/ur1.err"
rc=$?
check "a retry after a failed update exits 0 ($rc)" test "$rc" = 0
n=$(grep -c '^api -X PATCH' "$GH_LOG" || true)
check "the tag already names this source: no second move ($n)" test "$n" = 0
check "the notes still say the tag moved, from what the notes described" grep -qxF \
    "The tag moved to a fresh commit: this build's source changed from 5555555555555555555555555555555555555555 to $FULLU." \
    "$UDIR/notes.md"
# Notes from before the Source line: the Commit line names the source.
export GH_NOTES_BODY="$(printf 'Commit: 6666666666666666666666666666666666666666 x\n')"
: > "$GH_LOG"
(cd "$REPO" && main --update $UTAG) > "$WORK/ur2.out" 2> "$WORK/ur2.err"
check "notes with no Source line: the Commit line is the old source" grep -qxF \
    "The tag moved to a fresh commit: this build's source changed from 6666666666666666666666666666666666666666 to $FULLU." \
    "$UDIR/notes.md"
# A tag from before tags had commits of their own: no recorded source.
export GH_TAG_MSG="Initial commit" GH_NOTES_BODY=""
: > "$GH_LOG"
(cd "$REPO" && main --update test-20261004-00000004) > "$WORK/u3.out" 2> "$WORK/u3.err"
rc=$?
check "--update of an older TAG exits 0 ($rc)" test "$rc" = 0
check "it reads THAT tag" grep -qxF \
    "api repos/$REPO_SLUG/git/ref/tags/test-20261004-00000004 --jq .object.sha" "$GH_LOG"
check "an unrecorded source moves the tag, and the notes say so" grep -qxF \
    "The tag moved to a fresh commit: this build's source changed from an unrecorded source to $FULLU." \
    "$REPO/out/test-release/test-20261004-00000004/notes.md"
for t in v4.7.3 test-20260901-0000000a; do
    : > "$GH_LOG"; : > "$WORK/steps"
    (cd "$REPO" && main --update "$t") > "$WORK/ur.out" 2> "$WORK/ur.err"
    rc=$?
    check "--update $t refuses ($rc)" test "$rc" = 1
    check "...and builds nothing" test ! -s "$WORK/steps"
    n=$(grep -cE '^(api -X|release (upload|edit|delete))' "$GH_LOG" || true)
    check "...and writes nothing ($n)" test "$n" = 0
done
check "a v* tag is refused by shape" grep -qxF \
    "make-test-release: refusing to update 'v4.7.3': not a test-<YYYYMMDD>-<sha8> tag" \
    <(cd "$REPO" && main --update v4.7.3 2>&1)
# A dirty tree: refused without --dirty; with it, the patch ships too.
echo 4.7.4 > "$REPO/VERSION"
: > "$GH_LOG"; : > "$WORK/steps"
(cd "$REPO" && main --update) > "$WORK/u4.out" 2> "$WORK/u4.err"
rc=$?
check "--update of a dirty tree without --dirty refuses ($rc)" test "$rc" = 1
check "...naming --dirty" grep -qxF \
    "make-test-release: the tree has uncommitted changes; commit them first, or pass --dirty to build them as they are" \
    "$WORK/u4.err"
check "...and builds nothing" test ! -s "$WORK/steps"
export GH_TAG_MSG="AXL SDK test build $UTAG (axl-sdk $FULLU)"
: > "$GH_LOG"
(cd "$REPO" && main --update --dirty) > "$WORK/u5.out" 2> "$WORK/u5.err"
rc=$?
check "--update --dirty exits 0 ($rc)" test "$rc" = 0
UV5="4.7.4-$UTAG"
check "the upload carries the patch, named for the tag" grep -qxF \
    "release upload $UTAG --repo $REPO_SLUG --clobber $UDIR/axl-sdk-uefi-tools-$UV5-x64.tar.gz $UDIR/axl-sdk-uefi-tools-$UV5-aa64.tar.gz $UDIR/$UTAG.patch $UDIR/SHA256SUMS" \
    "$GH_LOG"
n=$(grep -c "^api -X PATCH repos/$REPO_SLUG/git/refs/tags/$UTAG " "$GH_LOG" || true)
check "a dirty build is a changed source: the tag moved ($n)" test "$n" = 1
# --dry-run: every write printed, none run.
: > "$GH_LOG"
(cd "$REPO" && main --update --dirty --dry-run) > "$WORK/u6.out" 2> "$WORK/u6.err"
rc=$?
check "--update --dry-run exits 0 ($rc)" test "$rc" = 0
for w in "gh api -X POST repos/$REPO_SLUG/git/commits " \
         "gh api -X PATCH repos/$REPO_SLUG/git/refs/tags/$UTAG -f sha=NEW_COMMIT_SHA -F force=true --silent" \
         "gh release upload $UTAG --repo $REPO_SLUG --clobber " \
         "gh release delete-asset $UTAG old-4.7.2-x64.tar.gz --repo $REPO_SLUG --yes" \
         "gh release edit $UTAG --repo $REPO_SLUG --notes-file $UDIR/notes.md"; do
    n=$(grep -cF "make-test-release: dry-run: $w" "$WORK/u6.out" || true)
    check "--dry-run prints: ${w:0:60}... ($n)" test "$n" = 1
done
n=$(grep -cE '^(api -X|release (upload|edit|delete))' "$GH_LOG" || true)
check "--update --dry-run writes nothing ($n)" test "$n" = 0
echo 4.7.3 > "$REPO/VERSION"
export GH_ASSETS="" GH_TAG_MSG=""

echo "=== stale-tool ==="
FAKE="$WORK/fake-prefix"
mkdir -p "$WORK/mkbin" "$FAKE/tools" "$WORK/stale-out"
# A STUB build wrapper (AXL_BUILD_WRAPPER, the staging script's seam): it
# answers --print-prefix with the scratch tree, fakes release-drivers, and
# "builds" sshd-rtk.efi / sshd-full-rtk.efi as the name of the driver it was
# given, into SSHD_RTK_OUT as the real build does (its default is the shared
# tools/). The release script must pass its own directory: the real build
# otherwise lands in tools/, which tests enumerate.
cat > "$WORK/mkbin/axl-build.sh" << STUB
#!/bin/bash
rtk="" built=0 out="$FAKE/tools"
for a in "\$@"; do
    case "\$a" in
        --print-prefix) echo "$FAKE"; exit 0 ;;
        RTK_USB_UNDI=*) rtk="\${a#RTK_USB_UNDI=}" ;;
        SSHD_RTK_OUT=*) out="\${a#SSHD_RTK_OUT=}" ;;
    esac
done
for a in "\$@"; do
    case "\$a" in
        sshd-rtk|sshd-full-rtk) mkdir -p "\$out"; echo "embeds \$rtk" > "\$out/\$a.efi"; built=1 ;;
        release-drivers)
            mkdir -p "$FAKE/release-drivers/x64"
            for d in NetworkCommon.efi UsbCdcEcm.efi UsbCdcNcm.efi UsbRndis.efi \\
                     RamDiskDxe.efi; do
                echo drv > "$FAKE/release-drivers/x64/\$d"
            done
            built=1 ;;
    esac
done
[[ "\$built" == 1 ]] && exit 0
echo "stub axl-build.sh: unexpected call: \$*" >&2
exit 2
STUB
chmod +x "$WORK/mkbin/axl-build.sh"
export AXL_BUILD_WRAPPER="$WORK/mkbin/axl-build.sh"
read -r -a SHIPPED <<< "$("$PROJECT_DIR/scripts/axl-list.sh" SHIPPED_TOOL_NAMES)"
check "SHIPPED_TOOL_NAMES names the shipped set (${#SHIPPED[@]})" test "${#SHIPPED[@]}" -ge 30
for t in "${SHIPPED[@]}"; do echo "$t" > "$FAKE/tools/$t.efi"; done
echo stale > "$FAKE/tools/stale.efi"
"$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools \
    --arch x64 --version 9.9.9 --out "$WORK/stale-out" \
    > "$WORK/stale.out" 2> "$WORK/stale.err"
rc=$?
check "a stale .efi in tools/ still builds the tarball ($rc)" test "$rc" = 0
STALE_TGZ="$WORK/stale-out/axl-sdk-uefi-tools-9.9.9-x64.tar.gz"
tar tzf "$STALE_TGZ" > "$WORK/stale.list" 2>/dev/null
n=$(grep -c '/stale\.efi$' "$WORK/stale.list" || true)
check "stale.efi is not shipped ($n)" test "$n" = 0
check "and is named, exactly" grep -qxF \
    "make-release-assets: not shipping stale.efi: it is not in SHIPPED_TOOL_NAMES (a stale build output?)" \
    "$WORK/stale.err"
n=$(grep -cE '^axl-sdk-uefi-tools-9\.9\.9-x64/[^/]+\.efi$' "$WORK/stale.list" || true)
check "every shipped tool is shipped, and nothing else ($n of ${#SHIPPED[@]})" \
    test "$n" = "${#SHIPPED[@]}"
n=$(grep -c 'sshd-rtk' "$WORK/stale.err" || true)
check "with no sshd-rtk.efi built, nothing is said of it ($n)" test "$n" = 0

echo "=== tarball --with-rtk ==="
rm -f "$STALE_TGZ"
echo "a leftover" > "$FAKE/tools/sshd-rtk.efi"
echo "a leftover" > "$FAKE/tools/sshd-full-rtk.efi"
"$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools \
    --arch x64 --version 9.9.9 --out "$WORK/stale-out" \
    > "$WORK/nortk.out" 2> "$WORK/nortk.err"
tar tzf "$STALE_TGZ" > "$WORK/nortk.list" 2>/dev/null
n=$(grep -c '/sshd-rtk\.efi$' "$WORK/nortk.list" || true)
check "without --with-rtk a built sshd-rtk.efi is not shipped ($n)" test "$n" = 0
check "...and the reason is said, exactly" grep -qxF \
    "make-release-assets: not shipping sshd-rtk.efi: it embeds Realtek's driver (give --with-rtk)" \
    "$WORK/nortk.err"
n=$(grep -c '/sshd-full-rtk\.efi$' "$WORK/nortk.list" || true)
check "without --with-rtk a built sshd-full-rtk.efi is not shipped ($n)" test "$n" = 0
check "...and the reason is said, exactly" grep -qxF \
    "make-release-assets: not shipping sshd-full-rtk.efi: it embeds Realtek's driver (give --with-rtk)" \
    "$WORK/nortk.err"
rm -f "$STALE_TGZ"
"$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools \
    --arch x64 --version 9.9.9 --out "$WORK/stale-out" --with-rtk "$WORK/nowhere" \
    > "$WORK/rtkmiss.out" 2> "$WORK/rtkmiss.err"
rc=$?
check "--with-rtk with no driver there refuses ($rc)" test "$rc" = 2
check "...naming it, exactly" grep -qxF \
    "make-release-assets: --with-rtk: no $WORK/nowhere/x64/RtkUsbUndiDxe.efi" "$WORK/rtkmiss.err"
"$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools \
    --arch x64 --version 9.9.9 --out "$WORK/stale-out" --with-rtk "$RTK" \
    > "$WORK/rtk-tar.out" 2> "$WORK/rtk-tar.err"
rc=$?
check "--with-rtk builds the tarball ($rc)" test "$rc" = 0
mkdir -p "$WORK/rtk-x"
tar -C "$WORK/rtk-x" -xzf "$STALE_TGZ" 2>/dev/null
check "it ships sshd-rtk.efi built from THAT driver, not the leftover" grep -qxF \
    "embeds $RTK/x64/RtkUsbUndiDxe.efi" "$WORK/rtk-x/axl-sdk-uefi-tools-9.9.9-x64/sshd-rtk.efi"
check "...and sshd-full-rtk.efi, built from THAT driver too" grep -qxF \
    "embeds $RTK/x64/RtkUsbUndiDxe.efi" "$WORK/rtk-x/axl-sdk-uefi-tools-9.9.9-x64/sshd-full-rtk.efi"
# The build must NOT land in the shared tools/ (tests enumerate it): the
# planted leftover there is untouched, and the build went to rtk-release/.
check "the shared tools/ still holds only the leftover, not the build" grep -qxF \
    "a leftover" "$FAKE/tools/sshd-rtk.efi"
check "the build went to its own rtk-release/ directory" grep -qxF \
    "embeds $RTK/x64/RtkUsbUndiDxe.efi" "$FAKE/rtk-release/sshd-rtk.efi"
check "the README says it embeds Realtek's driver" grep -qxF \
    "the two are one service. The embedded driver is Realtek's own binary," \
    "$WORK/rtk-x/axl-sdk-uefi-tools-9.9.9-x64/README.txt"
rm -f "$FAKE/tools/sshd-rtk.efi" "$FAKE/tools/sshd-full-rtk.efi" "$STALE_TGZ"
rm -rf "$FAKE/rtk-release"
rm -f "$FAKE/tools/sshd.efi" "$STALE_TGZ"
"$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools \
    --arch x64 --version 9.9.9 --out "$WORK/stale-out" \
    > "$WORK/miss.out" 2> "$WORK/miss.err"
rc=$?
check "a missing shipped tool refuses ($rc)" test "$rc" = 1
check "naming it, exactly" grep -qxF "make-release-assets: MISSING: sshd.efi" "$WORK/miss.err"
check "and writes no tarball" test ! -e "$STALE_TGZ"

echo ""
echo "make-test-release: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
