#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-axl-prune.sh -- `axl prune` bounds what accumulates, on both axes.
#
# WHY THIS EXISTS. Measured on this machine 2026-08-29: 7.1 GB across eight
# pinned SDK versions in $HOME, every one a SOURCE CHECKOUT whose out/ is ~85%
# of it -- because with no tarball a version could only be pinned by keeping
# the whole tree. The tarball makes a pinned version 42 MB extracted / 7.2 MB
# downloaded, so that axis is now cheap. What is NOT cheap is the toolchains:
# /opt/arm-gnu-toolchain-* is 500 MB and /opt/x86_64-elf-gcc-*-axl3 is 239 MB,
# 739 MB per generation, and --prefix just made them installable in more
# places. Release cadence is 12 tags in 17 days, so "it will not add up" is not
# available as an argument.
#
# The policy is current + one previous (§12.4): the value of an old root is a
# FAST ROLLBACK when a pin bump breaks a build, and beyond one, a pinned
# checksum plus a 7.2 MB download is cheaper than storage.
#
# WHAT MAKES IT SAFE. §12.3's contract -- a prefix writes nothing outside
# itself, so removing the directory is a complete uninstall. This test pins the
# refusals that keep that from becoming a footgun: never the running prefix,
# never what `current` points at, and never a directory that is not
# identifiably ours. /opt is shared with everything else on the machine, and a
# glob that grows one character deletes somebody's IDE.
#
# Runs entirely against synthetic directory trees: no installs, no downloads.
#
# Usage: ./test/integration/test-axl-prune.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

WORK="$(mktemp -d -t axl-prune.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== axl prune ==="
echo ""

# Build a synthetic /opt: several SDK roots, several toolchain roots, a
# `current` symlink, and two directories that are NOT ours.
# `axl` is placed in the NEWEST root and run from there, which is the real
# shape -- you prune with the version you just installed.
setup() {
    rm -rf "$WORK/opt"; mkdir -p "$WORK/opt"
    local v
    for v in 4.3.0 4.3.2 4.3.4 4.3.5; do
        mkdir -p "$WORK/opt/axl-sdk-$v/bin" "$WORK/opt/axl-sdk-$v/libexec/axl" \
                 "$WORK/opt/axl-sdk-$v/share/axl"
        echo "$v" > "$WORK/opt/axl-sdk-$v/share/axl/version"
        cp "$PROJECT_DIR/scripts/axl" "$WORK/opt/axl-sdk-$v/bin/axl"
        cp "$PROJECT_DIR/scripts/axl-prune.sh" \
           "$WORK/opt/axl-sdk-$v/libexec/axl/axl-prune.sh" 2>/dev/null
        chmod +x "$WORK/opt/axl-sdk-$v/bin/axl" \
                 "$WORK/opt/axl-sdk-$v/libexec/axl/axl-prune.sh" 2>/dev/null
        # The manifest, so prune can learn the toolchain family names from it
        # rather than hardcoding /opt -- the same reason the self-containment
        # test reads it.
        cat > "$WORK/opt/axl-sdk-$v/share/axl/axl-toolchains.conf" <<EOF
AXL_AA64_TOOLCHAIN_DIR=$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
AXL_X64_TOOLCHAIN_DIR=$WORK/opt/x86_64-elf-gcc-14.3.0-axl3
EOF
    done
    # THREE generations each, deliberately. With only two, "keep one previous"
    # keeps the old one and the assertion cannot tell a working prune from one
    # that does nothing.
    for t in arm-gnu-toolchain-12.1.rel1-x86_64-aarch64-none-elf \
             arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf \
             arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf \
             x86_64-elf-gcc-14.1.0-axl0 \
             x86_64-elf-gcc-14.2.0-axl1 x86_64-elf-gcc-14.3.0-axl3; do
        mkdir -p "$WORK/opt/$t/bin"
        # OURS: install-toolchain.sh leaves a receipt in every root it
        # installs, and §21 makes that the licence to delete it.
        printf 'AXL_RECEIPT_KIND=toolchain\nAXL_RECEIPT_NAME=%s\n' "$t" \
            > "$WORK/opt/$t/.axl-receipt"
    done
    # NOT OURS, and the whole point of §21. ARM publishes its toolchains under
    # exactly this name, so a developer who unpacked one by hand has a
    # directory matching the family stem prune derives from the manifest --
    # a DIFFERENT target triple, nothing to do with this SDK. Two of them,
    # because with one the keep-newest rule hides the bug.
    mkdir -p "$WORK/opt/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi/bin" \
             "$WORK/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin"
    # THE MANAGER FAMILY (§20 M2). `axl` links from here, not from an SDK
    # root, so this is the shape a real install has -- and `axl prune` itself
    # runs out of this tree's libexec, which is why its own prefix has to be
    # protected explicitly rather than by luck.
    local hv
    for hv in 4.4.0 4.5.0 4.6.0; do
        mkdir -p "$WORK/opt/axl-sdk-host-tools-$hv/bin" \
                 "$WORK/opt/axl-sdk-host-tools-$hv/libexec/axl" \
                 "$WORK/opt/axl-sdk-host-tools-$hv/share/axl"
        cp "$PROJECT_DIR/scripts/axl" "$WORK/opt/axl-sdk-host-tools-$hv/bin/axl"
        cp "$PROJECT_DIR/scripts/axl-prune.sh" \
           "$WORK/opt/axl-sdk-host-tools-$hv/libexec/axl/axl-prune.sh"
        chmod +x "$WORK/opt/axl-sdk-host-tools-$hv/bin/axl" \
                 "$WORK/opt/axl-sdk-host-tools-$hv/libexec/axl/axl-prune.sh"
        # DELIBERATELY NO axl-toolchains.conf: make-host-tools-tarball.sh
        # writes share/axl/version and nothing else. Staging one here would
        # hide the half of the regression where prune, running from the
        # manager, can no longer find the manifest at all.
        echo "$hv" > "$WORK/opt/axl-sdk-host-tools-$hv/share/axl/version"
    done
    ln -sfn "$WORK/opt/axl-sdk-host-tools-4.6.0" "$WORK/opt/axl-sdk-host-tools"

    # Not ours. `axl-sdk-workspace` is the one that discriminates: it matches
    # `^axl-sdk-` and fails `^axl-sdk-[0-9]`, so it is exactly what the digit
    # anchor exists for. The first draft used only `axl-utils-workspace`, which
    # does not match either pattern -- an assertion that could not fail.
    # `axl-sdk-host-tools-workspace` is the same discriminator one family over.
    mkdir -p "$WORK/opt/some-vendor-ide" "$WORK/opt/axl-utils-workspace" \
             "$WORK/opt/axl-sdk-workspace" "$WORK/opt/axl-sdk-host-tools-workspace"
    ln -sfn "$WORK/opt/axl-sdk-4.3.4" "$WORK/opt/axl-sdk"   # `current`
}

if [[ ! -f "$PROJECT_DIR/scripts/axl-prune.sh" ]]; then
    test_host_fail "scripts/axl-prune.sh exists"
    test_host_summary "axl-prune"
    exit 1
fi
test_host_pass "scripts/axl-prune.sh exists"

AXL_NEW="$WORK/opt/axl-sdk-4.3.5/bin/axl"

# ── --dry-run changes nothing ─────────────────────────────────
setup
DRY="$WORK/dry.txt"
"$AXL_NEW" prune --dry-run > "$DRY" 2>&1
if [[ $? -eq 0 ]]; then
    test_host_pass "--dry-run exits 0"
else
    test_host_fail "--dry-run exits 0"
    sed 's/^/      /' "$DRY" | head -6
fi
if [[ -d "$WORK/opt/axl-sdk-4.3.0" ]]; then
    test_host_pass "--dry-run removes nothing"
else
    test_host_fail "--dry-run removes nothing"
fi
if grep -q 'axl-sdk-4.3.0' "$DRY"; then
    test_host_pass "--dry-run names what it would remove"
else
    test_host_fail "--dry-run names what it would remove"
    sed 's/^/      /' "$DRY" | head -8
fi

# ── the default policy: current + one previous ────────────────
setup
OUT="$WORK/run.txt"
"$AXL_NEW" prune > "$OUT" 2>&1
RC=$?
if [[ $RC -eq 0 ]]; then
    test_host_pass "prune exits 0"
else
    test_host_fail "prune exits 0 (got $RC)"
    sed 's/^/      /' "$OUT" | head -8
fi

# The running prefix is never a candidate -- pruning the thing you are
# executing from is the one removal that cannot be undone by re-downloading,
# because it takes the tool that would do the re-download with it.
if [[ -d "$WORK/opt/axl-sdk-4.3.5" ]]; then
    test_host_pass "the running prefix survives"
else
    test_host_fail "the running prefix survives"
fi
# `current` points at 4.3.4: something else on this machine is using it.
if [[ -d "$WORK/opt/axl-sdk-4.3.4" ]]; then
    test_host_pass "what 'current' points at survives"
else
    test_host_fail "what 'current' points at survives"
fi
# One previous kept, older ones gone.
if [[ -d "$WORK/opt/axl-sdk-4.3.2" ]]; then
    test_host_pass "one previous version is kept (4.3.2)"
else
    test_host_fail "one previous version is kept (4.3.2)"
fi
if [[ ! -d "$WORK/opt/axl-sdk-4.3.0" ]]; then
    test_host_pass "older versions are removed (4.3.0)"
else
    test_host_fail "older versions are removed (4.3.0)"
fi

# ── the toolchain axis, which is where the bytes are ──────────
if [[ -d "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3" && \
      -d "$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf" ]]; then
    test_host_pass "the manifest's current toolchains survive"
else
    test_host_fail "the manifest's current toolchains survive"
fi
# One previous generation is KEPT, and that is not an oversight: an SDK root
# kept for rollback was built against the toolchain of its day, so discarding
# that toolchain would break the rollback the policy exists to provide. The
# generation before it goes.
if [[ -d "$WORK/opt/x86_64-elf-gcc-14.2.0-axl1" && \
      -d "$WORK/opt/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf" ]]; then
    test_host_pass "one previous toolchain generation is kept, matching the SDK policy"
else
    test_host_fail "one previous toolchain generation is kept, matching the SDK policy"
    ls -d "$WORK/opt"/*toolchain* "$WORK/opt"/x86_64-elf-* 2>/dev/null | sed 's/^/      /'
fi
if [[ ! -d "$WORK/opt/x86_64-elf-gcc-14.1.0-axl0" && \
      ! -d "$WORK/opt/arm-gnu-toolchain-12.1.rel1-x86_64-aarch64-none-elf" ]]; then
    test_host_pass "older toolchain generations are removed (739 MB each)"
else
    test_host_fail "older toolchain generations are removed"
    ls -d "$WORK/opt"/*toolchain* "$WORK/opt"/x86_64-elf-* 2>/dev/null | sed 's/^/      /'
fi

# ── nothing that is not ours ──────────────────────────────────
#
# The assertion that matters most. /opt is shared, and this is the failure that
# would be unrecoverable rather than merely annoying.
if [[ -d "$WORK/opt/some-vendor-ide" && -d "$WORK/opt/axl-utils-workspace" \
      && -d "$WORK/opt/axl-sdk-workspace" ]]; then
    test_host_pass "directories that are not ours are untouched"
else
    test_host_fail "directories that are not ours are untouched"
    echo "      NOTE: axl-sdk-workspace matches ^axl-sdk- and not"
    echo "            ^axl-sdk-[0-9]. Dropping the digit deletes it."
fi

# ── --keep is honoured ────────────────────────────────────────
setup
"$AXL_NEW" prune --keep 0 > "$WORK/keep0.txt" 2>&1
if [[ ! -d "$WORK/opt/axl-sdk-4.3.2" && -d "$WORK/opt/axl-sdk-4.3.5" \
      && -d "$WORK/opt/axl-sdk-4.3.4" ]]; then
    test_host_pass "--keep 0 keeps only the running prefix and 'current'"
else
    test_host_fail "--keep 0 keeps only the running prefix and 'current'"
    ls -d "$WORK/opt"/axl-sdk-* 2>/dev/null | sed 's/^/      /'
fi

# ...and the "not ours" check repeated under --keep 0, which is where it
# actually discriminates. At the default keep, `axl-sdk-workspace` sorts LAST
# under `sort -V` and is spared by the keep count rather than by the digit
# anchor -- so a sabotage that drops the anchor failed a different assertion
# and left this one green. With nothing spared by count, the anchor is the only
# thing standing between a consumer's directory and rm -rf.
if [[ -d "$WORK/opt/axl-sdk-workspace" && -d "$WORK/opt/axl-utils-workspace" \
      && -d "$WORK/opt/some-vendor-ide" ]]; then
    test_host_pass "--keep 0 still touches nothing that is not ours"
else
    test_host_fail "--keep 0 still touches nothing that is not ours"
    ls -d "$WORK/opt"/* 2>/dev/null | sed 's/^/      /'
fi

setup
"$AXL_NEW" prune --keep 5 > "$WORK/keep5.txt" 2>&1
if [[ -d "$WORK/opt/axl-sdk-4.3.0" && -d "$WORK/opt/axl-sdk-4.3.2" ]]; then
    test_host_pass "--keep 5 removes nothing when there is less than that"
else
    test_host_fail "--keep 5 removes nothing when there is less than that"
fi

echo ""
# ── the MANAGER family accumulates unless prune walks it ──────
# §20 M2 made the host-tools root the manager, and `^axl-sdk-[0-9]` does not
# match `axl-sdk-host-tools-<ver>` -- correct, because `axl use` and `axl
# prune` must never move or remove the thing managing versions. The
# consequence nobody bounded: old manager roots piled up forever.
setup
HT="$WORK/ht.txt"
"$AXL_NEW" prune > "$HT" 2>&1
if [[ ! -d "$WORK/opt/axl-sdk-host-tools-4.4.0" ]]; then
    test_host_pass "the oldest manager root is pruned"
else
    test_host_fail "the oldest manager root survived (host-tools never pruned)"
    sed 's/^/      /' "$HT" | head -8
fi
if [[ -d "$WORK/opt/axl-sdk-host-tools-4.5.0" ]]; then
    test_host_pass "one previous manager root is kept"
else
    test_host_fail "one previous manager root is kept"
fi
if [[ -d "$WORK/opt/axl-sdk-host-tools-4.6.0" ]]; then
    test_host_pass "the CURRENT manager root is never pruned"
else
    test_host_fail "the current manager root was REMOVED -- axl is now gone"
fi
if [[ -d "$WORK/opt/axl-sdk-host-tools-workspace" ]]; then
    test_host_pass "a non-versioned host-tools name is left alone"
else
    test_host_fail "pruned axl-sdk-host-tools-workspace -- the digit anchor is missing"
fi

# ── pruning FROM the manager must not delete itself ───────────
# The real shape: install.sh links `axl` out of the host-tools root, so
# `axl prune` executes from that tree's libexec. Removing it mid-run would
# delete the running script and the front door in one step.
#
# RUN FROM THE OLDEST, which is the only choice that discriminates. Running
# from 4.5.0 survives on `--keep 1` alone -- it is the one previous generation
# the policy retains anyway -- so dropping $PREFIX from the protected list
# changed nothing and the assertion could not fail. 4.4.0 is what the policy
# WOULD remove, so only the explicit protection saves it.
setup
ln -sfn "$WORK/opt/axl-sdk-host-tools-4.6.0" "$WORK/opt/axl-sdk-host-tools"
SELF="$WORK/self.txt"
"$WORK/opt/axl-sdk-host-tools-4.4.0/bin/axl" prune > "$SELF" 2>&1
if [[ -x "$WORK/opt/axl-sdk-host-tools-4.4.0/bin/axl" ]]; then
    test_host_pass "prune run from a manager root does not remove that root"
else
    test_host_fail "prune DELETED the manager it was running from"
    sed 's/^/      /' "$SELF" | head -8
fi
if [[ -d "$WORK/opt/axl-sdk-host-tools-4.6.0" ]]; then
    test_host_pass "and still protects the current manager"
else
    test_host_fail "and still protects the current manager"
fi

# ── prune from the MANAGER still prunes what it manages ───────
# §20 M2 links `axl` out of the host-tools root, so this is what `axl prune`
# does on a stock install -- and `axl-sdk-host-tools-4.6.0` does not match
# `^axl-sdk-[0-9]`, so the versioned-root guard rejected its own manager and
# announced "not applicable". Combined with the manifest living in the SDK
# prefix and not the manager's, `axl prune` pruned NOTHING at all: no SDK
# roots, no toolchains. A no-op that reports success.
setup
MGR="$WORK/mgr.txt"
"$WORK/opt/axl-sdk-host-tools-4.6.0/bin/axl" prune > "$MGR" 2>&1
if [[ ! -d "$WORK/opt/axl-sdk-4.3.0" ]]; then
    test_host_pass "prune from the manager still prunes old SDK roots"
else
    test_host_fail "prune from the manager pruned NO SDK roots"
    sed 's/^/      /' "$MGR" | head -8
fi
if [[ -d "$WORK/opt/axl-sdk-4.3.4" && -d "$WORK/opt/axl-sdk-4.3.5" ]]; then
    test_host_pass "and still protects current + one previous SDK"
else
    test_host_fail "and still protects current + one previous SDK"
fi
if [[ ! -d "$WORK/opt/x86_64-elf-gcc-14.1.0-axl0" ]]; then
    test_host_pass "prune from the manager finds the manifest via the current SDK"
else
    test_host_fail "toolchain roots untouched -- the manifest was not found"
    sed 's/^/      /' "$MGR" | head -8
fi
if [[ -d "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3" ]]; then
    test_host_pass "and never removes the toolchain in use"
else
    test_host_fail "and never removes the toolchain in use"
fi

# ── a trailing slash in the manifest must not unprotect ───────
# The protected paths are compared to glob results as strings, so a manifest
# path written with a trailing slash stopped matching the directory it names
# and the toolchain IN USE became a prune candidate. (Same root cause as a
# `$ROOT` of `/`, where the glob yields `//name` against a single-slash
# `$PREFIX` -- that one needs a manifest pointing at `/`, this one needs one
# stray character.) The in-use toolchain here is deliberately NOT the newest:
# as the newest it survives on `--keep` alone and the assertion cannot fail.
setup
cat > "$WORK/opt/axl-sdk-4.3.5/share/axl/axl-toolchains.conf" <<EOF
AXL_AA64_TOOLCHAIN_DIR=$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
AXL_X64_TOOLCHAIN_DIR=$WORK/opt/x86_64-elf-gcc-14.2.0-axl1/
EOF
SLASH="$WORK/slash.txt"
"$AXL_NEW" prune > "$SLASH" 2>&1
if [[ -d "$WORK/opt/x86_64-elf-gcc-14.2.0-axl1" ]]; then
    test_host_pass "a trailing slash in the manifest still protects that toolchain"
else
    test_host_fail "the toolchain named WITH a trailing slash was removed"
    sed 's/^/      /' "$SLASH" | head -8
fi
# The control: prune must still have done its job in the same run, or the
# assertion above passes simply because nothing was pruned at all.
if [[ ! -d "$WORK/opt/x86_64-elf-gcc-14.1.0-axl0" ]]; then
    test_host_pass "control: the superseded toolchain was still pruned"
else
    test_host_fail "control: nothing was pruned, so the assertion above is vacuous"
fi

# ── §21: a toolchain we did not install is never removed ─────
# `axl prune` matched toolchain families by a NAME PATTERN derived from the
# manifest, not by whether we put them there. §12.5 required the opposite from
# the start -- "only roots recorded in a manifest we wrote", and the row names
# the consequence: "a glob over /opt that grows a character deletes someone's
# IDE". Measured before this: with two foreign arm-none-eabi toolchains
# present, the older became a deletion candidate.
setup
OWN="$WORK/own.txt"
"$AXL_NEW" prune > "$OWN" 2>&1
for foreign in arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi \
               arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi; do
    if [[ -d "$WORK/opt/$foreign" ]]; then
        test_host_pass "a toolchain we did not install survives ($foreign)"
    else
        test_host_fail "prune REMOVED a foreign toolchain: $foreign"
        sed 's/^/      /' "$OWN" | head -8
    fi
done
# The control: ours must still be pruned in the same run, or the assertions
# above pass simply because pruning stopped working.
if [[ ! -d "$WORK/opt/arm-gnu-toolchain-12.1.rel1-x86_64-aarch64-none-elf" ]]; then
    test_host_pass "control: our own superseded toolchain was still pruned"
else
    test_host_fail "control: nothing was pruned, so the above proves nothing"
    sed 's/^/      /' "$OWN" | head -8
fi
# IT MUST SAY SO. A skipped root is invisible otherwise, and after §21 every
# toolchain installed before receipts existed lands in that bucket -- silence
# there is indistinguishable from "there was nothing to collect", which is how
# a /opt that stopped being bounded goes unnoticed for months.
# AND IT MUST NOT CLAIM MORE THAN IT KNOWS. The header used to assert "not
# installed by us, so not ours to remove" and then, three lines down, ask
# "(ours, but installed before receipts existed?)" -- a fact stated and
# retracted in one message. On this box the retraction was the true half: the
# named roots were /opt/x86_64-elf-gcc-14.3.0-axl and -axl2, our own naming.
# The old remedy could not work either. It said to re-run the installer to
# re-mark the root, but the installer only ever re-marks the root the MANIFEST
# names -- never the superseded ones this message is printed about.
if grep -qF "with no .axl-receipt -- not removing them" "$OWN"; then
    test_host_pass "a real run reports the roots it skipped, without claiming why"
else
    test_host_fail "nothing reported about the skipped foreign toolchains"
    sed 's/^/      /' "$OWN" | head -10
fi
if grep -q "not installed by us" "$OWN"; then
    test_host_fail "the skipped-roots header still asserts they are not ours"
    grep -n "not installed by us" "$OWN" | sed 's/^/      /'
else
    test_host_pass "the skipped-roots header no longer asserts they are not ours"
fi
# And it must NOT call the manifest's own toolchain foreign: the protected
# check has to run before the ownership check, or the compiler in use -- named
# in a file we wrote -- is announced as "not ours".
setup
DRYOWN="$WORK/dryown.txt"
"$AXL_NEW" prune --dry-run > "$DRYOWN" 2>&1
if grep -q "arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf" "$DRYOWN" \
   && grep -A2 "no .axl-receipt" "$DRYOWN" 2>/dev/null \
      | grep -q "14.3.rel1-x86_64-aarch64-none-elf"; then
    test_host_fail "the manifest's own toolchain was listed as not ours"
    sed 's/^/      /' "$DRYOWN" | head -10
else
    test_host_pass "the toolchain in use is never reported as not ours"
fi

test_host_summary "axl-prune"
