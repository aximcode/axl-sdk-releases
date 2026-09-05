#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-toolchain-manifest-drift.sh — a self-inconsistent manifest must FAIL,
# not quietly start a 40-minute compile.
#
# WHY THIS EXISTS. axl-toolchains.conf names the x64 toolchain three times: a
# VERSION, a DIR that must contain it, and a literal URL. `check-toolchain-conf`
# compared the first two and never looked at the third, so a bump that missed
# the URL left every field individually plausible.
#
# WHAT THAT COSTS, and it is the reason this is a test and not a comment: the
# download SUCCEEDS, the sha of the OLD tarball MATCHES (it is the artifact the
# URL points at), it extracts cleanly -- and $gxx, built from DIR, is still
# missing. install_x64 used to print one WARNING line and fall through to a
# ~40 MINUTE SOURCE BUILD of GCC that ends in a working toolchain, hiding the
# inconsistency until the next bump. Found by a prune test that created the
# inconsistency by hand; the degradation path is real and expensive.
#
# aa64 never had this shape: AA64_URL is BUILT from AXL_AA64_TOOLCHAIN_VERSION,
# so the two cannot disagree. Only the x64 URL is a literal, which is why the
# gate and this test are x64-specific.
#
# The script is staged into a private tree with its own conf -- the same move
# test-axl-prune.sh makes -- so this never touches /opt or the real manifest.
# Downloads are file:// URLs; no network.
#
# Usage: ./test/integration/test-toolchain-manifest-drift.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

WORK="$(mktemp -d -t axl-tcdrift.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# --------------------------------------------------------------------------
# A toolchain tarball that unpacks to $1 and carries a working g++ stub.
# --------------------------------------------------------------------------
make_tarball() {
    local topdir="$1" out="$2" build="$WORK/build.$$"
    rm -rf "$build"; mkdir -p "$build/$topdir/bin" "$build/$topdir/pad"
    # ENOUGH ENTRIES THAT `tar -t` CANNOT FIT THE PIPE BUFFER. A two-file
    # tarball made every reader of this listing look correct: the whole listing
    # fits in 64 KB, so a `| head -1` consumer never SIGPIPEs the tar. The real
    # toolchain is thousands of files, where it does -- and under `set -e` that
    # killed the installer's diagnostic before it printed anything, which two
    # of the assertions below then "passed" on, because a crash satisfies a
    # negative. Keep this larger than the pipe buffer.
    local i
    for i in $(seq 1 4000); do : > "$build/$topdir/pad/entry-$i"; done
    cat > "$build/$topdir/bin/x86_64-elf-g++" <<'EOF'
#!/bin/sh
echo "x86_64-elf-g++ (AXL test stub) 14.3.0"
EOF
    chmod +x "$build/$topdir/bin/x86_64-elf-g++"
    cp "$build/$topdir/bin/x86_64-elf-g++" "$build/$topdir/bin/x86_64-elf-gcc"
    tar -cJf "$out" -C "$build" "$topdir"
    rm -rf "$build"
}

# Stage install-toolchain.sh beside a conf we control. $1 = the DIR the
# manifest declares; the tarball always unpacks to TARBALL_TOP.
stage() {
    local declared_dir="$1" url="$2" sha="$3" dst="$4"
    mkdir -p "$dst/scripts"
    cp "$PROJECT_DIR/scripts/install-toolchain.sh" "$dst/scripts/"
    cp "$PROJECT_DIR/scripts/axl-common.sh"        "$dst/scripts/" 2>/dev/null
    # A BUILDER STUB, and it is load-bearing for what case 2 proves. Without a
    # builder present the old code failed too -- at "the builder is not
    # present", a DIFFERENT error -- so the test would have passed against the
    # very behaviour it exists to catch. With the stub, the old fall-through
    # reaches the compile, "succeeds", and case 2 goes red as it should.
    mkdir -p "$dst/toolchain/x86_64-elf"
    cat > "$dst/toolchain/x86_64-elf/build-toolchain.sh" <<'EOS'
#!/bin/sh
echo "STUB BUILDER RAN: this is the 40-minute compile, stubbed"
mkdir -p "$PREFIX/bin"
printf '#!/bin/sh\necho "stub g++"\n' > "$PREFIX/bin/x86_64-elf-g++"
chmod +x "$PREFIX/bin/x86_64-elf-g++"
EOS
    chmod +x "$dst/toolchain/x86_64-elf/build-toolchain.sh"
    cat > "$dst/scripts/axl-toolchains.conf" <<EOF
AXL_AA64_TOOLCHAIN_VERSION=14.3.rel1
AXL_AA64_TOOLCHAIN_DIR=/nonexistent/aa64
AXL_AA64_GXX_DEFAULT=/nonexistent/aa64/bin/aarch64-none-elf-g++
AXL_AA64_GCC_DEFAULT=/nonexistent/aa64/bin/aarch64-none-elf-gcc
AXL_AA64_BINUTILS_PREFIX_DEFAULT=/nonexistent/aa64/bin/aarch64-none-elf-
AXL_AA64_TOOLCHAIN_SHA256=$(printf '0%.0s' $(seq 64))
AXL_X64_TOOLCHAIN_VERSION=14.3.0-axl3
AXL_X64_TOOLCHAIN_DIR=$declared_dir
AXL_X64_TOOLCHAIN_URL=$url
AXL_X64_TOOLCHAIN_SHA256=$sha
AXL_X64_GXX_DEFAULT=$declared_dir/bin/x86_64-elf-g++
AXL_X64_GCC_DEFAULT=$declared_dir/bin/x86_64-elf-gcc
AXL_X64_BINUTILS_PREFIX_DEFAULT=$declared_dir/bin/x86_64-elf-
EOF
}

TARBALL_TOP="x86_64-elf-gcc-14.3.0-axl3"
TARBALL="$WORK/x86_64-elf-gcc-14.3.0-axl3.tar.xz"
make_tarball "$TARBALL_TOP" "$TARBALL"
SHA="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
URL="file://$TARBALL"

echo "=== 1. CONTROL: an AGREEING manifest installs and does not build ==="
# Without this, case 2 proves only that the fixture is broken somehow.
OK_ROOT="$WORK/opt-ok"; mkdir -p "$OK_ROOT"
stage "$OK_ROOT/$TARBALL_TOP" "$URL" "$SHA" "$WORK/ok"
if INSTALL_ROOT="$OK_ROOT" "$WORK/ok/scripts/install-toolchain.sh" x64 \
        > "$WORK/ok.log" 2>&1; then
    pass "an agreeing manifest installs from the tarball"
else
    fail "the agreeing control FAILED -- case 2 below would prove nothing"
    sed 's/^/      /' "$WORK/ok.log" | tail -8
fi
if grep -q "from source" "$WORK/ok.log"; then
    fail "the control started a source build"
else
    pass "the control never reached a source build"
fi
# §21: the receipt is what makes the tree PRUNABLE. `axl prune` deletes a
# toolchain root only if it carries one, so an installer that stops writing it
# fails safe into never pruning anything -- silently, and only visible as
# /opt filling up over months. That is the failure mode this pins.
RECEIPT="$OK_ROOT/$TARBALL_TOP/.axl-receipt"
if [[ -r "$RECEIPT" ]]; then
    pass "the install leaves a receipt marking the root as ours"
else
    fail "no .axl-receipt in $OK_ROOT/$TARBALL_TOP -- prune can never remove it"
fi
# It has to carry the FACTS, not merely exist: the version is what a later
# reader needs, and the URL is the provenance the temp-dir SHA256SUMS threw
# away.
if grep -q "^AXL_RECEIPT_KIND=toolchain$" "$RECEIPT" 2>/dev/null \
   && grep -q "^AXL_RECEIPT_VERSION=14.3.0-axl3$" "$RECEIPT" 2>/dev/null \
   && grep -q "^AXL_RECEIPT_SOURCE=file://" "$RECEIPT" 2>/dev/null; then
    pass "and it records kind, version and source"
else
    fail "the receipt is missing fields"
    sed 's/^/      /' "$RECEIPT" 2>/dev/null | head -8
fi


# THE DOCUMENTED REMEDY, and it was a no-op when first written. The
# already-installed path returns before the install work -- so if it does not
# write a receipt, every toolchain that predates receipts is unprunable
# FOREVER (an unmarked root is not merely spared; it does not count toward
# --keep either) and the CHANGELOG's "re-run axl-install-toolchain to re-mark
# it" is advice that does nothing. Delete the receipt and re-run: it must come
# back, without re-downloading.
rm -f "$OK_ROOT/$TARBALL_TOP/.axl-receipt"
if INSTALL_ROOT="$OK_ROOT" "$WORK/ok/scripts/install-toolchain.sh" x64 \
        > "$WORK/remark.log" 2>&1; then
    if grep -q "already installed" "$WORK/remark.log"; then
        pass "re-running takes the already-installed path (no re-download)"
    else
        fail "the re-run did not short-circuit; it reinstalled"
    fi
    if [[ -r "$OK_ROOT/$TARBALL_TOP/.axl-receipt" ]]; then
        pass "and it RE-MARKS the root -- the documented remedy works"
    else
        fail "re-running left no receipt; the remedy is a no-op"
    fi
else
    fail "the re-run exited $?"
    sed 's/^/      /' "$WORK/remark.log" | tail -5
fi

echo
echo "=== 2. a DIR that disagrees with the URL fails, and says why ==="
# The tarball still unpacks to ...-axl3; the manifest claims ...-axl4. This is
# exactly a version bump that missed the URL.
BAD_ROOT="$WORK/opt-bad"; mkdir -p "$BAD_ROOT"
stage "$BAD_ROOT/x86_64-elf-gcc-14.3.0-axl4" "$URL" "$SHA" "$WORK/bad"
if INSTALL_ROOT="$BAD_ROOT" "$WORK/bad/scripts/install-toolchain.sh" x64 \
        > "$WORK/bad.log" 2>&1; then
    fail "a self-inconsistent manifest was reported as a successful install"
    sed 's/^/      /' "$WORK/bad.log" | tail -8
else
    pass "a self-inconsistent manifest FAILS the install"
fi
# THE POINT OF THE WHOLE TEST: it must not have started the compile.
if grep -q "building x86_64-elf from source" "$WORK/bad.log" \
   || grep -q "STUB BUILDER RAN" "$WORK/bad.log"; then
    fail "it fell through to a ~40 minute source build"
    sed 's/^/      /' "$WORK/bad.log" | tail -6
else
    pass "it did NOT fall through to a source build"
fi
# A failure that does not name the cause sends the reader to the wrong file.
if grep -q "AXL_X64_TOOLCHAIN_URL" "$WORK/bad.log" \
   && grep -q "AXL_X64_TOOLCHAIN_DIR" "$WORK/bad.log"; then
    pass "the error names both halves of the disagreement"
else
    fail "the error did not name AXL_X64_TOOLCHAIN_URL and _DIR"
    sed 's/^/      /' "$WORK/bad.log" | tail -6
fi
# A root the install did NOT complete must not be marked ours -- the receipt
# is written last, after the tree is known good, so a failed or half-extracted
# install is never a deletion candidate.
# THE DIRECTORY THAT ACTUALLY EXISTS. This asserted on
# $BAD_ROOT/x86_64-elf-gcc-14.3.0-axl4 -- the DECLARED dir, which the tarball
# never creates -- so it passed on a path nothing writes, including in a run
# where install-toolchain.sh was absent entirely. The failed install DOES
# leave $BAD_ROOT/$TARBALL_TOP behind, and that is where a stray receipt
# would appear.
#
# WHAT THIS DOES AND DOES NOT PIN, stated because the version it replaced
# claimed more than it proved: it catches a receipt written onto the tree a
# failed install extracted. It does NOT pin the "written last, after the tree
# is known good" ordering -- in this scenario the declared root does not exist
# at all, so write_receipt is a no-op through its own `-d` guard whatever
# order it is called in. Pinning the ordering needs a failure where the
# declared root DOES exist, which this manifest-drift fixture cannot produce.
if [[ ! -d "$BAD_ROOT/$TARBALL_TOP" ]]; then
    fail "fixture: the failed install left no extracted tree to check"
elif [[ -r "$BAD_ROOT/$TARBALL_TOP/.axl-receipt" ]]; then
    fail "a failed install marked its extracted tree as ours"
else
    pass "a failed install leaves no receipt on what it did extract"
fi
if grep -q "the tarball contains: $BAD_ROOT/$TARBALL_TOP" "$WORK/bad.log"; then
    pass "the error names the directory the tarball actually delivered"
else
    fail "the error never says what WAS extracted"
    sed 's/^/      /' "$WORK/bad.log" | tail -6
fi

echo
echo "=== 3. the gate catches it before anyone installs anything ==="
GATE="$PROJECT_DIR/scripts/check-toolchain-conf.py"
if python3 "$GATE" >/dev/null 2>&1; then
    pass "the real manifest passes check-toolchain-conf"
else
    fail "the real manifest FAILS check-toolchain-conf"
fi
# Sabotage BOTH halves of the URL: the version appears twice in it (release tag
# and tarball name), and the first version of this check compared the whole URL
# as a substring -- so changing only the filename left the tag matching and the
# gate satisfied. Each half must be caught on its own.
for expr in 's|x86_64-elf-gcc-14.3.0-axl3.tar.xz|x86_64-elf-gcc-14.3.0-axl9.tar.xz|' \
            's|toolchain-x86_64-elf-14.3.0-axl3/|toolchain-x86_64-elf-14.3.0-axl9/|'; do
    cp "$PROJECT_DIR/scripts/axl-toolchains.conf" "$WORK/conf.orig"
    mkdir -p "$WORK/gatetree/scripts"
    sed "$expr" "$WORK/conf.orig" > "$WORK/gatetree/scripts/axl-toolchains.conf"
    if cmp -s "$WORK/conf.orig" "$WORK/gatetree/scripts/axl-toolchains.conf"; then
        fail "sabotage matched nothing: ${expr:0:40}... -- proves nothing"
        continue
    fi
    cp "$PROJECT_DIR/scripts/check-toolchain-conf.py" "$WORK/gatetree/scripts/"
    mkdir -p "$WORK/gatetree/toolchain/x86_64-elf"
    cp "$PROJECT_DIR/toolchain/x86_64-elf/build-toolchain.sh" \
       "$WORK/gatetree/toolchain/x86_64-elf/" 2>/dev/null
    if python3 "$WORK/gatetree/scripts/check-toolchain-conf.py" \
            > "$WORK/gate.log" 2>&1; then
        fail "a URL naming a different version passed the gate (${expr:2:30}...)"
    elif grep -q "AXL_X64_TOOLCHAIN_URL" "$WORK/gate.log"; then
        pass "the gate rejects a drifted URL and names it (${expr:2:30}...)"
    else
        fail "the gate failed but not for the URL reason"
        sed 's/^/      /' "$WORK/gate.log" | head -4
    fi
    rm -rf "$WORK/gatetree"
done

echo
echo "toolchain-manifest-drift: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
