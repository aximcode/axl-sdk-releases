#!/bin/bash
# make-test-release.sh -- publish a test build of the UEFI tools as a
# Pre-release on aximcode/axl-sdk-releases, built and gated HERE, with no CI.
#
# WHO IT IS FOR. A remote user testing sshd and the tools on real hardware
# downloads from the releases page; an official release is too heavy (and
# ask-first) for "try this build". So this builds both arches locally, gates
# each on the release-image test, and publishes the two UEFI-tools tarballs
# plus SHA256SUMS:
#
#   tag      test-<YYYYMMDD>-<8-hex sha of HEAD>, on aximcode/axl-sdk-releases
#            ONLY (with --dirty, test-<YYYYMMDD>-<sha8>-dirty-<hash8>). It
#            never matches v*, and nothing is pushed or tagged in axl-sdk, so
#            no workflow fires.
#   commit   the tag names a commit of its OWN, made with the git data API:
#            the releases repo's main tree, main as its parent, a fresh date.
#            GitHub orders releases by their tag's commit date, so a test
#            release tagged at main's (old) HEAD sorted below every release
#            since; this one sorts at the top. main is never moved, and
#            pruning (which deletes the tag) leaves the commit unreferenced.
#   flags    --prerelease --latest=false: never "Latest". packaging/install.sh
#            is fetched from releases/latest/download/, and a test build
#            carries no install.sh or VERSION, so a test release marked
#            latest would break every fresh install.
#   gate     test-efi-pack-release-qemu.sh per arch, staging its read-only
#            stick from THE TARBALL being published. Any failure publishes
#            nothing. No flag skips it.
#   prune    after publishing, keeps the --keep newest test pre-releases
#            (default 3) and deletes the older ones, tag included. "Newest"
#            is by the TAG COMMIT's date -- what GitHub orders by, and what
#            --update moves -- so a release just refreshed is not the first
#            to go.
#
# EVERY MODE touches only test pre-releases: a tag shaped
# test-<8 digits>-<8 hex>[-dirty-<8 hex>] whose release is a pre-release.
# Anything else on that repo -- a v* release, a test-shaped full release --
# is refused by name, never deleted, updated or pruned.
#
# The tarballs are versioned <VERSION>-<tag>, so a downloaded file says both
# which release line it is from and which build it is.
#
# Usage: scripts/make-test-release.sh [--dry-run] [--dirty] [--with-rtk DIR] [--keep N]
#        scripts/make-test-release.sh [--dry-run] [--dirty] [--with-rtk DIR] --update [TAG]
#        scripts/make-test-release.sh --list
#        scripts/make-test-release.sh [--dry-run] --delete TAG | --delete-all
#   (none)        build HEAD of a CLEAN tree and publish it as a new test
#                 pre-release; a tree with uncommitted tracked changes is
#                 refused (untracked files under share/ or third_party/, which
#                 the build globs in, are refused in every mode)
#   --dirty       build the working tree as it is. The patch is every
#                 tracked change PLUS every untracked, non-ignored file under
#                 the build inputs (a new header a tracked edit includes),
#                 `git diff --binary HEAD` over a throwaway index -- so it
#                 reproduces exactly what was built. The tag ends
#                 -dirty-<first 8 hex of its sha256>, the notes say it was
#                 built from uncommitted changes and list them (its --stat),
#                 and the patch ships as <tag>.patch, in SHA256SUMS too
#   --with-rtk DIR  also ship sshd-rtk.efi and sshd-full-rtk.efi in each
#                 tarball: sshd (and sshd-full) with
#                 Realtek's USB NIC driver embedded, DIR/<arch>/RtkUsbUndiDxe.efi
#                 (make-release-assets.sh uefi-tools --with-rtk); the notes say so.
#                 Never by default -- the driver is Realtek's binary
#   --update [TAG]  rebuild and re-gate from the current tree, then replace
#                 TAG's assets and notes in place (default: the newest test
#                 pre-release). The tag name -- so the link -- stays; when the
#                 source differs from what the tag was built from, the tag
#                 moves to a fresh commit of its own and the notes say so
#   --list        the test pre-releases, newest tag commit first: tag, that
#                 commit's date, assets
#   --delete TAG  delete one test pre-release and its tag
#   --delete-all  delete every test pre-release and its tag
#   --dry-run     everything (build, stage, gate, SHA256SUMS, notes, the gh
#                 reads) except the gh writes -- commits, tags, creates,
#                 uploads, edits and deletes -- whose exact command lines are
#                 printed instead
#
# The functions are callable when this file is SOURCED (main runs only when it
# is executed) -- test/integration/test-make-test-release.sh drives them with a
# stub gh.

RELEASES_REPO=aximcode/axl-sdk-releases
TEST_TAG_RE='^test-[0-9]{8}-[0-9a-f]{8}(-dirty-[0-9a-f]{8})?$'
MTR_SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MTR_DRY_RUN=0
MTR_DIRTY=0
MTR_RTK_DIR=""

# Written by the functions that compute them (a caller reads them after).
MTR_TAG_COMMIT=""    # tag_commit: the commit a tag now names
MTR_TAG_PARENT=""    # tag_commit: main's commit, that commit's parent
MTR_ASSETS=()        # build_and_gate: every file the release carries
MTR_NOTES=""         # build_and_gate: the notes file
MTR_NOTES_LEAD=""    # read by build_and_gate: a first line for the notes

mtr_die() {
    echo "make-test-release: $*" >&2
    return 1
}

# Run a mutating gh command, or under --dry-run print it, shell-quoted so the
# printed line can be pasted.
mtr_gh_mutate() {
    if (( MTR_DRY_RUN )); then
        local q
        q=$(printf '%q ' gh "$@")
        echo "make-test-release: dry-run: ${q% }"
    else
        gh "$@"
    fi
}

# Does the tree carry uncommitted TRACKED changes?
tree_is_dirty() {
    ! git diff --quiet HEAD --
}

# The build's inputs, as paths git can be asked about: every top-level
# directory the repository tracks -- DERIVED from HEAD, so a new one is an
# input without an edit here -- AND every untracked, non-ignored top-level
# directory (mtr_untracked_dirs), less what no test-release build reads:
#   .*       every HIDDEN top-level directory, tracked or not: .github is CI
#            configuration, and an untracked one is someone's private state
#            (.claude/ holds an agent's settings and worktrees) that a
#            public patch must never carry;
#   docs     a test release builds `all tools` and the UEFI-tools
#            tarball, whose README is generated from devkit.conf and share/
#            -- the Sphinx docs (which include docs/*.md) are not built.
# The root's own tracked files (CMakeLists.txt, VERSION, ...) reach the patch as
# tracked changes; an untracked FILE at the root (a scratch note) is not an
# input and stays out. Run under the caller's GIT_INDEX_FILE, so a staged
# new directory counts as untracked against HEAD too.
mtr_input_paths() {
    { git ls-tree -d --name-only HEAD; mtr_untracked_dirs; } | sort -u | mtr_drop_non_inputs
}

# Untracked, non-ignored, non-hidden top-level directories other than docs:
# the ones a --dirty patch takes in that no commit has.
mtr_untracked_dirs() {
    git ls-files --others --exclude-standard --directory --no-empty-directory \
        | sed -n 's|^\([^/]*\)/$|\1|p' | mtr_drop_non_inputs
}

# Filter top-level names on stdin: no hidden one, no docs.
mtr_drop_non_inputs() {
    local d
    while IFS= read -r d; do
        case "$d" in
            .*|docs) ;;
            *)       echo "$d" ;;
        esac
    done
}

# The largest untracked top-level directory --dirty publishes, in bytes. A
# bigger one is far more likely a build tree or a download than source, and
# is refused, named, rather than published.
MTR_UNTRACKED_DIR_MAX="${MTR_UNTRACKED_DIR_MAX:-52428800}"

# Under --dirty: say which untracked directories go into the patch, before
# anything is built -- nothing is published silently -- and refuse one over
# MTR_UNTRACKED_DIR_MAX.
mtr_report_untracked() {
    local d size list=""
    local -a dirs=()
    mapfile -t dirs < <(mtr_untracked_dirs)
    (( ${#dirs[@]} > 0 )) || return 0
    for d in "${dirs[@]}"; do
        size=$(du -sb -- "$d" | cut -f1) \
            || { mtr_die "--dirty: could not size untracked $d/"; return 1; }
        if (( size > MTR_UNTRACKED_DIR_MAX )); then
            mtr_die "--dirty: untracked $d/ is $size bytes, over the $MTR_UNTRACKED_DIR_MAX-byte bound; commit it, ignore it or remove it"
            return 1
        fi
        list+=" $d/"
    done
    echo "make-test-release: --dirty: including untracked:$list"
}

# Run `git diff ARGS HEAD --` over a THROWAWAY index (HEAD's tree, plus every
# untracked, non-ignored file under the build inputs added intent-to-add), so
# a new file the build compiles -- a header a tracked edit includes -- is in
# the diff as a new file. The real index is never touched.
mtr_diff() {
    local idx rc=0
    idx=$(mktemp) || return 1
    local -a inputs=() untracked=()
    if ! GIT_INDEX_FILE="$idx" git read-tree HEAD; then
        rm -f "$idx"; mtr_die "could not build a scratch index from HEAD"; return 1
    fi
    mapfile -t inputs < <(GIT_INDEX_FILE="$idx" mtr_input_paths)
    mapfile -t -d '' untracked < <(GIT_INDEX_FILE="$idx" git ls-files -z --others \
        --exclude-standard -- "${inputs[@]}")
    if (( ${#untracked[@]} > 0 )) \
        && ! GIT_INDEX_FILE="$idx" git add -N -- "${untracked[@]}"; then
        rm -f "$idx"; mtr_die "could not add the untracked inputs to a scratch index"; return 1
    fi
    GIT_INDEX_FILE="$idx" git diff "$@" HEAD -- || rc=$?
    rm -f "$idx"
    return "$rc"
}

# The exact patch a --dirty build is made of: tracked changes AND untracked
# build inputs (mtr_diff), --binary so a binary file is in it too.
mtr_patch() {
    mtr_diff --binary
}

# Does the tree differ from HEAD in anything the build reads: a tracked
# change, or an untracked file under the build inputs?
mtr_has_changes() {
    local p
    tree_is_dirty && return 0
    p=$(mtr_patch) || return 0   # cannot tell: treat as changed, never as clean
    [[ -n "$p" ]]
}

# What a build is made of: HEAD's full sha, plus -dirty-<hash8> for a --dirty
# build of a tree that has changes.
mtr_source_id() {
    local sha h
    sha=$(git rev-parse HEAD) || return 1
    if (( MTR_DIRTY )) && mtr_has_changes; then
        h=$(set -o pipefail; mtr_patch | sha256sum | cut -c1-8)             || { mtr_die "could not hash the patch"; return 1; }
        echo "${sha}-dirty-${h}"
    else
        echo "$sha"
    fi
}

# The tag for the checkout in the current directory: test-<date>-<sha8>, and
# -dirty-<hash8> when mtr_source_id says so.
test_release_tag() {
    local src sha
    src=$(mtr_source_id) || return 1
    sha=${src:0:8}
    if [[ "$src" == *-dirty-* ]]; then
        echo "test-$(date -u +%Y%m%d)-${sha}-dirty-${src##*-dirty-}"
    else
        echo "test-$(date -u +%Y%m%d)-${sha}"
    fi
}

# Every release on the releases repo: tag <TAB> isPrerelease <TAB> createdAt.
# A gh that fails is an ERROR, never an empty list: an empty list reads as
# "no tag exists" and "nothing to prune", which are the two wrong answers.
list_releases() {
    gh release list --repo "$RELEASES_REPO" --limit 200 \
        --json tagName,isPrerelease,createdAt \
        --template '{{range .}}{{.tagName}}{{"\t"}}{{.isPrerelease}}{{"\t"}}{{.createdAt}}{{"\n"}}{{end}}' \
        || mtr_die "gh release list --repo $RELEASES_REPO failed"
}

# The test pre-releases in a listing on stdin, newest first.
#
# DATES (optional; list_tag_dates' tag <TAB> date lines) replaces each
# release's date with its TAG COMMIT's date -- what GitHub orders releases by,
# and what --update moves -- so a release refreshed in place counts as new.
# A tag DATES does not name keeps its createdAt.
#
# Ordered by the INSTANT, not the string: a commit date is a GitTimestamp that
# can carry an offset (2026-10-04T11:00:00+02:00 is 09:00Z, yet sorts after
# 10:30Z as text), while createdAt is Z. Every date is parsed by date(1) to
# epoch seconds for the sort and printed back as UTC ...Z. A date that does
# not parse is an error, never a guess at its place.
test_prereleases() {
    local dates="${1:-}" rows norm
    rows=$(awk -F'\t' -v OFS='\t' -v re="$TEST_TAG_RE" '
        NR == FNR { if ($1 != "") d[$1] = $2; next }
        $1 ~ re && $2 == "true" { if (($1 in d) && d[$1] != "") $3 = d[$1]; print }' \
        <(printf '%s\n' "$dates") -)
    [[ -n "$rows" ]] || return 0
    norm=$(cut -f3 <<< "$rows" | date -u -f - '+%s %Y-%m-%dT%H:%M:%SZ') \
        || { mtr_die "a release date did not parse: $(cut -f3 <<< "$rows" | tr '\n' ' ')"; return 1; }
    awk -F'\t' -v OFS='\t' '
        NR == FNR { split($0, a, " "); e[FNR] = a[1]; z[FNR] = a[2]; next }
        { $3 = z[FNR]; print e[FNR], $0 }' <(printf '%s\n' "$norm") <(printf '%s\n' "$rows") \
        | sort -t$'\t' -k1,1nr | cut -f2-
}

# Every tag on the releases repo with its commit's date: tag <TAB> date (one
# GraphQL query, paginated). A failed query is an error, never an empty list.
list_tag_dates() {
    gh api graphql --paginate -f owner="${RELEASES_REPO%%/*}" -f name="${RELEASES_REPO#*/}" \
        -f query='query($owner: String!, $name: String!, $endCursor: String) { repository(owner: $owner, name: $name) { refs(refPrefix: "refs/tags/", first: 100, after: $endCursor) { nodes { name target { ... on Commit { committedDate } } } pageInfo { hasNextPage endCursor } } } }' \
        --jq '.data.repository.refs.nodes[] | [.name, (.target.committedDate // "")] | @tsv' \
        || mtr_die "listing the tags of $RELEASES_REPO failed"
}

# Refuse TAG unless LISTING shows it as a test pre-release. $3 names the act.
require_test_prerelease() {
    local tag="$1" listing="$2" what="$3" pre
    if ! [[ "$tag" =~ $TEST_TAG_RE ]]; then
        mtr_die "refusing to $what '$tag': not a test-<YYYYMMDD>-<sha8> tag"
        return 1
    fi
    pre=$(awk -F'\t' -v t="$tag" '$1 == t { print $2 }' <<< "$listing")
    if [[ -z "$pre" ]]; then
        mtr_die "refusing to $what $tag: no such release on $RELEASES_REPO"
        return 1
    fi
    if [[ "$pre" != true ]]; then
        mtr_die "refusing to $what $tag: it is not a pre-release"
        return 1
    fi
}

# Keep the KEEP newest test pre-releases; delete the rest of THOSE.
prune_test_releases() {
    local keep="$1" listing tag dates
    [[ "$keep" =~ ^[0-9]+$ ]] || { mtr_die "prune: KEEP must be a number"; return 1; }
    listing=$(list_releases) || return 1
    dates=$(list_tag_dates) || return 1
    while IFS= read -r tag; do
        [[ -n "$tag" ]] || continue
        echo "make-test-release: pruning $tag"
        mtr_gh_mutate release delete "$tag" --repo "$RELEASES_REPO" --cleanup-tag --yes \
            || return 1
    done < <(test_prereleases "$dates" <<< "$listing" | tail -n +"$((keep + 1))" | cut -f1)
}

build_arch() {
    "$MTR_SCRIPT_DIR/axl-build.sh" ARCH="$1" BUILD=RELEASE tools
}

# arch version outdir
stage_tarball() {
    "$MTR_SCRIPT_DIR/make-release-assets.sh" uefi-tools --arch "$1" --version "$2" --out "$3" \
        ${MTR_RTK_DIR:+--with-rtk "$MTR_RTK_DIR"}
}

# arch tarball
gate_arch() {
    local qarch
    case "$1" in x64) qarch=X64 ;; aa64) qarch=AARCH64 ;; *) return 1 ;; esac
    UEFI_TOOLS_TARBALL="$2" timeout 1200 \
        ./test/integration/test-efi-pack-release-qemu.sh --arch "$qarch"
}

# A commit of the releases repo for TAG: main's tree, main as its parent, the
# date GitHub stamps it with now. SRC (mtr_source_id) is named in its message,
# which is how --update later reads what a tag was built from. main is read,
# never written. Under --dry-run the reads run and the POST is printed, and
# the commit is NEW_COMMIT_SHA. An answer that is not a 40-hex sha is refused:
# an empty one is a failed read, not a commit. Sets MTR_TAG_COMMIT; prints
# nothing on stdout but a dry-run line.
tag_commit() {
    local tag="$1" src="$2" head tree commit
    head=$(gh api "repos/$RELEASES_REPO/git/ref/heads/main" --jq .object.sha) \
        || { mtr_die "reading $RELEASES_REPO main failed; nothing published"; return 1; }
    if ! [[ "$head" =~ ^[0-9a-f]{40}$ ]]; then
        mtr_die "$RELEASES_REPO main resolved to '$head', not a commit sha; nothing published"
        return 1
    fi
    tree=$(gh api "repos/$RELEASES_REPO/git/commits/$head" --jq .tree.sha) \
        || { mtr_die "reading $RELEASES_REPO main's tree failed; nothing published"; return 1; }
    if ! [[ "$tree" =~ ^[0-9a-f]{40}$ ]]; then
        mtr_die "$RELEASES_REPO main's tree resolved to '$tree', not a tree sha; nothing published"
        return 1
    fi
    local -a post=(api -X POST "repos/$RELEASES_REPO/git/commits"
                   -f "message=AXL SDK test build $tag (axl-sdk $src)"
                   -f "tree=$tree" -f "parents[]=$head" --jq .sha)
    if (( MTR_DRY_RUN )); then
        mtr_gh_mutate "${post[@]}"
        commit=NEW_COMMIT_SHA
    else
        commit=$(gh "${post[@]}") \
            || { mtr_die "creating the commit for $tag failed; nothing published"; return 1; }
        if ! [[ "$commit" =~ ^[0-9a-f]{40}$ ]]; then
            mtr_die "the commit for $tag came back as '$commit', not a sha; nothing published"
            return 1
        fi
    fi
    MTR_TAG_COMMIT="$commit"
    MTR_TAG_PARENT="$head"
}

# Create TAG on a commit of its own (tag_commit).
tag_own_commit() {
    tag_commit "$1" "$2" || return 1
    mtr_gh_mutate api -X POST "repos/$RELEASES_REPO/git/refs" \
        -f "ref=refs/tags/$1" -f "sha=$MTR_TAG_COMMIT" --silent \
        || { mtr_die "creating the tag $1 failed; nothing published (a tag left by an earlier failed run? delete it: gh api -X DELETE repos/$RELEASES_REPO/git/refs/tags/$1)"; return 1; }
    echo "make-test-release: $1 -> $MTR_TAG_COMMIT (main's tree, parent $MTR_TAG_PARENT)"
}

# Move the existing TAG to a fresh commit of its own (tag_commit). Only a
# refs/tags/<test tag> is ever PATCHed.
move_tag() {
    tag_commit "$1" "$2" || return 1
    mtr_gh_mutate api -X PATCH "repos/$RELEASES_REPO/git/refs/tags/$1" \
        -f "sha=$MTR_TAG_COMMIT" -F force=true --silent \
        || { mtr_die "moving the tag $1 failed"; return 1; }
    echo "make-test-release: moved $1 -> $MTR_TAG_COMMIT (main's tree, parent $MTR_TAG_PARENT)"
}

# What TAG was built from: the axl-sdk source named in its commit's message,
# or "" for a tag whose commit says nothing (one made before tags had commits
# of their own). A failed read is an error, not "".
tag_source() {
    local tag="$1" sha msg
    sha=$(gh api "repos/$RELEASES_REPO/git/ref/tags/$tag" --jq .object.sha) \
        || { mtr_die "reading the tag $tag failed"; return 1; }
    msg=$(gh api "repos/$RELEASES_REPO/git/commits/$sha" --jq .message) \
        || { mtr_die "reading the commit $tag names failed"; return 1; }
    sed -nE 's/^AXL SDK test build [^ ]+ \(axl-sdk ([0-9a-f]{40}(-dirty-[0-9a-f]{8})?)\)$/\1/p' <<< "$msg"
}

# Refuse a tree whose build would not be what the tag says it is.
check_tree() {
    # Tracked changes: only --dirty builds them, and says so in tag and notes.
    if tree_is_dirty && (( ! MTR_DIRTY )); then
        mtr_die "the tree has uncommitted changes; commit them first, or pass --dirty to build them as they are"
        return 1
    fi
    # Untracked files count only where the build GLOBS them in: share/*.json5
    # is staged by glob, and third_party/edk2/*-<arch>.efi is picked up by
    # the release-drivers file(GLOB) in tools/CMakeLists.txt. An untracked
    # file there would ship in a build the notes do not describe -- not even in a
    # --dirty build's patch, which holds tracked changes only. Elsewhere (a
    # scratch note) it ships nowhere, so it is no reason to refuse.
    local untracked
    untracked=$(git ls-files --others --exclude-standard -- share third_party) || return 1
    if [[ -n "$untracked" ]]; then
        mtr_die "untracked ${untracked%%$'\n'*} would ship in the build; commit or remove it first"
        return 1
    fi
    if (( MTR_DIRTY )); then
        mtr_report_untracked || return 1
    fi
}

# Build, stage and gate both arches for TAG into out/test-release/TAG, then
# write SHA256SUMS and the notes. EXTRA (any number of lines) goes into the
# notes after the commit. Sets MTR_ASSETS and MTR_NOTES.
build_and_gate() {
    local tag="$1" dir version arch name local_note="" src patch="" line
    shift
    local -a tarballs=()

    # The notes name HEAD; if it is not on origin/main (the LOCAL ref -- this
    # fetches nothing) the remote user cannot look it up. Say so and go on:
    # pushing is the owner's call, never this script's. A missing origin/main
    # ref is the same answer -- it cannot be shown pushed.
    if ! git merge-base --is-ancestor HEAD origin/main 2>/dev/null; then
        echo "make-test-release: warning: HEAD is not on origin/main; the notes name" \
             "a commit the remote user cannot see (git push to publish it)" >&2
        local_note=" (local commit, not yet pushed)"
    fi
    src=$(mtr_source_id) || return 1

    version="$(cat VERSION)-${tag}"
    dir="$PWD/out/test-release/$tag"
    rm -rf "$dir"
    mkdir -p "$dir"
    echo "make-test-release: building $tag (tarballs versioned $version)"
    # The patch is taken BEFORE the build, so it is what the build is of.
    if [[ "$src" == *-dirty-* ]]; then
        patch="$dir/$tag.patch"
        mtr_patch > "$patch" || return 1
    fi

    for arch in x64 aa64; do
        build_arch "$arch" || { mtr_die "build failed ($arch); nothing published"; return 1; }
    done
    for arch in x64 aa64; do
        name=$("$MTR_SCRIPT_DIR/make-release-assets.sh" uefi-tools --arch "$arch" \
            --version "$version" --print-name) || return 1
        stage_tarball "$arch" "$version" "$dir" \
            || { mtr_die "staging failed ($arch); nothing published"; return 1; }
        [[ -f "$dir/$name" ]] || { mtr_die "staging wrote no $dir/$name"; return 1; }
        tarballs+=("$dir/$name")
    done
    local i=0
    for arch in x64 aa64; do
        gate_arch "$arch" "${tarballs[$i]}" \
            || { mtr_die "the release-image gate failed ($arch); nothing published"; return 1; }
        i=$((i + 1))
    done
    # The tree must still be what was built and gated.
    if [[ "$(mtr_source_id)" != "$src" ]]; then
        mtr_die "the tree changed during the build; nothing published"
        return 1
    fi

    MTR_ASSETS=("${tarballs[@]}")
    [[ -n "$patch" ]] && MTR_ASSETS+=("$patch")
    (cd "$dir" && sha256sum -- "${MTR_ASSETS[@]##*/}" > SHA256SUMS) || return 1
    MTR_ASSETS+=("$dir/SHA256SUMS")

    MTR_NOTES="$dir/notes.md"
    {
        if [[ -n "$MTR_NOTES_LEAD" ]]; then
            echo "$MTR_NOTES_LEAD"
            echo ""
        fi
        echo "A test build for hands-on testing, not a release."
        echo ""
        echo "Commit: $(git rev-parse HEAD) $(git log -1 --format=%s HEAD)${local_note}"
        # Machine-read by the next --update: what these notes describe.
        echo "Source: axl-sdk $src"
        if [[ -n "$patch" ]]; then
            echo ""
            echo "Built from uncommitted changes on top of that commit; the exact patch"
            echo "is $tag.patch. Changed files:"
            echo ""
            echo '```'
            mtr_diff --stat
            echo '```'
        fi
        for line in "$@"; do
            echo ""
            echo "$line"
        done
        echo ""
        if [[ -n "$MTR_RTK_DIR" ]]; then
            echo "Each tarball also carries sshd-rtk.efi: sshd with Realtek's USB NIC driver"
            echo "(RtkUsbUndiDxe.efi, Realtek's own binary, not covered by the SDK's licence)"
            echo "embedded, for a box whose cabled NIC is a Realtek USB dongle the firmware has"
            echo "no driver for. It is run, stopped and queried exactly as sshd.efi is."
            echo "So does sshd-full-rtk.efi: the same, with the SFTP helper built in as well."
            echo ""
        fi
        echo "Assets: the UEFI tools for x64 and aa64 (extract one and copy its"
        if [[ -n "$patch" ]]; then
            echo "directory to a USB stick), $tag.patch and SHA256SUMS."
        else
            echo "directory to a USB stick), and SHA256SUMS."
        fi
        echo ""
        echo "The Source code links on this page are the last official snapshot, not this build."
    } > "$MTR_NOTES"
}

# The default mode: a new test pre-release from the tree, then prune.
cmd_publish() {
    local keep="$1" tag listing
    check_tree || return 1
    tag=$(test_release_tag) || return 1
    listing=$(list_releases) || return 1
    if awk -F'\t' -v t="$tag" '$1 == t { found = 1 } END { exit !found }' <<< "$listing"; then
        # A DRAFT on a test-shaped tag is what a create that died part way
        # (gh uploads to a draft, then publishes) leaves: ours, unpublished,
        # so it is cleaned. A published one is someone's link: refuse.
        local draft
        draft=$(gh release view "$tag" --repo "$RELEASES_REPO" --json isDraft --jq .isDraft) \
            || { mtr_die "$tag is on $RELEASES_REPO, but reading it failed"; return 1; }
        if [[ "$draft" == true ]]; then
            echo "make-test-release: $tag has a leftover draft release (an earlier create that failed); deleting it"
            mtr_gh_mutate release delete "$tag" --repo "$RELEASES_REPO" --cleanup-tag --yes \
                || { mtr_die "deleting the leftover draft $tag failed"; return 1; }
        else
            mtr_die "$tag is already a release on $RELEASES_REPO; rebuild it in place with --update $tag, or --delete $tag first"
            return 1
        fi
    fi
    build_and_gate "$tag" || return 1

    tag_own_commit "$tag" "$(mtr_source_id)" || return 1
    # --verify-tag: the release goes on the tag just made, never one gh would
    # create at main. A failed create leaves that tag naming nothing, so it
    # is deleted (its commit is then unreferenced).
    if ! mtr_gh_mutate release create "$tag" --repo "$RELEASES_REPO" --verify-tag \
            --prerelease --latest=false \
            --title "AXL SDK test build $tag" --notes-file "$MTR_NOTES" \
            "${MTR_ASSETS[@]}"; then
        if gh api -X DELETE "repos/$RELEASES_REPO/git/refs/tags/$tag" --silent; then
            mtr_die "release create failed; deleted the tag $tag it made"
        else
            mtr_die "release create failed, and deleting the tag $tag failed too; delete it by hand: gh api -X DELETE repos/$RELEASES_REPO/git/refs/tags/$tag"
        fi
        return 1
    fi
    if (( MTR_DRY_RUN )); then
        echo "make-test-release: dry-run: $tag built and gated; nothing published"
    else
        echo "make-test-release: published $tag"
    fi

    # The new release is the newest one kept; under --dry-run it does not
    # exist yet, so the listing is pruned one short to show the real result.
    if (( MTR_DRY_RUN )); then
        prune_test_releases "$((keep - 1))" || return 1
    else
        prune_test_releases "$keep" \
            || { mtr_die "$tag is published, but pruning failed"; return 1; }
    fi
}

# --update [TAG]: rebuild, re-gate, replace TAG's assets and notes in place.
cmd_update() {
    local tag="$1" listing old new name keep_it a names body described
    local -a extra=() existing=()
    check_tree || return 1
    listing=$(list_releases) || return 1
    if [[ -z "$tag" ]]; then
        local dates
        dates=$(list_tag_dates) || return 1
        tag=$(test_prereleases "$dates" <<< "$listing" | head -1 | cut -f1)
        [[ -n "$tag" ]] || { mtr_die "no test pre-release on $RELEASES_REPO to update"; return 1; }
    fi
    require_test_prerelease "$tag" "$listing" update || return 1
    # Two records of what this release was built from, which a run that
    # failed part way can leave disagreeing: the tag's commit (moved first)
    # and the published notes (replaced last). The tag moves when ITS record
    # differs; the notes say it moved when THEIRS does -- so a retry after a
    # failure past the move still tells the reader.
    old=$(tag_source "$tag") || return 1
    body=$(gh release view "$tag" --repo "$RELEASES_REPO" --json body --jq .body) \
        || { mtr_die "reading the notes of $tag failed"; return 1; }
    described=$(sed -nE 's/^Source: axl-sdk ([0-9a-f]{40}(-dirty-[0-9a-f]{8})?)\r?$/\1/p' <<< "$body" | head -1)
    if [[ -z "$described" ]]; then   # notes from before the Source line
        described=$(sed -nE 's/^Commit: ([0-9a-f]{40}) .*/\1/p' <<< "$body" | head -1)
    fi
    new=$(mtr_source_id) || return 1
    extra+=("Updated in place on $(date -u +%Y-%m-%d) from axl-sdk $new.")
    if [[ "$described" != "$new" || "$old" != "$new" ]]; then
        extra+=("The tag moved to a fresh commit: this build's source changed from ${described:-an unrecorded source} to $new.")
    fi
    # The tag keeps the name of the build first published under it, which
    # need not be this one: the first line says what this build is.
    MTR_NOTES_LEAD="Test build of axl-sdk $new, updated in place: the tag's name is from the first build published under it."
    build_and_gate "$tag" "${extra[@]}" || { MTR_NOTES_LEAD=""; return 1; }
    MTR_NOTES_LEAD=""

    if [[ "$old" != "$new" ]]; then
        move_tag "$tag" "$new" || return 1
    fi
    mtr_gh_mutate release upload "$tag" --repo "$RELEASES_REPO" --clobber "${MTR_ASSETS[@]}" \
        || { mtr_die "uploading to $tag failed"; return 1; }
    # An asset this build does not have (an old version's tarball, a patch a
    # clean rebuild no longer carries) would contradict SHA256SUMS: delete it.
    names=$(gh release view "$tag" --repo "$RELEASES_REPO" \
        --json assets --jq '.assets[].name') \
        || { mtr_die "listing the assets of $tag failed"; return 1; }
    mapfile -t existing <<< "$names"
    for name in "${existing[@]}"; do
        [[ -n "$name" ]] || continue
        keep_it=0
        for a in "${MTR_ASSETS[@]}"; do
            [[ "${a##*/}" == "$name" ]] && keep_it=1
        done
        if (( ! keep_it )); then
            mtr_gh_mutate release delete-asset "$tag" "$name" --repo "$RELEASES_REPO" --yes \
                || { mtr_die "deleting the stale asset $name from $tag failed"; return 1; }
        fi
    done
    mtr_gh_mutate release edit "$tag" --repo "$RELEASES_REPO" --notes-file "$MTR_NOTES" \
        || { mtr_die "replacing the notes of $tag failed"; return 1; }
    if (( MTR_DRY_RUN )); then
        echo "make-test-release: dry-run: $tag rebuilt and gated; nothing updated"
    else
        echo "make-test-release: updated $tag"
    fi
}

# --list: the test pre-releases, newest first, with their asset counts.
cmd_list() {
    local rows dates
    rows=$(gh api "repos/$RELEASES_REPO/releases?per_page=100" --paginate \
        --jq '.[] | [.tag_name, (.prerelease | tostring), .created_at, (.assets | length | tostring)] | @tsv') \
        || { mtr_die "listing the releases of $RELEASES_REPO failed"; return 1; }
    dates=$(list_tag_dates) || return 1
    rows=$(test_prereleases "$dates" <<< "$rows")
    if [[ -z "$rows" ]]; then
        echo "make-test-release: no test pre-releases on $RELEASES_REPO"
        return 0
    fi
    awk -F'\t' '{ printf "%s  %s  %d asset%s\n", $1, $3, $4, ($4 == 1 ? "" : "s") }' <<< "$rows"
}

# --delete TAG
cmd_delete() {
    local tag="$1" listing
    listing=$(list_releases) || return 1
    require_test_prerelease "$tag" "$listing" delete || return 1
    mtr_gh_mutate release delete "$tag" --repo "$RELEASES_REPO" --cleanup-tag --yes || return 1
    (( MTR_DRY_RUN )) || echo "make-test-release: deleted $tag"
}

# --delete-all
cmd_delete_all() {
    local listing tag n=0
    listing=$(list_releases) || return 1
    while IFS= read -r tag; do
        [[ -n "$tag" ]] || continue
        mtr_gh_mutate release delete "$tag" --repo "$RELEASES_REPO" --cleanup-tag --yes || return 1
        (( MTR_DRY_RUN )) || echo "make-test-release: deleted $tag"
        n=$((n + 1))
    done < <(test_prereleases <<< "$listing" | cut -f1)
    (( n > 0 )) || echo "make-test-release: no test pre-releases on $RELEASES_REPO"
}

main() {
    local keep=3 mode=publish target=""
    MTR_DRY_RUN=0
    MTR_DIRTY=0
    MTR_RTK_DIR=""
    set_mode() {
        if [[ "$mode" != publish ]]; then
            mtr_die "use only one of --list, --delete, --delete-all, --update"
            return 1
        fi
        mode="$1"
    }
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dry-run)    MTR_DRY_RUN=1; shift ;;
            --dirty)      MTR_DIRTY=1; shift ;;
            --with-rtk)
                [[ -n "${2:-}" ]] || { mtr_die "--with-rtk needs the directory holding <arch>/RtkUsbUndiDxe.efi"; return 2; }
                MTR_RTK_DIR="$2"; shift 2 ;;
            --keep)       keep="${2:-}"; shift 2 || { mtr_die "--keep needs a number"; return 2; } ;;
            --list)       set_mode list || return 2; shift ;;
            --delete-all) set_mode delete-all || return 2; shift ;;
            --delete)
                set_mode delete || return 2
                [[ -n "${2:-}" ]] || { mtr_die "--delete needs a tag"; return 2; }
                target="$2"; shift 2 ;;
            --update)
                set_mode update || return 2
                if [[ -n "${2:-}" && "$2" != --* ]]; then target="$2"; shift; fi
                shift ;;
            # The header comment, however long it grows.
            -h|--help) awk 'NR > 1 && !/^#/ { exit } NR > 1' "${BASH_SOURCE[0]}" \
                           | sed 's/^# \{0,1\}//'; return 0 ;;
            *) mtr_die "unknown argument '$1'"; return 2 ;;
        esac
    done
    if ! [[ "$keep" =~ ^[0-9]+$ ]] || (( keep < 1 )); then
        mtr_die "--keep must be at least 1 (the build just published is kept)"
        return 2
    fi
    if (( MTR_DIRTY )) && [[ "$mode" != publish && "$mode" != update ]]; then
        mtr_die "--dirty applies to a build (the default mode or --update)"
        return 2
    fi
    if [[ -n "$MTR_RTK_DIR" ]]; then
        if [[ "$mode" != publish && "$mode" != update ]]; then
            mtr_die "--with-rtk applies to a build (the default mode or --update)"
            return 2
        fi
        local a
        for a in x64 aa64; do
            [[ -f "$MTR_RTK_DIR/$a/RtkUsbUndiDxe.efi" ]] \
                || { mtr_die "--with-rtk: no $MTR_RTK_DIR/$a/RtkUsbUndiDxe.efi"; return 2; }
        done
        MTR_RTK_DIR="$(cd "$MTR_RTK_DIR" && pwd)"
    fi

    case "$mode" in
        publish)    cmd_publish "$keep" ;;
        update)     cmd_update "$target" ;;
        list)       cmd_list ;;
        delete)     cmd_delete "$target" ;;
        delete-all) cmd_delete_all ;;
    esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -euo pipefail
    cd "$MTR_SCRIPT_DIR/.."
    main "$@"
fi
