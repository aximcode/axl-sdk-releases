#!/bin/bash
# test-meta: arch=both needs= est=45 local-only=0
# test-install-idempotent.sh — install.sh must not rewrite files whose content
# has not changed.
#
# Consumers that build against a checkout reinstall the SDK on every build (an
# order-only prerequisite is the common pattern). When install.sh `cp`s every
# file unconditionally, every installed header's mtime bumps, and any consumer
# using gcc depfiles (-MD lists SDK headers because they arrive via -isystem)
# rebuilds its whole tree on every no-op build. Measured downstream: 84 of 85
# objects recompiled per no-op build, caused purely by 290 refreshed mtimes.
#
# So the contract this pins is: reinstalling unchanged sources is a no-op on
# the filesystem, and a changed source is still installed.
#
# Deliberately QEMU-free — it drives install.sh and stat(1) only, so it does
# not call test_setup (which resolves QEMU + firmware it would never use).
#
# Usage: ./test/integration/test-install-idempotent.sh [--arch X64|AARCH64]

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
# Every gate below is an expected-to-sometimes-fail comparison whose rc is
# captured and reported, so errexit (inherited from common-test.sh) would abort
# the run at the first red instead of printing the tally.
set +e

case "$TEST_ARCH" in
    AARCH64) INSTALL_ARCH="aa64" ;;
    *)       INSTALL_ARCH="x64"  ;;
esac

pass=0
fail=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1))
    else echo "FAIL: $2"; fail=$((fail+1)); fi
}

WORK=$(mktemp -d -t axl-install-idem.XXXXXXXX)
PREFIX="$WORK/prefix"
trap 'rm -rf "$WORK"' EXIT

# `build-date` is a dated file: it legitimately changes when the UTC day rolls
# over mid-run. Excluded from the stability assertion for that reason alone —
# every other installed file must be byte-stable across a no-op reinstall.
snapshot() {  # snapshot <outfile> — "<mtime> <path>" for every installed file
    ( cd "$PREFIX" && find . -type f ! -name build-date -printf '%T@ %p\n' \
        | LC_ALL=C sort ) > "$1"
}

run_install() {  # run_install <logfile>
    "$PROJECT_DIR/scripts/install.sh" --arch "$INSTALL_ARCH" --prefix "$PREFIX" \
        > "$1" 2>&1
}

# --- 1. Fresh prefix: everything is installed --------------------------------
run_install "$WORK/install1.log"
rc=$?
check "$rc" "fresh install into an empty prefix succeeds (rc=$rc)"
if [[ "$rc" -ne 0 ]]; then
    tail -20 "$WORK/install1.log" >&2
    echo "--- results ---"
    echo "install idempotence ($TEST_ARCH): $pass passed, $((fail+1)) failed"
    exit 1
fi

HDR_DIR="$PREFIX/include/axl-sdk"
n_hdr=$(find "$HDR_DIR" -name '*.h' | wc -l)
[[ "$n_hdr" -gt 100 ]]
check "$?" "fresh install staged the public headers ($n_hdr found)"

# Installed headers are byte-identical to their sources.
diff -r -q "$PROJECT_DIR/include/axl" "$HDR_DIR/axl" >/dev/null 2>&1
check "$?" "installed axl/ headers are byte-identical to source"

# --- 2. Reinstall with no source change is a filesystem no-op ----------------
snapshot "$WORK/before.txt"
# install.sh's own mtime resolution is whole seconds in the worst case; sleep
# past a full second so a rewrite is guaranteed to be VISIBLE as a bump. Without
# this the assertion could pass on a fast machine even against a plain `cp`.
sleep 1.1
run_install "$WORK/install2.log"
rc=$?
check "$rc" "second install (no source change) succeeds (rc=$rc)"
snapshot "$WORK/after.txt"

diff -u "$WORK/before.txt" "$WORK/after.txt" > "$WORK/mtime.diff" 2>&1
check "$?" "reinstall of unchanged sources leaves every installed mtime alone"
if [[ -s "$WORK/mtime.diff" ]]; then
    echo "      churned files (first 15):" >&2
    grep '^+[0-9]' "$WORK/mtime.diff" | head -15 | sed 's/^/        /' >&2
    echo "      total churned: $(grep -c '^+[0-9]' "$WORK/mtime.diff")" >&2
fi

# --- 3. DISCRIMINATION: a destination that DIFFERS is still reinstalled ------
# An over-eager skip passes gate 2 and fails here, so this is what stops the
# fix from degenerating into "never copy anything".
#
# Corrupting each installed copy also makes it strictly NEWER than its source,
# which is precisely the case a plain `cp -u` gets wrong: cp -u compares
# mtimes, so it leaves the corruption in place forever. This gate is what rules
# that implementation out.
#
# Note this deliberately perturbs the DESTINATION, never the source tree. An
# earlier draft mutated scripts/efi-localize.ver in place to prove "a changed
# SOURCE is reinstalled", which is unsafe twice over: a Ctrl-C in the window
# leaves the developer's git-tracked tree dirty, and test-jose-cc-qemu.sh runs
# its own install.sh --arch x64 concurrently under run-integration.sh, so it
# could stage the corrupted linker script into an unrelated test's prefix. The
# coverage is not lost: `install -C` compares CONTENT, so source-differs and
# destination-differs take the identical branch — and the destination-side
# version is strictly stronger, because only it also kills `cp -u`.
#
# One victim per copy mechanism, so the restore path is proven for all of them.
declare -A VICTIMS=(
    ["$HDR_DIR/axl/axl-mem.h"]="header (install -C -m 644)"
    ["$PREFIX/lib/axl/efi-localize.ver"]="linker version script (install -C)"
    ["$PREFIX/bin/axl-cc"]="axl-cc driver (install -C -m 755)"
    ["$PREFIX/lib/pkgconfig/axl-$INSTALL_ARCH.pc"]="pkg-config (write_if_changed)"
    ["$PREFIX/lib/cmake/axl/axl-config.cmake"]="cmake config (write_if_changed)"
    ["$PREFIX/share/axl/backend"]="metadata (write_if_changed)"
)
mkdir -p "$WORK/orig"
i=0
for v in "${!VICTIMS[@]}"; do
    i=$((i+1))
    cp "$v" "$WORK/orig/$i"
    printf '\n# corrupted by test-install-idempotent\n' >> "$v"
    touch "$v"                     # destination strictly newer than its source
done

run_install "$WORK/install3.log"
rc=$?
check "$rc" "third install (corrupted destinations) succeeds (rc=$rc)"

i=0
for v in "${!VICTIMS[@]}"; do
    i=$((i+1))
    cmp -s "$v" "$WORK/orig/$i"
    check "$?" "a differing, NEWER destination is reinstalled — ${VICTIMS[$v]}"
done

# Executable bits must survive the repair, not just content.
[[ -x "$PREFIX/bin/axl-cc" ]]
check "$?" "the repaired axl-cc is still executable"

# --- 4. A fourth no-op install is still a no-op ------------------------------
# Guards against a fix that is idempotent only on the first repeat (e.g. one
# that rewrites on alternating runs).
snapshot "$WORK/before2.txt"
sleep 1.1
run_install "$WORK/install4.log"
snapshot "$WORK/after2.txt"
diff -q "$WORK/before2.txt" "$WORK/after2.txt" >/dev/null 2>&1
check "$?" "a further no-op install after a repair is still mtime-stable"

# --- N. --cpp requires a toolchain only for the arch being BUILT -------------
#
# The check used to test the x64 AND the AArch64 compiler unconditionally, so
# `--arch x64 --cpp` hard-failed on a missing AArch64 bare-metal g++ it was
# never going to invoke. That is exactly what CI's integration job runs, so
# that step had never once succeeded there.
#
# AXL_AA64_GXX is the documented override (scripts/axl-toolchains.conf), so
# pointing it at a nonexistent path is a supported way to simulate the absence
# rather than a trick. The two negative cases matter as much as the positive
# one: a fix that scoped too far would silently stop requiring a toolchain the
# build genuinely needs, and these are what would catch that.
CPPW="$WORK/cpp"; mkdir -p "$CPPW"
NOARM="/nonexistent/aarch64-none-elf-g++"
NOX64="/nonexistent/x86_64-g++"

# These two consult no host compiler: the first skips the x64 arm via the
# predicate under test, the second overrides AXL_X64_GXX itself. So they run
# unconditionally -- guarding them on host g++ would retire runnable coverage.
AXL_AA64_GXX="$NOARM" "$PROJECT_DIR/scripts/install.sh" \
    --arch aa64 --cpp --prefix "$CPPW/aa64" > "$CPPW/aa64.log" 2>&1
rc=$?
[[ "$rc" -ne 0 ]] && grep -qi "ARM bare-metal g++ not found" "$CPPW/aa64.log"
check "$?" "--arch aa64 --cpp still fails when the AArch64 toolchain is absent (rc=$rc)"

# The x64 half of the same predicate. Without this the aa64 gates do NOT catch
# a fix that scoped too far: breaking the x64 arm alone leaves them green,
# because neither of them builds x64.
AXL_X64_GXX="$NOX64" "$PROJECT_DIR/scripts/install.sh" \
    --arch x64 --cpp --prefix "$CPPW/nox64" > "$CPPW/nox64.log" 2>&1
rc=$?
[[ "$rc" -ne 0 ]] && grep -qi "not found (needed for x64" "$CPPW/nox64.log"
check "$?" "--arch x64 --cpp still fails when the x64 compiler is absent (rc=$rc)"

# `all` DOES consult the x64 C++ compiler: without it the x64 arm fails first
# and the log carries THAT message, so the ARM grep would go red for the wrong
# reason.
#
# Resolved through the manifest rather than `command -v g++`. x64 C++ moved off
# the host compiler in T2, so probing for host g++ answers a question nothing
# asks any more -- and it answers it WRONG in both directions: green on a box
# with g++ and no cross (the run then fails for the reason this guard exists to
# avoid), and skipped on a box with the cross and no g++ (retiring coverage
# that would have run).
# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
X64_CXX="${AXL_X64_GXX:-$AXL_X64_GXX_DEFAULT}"
# The predicate must MATCH install.sh's, not merely resemble it. install.sh
# accepts a bare name found on PATH as well as a path, and AXL_X64_GXX spelled
# as a bare name is a supported override (the Makefile's guard says so
# explicitly). A plain `-x` test rejects that spelling and SKIPS the gate --
# quietly returning the wrong answer, which is the failure this guard was
# rewritten to stop doing.
have_x64_cxx() {
    [[ -n "$X64_CXX" ]] &&
        { [[ -x "$X64_CXX" ]] || command -v "$X64_CXX" >/dev/null 2>&1; }
}
if have_x64_cxx; then
    AXL_AA64_GXX="$NOARM" "$PROJECT_DIR/scripts/install.sh" \
        --arch all --cpp --prefix "$CPPW/all" > "$CPPW/all.log" 2>&1
    rc=$?
    [[ "$rc" -ne 0 ]] && grep -qi "ARM bare-metal g++ not found" "$CPPW/all.log"
    check "$?" "--arch all --cpp still fails when the AArch64 toolchain is absent (rc=$rc)"
else
    check 0 "SKIP: no x64 bare-metal g++ — --arch all --cpp gate not run"
fi

# THE DISCRIMINATOR. The three gates above all pass against the OLD, broken
# predicate too -- only this one tells fixed from broken, so read an aa64
# result as "this regression was not exercised here", not as coverage.
#
# It is the only gate that BUILDS, and it is restricted to the x64 shard
# because sections 1-4 of THIS script have already warmed out/native-x64-release
# there (measured: 5s). run-integration.sh's prebuild is NOT the warmer -- that
# builds out/native-x64 at BUILD=DEBUG, a different PREFIX and so a different
# BUILDDIR. On an aa64 shard those sections warm aa64 instead, leaving this a
# from-scratch RELEASE build against a `# test-meta:` est of 45s.
if [[ "$INSTALL_ARCH" != "x64" ]]; then
    check 0 "SKIP: aa64 shard — the discriminating x64 --cpp gate runs on the x64 shard"
elif ! have_x64_cxx; then
    check 0 "SKIP: no x64 bare-metal g++ — the discriminating x64 --cpp gate not run"
else
    AXL_AA64_GXX="$NOARM" "$PROJECT_DIR/scripts/install.sh" \
        --arch x64 --cpp --prefix "$CPPW/x64" > "$CPPW/x64.log" 2>&1
    rc=$?
    # Keyed on a cxxrt OBJECT: P4 deleted libaxl-cxx.a, and keying a
    # PRESENCE check on a file that no longer exists would fail forever.
    [[ "$rc" -eq 0 && -f "$CPPW/x64/lib/axl/x64/axl-cxxrt-terminate.o" ]]
    check "$?" "--arch x64 --cpp succeeds without the AArch64 toolchain (rc=$rc)"
fi

echo "--- results ---"
echo "install idempotence ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
