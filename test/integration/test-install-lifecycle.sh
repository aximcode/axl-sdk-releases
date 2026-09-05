#!/bin/bash
# test-meta: arch=none needs=podman est=60 local-only=0
# test-install-lifecycle.sh — install, upgrade, downgrade, prune, recover.
#
# WHY THIS EXISTS. Every other installer test checks ONE operation in
# isolation: does it resolve the right asset name, is it idempotent, do two
# concurrent runs collide, does a fresh machine end up working. Nothing
# exercised the SEQUENCE, and the sequence is where this install model is
# actually interesting -- versioned roots, a `current` marker per component, a
# manager that must outlive every version it manages, and a prune policy that
# deletes things.
#
# THE COST OF NOT HAVING IT, measured: §20 M2 linked `axl` out of the manager
# root, which made `axl prune` fail its own versioned-root guard and read the
# toolchain manifest from a prefix that never carries one. `axl prune` removed
# NOTHING, on every install from v4.6.0 onward, and exited 0 while doing it.
# Every existing test stayed green, because none of them installs two versions
# and then asks whether the old one goes away. Step 6 below is that question.
#
# WHY A CONTAINER. The transitions are about $HOME, $PATH and a prefix root
# with real symlinks in it. Run on the developer's box those either collide
# with a real install or get neutered into a temp directory that proves less.
# A container is a machine with nothing on it, and podman is already how
# test-consumer-install.sh and test-host-deps-minimal.sh do this.
#
# NO RELEASE IS FETCHED. install.sh's `--base-url` takes a local directory, so
# the whole release history is fabricated on the host by release-fixture.sh and
# mounted read-only. (The run is not network-free in the absolute: it pulls its
# image and apt-gets curl. What it never does is depend on a published
# release, which is what would make it flaky.) Three versions, because "keep current plus one previous" cannot
# be distinguished from "keep everything" with only two.
#
# Usage: ./test/integration/test-install-lifecycle.sh

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
set +e

INSTALLER="$PROJECT_DIR/packaging/install.sh"
[[ -f "$INSTALLER" ]] || { echo "FAIL: no $INSTALLER"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck source=/dev/null
source "$(dirname "$0")/release-fixture.sh"

echo "=== install lifecycle ==="
echo ""

# Absent podman is a SKIP that exits before any tally, matching
# test-host-deps-minimal.sh and test-consumer-install.sh. It says WHICH
# capability was missing rather than reporting a clean run of nothing.
command -v podman >/dev/null 2>&1 \
    || { echo "SKIP: podman not available -- the lifecycle runs in a container"; exit 0; }

# ---------------------------------------------------------------------------
# Three releases, each in its own directory, exactly as `--base-url` sees a
# real one: the SDK asset, the manager asset, VERSION and SHA256SUMS.
# ---------------------------------------------------------------------------
V1="1.0.0"; V2="1.1.0"; V3="1.2.0"
REL="$WORK/releases"
for v in "$V1" "$V2" "$V3"; do
    r="$REL/$v"
    publish "$r" "axl-sdk-linux-$v-x86_64.tar.gz"   "axl-sdk-$v"              "$v"
    publish "$r" "axl-sdk-host-tools-$v.tar.gz"     "axl-sdk-host-tools-$v"   "$v" make_manager_tree
    seal    "$r" "$v"
done
# An empty release: `axl use <on-disk version>` must not need the network, and
# pointing at a mirror with nothing in it is how that gets proven rather than
# assumed.
mkdir -p "$REL/empty"

for v in "$V1" "$V2" "$V3"; do
    [[ -f "$REL/$v/SHA256SUMS" ]] || { echo "FAIL: fixture $v has no SHA256SUMS"; exit 1; }
done
test_host_pass "fixture: three releases published with SHA256SUMS"

# ---------------------------------------------------------------------------
# The lifecycle, as one shell session on a machine with nothing on it.
#
# Every step re-checks that `axl` itself still runs. The manager outliving the
# versions it manages is the §20 invariant, and an upgrade or a prune that
# breaks it leaves the user with no way to fix anything -- so it is asserted
# after each transition rather than once at the end.
# ---------------------------------------------------------------------------
# !!! NO APOSTROPHES INSIDE THE LIFECYCLE STRING -- NOT EVEN A BALANCED PAIR !!!
#
# Everything from LIFECYCLE=' to the closing ' is ONE single-quoted bash string.
# A single quote anywhere inside CLOSES it and reopens it, so whatever sits
# between two of them stops being container script and becomes live HOST shell.
# A PAIR is even, so a quote-parity check does NOT catch it.
#
# Writing "control: 'toolchain list' reports MISSING" in a PASS message did
# exactly that: the host executed several hundred lines of the container script
# as its own code, and the failure surfaced ~150 lines further down as
# "LIFECYCLE: unbound variable". It has now cost two separate authors a debug
# cycle, which is why this warning is here and not only in a handoff note.
#
# So: no apostrophes, and no quoted phrases, in any comment or message below.
# Write "8f-s leftover", not "8f's leftover". The ONLY legal single quotes in
# the block are the two in the '"$VAR"' interpolation idiom.
#
# To check after editing -- strip that idiom, then assert nothing is left:
#
#   awk "/^LIFECYCLE='/,/^'$/" test/integration/test-install-lifecycle.sh \
#     | sed "s/'\"\$[A-Za-z_][A-Za-z0-9_]*\"'//g" | grep -n "'"
#
# It must print nothing but the opening and closing lines of the block.
#
LIFECYCLE='
set -u
# The image is deliberately bare -- install.sh `need_cmd`s curl, and tar needs
# ONLY curl: the fixture publishes .tar.gz, so tar needs no xz. Provisioned the way
# test-consumer-install.sh does, and fail loudly rather than letting every
# later step report "axl: not found" for a reason that has nothing to do with
# the lifecycle.
apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq curl >/dev/null 2>&1 \
    || { echo "  FAIL could not provision curl"; exit 1; }
P="$HOME/axlroot"; B="$HOME/bin"; PATH="$B:$PATH"; export PATH
# The manager now TRACKS the SDK across installs and updates -- they ship
# together, and a manager frozen at whatever version first created it never
# receives a fix to `axl` itself. It does NOT track a DOWNGRADE: step 5.
MGR_V="'"$V1"'"
rc=0
# A literal apostrophe cannot appear in this block (see the warning above the
# LIFECYCLE definition), and several assertions need one: `axl` single-quotes
# the --prefix it prints. \47 is that byte, built at run time instead.
SQ=$(printf "\47")
# `axl toolchain list --porcelain` is tab-separated, and every field read in
# this file goes through it. The readers used to parse the HUMAN listing by
# column index -- which is why that listing could not be laid out for a human
# without breaking them, and had not been. A literal tab cannot be written in
# an awk -F inside this block, so it is built once here.
TAB=$(printf "\t")
ok()   { echo "  PASS $1"; }
bad()  { echo "  FAIL $1"; rc=1; }
# Does the front door still work? Asked after every transition.
# EXACT, not "exited 0". `axl --version >/dev/null` passes for any axl on PATH
# that can read a version file at all -- including one belonging to a tree we
# have just switched away from.
manager_alive() {
    _mv="$(axl --version 2>/dev/null)"
    if [ "$_mv" = "axl $MGR_V" ]; then
        ok "$1: axl still runs, still $MGR_V"
    else
        bad "$1: expected axl $MGR_V, got: $_mv"
    fi
}
inst() {  # inst <version> <extra args...>
    # NOT `v`: that is the step-4 loop variable, and a global assignment
    # here would corrupt the iteration if inst were ever called inside it.
    _iv="$1"; shift
    sh /tmp/install.sh --yes --version "$_iv" --base-url "file:///rel/$_iv" \
       --prefix "$P" --bin-dir "$B" "$@"
}

echo "--- 1. fresh install of '"$V1"'"
if inst '"$V1"' > /tmp/1.log 2>&1; then ok "installs '"$V1"'"; else bad "install '"$V1"' exited $?"; tail -5 /tmp/1.log; fi
[ -d "$P/axl-sdk-'"$V1"'" ] && ok "the versioned root exists" || bad "no $P/axl-sdk-'"$V1"'"
[ "$(axl --print-version 2>/dev/null)" = "'"$V1"'" ] \
    && ok "axl --print-version is '"$V1"'" || bad "axl --print-version = $(axl --print-version 2>/dev/null)"
[ "$(axl --print-prefix 2>/dev/null)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "--print-prefix names the SDK root" || bad "--print-prefix = $(axl --print-prefix 2>/dev/null)"
manager_alive "after fresh install"

echo "--- 2. upgrade to '"$V2"'"
if inst '"$V2"' > /tmp/2.log 2>&1; then ok "installs '"$V2"'"; else bad "upgrade exited $?"; tail -5 /tmp/2.log; fi
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V2"'" ] \
    && ok "current moved to '"$V2"'" || bad "current is $(axl --print-prefix)"
[ -d "$P/axl-sdk-'"$V1"'" ] && ok "'"$V1"' is still on disk (rollback stays possible)" \
    || bad "the upgrade deleted '"$V1"'"
# The manager follows an UPGRADE. Frozen, it would never deliver a fix to
# `axl` itself -- the whole reason install.sh ships a manager component.
MGR_V="'"$V2"'"
manager_alive "after upgrade"

echo "--- 3. upgrade to '"$V3"'"
inst '"$V3"' > /tmp/3.log 2>&1 || { bad "upgrade to '"$V3"' exited $?"; tail -5 /tmp/3.log; }
n=$(ls -d "$P"/axl-sdk-[0-9]* 2>/dev/null | wc -l)
[ "$n" -eq 3 ] && ok "three SDK roots on disk" || bad "expected 3 SDK roots, found $n"
MGR_V="'"$V3"'"
manager_alive "after second upgrade"

echo "--- 4. axl list sees them and marks the current one"
axl list > /tmp/list.txt 2>&1
for v in '"$V1"' '"$V2"' '"$V3"'; do
    grep -q "axl-sdk-$v" /tmp/list.txt && ok "list shows $v" || bad "list omits $v"
done
grep -q "^\* axl-sdk-'"$V3"'" /tmp/list.txt \
    && ok "list marks '"$V3"' as current" || { bad "current not marked"; cat /tmp/list.txt; }
# EVERY ROW CARRIES ITS SIZE ON DISK. The legend directly under this listing
# advises axl prune, and the size is the number that decides whether that is
# worth running -- it was the one fact a reader had to leave and measure for
# themselves. Counted rather than spot-checked: one sized row out of three
# would satisfy a grep and mean the du had failed for the other two.
_rows=$(grep -c "axl-sdk-[0-9]" /tmp/list.txt)
_sized=$(awk "/axl-sdk-[0-9]/ && \$NF ~ /^[0-9]+([.][0-9]+)?[KMGT]?\$/ { n++ } END { print n+0 }" /tmp/list.txt)
[ "$_rows" -gt 0 ] && [ "$_sized" = "$_rows" ] \
    && ok "and every one of the $_rows rows carries its size on disk" \
    || { bad "sizes: $_sized of $_rows rows"; cat /tmp/list.txt; }

echo "--- 5. DOWNGRADE to '"$V1"', offline"
# An empty mirror: a version already on disk must not need the network.
if axl use '"$V1"' --base-url file:///rel/empty > /tmp/use.log 2>&1; then
    ok "axl use '"$V1"' succeeds against an EMPTY mirror"
else
    bad "axl use '"$V1"' exited $?"; tail -5 /tmp/use.log
fi
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "--print-prefix follows the downgrade" || bad "--print-prefix = $(axl --print-prefix)"
[ "$(axl-cc --version 2>/dev/null)" = "axl-cc '"$V1"'" ] \
    && ok "axl-cc resolves to the downgraded SDK" || bad "axl-cc = $(axl-cc --version 2>/dev/null)"
manager_alive "after downgrade"
# §20: a downgrade must NOT drag the manager backwards. do_use returns before
# ensure_manager so this holds by construction -- asserted anyway, because it
# is exactly the kind of thing a later refactor relocates.
[ "$(axl --version)" = "axl '"$V3"'" ] \
    && ok "a downgrade leaves the manager at '"$V3"'" \
    || bad "the manager moved on a downgrade: $(axl --version)"
# THE BIN DIR THE INSTALL USED, not the one `axl` guesses. `axl use` re-execs
# the staged installer with a --bin-dir it recomputes from
# AXL_BIN_DIR/XDG_BIN_HOME/$HOME/.local/bin -- it does not know the --bin-dir
# this install was created with. So a custom bin dir silently splits the
# install across two directories: the old links stay stale on PATH and the new
# ones land somewhere nothing is looking.
# BOTH HALVES. The negative alone ("nothing appeared in the default dir") is
# satisfied by `axl use` having crashed before writing anything -- the shape
# that makes a green run mean nothing.
[ "$(readlink -f "$B/axl-cc")" = "$P/axl-sdk-'"$V1"'/bin/axl-cc" ] \
    && ok "the PATH axl-cc now points into the downgraded SDK" \
    || bad "PATH axl-cc -> $(readlink -f "$B/axl-cc")"
if [ -e "$HOME/.local/bin/axl-cc" ]; then
    bad "axl use ALSO wrote links into the default bin dir"
else
    ok "and nothing was written to the default bin dir"
fi
# §20: the manager is NOT the managed. Downgrading the SDK must not drag the
# manager backwards -- that is what would strand a user on an installer too
# old to fix itself.
mgr="$(readlink -f "$B/axl")"
case "$mgr" in
    "$P"/axl-sdk-host-tools-*) ok "axl still comes from the manager root" ;;
    *) bad "axl now resolves into $mgr" ;;
esac

echo "--- 6. prune: current + one previous, and it MUST remove something"
axl prune > /tmp/prune.log 2>&1 || bad "prune exited $?"
[ -d "$P/axl-sdk-'"$V1"'" ] && ok "prune keeps the CURRENT version" || bad "prune removed the current version"
[ -d "$P/axl-sdk-'"$V3"'" ] && ok "prune keeps one previous" || bad "prune kept no previous"
# The assertion that would have caught M2 breaking prune entirely.
if [ -d "$P/axl-sdk-'"$V2"'" ]; then
    bad "prune removed NOTHING -- the superseded version is still there"
    cat /tmp/prune.log
else
    ok "prune removed the superseded version"
fi
manager_alive "after prune"

echo "--- 7. recovery: a pruned version comes back"
if axl use '"$V2"' --base-url "file:///rel/'"$V2"'" > /tmp/recover.log 2>&1; then
    ok "axl use re-downloads a pruned version"
else
    bad "recovery exited $?"; tail -5 /tmp/recover.log
fi
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V2"'" ] \
    && ok "and it becomes current" || bad "--print-prefix = $(axl --print-prefix)"
manager_alive "after recovery"

echo "--- 8. axl update self-updates: the manager moves too"
# The manager carries `axl` and the staged install.sh, so a manager frozen at
# install time means every fix to either is invisible to a machine that
# already has one. `axl update` now updates the manager FIRST and re-execs the
# new `axl` to do the SDK -- so a fix to install.sh applies on THIS update
# rather than the next one. AXL_SELF_UPDATED guards the re-exec against a loop;
# this step completing at all is that guard working.
axl use '"$V1"' --base-url file:///rel/empty > /tmp/pre.log 2>&1
sh /tmp/install.sh --yes --host-tools --version '"$V1"' \
   --base-url "file:///rel/'"$V1"'" --prefix "$P" --bin-dir "$B" > /tmp/mgrdown.log 2>&1
[ "$(axl --version)" = "axl '"$V1"'" ] \
    && ok "fixture: the manager is back at '"$V1"'" \
    || bad "fixture setup left the manager at $(axl --version)"
# The SDK half of the precondition was asserted nowhere, and step 8-s closing
# check passes either way because `update` moves the SDK to V3 regardless.
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "fixture: and the SDK is back at '"$V1"'" \
    || bad "fixture setup left the SDK at $(axl --print-prefix)"
if axl update --base-url "file:///rel/'"$V3"'" > /tmp/upd.log 2>&1; then
    ok "axl update succeeds"
else
    bad "axl update exited $?"; tail -8 /tmp/upd.log
fi
# EXACTLY ONE. The line used to be an unconditional echo printed before the
# install was even attempted, so it matched with the whole self-update block
# deleted. It now prints only when the manager actually moved, and a second
# one would mean the re-exec looped.
n_mgr=$(grep -c "updating the manager" /tmp/upd.log)
[ "$n_mgr" = "1" ] \
    && ok "the manager was updated exactly once" \
    || { bad "expected 1 manager-update line, got $n_mgr"; tail -8 /tmp/upd.log; }
# THE RE-EXEC, observably. Each staged install.sh announces its own version,
# so this is the difference between the SDK being installed by the NEW
# installer (the point of re-execing) and by the old one. Without the re-exec
# only the '"$V1"' marker appears.
grep -q "AXL-INSTALLER-MARKER '"$V3"'" /tmp/upd.log \
    && ok "the SDK was installed by the NEW install.sh" \
    || { bad "no '"$V3"' installer marker -- the re-exec did not happen"
         grep "AXL-INSTALLER-MARKER" /tmp/upd.log | sort -u | sed "s/^/        /"; }
MGR_V="'"$V3"'"
manager_alive "after axl update"
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK moved to '"$V3"' in the same run" \
    || bad "SDK is $(axl --print-prefix)"

echo "--- 8b. axl version reports BOTH, because they can differ"
# `axl --version` is the PROGRAM version, which since §20 M2 is the manager --
# so on a real install it answers a different question than "which SDK am I
# using", and after any update the two legitimately diverge. Nothing reported
# the SDK version as a NUMBER at all; --print-prefix returns a path.
axl use '"$V1"' --base-url file:///rel/empty > /tmp/use2.log 2>&1
axl version > /tmp/ver.txt 2>&1 || bad "axl version exited $?"
# BY FIELD. Grepping the version as a substring passed against `unknown`,
# because the version also appears inside the prefix path printed on the same
# line -- all three of these assertions did.
mv_seen=$(awk "\$1 == \"manager\" { print \$2 }" /tmp/ver.txt)
sv_seen=$(awk "\$1 == \"sdk\" { print \$2 }" /tmp/ver.txt)
[ "$mv_seen" = "'"$V3"'" ] && ok "axl version reports manager '"$V3"'" \
    || { bad "manager field = $mv_seen"; cat /tmp/ver.txt; }
[ "$sv_seen" = "'"$V1"'" ] && ok "and sdk '"$V1"', which differs" \
    || { bad "sdk field = $sv_seen"; cat /tmp/ver.txt; }
grep -q "does not follow" /tmp/ver.txt \
    && ok "and explains why the two differ" \
    || { bad "no divergence note"; cat /tmp/ver.txt; }

echo "--- 8bb. a PINNED OLDER install must not roll the manager back"
# §20, second downgrade path. `axl use <older>` returns before the manager
# logic so it was safe -- but `install.sh --version <older>` runs the full
# install path and reached it, dragging the manager back to the pinned
# version and stranding the user on an installer too old to fix itself. That
# is the command the upgrade notes teach, and what a consumer pinning in CI
# runs every build. The lifecycle only ever installed ASCENDING, so nothing
# saw it.
[ "$(axl --version)" = "axl '"$V3"'" ] \
    && ok "fixture: the manager is at '"$V3"' before the pinned install" \
    || bad "fixture: manager is $(axl --version)"
# --force, because THIS ASSERTION DID NOT REACH THE CODE IT TESTS without it:
# axl-sdk-1.0.0 is already on disk from step 1, so already_installed
# short-circuits main() before ensure_manager ever runs, and the check passed
# with the forward-only guard deliberately disabled. --force is what makes the
# pinned install actually perform an install.
inst '"$V1"' --force > /tmp/pinned.log 2>&1 || bad "pinned install exited $?"
# `inst` runs the MOUNTED installer, which carries no fixture marker -- so the
# precondition is checked the other way round: it must not have short-circuited.
if grep -q "nothing to do" /tmp/pinned.log; then
    bad "the pinned install short-circuited; ensure_manager was never reached"
    tail -4 /tmp/pinned.log
else
    ok "fixture: the pinned install really performed an install"
fi
[ "$(axl --version)" = "axl '"$V3"'" ] \
    && ok "installing a PINNED older SDK leaves the manager at '"$V3"'" \
    || bad "the manager rolled back to $(axl --version)"
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "while the SDK itself did move to '"$V1"'" \
    || bad "the pinned install did not move the SDK: $(axl --print-prefix)"

echo "--- 8c. update refuses args that are not an update"
for bad_arg in --uninstall --host-tools; do
    if axl update "$bad_arg" > /tmp/rej.txt 2>&1; then
        bad "axl update $bad_arg was accepted"
    else
        ok "axl update $bad_arg is refused"
    fi
done
# The manager must still be there after those refusals -- the point of
# refusing --uninstall is that it used to DELETE the manager root and then
# exec a path inside it.
[ -x "$P/axl-sdk-host-tools-'"$V3"'/bin/axl" ] \
    && ok "and the manager survived the refusals" \
    || bad "the manager root is gone after a refused arg"
# Re-install $V2 so step 9 has the version it uninstalls.
axl use '"$V2"' --base-url "file:///rel/'"$V2"'" > /tmp/re2.log 2>&1

echo "--- 8d. every documented option runs on a REAL install"
# The option surface was covered across four files, three of them on the HOST
# against synthetic prefixes -- and `-h`, a documented alias, was covered
# nowhere. A synthetic prefix is a fair test of parsing and a poor one of
# resolution: $PREFIX, $PATH, the markers and the links are the things a real
# install has and a fixture approximates. This walks the whole surface once,
# where all of that is real.
axl > /tmp/bare.txt 2>&1;        bare_rc=$?
axl -h > /tmp/h.txt 2>&1;        h_rc=$?
axl --help > /tmp/help.txt 2>&1; help_rc=$?
# THE TEXT IS THE SAME AND THE STATUS IS NOT, on purpose. `-h`/`--help` were
# ASKED for the listing and got it: 0. A bare `axl` supplied no command, which
# is the same mistake `axl use` with no version and `axl <unknown>` both exit 2
# for -- and it used to exit 0, so `axl "$cmd"` with an empty $cmd reported
# success for doing nothing. The listing still goes to stdout either way,
# because hiding the discovery surface behind a redirect was never the point of
# the status.
[ "$bare_rc" = 2 ] && [ "$h_rc" = 0 ] && [ "$help_rc" = 0 ] \
    && ok "-h and --help exit 0; a bare axl exits 2" \
    || bad "exit codes: bare=$bare_rc (want 2) -h=$h_rc --help=$help_rc (want 0)"
# -h is an ALIAS, so identical output is the contract -- not merely "it also
# exits 0", which a branch that fell through to usage-by-accident would pass.
if cmp -s /tmp/h.txt /tmp/help.txt && cmp -s /tmp/bare.txt /tmp/help.txt; then
    ok "-h and bare print exactly what --help prints"
else
    bad "the three spellings disagree"; diff /tmp/h.txt /tmp/help.txt | head -4
fi
grep -q "axl toolchain" /tmp/help.txt && ok "--help documents the toolchain verb" \
    || bad "--help does not mention toolchain"
# The machine interfaces: a bare value each, no banner.
[ "$(axl --print-version)" = "$(axl --version | sed "s/^axl //")" ] \
    && ok "--print-version is --version without the program name" \
    || bad "--print-version=$(axl --print-version) --version=$(axl --version)"
case "$(axl --print-prefix)" in
    /*) ok "--print-prefix emits an absolute path" ;;
    *)  bad "--print-prefix emitted: $(axl --print-prefix)" ;;
esac
# The dispatcher half, on a real install: a staged libexec command must be
# offered and runnable, and an unknown one must be refused.
if axl prune --dry-run > /tmp/disp.txt 2>&1; then
    ok "a libexec command runs through the dispatcher"
else
    bad "axl prune --dry-run exited $?"; tail -3 /tmp/disp.txt
fi
axl definitely-not-a-command > /tmp/unk.txt 2>&1
[ "$?" = 2 ] && ok "an unknown command exits 2" || bad "unknown command exit $?"
# `toolchain list` on an SDK that ships no manifest must fail CLEANLY and name
# the file -- not crash, and not report an empty toolchain set as success.
if axl toolchain list > /tmp/tc.txt 2>&1; then
    ok "toolchain list succeeded (this SDK carries a manifest)"
else
    grep -q "axl-toolchains.conf" /tmp/tc.txt \
        && ok "toolchain list fails cleanly and names the missing manifest" \
        || { bad "toolchain list failed without naming the manifest"; head -3 /tmp/tc.txt; }
fi

echo "--- 8e. the REAL toolchain, resolved through a real install"
# Everything about toolchains so far ran against directories the test made up.
# This mounts the actual /opt tree at its own path, so `axl toolchain list`
# resolves the manifest, finds the root, and PROBES the compiler -- the
# difference between "a directory exists" and "the toolchain works", which is
# why list asks the compiler rather than stat-ing.
#
# It also exercises the ALREADY-INSTALLED branch of the installer, which is
# where the receipt bug lived: it returned before writing one, leaving every
# pre-existing toolchain unprunable forever.
#
# BALANCED SKIP: the same number of assertions on both paths, so the exact
# assertion count stays a valid check when no toolchain is present.
if [ "${HAVE_TC:-0}" = "1" ]; then
    mkdir -p "$P/axl-sdk/share/axl"
    # BOTH arches: install-toolchain.sh builds the aa64 URL at script level,
    # so an x64-only manifest dies on an unbound variable before it parses an
    # argument. aa64 points at a path that does not exist -- this case is
    # about x64, and the script only installs the target it is asked for.
    cat > "$P/axl-sdk/share/axl/axl-toolchains.conf" <<CONF
AXL_AA64_TOOLCHAIN_VERSION=14.3.rel1
AXL_AA64_TOOLCHAIN_DIR=/nonexistent-aa64
AXL_AA64_GXX_DEFAULT=/nonexistent-aa64/bin/aarch64-none-elf-g++
AXL_AA64_GCC_DEFAULT=/nonexistent-aa64/bin/aarch64-none-elf-gcc
AXL_AA64_BINUTILS_PREFIX_DEFAULT=/nonexistent-aa64/bin/aarch64-none-elf-
AXL_AA64_TOOLCHAIN_SHA256=$(printf "0%.0s" $(seq 64))
AXL_X64_TOOLCHAIN_VERSION=real
AXL_X64_TOOLCHAIN_DIR=$X64_TC
AXL_X64_GXX_DEFAULT=$X64_TC/bin/x86_64-elf-g++
AXL_X64_GCC_DEFAULT=$X64_TC/bin/x86_64-elf-gcc
AXL_X64_BINUTILS_PREFIX_DEFAULT=$X64_TC/bin/x86_64-elf-
CONF
    axl toolchain list > /tmp/tcreal.txt 2>&1
    axl toolchain list --porcelain > /tmp/tcreal-p.txt 2>&1
    if [ "$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/tcreal-p.txt)" = installed ]; then
        ok "toolchain list finds the real toolchain and marks it installed"
    else
        bad "the real toolchain was not reported installed"; cat /tmp/tcreal.txt
    fi
    # The probe, not the directory: this line can only come from running it.
    if grep -q "x86_64-elf-g++" /tmp/tcreal.txt; then
        ok "and reports what the compiler itself prints"
    else
        bad "no compiler version line"; cat /tmp/tcreal.txt
    fi
    # A READ-ONLY toolchain root: the installer cannot write its receipt, and
    # must say so and carry on rather than abort an install that is fine.
    axl toolchain install x64 > /tmp/tcinst.txt 2>&1; tci_rc=$?
    if [ "$tci_rc" = 0 ] && grep -q "already installed" /tmp/tcinst.txt; then
        ok "toolchain install takes the already-installed path on a real tree"
    else
        bad "toolchain install rc=$tci_rc"; tail -4 /tmp/tcinst.txt
    fi
    # THE READ-ONLY CASE, which this mount gives for free and which a shared
    # or managed /opt makes real. The receipt cannot be written; the install
    # is nonetheless fine, so it must WARN and exit 0 rather than abort a
    # completed install -- and it must say so, because the consequence (prune
    # will not remove this root) is otherwise invisible.
    if grep -q "no receipt written" /tmp/tcinst.txt; then
        ok "a read-only toolchain root warns about the receipt and carries on"
    else
        bad "no receipt warning on a read-only root"; tail -4 /tmp/tcinst.txt
    fi
else
    ok "SKIP: no x64 toolchain mounted -- list-finds-it"
    ok "SKIP: no x64 toolchain mounted -- compiler-probe"
    ok "SKIP: no x64 toolchain mounted -- already-installed path"
    ok "SKIP: no x64 toolchain mounted -- read-only receipt warning"
fi

echo "--- 8f. toolchain uninstall: refuses an unmarked root, removes a marked one"
# THE ONLY PLACE A REMOVAL IS ASSERTED, and the reason it is here rather than
# in test-axl-toolchain-verb.sh: that file declares needs= and runs on the
# developer box, where the real manifest names the /opt toolchain the whole
# suite compiles with -- and that root carries a valid receipt, so a removal
# assertion there would not be refused, it would succeed. Here the tree under
# test is SYNTHETIC and created inside the container. $X64_TC, the real
# toolchain mounted read-only above, is never named by the manifest this step
# writes, and the case aborts if it ever is.
TCF="$HOME/tcfixture/x86_64-elf-gcc-uninstall-fixture"
mkdir -p "$TCF/bin"
for t in gcc g++; do
    printf "#!/bin/sh\necho x86_64-elf-$t 14.3.0-fixture\n" > "$TCF/bin/x86_64-elf-$t"
    chmod +x "$TCF/bin/x86_64-elf-$t"
done
mkdir -p "$P/axl-sdk/share/axl"
cat > "$P/axl-sdk/share/axl/axl-toolchains.conf" <<CONF
AXL_AA64_TOOLCHAIN_VERSION=14.3.rel1
AXL_AA64_TOOLCHAIN_DIR=/nonexistent-aa64
AXL_AA64_GXX_DEFAULT=/nonexistent-aa64/bin/aarch64-none-elf-g++
AXL_AA64_GCC_DEFAULT=/nonexistent-aa64/bin/aarch64-none-elf-gcc
AXL_AA64_BINUTILS_PREFIX_DEFAULT=/nonexistent-aa64/bin/aarch64-none-elf-
AXL_AA64_TOOLCHAIN_SHA256=0000000000000000000000000000000000000000000000000000000000000000
AXL_X64_TOOLCHAIN_VERSION=14.3.0-fixture
AXL_X64_TOOLCHAIN_DIR=$TCF
AXL_X64_GXX_DEFAULT=$TCF/bin/x86_64-elf-g++
AXL_X64_GCC_DEFAULT=$TCF/bin/x86_64-elf-gcc
AXL_X64_BINUTILS_PREFIX_DEFAULT=$TCF/bin/x86_64-elf-
CONF
# SAFETY ABORT -- the container-side twin of the preflight in
# test-axl-toolchain-verb.sh. Everything below invokes a verb whose success
# path is `rm -rf <dir>`, and what makes that safe is the root being one this
# step created under $HOME. Read it back out of the manifest actually on disk
# rather than trusting the variable that wrote it.
tcdir=$(sed -n "s/^AXL_X64_TOOLCHAIN_DIR=//p" "$P/axl-sdk/share/axl/axl-toolchains.conf")
case "$tcdir" in
    "$HOME"/*) ok "safety: the root under test is inside the container HOME" ;;
    *) bad "safety: the root under test is $tcdir -- ABORTING before any uninstall"
       exit 1 ;;
esac
# THE OWNERSHIP GUARD, from the same file and the same keys axl-prune.sh
# reads. Existing toolchains predate receipts, so a refusal has to carry the
# way to make the root ours rather than a dead end.
axl toolchain uninstall x64 > /tmp/un1.txt 2>&1; u1=$?
if [ "$u1" != 0 ] && grep -q "axl-install-toolchain x64" /tmp/un1.txt; then
    ok "uninstall refuses an unmarked root and names the re-mark remedy"
else
    bad "unmarked root: rc=$u1"; cat /tmp/un1.txt
fi
# Separate, and pre-satisfied by construction -- which is why the assertion
# above pins the refusal TEXT. A surviving directory alone is equally
# satisfied by a crash before the delete.
[ -d "$TCF" ] && ok "and the refused root is still there"               || bad "THE REFUSED ROOT WAS REMOVED"
# THE ROUND TRIP: the installer marks, the uninstaller reads the same mark.
# --prefix, because install-toolchain.sh relocates the manifest directory NAME
# under INSTALL_ROOT (default /opt) rather than installing at the path the
# manifest spells. Without it the re-mark lands at
# /opt/x86_64-elf-gcc-uninstall-fixture -- a path this step never created, and
# the same /opt the real toolchain is mounted into.
axl toolchain install x64 --prefix "$HOME/tcfixture" > /tmp/un2.txt 2>&1
if [ -f "$TCF/.axl-receipt" ]; then
    ok "axl toolchain install re-marks the root with a receipt"
else
    bad "no receipt after install"; cat /tmp/un2.txt
fi
axl toolchain uninstall x64 > /tmp/un3.txt 2>&1; u3=$?
# NOT the exit code alone: the nothing-to-remove branch exits 0 too, so a
# resolution bug pointing at an absent path would pass an rc-only check while
# the root sat untouched. The success line names what it removed.
if [ "$u3" = 0 ] && grep -q "removed $TCF" /tmp/un3.txt; then
    ok "uninstall removes a root we marked, and names it"
else
    bad "marked uninstall: rc=$u3"; cat /tmp/un3.txt
fi
[ -d "$TCF" ] && bad "the root is STILL THERE after a successful uninstall"               || ok "and the marked root is gone"
axl toolchain uninstall x64 > /tmp/un4.txt 2>&1; u4=$?
if [ "$u4" = 0 ] && grep -q "nothing to remove" /tmp/un4.txt; then
    ok "a second uninstall is a no-op, not an error"
else
    bad "second uninstall: rc=$u4"; cat /tmp/un4.txt
fi
# REMOVAL AND RESOLUTION AGREE. The compiler half of that chain -- auto
# falling back to host once the bare-metal toolchain is gone -- belongs to
# Task 3 and is pinned by test-axl-toolchain-verb.sh case 12; the axl-cc in
# this fixture is a stub, so what can be proved here is that the manager
# probe of the same root now reports it absent.
axl toolchain list --porcelain > /tmp/un5.txt 2>&1
tcstate=$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/un5.txt)
# THE g++ FIELD, read from --porcelain. The human listing spells this as words
# a reader can act on (gcc + g++, gcc only no C++, not installed); the machine
# form keeps installed/MISSING per locator, and which locator it is comes from
# the field POSITION rather than from a prefix on the value.
# (g++) since it and the separate "builds with:" line below it can name
# different compilers -- see AXL-Host-Toolchain-Design.md and the `list`
# case in scripts/axl.
[ "$tcstate" = "MISSING" ] && ok "and toolchain list now reports x64 MISSING" \
    || { bad "list says x64 is $tcstate after the uninstall"; cat /tmp/un5.txt; }

echo "--- 8g. uninstall: the rename-first form REFUSES where -w is blind"
# THE CASE THAT DISTINGUISHES THE TWO FORMS, and a green run does not: `-w`
# and rename-first behave identically everywhere `-w` is accurate. `-w` is
# ADVISORY. A sticky, world-writable parent holding an entry owned by someone
# else -- the classic /tmp shape -- leaves it TRUE while both the rename and
# the unlink are refused. Under the old in-place `rm -rf` the tree is emptied
# on the way to finding that out, and the compiler is gone before the error
# is printed; under rename-first the refusal arrives with nothing touched.
#
# A read-only mount is deliberately NOT used here: access(2) returns EROFS, so
# `-w` does see that one (measured), and a case `-w` catches would prove
# nothing about the difference.
STICKY="$HOME/tcsticky"
SD="$STICKY/x86_64-elf-gcc-sticky-fixture"
mkdir -p "$SD/bin"
printf "#!/bin/sh\necho x86_64-elf-gcc 14.3.0-sticky\n" > "$SD/bin/x86_64-elf-gcc"
cp "$SD/bin/x86_64-elf-gcc" "$SD/bin/x86_64-elf-g++"
cat > "$SD/.axl-receipt" <<RCPT
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=x64
RCPT
# The tree is world-writable so the unprivileged user CAN delete its contents
# -- that is what makes the destructive form destructive. The entries stay
# root-owned and the parent is sticky, so it cannot unlink the root itself.
chmod -R 0777 "$SD"
chmod 1777 "$STICKY"
sed -i "s|^AXL_X64_TOOLCHAIN_DIR=.*|AXL_X64_TOOLCHAIN_DIR=$SD|; \
        s|^AXL_X64_GXX_DEFAULT=.*|AXL_X64_GXX_DEFAULT=$SD/bin/x86_64-elf-g++|; \
        s|^AXL_X64_GCC_DEFAULT=.*|AXL_X64_GCC_DEFAULT=$SD/bin/x86_64-elf-gcc|; \
        s|^AXL_X64_BINUTILS_PREFIX_DEFAULT=.*|AXL_X64_BINUTILS_PREFIX_DEFAULT=$SD/bin/x86_64-elf-|" \
    "$P/axl-sdk/share/axl/axl-toolchains.conf"
# The same safety abort as 8f, re-asked because this is a different root.
tcdir=$(sed -n "s/^AXL_X64_TOOLCHAIN_DIR=//p" "$P/axl-sdk/share/axl/axl-toolchains.conf")
case "$tcdir" in
    "$HOME"/*) ok "safety: the sticky root under test is inside the container HOME" ;;
    *) bad "safety: the root under test is $tcdir -- ABORTING before any uninstall"
       exit 1 ;;
esac
useradd -m tu >/dev/null 2>&1 || adduser --disabled-password --gecos "" tu >/dev/null 2>&1
chmod o+x "$HOME"
# CONTROL FIRST: the blind spot has to actually exist, or the refusal below is
# just the -w check firing and the case proves nothing about the rename.
if su tu -c "[ -w $STICKY ]"; then
    ok "control: -w reports the sticky parent WRITABLE -- the blind spot is real"
else
    bad "control: -w already refuses this parent, so 8g cannot distinguish the forms"
fi
su tu -c "PATH=$B:\$PATH axl toolchain uninstall x64" > /tmp/st1.txt 2>&1; s1=$?
if [ "$s1" != 0 ] && grep -q "NOTHING was removed" /tmp/st1.txt; then
    ok "uninstall refuses when the rename is denied, saying nothing was removed"
else
    bad "sticky-parent uninstall: rc=$s1"; cat /tmp/st1.txt
fi
# THE WHOLE POINT. A non-zero exit and a surviving directory are BOTH true of
# the destructive form as well -- the compiler still running is the only thing
# that separates prevention from a post-hoc detector.
[ -x "$SD/bin/x86_64-elf-gcc" ] \
    && ok "and the toolchain is untouched -- the compiler still runs" \
    || bad "THE COMPILER WAS DESTROYED by a refused uninstall"
# CONTROL, and it is what makes the assertion above mean anything: the same
# scenario under a plain in-place rm -rf DOES empty the tree.
su tu -c "rm -rf -- $SD" > /tmp/st2.txt 2>&1
if [ ! -x "$SD/bin/x86_64-elf-gcc" ] && [ -d "$SD" ]; then
    ok "control: a plain rm -rf here empties the tree and still cannot unlink it"
else
    bad "control failed -- this scenario is not destructive, so 8g proves nothing"
    ls -l "$SD/bin" 2>&1 | head -3
fi

echo "--- 8h. axl update fetches NOTHING for an arch with no toolchain"
# THE ASSERTION THAT PROTECTS THE HOST-GCC CONSUMER, and the reason `axl
# update` may not simply pass --toolchain all. x64 defaults to
# AXL_TOOLCHAIN=auto, so someone who deliberately has no bare-metal toolchain
# compiles C with the host gcc -- and posting them a ~55 MB download during a
# routine update would undo the entire point of that.
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8h-pre.log 2>&1
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "fixture: back on '"$V1"' for the negative case" \
    || bad "fixture: --print-prefix = $(axl --print-prefix)"
# A COUPLING TO install.sh, ASSERTED RATHER THAN COMMENTED. Steps 8e/8f/8g
# overwrite the CURRENT SDK-s share/axl/axl-toolchains.conf with manifests of
# their own; 8h/8i work only because extract_tree() `rm -rf`s the versioned root
# and re-extracts it, restoring the fixture manifest each version ships. If that
# ever became "skip if present", every assertion below would still pass -- while
# testing the wrong roots. A comment does not stop that; this does.
mf="$P/axl-sdk/share/axl/axl-toolchains.conf"
mfdir=$(sed -n "s/^AXL_X64_TOOLCHAIN_DIR=//p" "$mf" 2>/dev/null)
[ "$mfdir" = "/opt/axl-fixture-x64-'"$V1"'" ] \
    && ok "fixture: the manifest in play is '"$V1"'-s own, not 8f-s leftover" \
    || bad "fixture: manifest names $mfdir -- 8h/8i would test the wrong roots"
# CONTROL. A negative assertion proves nothing unless the detector COULD have
# said yes -- so pin that both arches read MISSING here, through the same probe
# `axl update` uses.
axl toolchain list --porcelain > /tmp/8h-tc.txt 2>&1
x0=$(awk -F"$TAB" "\$1 == \"x64\"  { print \$5 }" /tmp/8h-tc.txt)
a0=$(awk -F"$TAB" "\$1 == \"aa64\" { print \$5 }" /tmp/8h-tc.txt)
if [ "$x0" = "MISSING" ] && [ "$a0" = "MISSING" ]; then
    ok "control: neither arch has a toolchain before the update"
else
    bad "control: x64=$x0 aa64=$a0 -- the negative below would prove nothing"
    cat /tmp/8h-tc.txt
fi
axl update --base-url "file:///rel/'"$V3"'" > /tmp/8h.log 2>&1; u8h=$?
# THREE THINGS, not one. "No toolchain was installed" is equally true of an
# update that died before it ever reached the toolchain step, so the exit
# status and the SDK actually moving have to be asserted alongside the absence.
[ "$u8h" = 0 ] && ok "the update exits 0" \
    || { bad "axl update exited $u8h"; tail -8 /tmp/8h.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK really did move to '"$V3"'" \
    || bad "--print-prefix = $(axl --print-prefix)"
if grep -qE "cross toolchain|\[install-toolchain\]" /tmp/8h.log; then
    bad "the update fetched a toolchain for an arch that had none"
    grep -nE "cross toolchain|\[install-toolchain\]" /tmp/8h.log | head -4
else
    ok "and nothing toolchain-shaped ran -- the host-gcc consumer is left alone"
fi
[ -e "/opt/axl-fixture-x64-'"$V3"'" ] \
    && bad "a toolchain root appeared at /opt/axl-fixture-x64-'"$V3"'" \
    || ok "and no toolchain root was created"

echo "--- 8i. axl update DOES bring along a toolchain the user already has"
# THE OTHER HALF, and only the pair proves the rule. The SDK pins the
# toolchain -- axl-toolchains.conf ships inside it -- so before this, `axl
# update` moved the SDK and left the compiler behind. Under AXL_TOOLCHAIN=auto
# that is SILENT: axl-cc just falls back to the host gcc for C.
#
# BOTH ROOTS ARE ~30-BYTE STUBS. The real x64 toolchain is ~239 MB, and what is
# under test is which arch the update ASKS for. Pre-creating the '"$V3"' root
# keeps install-toolchain.sh on its already-installed path instead of
# downloading -- and the RECEIPT it writes there is the evidence, because
# nothing else in this container can produce one.
for tv in '"$V1"' '"$V3"'; do
    mkdir -p "/opt/axl-fixture-x64-$tv/bin"
    for t in gcc g++; do
        printf "#!/bin/sh\necho x86_64-elf-$t 14.3.0-fixture-$tv\n" \
            > "/opt/axl-fixture-x64-$tv/bin/x86_64-elf-$t"
        chmod +x "/opt/axl-fixture-x64-$tv/bin/x86_64-elf-$t"
    done
done
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8i-pre.log 2>&1
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "fixture: back on '"$V1"' for the positive case" \
    || bad "fixture: --print-prefix = $(axl --print-prefix)"
axl toolchain list --porcelain > /tmp/8i-tc.txt 2>&1
x1=$(awk -F"$TAB" "\$1 == \"x64\"  { print \$5 }" /tmp/8i-tc.txt)
a1=$(awk -F"$TAB" "\$1 == \"aa64\" { print \$5 }" /tmp/8i-tc.txt)
if [ "$x1" = "installed" ] && [ "$a1" = "MISSING" ]; then
    ok "control: x64 installed and aa64 not -- the per-arch case"
else
    bad "control: x64=$x1 aa64=$a1"; cat /tmp/8i-tc.txt
fi
# PRE-SATISFACTION CHECK. The receipt assertion below is the whole case; if one
# were already sitting there it would pass with the feature deleted.
[ -e "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && bad "fixture: a receipt already exists -- the assertion below is pre-satisfied" \
    || ok "fixture: no receipt at the '"$V3"' root before the update"
axl update --base-url "file:///rel/'"$V3"'" > /tmp/8i.log 2>&1; u8i=$?
[ "$u8i" = 0 ] && ok "the update exits 0" \
    || { bad "axl update exited $u8i"; tail -10 /tmp/8i.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK moved to '"$V3"'" \
    || bad "--print-prefix = $(axl --print-prefix)"
# THE ANNOUNCEMENT, exact and x64-ONLY. A silent multi-megabyte download is its
# own defect; and "x64" rather than "all" IS the per-arch rule holding.
#
# IT COMES FROM install.sh, not from `axl update`: main() returns at
# already_installed() before maybe_toolchain(), so a line printed by the caller
# promised a download on an already-current update and then did nothing. Step
# 8j is that case.
if grep -q "^  installing the x64 cross toolchain (uses sudo for /opt;$" /tmp/8i.log; then
    ok "it names the arch it is installing, and names only x64"
else
    bad "no exact announce line"; grep -n "cross toolchain" /tmp/8i.log | head -4
fi
grep -q "pins which toolchain" /tmp/8i.log \
    && ok "and says why, so it does not read as the update overreaching" \
    || { bad "the announce carries no reason"; grep -n "cross toolchain" /tmp/8i.log | head -4; }
# THE OTHER HALF OF THE 8h COUPLING CHECK: the manifest now in play must be
# '"$V3"'-s own, restored by extract_tree. Everything below reads roots out of it.
mfdir=$(sed -n "s/^AXL_X64_TOOLCHAIN_DIR=//p" \
        "$P/axl-sdk/share/axl/axl-toolchains.conf" 2>/dev/null)
[ "$mfdir" = "/opt/axl-fixture-x64-'"$V3"'" ] \
    && ok "and the new SDK brought its OWN manifest, naming a different root" \
    || bad "post-update manifest names $mfdir"
# THE RECEIPT, at the root the NEW SDK pins. Only install-toolchain.sh writes
# one, so this cannot pass unless the update actually invoked it -- and it
# cannot pass off the OLD pin, which is a different directory entirely.
if [ -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ]; then
    ok "the toolchain the NEW SDK pins was installed"
else
    bad "no receipt at /opt/axl-fixture-x64-'"$V3"'"; tail -10 /tmp/8i.log
fi
rv=$(sed -n "s/^AXL_RECEIPT_VERSION=//p" \
     "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" 2>/dev/null)
[ "$rv" = "fixture-'"$V3"'" ] \
    && ok "and the receipt names the newly pinned toolchain version" \
    || bad "receipt version is [$rv], expected fixture-'"$V3"'"
# PER-ARCH, the second half of the rule: aa64 was never installed here, so the
# update must not have gone looking for it.
[ -e "/opt/axl-fixture-aa64-'"$V3"'" ] \
    && bad "aa64 was fetched onto a machine that never had one" \
    || ok "and aa64 -- never installed -- was left alone"

echo "--- 8ib. detection accepts EITHER locator: gcc alone counts"
# A consumer who located their toolchain with AXL_X64_GCC alone -- or whose tree
# is missing only the C++ driver -- used to read MISSING and be declined
# SILENTLY, which is the same class of silence 8h/8i exist to remove, one layer
# up. Either locator resolving to an executable now counts.
rm -f "/opt/axl-fixture-x64-'"$V1"'/bin/x86_64-elf-g++"
rm -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt"
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8ib-pre.log 2>&1
# CONTROL, and it is the whole point of the case: `axl toolchain list` probes
# the g++ locator ONLY -- deliberately, it pairs with a "builds with:" line that
# asks axl-cc -- so it reports MISSING here. `axl update` must NOT agree with
# it. If list said installed, this step would prove nothing.
axl toolchain list --porcelain > /tmp/8ib-tc.txt 2>&1
xb=$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/8ib-tc.txt)
[ "$xb" = "MISSING" ] \
    && ok "control: with g++ gone, toolchain list reports g++ MISSING" \
    || { bad "control: list says x64 is $xb -- 8ib cannot discriminate"; cat /tmp/8ib-tc.txt; }
# AND IT NO LONGER CONTRADICTS ITSELF. list used to report the g++ half only,
# so on this exact tree it said MISSING about a toolchain the update was about
# to refresh -- one command contradicting another about one machine. Reporting
# both locators in their own fields is what closes that.
cb=$(awk -F"$TAB" "\$1 == \"x64\" { print \$4 }" /tmp/8ib-tc.txt)
[ "$cb" = "installed" ] \
    && ok "and reports gcc installed alongside, so list agrees with update" \
    || { bad "porcelain gcc field is [$cb], expected installed"; cat /tmp/8ib-tc.txt; }
# THE ROOT IS STILL WHERE THE CONTRACT SAYS: case 17 of the toolchain-verb test
# reads the toolchain ROOT out of porcelain field 3 to decide where a command
# ending in `rm -rf` points, so a column moved before it would silently hand
# that assertion a compiler state where it expects a path.
rb=$(awk -F"$TAB" "\$1 == \"x64\" { print \$3 }" /tmp/8ib-tc.txt)
[ "$rb" = "/opt/axl-fixture-x64-'"$V1"'" ] \
    && ok "and porcelain field 3 is still the root path" \
    || { bad "field 3 is [$rb] -- the columns shifted"; cat /tmp/8ib-tc.txt; }
axl update --base-url "file:///rel/'"$V3"'" > /tmp/8ib.log 2>&1; u8ib=$?
[ "$u8ib" = 0 ] && ok "the update exits 0" \
    || { bad "axl update exited $u8ib"; tail -10 /tmp/8ib.log; }
[ -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && ok "and the gcc-only toolchain WAS carried anyway" \
    || { bad "a gcc-only toolchain was silently declined"; tail -10 /tmp/8ib.log; }
# Put g++ back: later steps assert against the both-locators state.
cp "/opt/axl-fixture-x64-'"$V1"'/bin/x86_64-elf-gcc" \
   "/opt/axl-fixture-x64-'"$V1"'/bin/x86_64-elf-g++"

echo "--- 8ic. AXL_TOOLCHAIN=host declines the toolchain, and SAYS so"
# The hard constraint arriving through a different door. Someone who has set
# AXL_TOOLCHAIN=host has SAID they build with the host compiler; a leftover AXL
# root under /opt is not a request to refresh it, and tens of MB during their
# `axl update` is exactly the foisting this feature forbids.
rm -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt"
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8ic-pre.log 2>&1
# CONTROL: x64 IS installed, so WITHOUT the variable this same update carries
# it -- 8i proved exactly that on this state. Without this line the absence
# below is satisfied by there being nothing to carry.
axl toolchain list --porcelain > /tmp/8ic-tc.txt 2>&1
xc=$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/8ic-tc.txt)
[ "$xc" = "installed" ] \
    && ok "control: x64 is installed, so the default WOULD carry it" \
    || { bad "control: x64=$xc -- 8ic cannot prove the variable did anything"; cat /tmp/8ic-tc.txt; }
AXL_TOOLCHAIN=host axl update --base-url "file:///rel/'"$V3"'" > /tmp/8ic.log 2>&1; u8ic=$?
# FOUR THINGS. A decline is equally true of an update that crashed, and a
# SILENT decline is the defect -- so the printed reason is asserted too.
[ "$u8ic" = 0 ] && ok "the update exits 0" \
    || { bad "AXL_TOOLCHAIN=host axl update exited $u8ic"; tail -10 /tmp/8ic.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK still moved to '"$V3"'" \
    || bad "--print-prefix = $(axl --print-prefix)"
grep -q "^axl: AXL_TOOLCHAIN=host -- NOT updating the x64 cross$" /tmp/8ic.log \
    && ok "and it NAMES the variable and the arch it declined" \
    || { bad "the decline was silent"; head -6 /tmp/8ic.log; }
[ -e "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && bad "it carried the toolchain despite AXL_TOOLCHAIN=host" \
    || ok "and carried nothing"

echo "--- 8id. an OVERRIDE-located toolchain is declined, not installed into /opt"
# THE FOURTH DOOR TO THE SAME FOISTING. Detection honours AXL_X64_GXX, which is
# how a consumer who installed with --prefix $HOME/.local/opt is seen at all.
# But install-toolchain.sh resolves the manifest *_DEFAULT paths under
# INSTALL_ROOT (default /opt) and reads NO override, and install.sh passes no
# --prefix -- so carrying such an arch would sudo-install a SECOND toolchain
# into a root the consumer never chose, leaving theirs untouched beside it.
# Detected, declined, and told which command actually moves theirs.
OWN="$HOME/myopt/x86_64-elf-gcc-mine"
mkdir -p "$OWN/bin"
for t in gcc g++; do
    printf "#!/bin/sh\necho x86_64-elf-$t 14.3.0-mine\n" > "$OWN/bin/x86_64-elf-$t"
    chmod +x "$OWN/bin/x86_64-elf-$t"
done
rm -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt"
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8id-pre.log 2>&1
# CONTROL: the override really is what resolves -- list reports the overridden
# root, not the manifest one. Without this the decline below could be firing
# for some entirely different reason.
AXL_X64_GXX="$OWN/bin/x86_64-elf-g++" axl toolchain list --porcelain > /tmp/8id-tc.txt 2>&1
rd=$(awk -F"$TAB" "\$1 == \"x64\" { print \$3 }" /tmp/8id-tc.txt)
[ "$rd" = "$OWN" ] \
    && ok "control: the override moves the root list reports" \
    || { bad "control: list reports root [$rd], expected $OWN"; cat /tmp/8id-tc.txt; }
AXL_X64_GXX="$OWN/bin/x86_64-elf-g++" \
    axl update --base-url "file:///rel/'"$V3"'" > /tmp/8id.log 2>&1; u8id=$?
[ "$u8id" = 0 ] && ok "the update exits 0" \
    || { bad "axl update exited $u8id"; tail -10 /tmp/8id.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK still moved to '"$V3"'" \
    || bad "--print-prefix = $(axl --print-prefix)"
grep -q "^axl: x64 is located by AXL_X64_GXX, outside the root the SDK pins:$" /tmp/8id.log \
    && ok "and it NAMES the override rather than declining silently" \
    || { bad "the decline was silent"; head -8 /tmp/8id.log; }
# THE LABEL ON THE NEXT LINE. It carries the LOCATOR, not a root -- under a
# line ending "the root the SDK pins:" an unlabelled path read as one. Without
# an assertion that correction can regress and nothing notices.
grep -qF "       compiler: $OWN/bin/x86_64-elf-g++" /tmp/8id.log \
    && ok "and labels the next line as the compiler, not as a root" \
    || { bad "the locator line is unlabelled or wrong"; head -8 /tmp/8id.log; }
# THE REMEDY MUST BE THE ONE THAT WORKS: install-toolchain.sh puts
# <prefix>/<pinned-name>, so the prefix is the PARENT of their root.
# ALWAYS QUOTED now, not only when it contains a space: quoting the space case
# alone fixed the instance and left the class (a quote, a $ or a backtick in the
# path still pastes broken, the last as a command).
grep -qF "axl toolchain install x64 --prefix $SQ$HOME/myopt$SQ" /tmp/8id.log \
    && ok "and prints the --prefix that would actually install alongside, quoted" \
    || { bad "no usable remedy"; head -12 /tmp/8id.log; }
# AND DOES NOT OVERPROMISE: that command writes a NEW root and leaves both the
# old tree and the variable pointing at it alone.
grep -qF "     That writes a NEW root and does not modify yours -- AXL_X64_GXX still" /tmp/8id.log \
    && ok "and says the variable still points at the old root until re-exported" \
    || { bad "the remedy overpromises"; head -12 /tmp/8id.log; }
# THE POINT OF THE WHOLE CASE: the toolchain step never ran at all.
if grep -qE "cross toolchain|\[install-toolchain\]" /tmp/8id.log; then
    bad "the toolchain step ran for an override-located arch"
    grep -nE "cross toolchain|\[install-toolchain\]" /tmp/8id.log | head -4
else
    ok "and no toolchain step ran"
fi
# The receipt is the narrower, sharper witness: carrying x64 would have taken
# install-toolchain-s already-installed path on the '"$V3"' root and written one.
[ -e "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && bad "it marked a root under /opt -- a SECOND toolchain beside the users own" \
    || ok "and wrote no receipt under /opt"
# THEIR OWN TREE IS UNTOUCHED, and RUN rather than stat-ed: an -x test passes
# on a file that has been truncated to nothing.
if "$OWN/bin/x86_64-elf-g++" --version >/dev/null 2>&1; then
    ok "and the users own compiler still runs"
else
    bad "the users own compiler no longer runs"
fi

echo "--- 8ie. a locator we cannot derive a prefix from gets NO fabricated prefix"
# tc_have_compiler accepts a BARE NAME on PATH -- deliberately, that was the
# point of accepting either locator. But the remedy prefix is
# dirname(dirname(locator)), so a bare name yields "." and an ordinary distro
# cross-gcc at /usr/bin/... yields "/". Printing either would tell someone to
# install a toolchain into their working directory or into the root of the
# filesystem, which is worse than printing no command at all.
mkdir -p "$HOME/barebin"
for t in gcc g++; do
    printf "#!/bin/sh\necho x86_64-elf-bare-$t 1.0\n" > "$HOME/barebin/x86_64-elf-bare-$t"
    chmod +x "$HOME/barebin/x86_64-elf-bare-$t"
    printf "#!/bin/sh\necho x86_64-elf-usr-$t 1.0\n" > "/usr/bin/x86_64-elf-usr-$t"
    chmod +x "/usr/bin/x86_64-elf-usr-$t"
done
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8ie-pre.log 2>&1
AXL_X64_GXX=x86_64-elf-bare-g++ PATH="$HOME/barebin:$PATH" \
    axl update --base-url "file:///rel/'"$V3"'" > /tmp/8ie1.log 2>&1; u8ie1=$?
[ "$u8ie1" = 0 ] && ok "a bare-name locator: the update still exits 0" \
    || { bad "bare-name update exited $u8ie1"; tail -10 /tmp/8ie1.log; }
# CONTROL that the case was reached at all -- otherwise the two absences below
# are satisfied by the decline never having been printed.
grep -q "^axl: x64 is located by AXL_X64_GXX, outside the root the SDK pins:$" /tmp/8ie1.log \
    && ok "and the decline fired for it" \
    || { bad "the bare-name locator did not reach the decline"; head -8 /tmp/8ie1.log; }
if grep -qE -- "--prefix \.$" /tmp/8ie1.log; then
    bad "it told the user to install a toolchain into the current directory"
    grep -n -- "--prefix" /tmp/8ie1.log | head -3
else
    ok "and never printed --prefix ."
fi
grep -qF -- "axl toolchain install x64 --prefix <dir>" /tmp/8ie1.log \
    && ok "and handed back the generic form instead" \
    || { bad "no generic remedy"; head -10 /tmp/8ie1.log; }
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8ie-pre2.log 2>&1
AXL_X64_GXX=/usr/bin/x86_64-elf-usr-g++ \
    axl update --base-url "file:///rel/'"$V3"'" > /tmp/8ie2.log 2>&1; u8ie2=$?
[ "$u8ie2" = 0 ] && ok "a /usr/bin locator: the update still exits 0" \
    || { bad "/usr/bin update exited $u8ie2"; tail -10 /tmp/8ie2.log; }
if grep -qE -- "--prefix /$" /tmp/8ie2.log; then
    bad "it told the user to install a toolchain into /"
    grep -n -- "--prefix" /tmp/8ie2.log | head -3
else
    ok "and never printed --prefix /"
fi
grep -qF -- "axl toolchain install x64 --prefix <dir>" /tmp/8ie2.log \
    && ok "and handed back the generic form for that one too" \
    || { bad "no generic remedy"; head -10 /tmp/8ie2.log; }
# A THIRD SHAPE: A PATH WITH A SPACE. The classification line is
# tab-separated, but a whitespace-split read turns
# "$HOME/my dir/tc/bin/g++" into six fields, leaving the root as "$HOME/my" --
# which is absolute and is not "/", so it PASSES the guard above and prints
# "--prefix $HOME". A wrong path that looks plausible is worse than an
# obviously wrong one, which is why this is its own case and not a footnote.
SPACED="$HOME/my dir/x86_64-elf-gcc-spaced"
mkdir -p "$SPACED/bin"
for t in gcc g++; do
    printf "#!/bin/sh\necho x86_64-elf-spaced-$t 1.0\n" > "$SPACED/bin/x86_64-elf-$t"
    chmod +x "$SPACED/bin/x86_64-elf-$t"
done
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8ie-pre3.log 2>&1
AXL_X64_GXX="$SPACED/bin/x86_64-elf-g++" \
    axl update --base-url "file:///rel/'"$V3"'" > /tmp/8ie3.log 2>&1; u8ie3=$?
[ "$u8ie3" = 0 ] && ok "a spaced-path locator: the update still exits 0" \
    || { bad "spaced-path update exited $u8ie3"; tail -10 /tmp/8ie3.log; }
# THE BUG, NAMED: the truncated prefix is exactly $HOME at end of line.
if grep -qE -- "--prefix $HOME$" /tmp/8ie3.log; then
    bad "the path was split on spaces -- it printed the truncated --prefix $HOME"
    grep -n -- "--prefix" /tmp/8ie3.log | head -3
else
    ok "and did not print a truncated prefix"
fi
# AND THE WHOLE DIRECTORY SURVIVED, single-quoted so the command can be pasted.
grep -qF -- "--prefix $SQ$HOME/my dir$SQ" /tmp/8ie3.log \
    && ok "and printed the full directory, quoted so it pastes" \
    || { bad "the spaced prefix did not survive"; grep -n -- "--prefix" /tmp/8ie3.log | head -3; }

echo "--- 8if. AXL_TOOLCHAIN=host AND an override: the two declines compose"
# THE BRANCH NOTHING COVERED. The override decline moved out of the read loop
# into the else branch so it prints BELOW the variant check, matching the
# precedence table -- but in-loop and else-branch differ ONLY when the
# host/cross branch is taken AND an override exists. 8ic sets the variable with
# a manifest-default toolchain; 8id/8ie set an override with the variable
# unset. Neither can see the difference, so the sabotage row claiming 8ic
# covered it was false.
#
# It hid a real defect: with both, the user was told "Carry it anyway with: axl
# update --toolchain x64" -- which for an override-located arch installs a
# SECOND toolchain under /opt, the exact overpromise fixed on the other branch.
rm -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt"
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8if-pre.log 2>&1
# CONTROL: the override really is in play for this run.
AXL_X64_GXX="$OWN/bin/x86_64-elf-g++" axl toolchain list --porcelain > /tmp/8if-tc.txt 2>&1
rf=$(awk -F"$TAB" "\$1 == \"x64\" { print \$3 }" /tmp/8if-tc.txt)
[ "$rf" = "$OWN" ] \
    && ok "control: the override is what resolves for this run" \
    || { bad "control: list reports root [$rf], expected $OWN"; cat /tmp/8if-tc.txt; }
AXL_TOOLCHAIN=host AXL_X64_GXX="$OWN/bin/x86_64-elf-g++" \
    axl update --base-url "file:///rel/'"$V3"'" > /tmp/8if.log 2>&1; u8if=$?
[ "$u8if" = 0 ] && ok "the update exits 0" \
    || { bad "axl update exited $u8if"; tail -12 /tmp/8if.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK still moved to '"$V3"'" \
    || bad "--print-prefix = $(axl --print-prefix)"
# $_seen, NOT $_have: x64 is installed but override-located, so it is not in
# the carryable list. Naming $_have would print an empty arch list here.
grep -q "^axl: AXL_TOOLCHAIN=host -- NOT updating the x64 cross$" /tmp/8if.log \
    && ok "the variant decline still NAMES x64, though it is not carryable" \
    || { bad "the variant decline named the wrong set"; head -8 /tmp/8if.log; }
# THE ORDERING: the override block must NOT have printed, because the variant
# check outranks it. In-loop, it did.
if grep -q "^axl: x64 is located by AXL_X64_GXX, outside the root the SDK pins:$" /tmp/8if.log; then
    bad "the override decline printed above the variant check, inverting the table"
    head -12 /tmp/8if.log
else
    ok "and the override block did not print, because the variant outranks it"
fi
# THE OVERPROMISE: --toolchain is not offered, because for this arch it would
# install a SECOND toolchain under /opt.
if grep -qF -- "       axl update --toolchain <aa64|x64|all>" /tmp/8if.log; then
    bad "it offered --toolchain for an override-located arch -- a second /opt install"
    head -12 /tmp/8if.log
else
    ok "and did NOT offer --toolchain, which would install a second toolchain"
fi
grep -q "^     NOTE x64 is located by an AXL_<ARCH>_GCC/_GXX override,$" /tmp/8if.log \
    && ok "but says why, and points at the --prefix form instead" \
    || { bad "no override caveat on the variant decline"; head -12 /tmp/8if.log; }
[ -e "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && bad "it carried the toolchain anyway" \
    || ok "and carried nothing"

echo "--- 8ig. PER ARCH: an x64 override does not stop aa64 being carried"
# THE OTHER INTERACTION THE TABLE CLAIMS, and it was pinned by nothing either.
# §21a.4a says the override decline is per arch. Every step so far has only one
# arch installed, so "declined" and "declined everything" are indistinguishable
# -- and if they were the same, an override on one arch would silently suppress
# the other, which is the exact silent-decline defect this whole section exists
# to remove.
#
# aa64 stubs at BOTH pins: detection reads '"$V1"'-s manifest, install-toolchain
# reads '"$V3"'-s. install_aa64 also version-matches the compiler output against
# the pinned version, so the stub prints it.
for tv in '"$V1"' '"$V3"'; do
    mkdir -p "/opt/axl-fixture-aa64-$tv/bin"
    for t in gcc g++; do
        # NO PARENTHESES in the stub body: they are shell metacharacters
        # unquoted, so the generated script is a syntax error, --version
        # prints nothing, install_aa64-s version match fails and it falls
        # through to a 96 MB download that cannot succeed here.
        printf "#!/bin/sh\necho aarch64-none-elf-$t fixture-$tv\n" \
            > "/opt/axl-fixture-aa64-$tv/bin/aarch64-none-elf-$t"
        chmod +x "/opt/axl-fixture-aa64-$tv/bin/aarch64-none-elf-$t"
    done
done
rm -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" "/opt/axl-fixture-aa64-'"$V3"'/.axl-receipt"
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8ig-pre.log 2>&1
# CONTROL, both halves: aa64 installed at the pinned root, x64 override-located.
AXL_X64_GXX="$OWN/bin/x86_64-elf-g++" axl toolchain list --porcelain > /tmp/8ig-tc.txt 2>&1
ag=$(awk -F"$TAB" "\$1 == \"aa64\" { print \$5 }" /tmp/8ig-tc.txt)
xg=$(awk -F"$TAB" "\$1 == \"x64\"  { print \$3 }" /tmp/8ig-tc.txt)
if [ "$ag" = "installed" ] && [ "$xg" = "$OWN" ]; then
    ok "control: aa64 at the pinned root, x64 override-located"
else
    bad "control: aa64=$ag x64root=$xg -- 8ig cannot show per-arch"
    cat /tmp/8ig-tc.txt
fi
AXL_X64_GXX="$OWN/bin/x86_64-elf-g++" \
    axl update --base-url "file:///rel/'"$V3"'" > /tmp/8ig.log 2>&1; u8ig=$?
[ "$u8ig" = 0 ] && ok "the update exits 0" \
    || { bad "axl update exited $u8ig"; tail -12 /tmp/8ig.log; }
grep -q "^axl: x64 is located by AXL_X64_GXX, outside the root the SDK pins:$" /tmp/8ig.log \
    && ok "x64 is declined, as it was on its own" \
    || { bad "x64 was not declined"; head -12 /tmp/8ig.log; }
# THE CLAIM ITSELF: aa64 was carried in the SAME run.
[ -f "/opt/axl-fixture-aa64-'"$V3"'/.axl-receipt" ] \
    && ok "and aa64 was carried anyway, in the same run" \
    || { bad "the x64 override suppressed aa64 -- the decline is not per arch"
         tail -12 /tmp/8ig.log; }
[ -e "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && bad "x64 was carried despite the override" \
    || ok "while x64 itself was not"
# CLEAN UP AFTER THIS STEP. It is the only one with aa64 installed, and every
# later step assumes x64-only: left in place, 8L-s single-arch failure becomes
# a `--toolchain all` failure and its assertions -- which name x64 -- fail
# against a banner saying "all". Cross-step state is what 8h-s manifest
# assertion exists to catch; this is the same hazard, so this step undoes
# itself rather than leaving it to be discovered.
rm -rf "/opt/axl-fixture-aa64-'"$V1"'" "/opt/axl-fixture-aa64-'"$V3"'"

echo "--- 8j. an ALREADY-CURRENT update announces no download, because it does none"
# THE MOST COMMON INVOCATION OF THE VERB, and the one the first version of this
# feature got wrong. install.sh main() returns at already_installed() BEFORE it
# reaches maybe_toolchain(), so an announce printed by `axl update` just before
# exec-ing the installer promised "may download ~55-96 MB" and then did
# nothing -- 21a.6-s own guarantee inverted. Neither 8h nor 8i can see it: both
# perform a real install.
#
# The state here is 8ic-s: SDK current at '"$V3"', x64 toolchain installed, and
# AXL_TOOLCHAIN unset -- so the ONLY reason nothing is announced is that
# already_installed() returned before the toolchain step.
# CONTROL, and 8j was the one step without it: the negative below is equally
# satisfied by there having been nothing to carry. Assert x64 IS detected
# installed under the CURRENT manifest, so the silence means what it claims.
axl toolchain list --porcelain > /tmp/8j-tc.txt 2>&1
xj=$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/8j-tc.txt)
[ "$xj" = "installed" ] \
    && ok "control: x64 is installed, so a non-short-circuiting update WOULD carry it" \
    || { bad "control: x64=$xj -- 8j cannot prove the short-circuit did it"; cat /tmp/8j-tc.txt; }
axl update --base-url "file:///rel/'"$V3"'" > /tmp/8j.log 2>&1; u8j=$?
[ "$u8j" = 0 ] && ok "a repeat update exits 0" \
    || { bad "repeat axl update exited $u8j"; tail -8 /tmp/8j.log; }
# CONTROL: it must really have short-circuited, or this proves nothing about
# the already-current path.
grep -q "nothing to do" /tmp/8j.log \
    && ok "control: it really did short-circuit as already-current" \
    || { bad "control: the repeat update was not a no-op"; tail -8 /tmp/8j.log; }
if grep -qE "cross toolchain|\[install-toolchain\]" /tmp/8j.log; then
    bad "it announced a toolchain step it was never going to run"
    grep -nE "cross toolchain|\[install-toolchain\]" /tmp/8j.log | head -4
else
    ok "and says nothing about the toolchain, because it installs none"
fi

echo "--- 8k. axl update --no-toolchain, for an offline or sudo-less box"
# Carrying the toolchain needs the network and, for /opt, sudo -- so making it
# the default created a path that FAILS where it used to succeed. This is the
# escape hatch. `--toolchain=` was not one: undocumented, and the two-word
# `--toolchain ""` dies on install.sh-s ${2:?} before it means anything.
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8k-pre.log 2>&1
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V1"'" ] \
    && ok "fixture: back on '"$V1"' with x64 still on disk" \
    || bad "fixture: --print-prefix = $(axl --print-prefix)"
# CONTROL: x64 IS installed, so WITHOUT the flag this exact update carries it
# -- which is what 8i just proved on this exact state. Without this line the
# absence below is satisfied by there being nothing to carry.
axl toolchain list --porcelain > /tmp/8k-tc.txt 2>&1
xk=$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/8k-tc.txt)
[ "$xk" = "installed" ] \
    && ok "control: x64 is installed, so the default WOULD carry it" \
    || { bad "control: x64=$xk -- 8k cannot prove the flag did anything"; cat /tmp/8k-tc.txt; }
# 8i left a receipt at the '"$V3"' root; clear it so the absence below is the
# flag-s doing and not a leftover.
rm -f "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt"
axl update --no-toolchain --base-url "file:///rel/'"$V3"'" > /tmp/8k.log 2>&1; u8k=$?
[ "$u8k" = 0 ] && ok "--no-toolchain update exits 0" \
    || { bad "axl update --no-toolchain exited $u8k"; tail -10 /tmp/8k.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK still moved to '"$V3"'" \
    || bad "--print-prefix = $(axl --print-prefix)"
if grep -qE "cross toolchain|\[install-toolchain\]" /tmp/8k.log; then
    bad "--no-toolchain still ran the toolchain step"
    grep -nE "cross toolchain|\[install-toolchain\]" /tmp/8k.log | head -4
else
    ok "and no toolchain step ran"
fi
[ -e "/opt/axl-fixture-x64-'"$V3"'/.axl-receipt" ] \
    && bad "a receipt was written despite --no-toolchain" \
    || ok "and no receipt was written"
# THE CONTRADICTION, refused by name rather than silently resolved -- and
# refused BEFORE the manager phase, so a validation error costs no work.
# --base-url IS LOAD-BEARING here: with it, a manager phase that DID run would
# print its own "nothing to do", which is what the third assertion looks for.
# Without it the manager phase would instead fail at the network and leave the
# same silence a correct refusal produces, so the assertion would prove nothing.
axl update --no-toolchain --toolchain x64 --base-url "file:///rel/'"$V3"'" \
    > /tmp/8k2.log 2>&1; u8k2=$?
if [ "$u8k2" = 2 ] && grep -q "contradict" /tmp/8k2.log; then
    ok "--no-toolchain with --toolchain is refused, exit 2"
else
    bad "contradiction: rc=$u8k2"; cat /tmp/8k2.log
fi
if grep -q "nothing to do" /tmp/8k2.log; then
    bad "the refusal came AFTER the manager phase -- a bad arg cost real work"
    cat /tmp/8k2.log
else
    ok "and refused before the manager phase ran at all"
fi
# --no-toolchain TAKES NO VALUE. Unhandled it fell through into the forwarded
# args and install.sh died with "unknown option", naming a flag the user had
# every reason to think existed.
axl update --no-toolchain=1 --base-url "file:///rel/'"$V3"'" > /tmp/8k3.log 2>&1; u8k3=$?
if [ "$u8k3" = 2 ] && grep -q "takes no value" /tmp/8k3.log; then
    ok "--no-toolchain=1 is refused by name, not by install.sh"
else
    bad "--no-toolchain=1: rc=$u8k3"; cat /tmp/8k3.log
fi
# AN INVALID VARIANT IS AN ERROR, matching axl-cc rather than silently
# carrying the toolchain the typo was trying to opt out of.
AXL_TOOLCHAIN=hsot axl update --base-url "file:///rel/'"$V3"'" > /tmp/8k4.log 2>&1; u8k4=$?
if [ "$u8k4" = 2 ] && grep -q "not a toolchain variant" /tmp/8k4.log; then
    ok "a misspelt AXL_TOOLCHAIN is refused, not ignored"
else
    bad "AXL_TOOLCHAIN=hsot: rc=$u8k4"; cat /tmp/8k4.log
fi
# AND ABOVE THE MANAGER PHASE, same as the contradiction. rc=2 plus the message
# is equally true of a check that runs after a full manager self-update, so the
# ordering needs its own assertion -- and the same --base-url, so a manager
# phase that DID run leaves its "nothing to do" behind.
if grep -q "nothing to do" /tmp/8k4.log; then
    bad "the variant check ran AFTER the manager phase -- a typo cost real work"
    cat /tmp/8k4.log
else
    ok "and refused before the manager phase ran at all"
fi
# THE INTERACTION THE TABLE CLAIMS, pinned. §21a.4a says the variant refusal is
# NOT pre-empted by the flags -- an unconditional check, so a typo in the
# variable is a typo whatever else is on the command line. Every run above
# passes the typo ALONE, so the sentence describing the interaction was true and
# tested by nothing. When a doc states an interaction, the interaction is what
# needs the assertion.
AXL_TOOLCHAIN=hsot axl update --toolchain x64 \
    --base-url "file:///rel/'"$V3"'" > /tmp/8k5.log 2>&1; u8k5=$?
if [ "$u8k5" = 2 ] && grep -q "not a toolchain variant" /tmp/8k5.log; then
    ok "and --toolchain does not pre-empt it: still refused"
else
    bad "AXL_TOOLCHAIN=hsot with --toolchain: rc=$u8k5"; cat /tmp/8k5.log
fi
AXL_TOOLCHAIN=hsot axl update --no-toolchain \
    --base-url "file:///rel/'"$V3"'" > /tmp/8k6.log 2>&1; u8k6=$?
if [ "$u8k6" = 2 ] && grep -q "not a toolchain variant" /tmp/8k6.log; then
    ok "nor does --no-toolchain: still refused"
else
    bad "AXL_TOOLCHAIN=hsot with --no-toolchain: rc=$u8k6"; cat /tmp/8k6.log
fi

echo "--- 8L. a FAILED toolchain step reports BOTH halves and still exits non-zero"
# maybe_toolchain runs AFTER link_tree, so by the time it can fail the SDK is
# already installed and linked. It used to run under `ensure`, whose die then
# skipped maybe_prune, check_path AND the closing banner -- so a network hiccup
# or a missing sudo left a user whose SDK DID update with nothing but a
# toolchain error, reading as a failed install. Keeping it fatal-and-silent
# would re-create this feature-s own defect in a quieter form.
axl use '"$V1"' --base-url file:///rel/empty > /tmp/8L-pre.log 2>&1
# Make the '"$V3"'-pinned root unreachable. The fixture manifest carries no
# AXL_X64_TOOLCHAIN_URL and a staged prefix has no source builder, so
# install-toolchain.sh returns 1 -- deterministically, with no network.
rm -rf "/opt/axl-fixture-x64-'"$V3"'"
# CONTROL: x64 must still read installed at '"$V1"'-s pin, or the update carries
# nothing and there is no failure to report.
axl toolchain list --porcelain > /tmp/8L-tc.txt 2>&1
xl=$(awk -F"$TAB" "\$1 == \"x64\" { print \$5 }" /tmp/8L-tc.txt)
[ "$xl" = "installed" ] \
    && ok "control: x64 is installed, so this update does try to carry it" \
    || { bad "control: x64=$xl -- nothing would be attempted"; cat /tmp/8L-tc.txt; }
axl update --base-url "file:///rel/'"$V3"'" > /tmp/8L.log 2>&1; u8L=$?
[ "$u8L" != 0 ] \
    && ok "a failed toolchain step exits non-zero, so CI still sees it" \
    || { bad "the failure was swallowed: rc=$u8L"; tail -10 /tmp/8L.log; }
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V3"'" ] \
    && ok "and the SDK moved anyway, because it had already been linked" \
    || bad "--print-prefix = $(axl --print-prefix)"
grep -q "AXL SDK '"$V3"' installed" /tmp/8L.log \
    && ok "the banner still reports the half that DID work" \
    || { bad "no install banner -- the die skipped it"; tail -10 /tmp/8L.log; }
grep -q "BUT the x64 cross toolchain did NOT update" /tmp/8L.log \
    && ok "and a marked line names the half that did not" \
    || { bad "the failed half was not reported"; tail -10 /tmp/8L.log; }
grep -q "axl toolchain install x64" /tmp/8L.log \
    && ok "with the command that finishes it" \
    || { bad "no remedy printed"; tail -10 /tmp/8L.log; }

# RESTORE '"$V2"' as current: step 9 uninstalls whatever is current and asserts
# that '"$V2"' is the root that went away.
axl use '"$V2"' --base-url file:///rel/empty > /tmp/8k-post.log 2>&1
[ "$(axl --print-prefix)" = "$P/axl-sdk-'"$V2"'" ] \
    && ok "fixture: restored to '"$V2"' for the uninstall step" \
    || bad "fixture: --print-prefix = $(axl --print-prefix)"

echo "--- 9. uninstall removes the VERSION, and deliberately not the manager"
axl uninstall --yes > /tmp/un.log 2>&1 \
    && ok "uninstall exits 0" || bad "uninstall exited $?"
[ -d "$P/axl-sdk-'"$V2"'" ] && bad "uninstall left the version root" \
    || ok "the version root is gone"
[ -e "$B/axl-cc" ] && bad "uninstall left $B/axl-cc" \
    || ok "the axl-cc link is gone"
# §20, and the reason this step is NOT called "leaves nothing behind": the
# manager root, its marker and the axl link all survive by design. That is
# what lets a user reinstall without re-fetching an installer from a web page.
[ -L "$B/axl" ] && ok "the manager link survives uninstall" \
    || bad "uninstall removed $B/axl -- there is now no way back"
[ -L "$P/axl-sdk-host-tools" ] && ok "the manager marker survives" \
    || bad "the manager marker is gone"
exit $rc
'

# THE REAL TOOLCHAIN, mounted READ-ONLY at its own absolute path -- the
# idiom test-consumer-install.sh uses, and the path matters: the manifest
# names it absolutely, so mounting it anywhere else would test a rewritten
# path rather than the one that ships. Read-only is not a limitation here, it
# is a second scenario worth having: a shared or managed /opt is real, and the
# installer must degrade rather than fail on one.
# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
X64_TC="${AXL_X64_TOOLCHAIN_DIR:-}"
TC_ARGS=()
HAVE_TC=0
if [[ -n "$X64_TC" && -d "$X64_TC" ]]; then
    TC_ARGS=(-v "$X64_TC:$X64_TC:ro")
    HAVE_TC=1
else
    echo "  (no x64 toolchain at $X64_TC -- step 8e runs its SKIP path)"
fi

LOG="$WORK/lifecycle.log"
podman run --rm \
    -v "$REL:/rel:ro" \
    -v "$INSTALLER:/tmp/install.sh:ro" \
    "${TC_ARGS[@]}" \
    -e "HAVE_TC=$HAVE_TC" -e "X64_TC=$X64_TC" \
    docker.io/library/debian:stable-slim \
    sh -c "$LIFECYCLE" > "$LOG" 2>&1
RC=$?
sed 's/^/    /' "$LOG"

# Count the container's own verdicts rather than trusting its exit status
# alone: a step that never ran produces no FAIL, and an exit code cannot tell
# "all passed" from "died before asserting anything".
# `|| true`, NOT `|| echo 0`: grep -c already PRINTS 0 when it matches
# nothing, and exits 1 while doing it -- so `|| echo 0` appends a second line
# and the count becomes the two-line string "0\n0", which then blows up the
# arithmetic below with a syntax error instead of comparing anything.
PASSES="$(grep -c '^  PASS ' "$LOG" 2>/dev/null || true)"
FAILS="$(grep -c '^  FAIL ' "$LOG" 2>/dev/null || true)"
# EXACT, not a floor. The floor was >=25, and the cumulative count reaches 25
# at the END OF STEP 6 -- so deleting steps 7 and 8 left rc=0, zero FAILs and
# a green "ran the whole lifecycle". A count is the only thing standing
# between "every assertion passed" and "most of them never ran".
EXPECTED_ASSERTIONS=163
if [[ "$PASSES" -ne "$EXPECTED_ASSERTIONS" ]]; then
    test_host_fail "the container ran every assertion ($PASSES of $EXPECTED_ASSERTIONS)"
else
    test_host_pass "the container ran every assertion ($PASSES)"
fi
if [[ "$RC" -eq 0 && "$FAILS" -eq 0 ]]; then
    test_host_pass "install -> upgrade -> downgrade -> prune -> recover -> uninstall"
else
    test_host_fail "lifecycle failed (rc=$RC, $FAILS failed assertion(s))"
fi

test_host_summary "install-lifecycle"
