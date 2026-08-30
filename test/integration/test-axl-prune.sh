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
    done
    # Not ours. `axl-sdk-workspace` is the one that discriminates: it matches
    # `^axl-sdk-` and fails `^axl-sdk-[0-9]`, so it is exactly what the digit
    # anchor exists for. The first draft used only `axl-utils-workspace`, which
    # does not match either pattern -- an assertion that could not fail.
    mkdir -p "$WORK/opt/some-vendor-ide" "$WORK/opt/axl-utils-workspace" \
             "$WORK/opt/axl-sdk-workspace"
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
test_host_summary "axl-prune"
