#!/bin/bash
# test-meta: arch=x64 needs= est=8 local-only=0
# test-installer-assets.sh — install.sh must resolve the RIGHT asset name, for
# new releases and for the ones published before the rename.
#
# WHAT THIS GUARDS. AXL-Distribution-Design.md §14.1a settles one format string
# for every release asset -- `axl-sdk-<component>-<ver>[-<arch>].tar.gz` -- and
# §19's D2 makes release.yml emit it. The names are therefore stated in two
# places that cannot import each other: release.yml emits them, and install.sh
# (which runs on a machine with nothing of ours on it) constructs them. A gate
# keeps the SPELLINGS equal; this test proves the installer can actually FETCH
# and UNPACK what a release publishes.
#
# Two failure modes it exists for:
#
#   1. `axl use <older>` must keep working. Every release before the rename
#      published `axl-sdk-<ver>-linux-x86_64.tar.gz` and an UNVERSIONED
#      `axl-sdk-host-tools.tar.gz`. An installer that only knows the new names
#      breaks rollback -- the one thing `axl prune`'s keep-current-plus-one
#      policy exists to make possible -- on the day D2 ships.
#
#   2. The legacy host-tools tarball is a TARBOMB (§14.1c measured 6 top-level
#      entries). Extracting it straight into the prefix root scatters `scripts/`,
#      `LICENSE`, `VERSION` and friends beside the versioned directories that
#      `axl prune` walks. That is a mess in the user's data directory, not a
#      cosmetic defect.
#
# Deliberately QEMU-free and build-free: it drives install.sh against a
# `file://` release directory it fabricates, which is the same `--base-url`
# seam a mirror would use. Seconds, not minutes.
#
# Usage: ./test/integration/test-installer-assets.sh [--arch X64|AARCH64]

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
# Every check below captures an rc it expects to sometimes be non-zero;
# errexit (inherited from common-test.sh) would abort before the tally.
set +e

INSTALLER="$PROJECT_DIR/packaging/install.sh"
[[ -f "$INSTALLER" ]] || { echo "FAIL: no $INSTALLER"; exit 1; }

NEW_VER="9.9.9"      # a release published with the settled names
OLD_VER="4.4.0"      # a release published before the rename

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Fixtures. A prefix, not a stub: link_tree walks bin/ and skips pe-set-debug,
# already_installed reads share/axl/version, and `axl uninstall` runs the
# staged libexec/axl/install.sh -- so all three are present.
# ---------------------------------------------------------------------------
make_tree() {  # make_tree <dir> <version>
    local d="$1" v="$2"
    mkdir -p "$d/bin" "$d/share/axl" "$d/libexec/axl"
    # The REAL dispatcher, not a stub: §20's self-heal lives in it, and a
    # fixture that stubs the thing under test proves nothing about it.
    install -m 0755 "$PROJECT_DIR/scripts/axl" "$d/bin/axl"
    echo "placeholder" > "$d/bin/.axl-stub-marker"
    printf '#!/bin/sh\necho "axl-cc %s"\n' "$v" > "$d/bin/axl-cc"
    printf 'not-a-script\n' > "$d/bin/pe-set-debug"
    chmod +x "$d/bin/axl-cc" "$d/bin/pe-set-debug"
    echo "$v" > "$d/share/axl/version"
    cp "$INSTALLER" "$d/libexec/axl/install.sh"
    chmod 0644 "$d/libexec/axl/install.sh"
}

# The v4.4.0 host-tools tarball, reproduced: a flat `scripts/` directory with
# no `bin/` and no share/axl/version, unpacked at top level. §14.2 measured
# both defects and §14.1c counted the six top-level entries.
make_legacy_host_tools() {  # make_legacy_host_tools <dir> <version>
    local d="$1" v="$2"
    mkdir -p "$d/scripts"
    printf '#!/bin/bash\necho run-qemu\n' > "$d/scripts/run-qemu.sh"
    chmod +x "$d/scripts/run-qemu.sh"
    printf 'Apache-2.0\n' > "$d/LICENSE"
    printf 'NOTICE\n'     > "$d/NOTICE"
    printf '# changelog\n' > "$d/CHANGELOG.md"
    printf '# host tools\n' > "$d/README.md"
    echo "$v" > "$d/VERSION"
}

# publish <reldir> <asset> <root|-> <version> [maker]
#   <root> archives the tree under that single top-level directory.
#   '-'    archives its CONTENTS at top level -- a tarbomb, the legacy shape.
publish() {
    local rel="$1" asset="$2" root="$3" v="$4" maker="${5:-make_tree}"
    local stage; stage="$(mktemp -d -p "$WORK")"
    mkdir -p "$rel"
    if [[ "$root" == "-" ]]; then
        "$maker" "$stage" "$v"
        tar -C "$stage" -czf "$rel/$asset" .
    else
        "$maker" "$stage/$root" "$v"
        tar -C "$stage" -czf "$rel/$asset" "$root"
    fi
    rm -rf "$stage"
}

# SHA256SUMS is what install.sh verifies against, and -- after D2 -- what it
# reads to decide WHICH name this release published. Regenerate after every
# publish, exactly as the release job does.
seal() {  # seal <reldir> <version>
    local rel="$1" v="$2"
    echo "$v" > "$rel/VERSION"
    ( cd "$rel" && sha256sum -- *.tar.gz > SHA256SUMS )
}

run_installer() {  # run_installer <log> <args...>
    local log="$1"; shift
    HOME="$WORK/home" timeout 60 sh "$INSTALLER" --yes "$@" > "$log" 2>&1
}

# check <rc> <message> [log-to-show-on-failure]
check() {
    if [[ "$1" -eq 0 ]]; then
        test_host_pass "$2"
    else
        test_host_fail "$2"
        [[ -n "${3:-}" && -f "${3:-}" ]] && sed -n '1,20p' "$3"
    fi
    return 0
}

# Names AND contents: a refused install that left the tree in place but
# rewrote a file inside it would pass a names-only comparison, and "changes
# nothing" is the whole claim. Symlinks are hashed as their targets, which is
# what matters here -- `current` moving is a change.
tree_hash() {  # tree_hash <dir>
    (
        cd "$1" 2>/dev/null || exit 1
        find . | sort
        find . -type f -exec md5sum {} + 2>/dev/null | sort
        find . -type l -printf '%p -> %l\n' 2>/dev/null | sort
    ) | md5sum || echo "MISSING"
}

# ---------------------------------------------------------------------------
# The two release directories.
# ---------------------------------------------------------------------------
NEW_REL="$WORK/rel-new"
publish "$NEW_REL" "axl-sdk-linux-$NEW_VER-x86_64.tar.gz"  "axl-sdk-$NEW_VER"            "$NEW_VER"
publish "$NEW_REL" "axl-sdk-host-tools-$NEW_VER.tar.gz"    "axl-sdk-host-tools-$NEW_VER" "$NEW_VER"
seal    "$NEW_REL" "$NEW_VER"

OLD_REL="$WORK/rel-old"
publish "$OLD_REL" "axl-sdk-$OLD_VER-linux-x86_64.tar.gz"  "axl-sdk-$OLD_VER"            "$OLD_VER"
publish "$OLD_REL" "axl-sdk-host-tools.tar.gz"             "-"                           "$OLD_VER" make_legacy_host_tools
seal    "$OLD_REL" "$OLD_VER"

EMPTY_REL="$WORK/rel-empty"
mkdir -p "$EMPTY_REL"

PREFIX="$WORK/home/.local/share"
BIN="$WORK/home/.local/bin"
mkdir -p "$PREFIX" "$BIN"

echo "=== the settled names (§14.1a) ==="

run_installer "$WORK/new-sdk.log" --base-url "file://$NEW_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -eq 0 ]]
check $? "installs from axl-sdk-linux-$NEW_VER-x86_64.tar.gz (rc=$rc)" "$WORK/new-sdk.log"

[[ "$(cat "$PREFIX/axl-sdk-$NEW_VER/share/axl/version" 2>/dev/null)" == "$NEW_VER" ]]
check $? "extracts to axl-sdk-$NEW_VER/"

[[ "$(readlink "$PREFIX/axl-sdk" 2>/dev/null)" == "axl-sdk-$NEW_VER" ]]
check $? "current symlink points at axl-sdk-$NEW_VER"

[[ -L "$BIN/axl" && -L "$BIN/axl-cc" && ! -e "$BIN/pe-set-debug" ]]
check $? "links axl + axl-cc, not pe-set-debug"

run_installer "$WORK/new-ht.log" --host-tools --base-url "file://$NEW_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -eq 0 && -d "$PREFIX/axl-sdk-host-tools-$NEW_VER" \
   && "$(readlink "$PREFIX/axl-sdk-host-tools")" == "axl-sdk-host-tools-$NEW_VER" ]]
check $? "installs from axl-sdk-host-tools-$NEW_VER.tar.gz (rc=$rc)" "$WORK/new-ht.log"

echo "=== the names published before the rename ==="

run_installer "$WORK/old-sdk.log" --version "$OLD_VER" --base-url "file://$OLD_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -eq 0 && "$(cat "$PREFIX/axl-sdk-$OLD_VER/share/axl/version" 2>/dev/null)" == "$OLD_VER" ]]
check $? "falls back to axl-sdk-$OLD_VER-linux-x86_64.tar.gz (rc=$rc)" "$WORK/old-sdk.log"

# The legacy host-tools asset carries no version in its NAME, so the fallback
# has to accept a name that cannot be derived from $VERSION at all.
run_installer "$WORK/old-ht.log" --host-tools --version "$OLD_VER" \
    --base-url "file://$OLD_REL" --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -eq 0 && -d "$PREFIX/axl-sdk-host-tools-$OLD_VER" ]]
check $? "falls back to the unversioned axl-sdk-host-tools.tar.gz (rc=$rc)" "$WORK/old-ht.log"

# That tree carries no bin/, so nothing gets linked. Silence there reads as a
# successful install whose commands are simply missing from PATH.
grep -q "no commands to link" "$WORK/old-ht.log"
check $? "says so when a tree has no bin/ to link from" "$WORK/old-ht.log"

# A tarbomb extracted with `tar -C "$PREFIX_ROOT"` drops LICENSE / VERSION /
# share / libexec / bin beside the versioned roots. Name the entries rather
# than only counting, so a failure says WHAT leaked.
scattered="$(ls -A "$PREFIX" | grep -Ev '^axl-sdk(-host-tools)?(-[0-9]|$)' | tr '\n' ' ')"
[[ -z "$scattered" ]]
check $? "the legacy tarbomb does not scatter into the prefix root${scattered:+ (leaked: $scattered)}"

# A mirror that REGENERATES its checksums rather than copying ours writes
# "./name" entries -- `find . -exec sha256sum {} +` and `sha256sum ./*.tar.gz`
# both do. `sha256sum -c` accepts those, so refusing them means answering "this
# release publishes nothing I recognise" about a directory that plainly holds
# the asset. Hit while writing test-consumer-install.sh.
DOT_REL="$WORK/rel-dot"
publish "$DOT_REL" "axl-sdk-linux-$NEW_VER-x86_64.tar.gz" "axl-sdk-$NEW_VER" "$NEW_VER"
echo "$NEW_VER" > "$DOT_REL/VERSION"
( cd "$DOT_REL" && sha256sum -- ./*.tar.gz > SHA256SUMS )
grep -q '  \./axl-sdk-linux' "$DOT_REL/SHA256SUMS"
check $? "fixture: the mirror's SHA256SUMS really does carry ./ prefixes"
run_installer "$WORK/dot.log" --force --version "$NEW_VER" \
    --base-url "file://$DOT_REL" --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -eq 0 && -d "$PREFIX/axl-sdk-$NEW_VER" ]]
check $? "installs from a SHA256SUMS whose names carry a ./ prefix (rc=$rc)" "$WORK/dot.log"

echo "=== rollback, and the refusals ==="

# `axl use <on-disk version>` is the rollback path axl prune's retention exists
# for. An EMPTY release directory proves it never went to the network.
run_installer "$WORK/use-offline.log" --use "$OLD_VER" --base-url "file://$EMPTY_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -eq 0 && "$(readlink "$PREFIX/axl-sdk")" == "axl-sdk-$OLD_VER" ]]
check $? "--use switches to an on-disk version with no network (rc=$rc)" "$WORK/use-offline.log"

# Verify-before-touch: a corrupt asset must leave the install exactly as it was.
BAD_REL="$WORK/rel-bad"
publish "$BAD_REL" "axl-sdk-linux-8.8.8-x86_64.tar.gz" "axl-sdk-8.8.8" "8.8.8"
seal    "$BAD_REL" "8.8.8"
printf 'x' >> "$BAD_REL/axl-sdk-linux-8.8.8-x86_64.tar.gz"
BEFORE="$(tree_hash "$PREFIX")"
run_installer "$WORK/bad.log" --version 8.8.8 --base-url "file://$BAD_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -ne 0 && "$(tree_hash "$PREFIX")" == "$BEFORE" ]]
check $? "a corrupt asset is refused and changes nothing (rc=$rc)" "$WORK/bad.log"

# A release that publishes NEITHER spelling must say so, naming what it looked
# for. "could not download <one url>" sends the reader to the wrong question.
NONE_REL="$WORK/rel-none"
mkdir -p "$NONE_REL"
publish "$NONE_REL" "something-else-7.7.7.tar.gz" "axl-sdk-7.7.7" "7.7.7"
seal    "$NONE_REL" "7.7.7"
run_installer "$WORK/none.log" --version 7.7.7 --base-url "file://$NONE_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rc=$?
[[ "$rc" -ne 0 ]] \
    && grep -q "axl-sdk-linux-7.7.7-x86_64.tar.gz" "$WORK/none.log" \
    && grep -q "axl-sdk-7.7.7-linux-x86_64.tar.gz" "$WORK/none.log"
check $? "a release with neither name fails, naming both candidates (rc=$rc)" "$WORK/none.log"

echo "=== the manager is not the managed (§20) ==="

# THE STRANDING THIS GUARDS. `link_tree` symlinks every executable in
# <prefix>/bin, and `axl` is one of them -- so switching versions switched the
# MANAGER too. Roll back to a release predating D1, which staged no
# libexec/axl/install.sh, and both `axl update` and `axl use` died with "no
# installer at ..."; the only escape was re-fetching install.sh from a web
# page, which is the dependency D1 existed to remove. `axl prune` retains a
# previous version precisely so rollback is possible, so we shipped the
# retention policy together with the trap it walked into.
OLD_SDK="4.0.0"
publish "$OLD_REL" "axl-sdk-linux-$OLD_SDK-x86_64.tar.gz" "axl-sdk-$OLD_SDK" "$OLD_SDK"
seal "$OLD_REL" "$OLD_VER"
# A pre-D1 prefix: no staged installer, the shape every release before 4.5.0
# actually has.
rm -f "$WORK"/rel-old/../*.stamp 2>/dev/null || true

run_installer "$WORK/old-mgr.log" --version "$OLD_SDK" --base-url "file://$OLD_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
rm -f "$PREFIX/axl-sdk-$OLD_SDK/libexec/axl/install.sh"

# Re-link, the way `axl use <old>` does, now that the old prefix is staged.
run_installer "$WORK/use-old.log" --use "$OLD_SDK" --base-url "file://$EMPTY_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"

# The manager must NOT have been dragged backwards.
_axl_target="$(readlink -f "$BIN/axl" 2>/dev/null || echo "")"
[[ "$_axl_target" != "$PREFIX/axl-sdk-$OLD_SDK/bin/axl" ]]
check $? "\`use <older>\` does not repoint axl at a prefix with no installer"

# ...and the escape hatch still works: whatever axl now resolves to must be
# able to find an installer, or the user is stranded.
[[ -n "$_axl_target" ]] && [[ -f "$(dirname "$(dirname "$_axl_target")")/libexec/axl/install.sh" ]]
check $? "the axl on PATH can still reach a staged installer"

# The SDK itself DID roll back -- that is the point of `use`. Only the manager
# is pinned forward.
[[ "$(readlink "$PREFIX/axl-sdk")" == "axl-sdk-$OLD_SDK" ]]
check $? "...while the SDK itself is still rolled back to $OLD_SDK"

# A user stranded BEFORE this fix cannot run install.sh to get it -- fetching
# install.sh is the very thing they lost. So `axl` looks sideways itself.
ln -sfn "$PREFIX/axl-sdk-$OLD_SDK/bin/axl" "$BIN/axl"
"$BIN/axl" update --help > "$WORK/heal.log" 2>&1
grep -q "usage: sh install.sh" "$WORK/heal.log"
check $? "a stranded axl finds a sibling prefix's installer instead of dying"
! grep -q "no installer at" "$WORK/heal.log"
check $? "...and does not report the old dead end"

test_host_summary "installer asset resolution ($TEST_ARCH)"
