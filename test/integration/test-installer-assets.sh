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
# Fixture builders live in release-fixture.sh -- test-install-lifecycle.sh
# builds the same shape of release and a second copy would drift.
# shellcheck source=/dev/null
source "$(dirname "$0")/release-fixture.sh"

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
# The real host-tools component carries bin/axl and NOTHING else executable --
# a fixture with bin/axl-cc in it would let the manager shadow the SDK's own
# tools, and then assert that it does.
publish "$NEW_REL" "axl-sdk-host-tools-$NEW_VER.tar.gz"    "axl-sdk-host-tools-$NEW_VER" "$NEW_VER" make_manager_tree
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

# --print-prefix must name the SDK, not whatever tree `axl` happens to live in.
# §6 gives its purpose as stopping a consumer hardcoding /usr/include/axl-sdk,
# and every path a consumer wants from it -- headers, libs,
# share/axl/pci-ids.json5 -- is SDK content. Under §20's M2 `axl` moves into a
# manager root that holds none of that, so a self-referential answer would send
# the flagship consumer's PCI_IDS lookup somewhere with no sidecars in it.
run_installer "$WORK/pp.log" --use "$NEW_VER" --base-url "file://$NEW_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
_pp="$("$BIN/axl" --print-prefix 2>/dev/null)"
[[ "$_pp" == "$PREFIX/axl-sdk-$NEW_VER" ]]
check $? "--print-prefix names the current SDK (got '${_pp:-<empty>}')"

# ...and it follows `axl use`, because that is what "current" means.
run_installer "$WORK/pp2.log" --use "$OLD_SDK" --base-url "file://$EMPTY_REL" \
    --prefix "$PREFIX" --bin-dir "$BIN"
_pp="$("$BIN/axl" --print-prefix 2>/dev/null)"
[[ "$_pp" == "$PREFIX/axl-sdk-$OLD_SDK" ]]
check $? "--print-prefix follows \`axl use\` (got '${_pp:-<empty>}')"

echo "=== M2: the manager is its own component ==="

# A clean prefix, so this is the bootstrap's behaviour and not a leftover.
M2P="$WORK/m2/share"; M2B="$WORK/m2/bin"; mkdir -p "$M2P" "$M2B"
run_installer "$WORK/m2.log" --base-url "file://$NEW_REL" --prefix "$M2P" --bin-dir "$M2B"
rc=$?

[[ "$rc" -eq 0 && -d "$M2P/axl-sdk-host-tools-$NEW_VER" ]]
check $? "installing the SDK also installs the host-tools manager (rc=$rc)" "$WORK/m2.log"

# `axl` comes from the MANAGER, not from the versioned SDK tree.
_t="$(readlink -f "$M2B/axl" 2>/dev/null)"
[[ "$_t" == "$(readlink -f "$M2P/axl-sdk-host-tools")/bin/axl" ]]
check $? "axl resolves to the manager, not the SDK prefix"

# ...while the per-version TOOLS still come from the SDK, which is the whole
# point of switching versions.
_c="$(readlink -f "$M2B/axl-cc" 2>/dev/null)"
[[ "$_c" == "$M2P/axl-sdk-$NEW_VER/bin/axl-cc" ]]
check $? "axl-cc still resolves into the SDK prefix"

# The manager survives a rollback -- the §20.1 defect, from the other side.
run_installer "$WORK/m2-use.log" --use "$OLD_SDK" --base-url "file://$OLD_REL" \
    --prefix "$M2P" --bin-dir "$M2B"
_t2="$(readlink -f "$M2B/axl" 2>/dev/null)"
[[ "$_t2" == "$_t" ]]
check $? "\`axl use <older>\` does not move the manager at all"
[[ "$(readlink "$M2P/axl-sdk")" == "axl-sdk-$OLD_SDK" ]]
check $? "...while the SDK marker does follow it"

# And `axl prune` must never remove the manager IN USE. It used to be enough
# to say "prune walks ^axl-sdk-[0-9] and this is not that" -- but prune now
# walks the manager family too, precisely so old generations stop piling up,
# so the guarantee is no longer "never touches host-tools" and asserting that
# would go red the first time a third manager root exists. What must hold is
# that the CURRENT manager and the running prefix are never proposed.
"$M2B/axl" prune --dry-run > "$WORK/m2-prune.log" 2>&1 || true
_mgr_cur="$(cd -P "$M2P/axl-sdk-host-tools" 2>/dev/null && pwd)"
! grep -qF "would remove      $_mgr_cur" "$WORK/m2-prune.log"
check $? "axl prune never proposes removing the CURRENT manager"

# ── `axl version` when the two numbers disagree, BOTH WAYS ────────────────
#
# The fixture above already left the SDK rolled back and the manager where it
# was, so this is the manager-NEWER case as it actually arises. It is ordinary
# and the advice is "axl update moves both".
"$M2B/axl" version > "$WORK/ver-newer.log" 2>&1
grep -qF "The manager is versioned separately and does not follow" "$WORK/ver-newer.log"
check $? "axl version: a NEWER manager is explained as the normal 'axl use' state" \
      "$WORK/ver-newer.log"

# THE OTHER DIRECTION, which had the same message and needed the opposite
# advice. A manager older than the SDK is what every install made at 4.6.0 or
# earlier looks like after an update -- install.sh only ever created a manager
# when there was none, and that `axl` predates the code that moves the manager,
# so it CANNOT carry itself. "axl update moves both" points that reader at the
# one command that cannot fix it, and the release notes were left telling them
# to compare two numbers by eye against a web page to find out which case they
# were in.
#
# Simulated by writing an older number into the manager root, because the
# faithful fixture is an actual pre-4.7.0 `axl` binary and the branch under
# test reads these two files.
_mgr_root="$(cd -P "$M2P/axl-sdk-host-tools" 2>/dev/null && pwd)"
printf '0.0.1\n' > "$_mgr_root/share/axl/version"
[[ -f "$_mgr_root/VERSION" ]] && printf '0.0.1\n' > "$_mgr_root/VERSION"
# EXACT, not the fragment "cannot": a six-letter substring is satisfied by any
# future diagnostic that happens to contain it, which is how an assertion comes
# to pin nothing while still reading like it pins the branch.
"$M2B/axl" version > "$WORK/ver-older.log" 2>&1
grep -qF "The manager is OLDER than the SDK. A manager this old cannot" "$WORK/ver-older.log" \
    && grep -qF "sh install.sh --host-tools" "$WORK/ver-older.log"
check $? "axl version: an OLDER manager gets the bootstrap, not 'axl update'" \
      "$WORK/ver-older.log"
# ...and specifically NOT the other message, which would send them to the one
# command that cannot help. Asserted separately: the bootstrap text appearing
# does not by itself mean the wrong advice stopped appearing.
! grep -qF "The manager is versioned separately and does not follow" "$WORK/ver-older.log"
check $? "and does not also print the 'axl update moves both' advice" \
      "$WORK/ver-older.log"

test_host_summary "installer asset resolution ($TEST_ARCH)"
