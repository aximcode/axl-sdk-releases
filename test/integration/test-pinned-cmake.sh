#!/bin/bash
# test-meta: arch=none needs=flock est=5 local-only=0
# test-pinned-cmake.sh -- every local build and test entry point runs the
# SAME CMake CI and the release do (scripts/axl-cmake-pin.sh;
# docs/AXL-Build-System-Design.md §10.13).
#
# WHY. CI and the release pin CMake 4.2.8 (scripts/ci-cmake.sh, §10.12);
# local runs took whatever `cmake` the box had (3.31.8 here). A test then
# passed on every local run and failed only on CI: CMake 4.2 writes
# compile_commands.json's `output` absolute where 3.31 writes it relative.
#
# HOW. Stubs only -- nothing is downloaded and no real tree is touched:
#   - the helper and ci-cmake.sh are COPIED into a stub scripts/ dir, so a
#     stub installer can sit beside the helper where it looks for the real
#     one (no test-only knob in the helper);
#   - `cmake`/`ctest` stubs answer --version with a chosen version and log
#     every other call;
#   - XDG_CACHE_HOME points at a private dir, so the per-user install the
#     helper uses is this test's own.
#
# CONTROLS: each assertion has a twin that must come out the other way
# (the system stub reporting the pinned version is NOT moved; a tree the
# pinned cmake configured is NOT reconfigured), and sabotage of the PATH
# order, the version check and the reconfigure check is caught
# (scripts/sabotage.sh, recorded in the commit).
#
# Host-only. Usage: ./test/integration/test-pinned-cmake.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

WORK="$(mktemp -d -t axl-pincmake.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

V="$("$PROJECT_DIR/scripts/ci-cmake.sh" --version)"
M="$(uname -m)"
NAME="cmake-$V-linux-$M"

# stub_tools DIR VERSION -- cmake and ctest in DIR that report VERSION and
# append every other call, prefixed with their own path, to $CALL_LOG.
stub_tools() {
    mkdir -p "$1"
    local t
    for t in cmake ctest; do
        cat > "$1/$t" <<EOF
#!/bin/bash
if [[ "\${1:-}" == --version ]]; then echo "$t version $2"; exit 0; fi
echo "$1/$t \$*" >> "\$CALL_LOG"
[[ -n "\${STUB_FAIL_FRESH:-}" && "\${1:-}" == --fresh ]] && exit 1
# A configure (run from the repo) writes what a real one would: the tree's
# build.ninja and its CTest registration.
if [[ " \$* " == *" --preset "* ]]; then
    mkdir -p build/x64-debug
    touch build/x64-debug/build.ninja build/x64-debug/CTestTestfile.cmake build/x64-debug/axl-integration-ctest.cmake
fi
exit 0
EOF
        chmod +x "$1/$t"
    done
}
export CALL_LOG="$WORK/calls.log"

# A stub scripts/ dir: the helper beside a copy of the real ci-cmake.sh.
mkdir -p "$WORK/real/scripts"
cp "$PROJECT_DIR/scripts/axl-cmake-pin.sh" "$PROJECT_DIR/scripts/ci-cmake.sh" "$WORK/real/scripts/"
# The same helper beside a STUB installer that installs pinned stubs.
mkdir -p "$WORK/fake/scripts"
cp "$PROJECT_DIR/scripts/axl-cmake-pin.sh" "$WORK/fake/scripts/"
cat > "$WORK/fake/scripts/ci-cmake.sh" <<EOF
#!/bin/bash
[[ "\$1" == --version ]] && { echo "$V"; exit 0; }
echo "ci-cmake.sh \$*" >> "$WORK/installer.log"
echo "ci-cmake.sh: fetching https://example.invalid/$NAME.tar.gz" >&2
mkdir -p "\$2/$NAME/bin"
for t in cmake ctest; do
    printf '#!/bin/sh\necho "%s version $V"\n' "\$t" > "\$2/$NAME/bin/\$t"
    chmod +x "\$2/$NAME/bin/\$t"
done
echo "\$2/$NAME/bin"
EOF
chmod +x "$WORK/fake/scripts/ci-cmake.sh"

stub_tools "$WORK/sys" 3.31.8          # the box's own cmake
stub_tools "$WORK/ci" "$V"             # CI's, after ci-cmake.sh

# run_helper LIBDIR PATH-PREFIX -- source the helper in a clean bash with
# PATH-PREFIX first on PATH and a private cache; print, on separate lines,
# its status, the cmake and ctest then on PATH, and its stderr.
run_helper() {
    env XDG_CACHE_HOME="$WORK/cache" PATH="$2:/usr/bin:/bin" bash -c '
        source "$1/axl-cmake-pin.sh"
        axl_use_pinned_cmake 2> "$2"; rc=$?
        echo "rc=$rc"
        echo "cmake=$(command -v cmake)"
        echo "ctest=$(command -v ctest)"
    ' _ "$1" "$WORK/err.txt"
}
CACHE_BIN="$WORK/cache/axl-sdk/cmake/$V/$NAME/bin"

echo "=== the no-op path: the pinned CMake is already first on PATH (CI) ==="
out="$(run_helper "$WORK/fake/scripts" "$WORK/ci")"
check "the pinned cmake already on PATH is left there, silently" \
    "$out|$(cat "$WORK/err.txt")" "rc=0
cmake=$WORK/ci/cmake
ctest=$WORK/ci/ctest|"
check "...and nothing is installed, not even the cache dir" \
    "$(ls -A "$WORK/cache" 2>/dev/null; cat "$WORK/installer.log" 2>/dev/null)" ""

echo "=== first use: another cmake on PATH, nothing cached ==="
out="$(run_helper "$WORK/fake/scripts" "$WORK/sys")"
check "the pinned cmake and ctest are put first on PATH" "$out" "rc=0
cmake=$CACHE_BIN/cmake
ctest=$CACHE_BIN/ctest"
check "...installed through ci-cmake.sh into the per-user cache" \
    "$(cat "$WORK/installer.log")" "ci-cmake.sh install $WORK/cache/axl-sdk/cmake/$V"
check "...saying so in ONE line (the installer's own chatter swallowed)" \
    "$(cat "$WORK/err.txt")" \
    "axl: installing the pinned CMake $V (first use) into $WORK/cache/axl-sdk/cmake/$V -- ~50 MB, once per user"

echo "=== later use: reused from the cache, no install, no line ==="
: > "$WORK/installer.log"
out="$(run_helper "$WORK/fake/scripts" "$WORK/sys")"
check "the cached pinned cmake is put first, silently" \
    "$out|$(cat "$WORK/err.txt")|$(cat "$WORK/installer.log")" "rc=0
cmake=$CACHE_BIN/cmake
ctest=$CACHE_BIN/ctest||"

# The pinned cmake first, but ANOTHER ctest ahead of the one beside it: the
# ctest that would run is not the pinned one, so this is not the no-op.
mkdir -p "$WORK/sysctest"; cp "$WORK/sys/ctest" "$WORK/sysctest/"
out="$(run_helper "$WORK/fake/scripts" "$WORK/sysctest:$WORK/ci")"
check "a pinned cmake behind another ctest is not enough: the pinned pair goes first" \
    "$out" "rc=0
cmake=$CACHE_BIN/cmake
ctest=$CACHE_BIN/ctest"

echo "=== two first uses at once install it ONCE ==="
# The installer is held under a per-user lock and asked again once inside: a
# slow stub install, started twice together, must run once.
rm -rf "$WORK/cache"; : > "$WORK/installer.log"
sed -i.bak 's|^mkdir -p "\$2/|sleep 1; mkdir -p "$2/|' "$WORK/fake/scripts/ci-cmake.sh" && rm "$WORK/fake/scripts/ci-cmake.sh.bak"
( run_helper "$WORK/fake/scripts" "$WORK/sys" > "$WORK/a.out" ) & pa=$!
( run_helper "$WORK/fake/scripts" "$WORK/sys" > "$WORK/b.out" ) & pb=$!
wait "$pa" "$pb"
check "two concurrent first uses: one install, both get the pinned cmake" \
    "$(wc -l < "$WORK/installer.log")|$(head -2 "$WORK/a.out" | paste -sd' ')|$(head -2 "$WORK/b.out" | paste -sd' ')" \
    "1|rc=0 cmake=$CACHE_BIN/cmake|rc=0 cmake=$CACHE_BIN/cmake"

echo "=== no HOME and no XDG_CACHE_HOME: an error, not an unbound-variable abort ==="
out="$(env -u HOME -u XDG_CACHE_HOME PATH="$WORK/sys:/usr/bin:/bin" bash -c '
    set -u; source "$1/axl-cmake-pin.sh"; axl_use_pinned_cmake || echo "rc=$?"' _ "$WORK/fake/scripts" 2>&1)"
check "it says why and returns 1 under set -u" "$out" \
    "axl: neither XDG_CACHE_HOME nor HOME is set -- nowhere to keep the pinned CMake $V
rc=1"

echo "=== a failed install is an ERROR, never the system cmake ==="
# The REAL installer, offline (a curl that fails) and then fed a tarball
# that is not Kitware's (a checksum mismatch).
mkdir -p "$WORK/curlbin"
cat > "$WORK/curlbin/curl" <<'EOF'
#!/bin/bash
[[ -n "${STUB_BODY:-}" ]] || exit 6
while [[ $# -gt 0 ]]; do [[ "$1" == -o ]] && { cp "$STUB_BODY" "$2"; exit 0; }; shift; done
exit 1
EOF
chmod +x "$WORK/curlbin/curl"
echo junk > "$WORK/junk.tar.gz"
rm -rf "$WORK/cache"
want_err="axl: installing the pinned CMake $V (first use) into $WORK/cache/axl-sdk/cmake/$V -- ~50 MB, once per user
axl: could not install the pinned CMake $V into $WORK/cache/axl-sdk/cmake/$V:
ci-cmake.sh: download failed: https://github.com/Kitware/CMake/releases/download/v$V/$NAME.tar.gz
axl: refusing to fall back to $WORK/sys/cmake (cmake version 3.31.8): CI and the release build with $V.
axl: retry: $WORK/real/scripts/ci-cmake.sh install $WORK/cache/axl-sdk/cmake/$V"
out="$(run_helper "$WORK/real/scripts" "$WORK/curlbin:$WORK/sys")"
check "offline: the helper fails, and the system cmake is NOT put first" \
    "$(head -1 <<< "$out")|$(cat "$WORK/err.txt")" "rc=1|$want_err"
out="$(STUB_BODY="$WORK/junk.tar.gz" run_helper "$WORK/real/scripts" "$WORK/curlbin:$WORK/sys")"
check "a checksum mismatch fails the same way, by name" \
    "$(head -1 <<< "$out")|$(sed -n 3p "$WORK/err.txt")" \
    "rc=1|ci-cmake.sh: $NAME.tar.gz does not match its pinned SHA-256 ($(sed -n "s/^AXL_CI_CMAKE_SHA256_$M=//p" "$PROJECT_DIR/scripts/ci-cmake.sh"))"
check "...and nothing is left installed" \
    "$(find "$WORK/cache" -name cmake -type f 2>/dev/null)" ""

# ---------------------------------------------------------------------------
# THE CONSUMER FLOOR (axl_floor_cmake_bin) and the release BELOW it that the
# package must refuse (axl_below_floor_cmake_bin): the same install path and
# cache, other versions, and PATH left alone -- test-cmake-package.sh runs
# them BESIDE the pinned one.
# ---------------------------------------------------------------------------
FV="$("$PROJECT_DIR/scripts/ci-cmake.sh" --floor-version)"
BFV="$("$PROJECT_DIR/scripts/ci-cmake.sh" --below-floor-version)"
# The names are ci-cmake.sh's (`name`), so the stub installer answers them;
# the real one's naming is asserted by test-ci-cmake.sh and, below, by the
# URL the REAL installer fetches offline.
FNAME="cmake-$FV-linux-$M"
BFNAME="cmake-$BFV-Linux-x86_64"
mkdir -p "$WORK/ffake/scripts"
cp "$PROJECT_DIR/scripts/axl-cmake-pin.sh" "$WORK/ffake/scripts/"
cat > "$WORK/ffake/scripts/ci-cmake.sh" <<EOF
#!/bin/bash
[[ "\$1" == --version ]] && { echo "$V"; exit 0; }
[[ "\$1" == --floor-version ]] && { echo "$FV"; exit 0; }
[[ "\$1" == --below-floor-version ]] && { echo "$BFV"; exit 0; }
case "\$2" in
    --floor)       n="$FNAME" v="$FV" ;;
    --below-floor) n="$BFNAME" v="$BFV" ;;
    *)             exit 9 ;;
esac
[[ "\$1" == name ]] && { echo "\$n"; exit 0; }
echo "ci-cmake.sh \$*" >> "$WORK/floor-installer.log"
echo "ci-cmake.sh: fetching https://example.invalid/\$n.tar.gz" >&2
mkdir -p "\$3/\$n/bin"
for t in cmake ctest; do
    printf '#!/bin/sh\necho "%s version %s"\n' "\$t" "\$v" > "\$3/\$n/bin/\$t"
    chmod +x "\$3/\$n/bin/\$t"
done
echo "\$3/\$n/bin"
EOF
chmod +x "$WORK/ffake/scripts/ci-cmake.sh"
# run_floor FUNC VAR LIBDIR PATH-PREFIX -- as run_helper, for FUNC
# (axl_floor_cmake_bin / axl_below_floor_cmake_bin), printing the bin/ it
# leaves in VAR.
run_floor() {
    env XDG_CACHE_HOME="$WORK/cache" PATH="$4:/usr/bin:/bin" bash -c '
        source "$1/axl-cmake-pin.sh"
        "$3" 2> "$2"; rc=$?
        echo "rc=$rc"
        echo "bin=${!4}"
        echo "cmake=$(command -v cmake)"
    ' _ "$3" "$WORK/err.txt" "$1" "$2"
}
# floor_suite KIND FUNC VAR VERSION NAME SIZE WHY -- one extra CMake, end to
# end: installed once, reused silently, and an offline or forged download
# refused by name.
floor_suite() {
    local kind="$1" fn="$2" var="$3" v="$4" n="$5" size="$6" why="$7"
    local bin="$WORK/cache/axl-sdk/cmake/$v/$n/bin" out
    echo "=== the $kind CMake: installed once, beside the pin, PATH untouched ==="
    rm -rf "$WORK/cache"; : > "$WORK/floor-installer.log"
    out="$(run_floor "$fn" "$var" "$WORK/ffake/scripts" "$WORK/sys")"
    check "$kind first use: its bin/ is returned and PATH still leads with the system cmake" \
        "$out" "rc=0
bin=$bin
cmake=$WORK/sys/cmake"
    check "...installed through ci-cmake.sh install --$kind, into the same per-user cache" \
        "$(cat "$WORK/floor-installer.log")" "ci-cmake.sh install --$kind $WORK/cache/axl-sdk/cmake/$v"
    check "...saying so in ONE line" "$(cat "$WORK/err.txt")" \
        "axl: installing the $kind CMake $v (first use) into $WORK/cache/axl-sdk/cmake/$v -- $size, once per user"
    : > "$WORK/floor-installer.log"
    out="$(run_floor "$fn" "$var" "$WORK/ffake/scripts" "$WORK/sys")"
    check "$kind later use: reused, silently, nothing installed" \
        "$(head -2 <<< "$out")|$(cat "$WORK/err.txt")|$(cat "$WORK/floor-installer.log")" "rc=0
bin=$bin||"

    # The REAL installer offline: a named error and an EMPTY bin, never
    # another cmake (the system one is right there on PATH).
    rm -rf "$WORK/cache"
    out="$(run_floor "$fn" "$var" "$WORK/real/scripts" "$WORK/curlbin:$WORK/sys")"
    check "$kind offline: rc=1, no bin, and the error names the download and the retry" \
        "$(head -2 <<< "$out")|$(cat "$WORK/err.txt")" "rc=1
bin=|axl: installing the $kind CMake $v (first use) into $WORK/cache/axl-sdk/cmake/$v -- $size, once per user
axl: could not install the $kind CMake $v into $WORK/cache/axl-sdk/cmake/$v:
ci-cmake.sh: download failed: https://github.com/Kitware/CMake/releases/download/v$v/$n.tar.gz
axl: refusing to go on without it: $why is tested with $v, not another cmake.
axl: retry: $WORK/real/scripts/ci-cmake.sh install --$kind $WORK/cache/axl-sdk/cmake/$v"
    out="$(STUB_BODY="$WORK/junk.tar.gz" run_floor "$fn" "$var" "$WORK/real/scripts" "$WORK/curlbin:$WORK/sys")"
    check "a tarball that is not Kitware's $v is refused by the $kind checksum" \
        "$(head -1 <<< "$out")|$(sed -n 3p "$WORK/err.txt")" \
        "rc=1|ci-cmake.sh: $n.tar.gz does not match its $kind SHA-256 ($(sed -n "s/^AXL_$(tr 'a-z-' 'A-Z_' <<< "$kind")_CMAKE_SHA256_$M=//p" "$PROJECT_DIR/scripts/ci-cmake.sh"))"
}
if [[ "$M" == x86_64 || "$M" == aarch64 ]]; then
    floor_suite floor axl_floor_cmake_bin AXL_FLOOR_CMAKE_BIN "$FV" "$FNAME" "~45 MB" "the consumer floor"
fi
if [[ "$M" == x86_64 ]]; then
    floor_suite below-floor axl_below_floor_cmake_bin AXL_BELOW_FLOOR_CMAKE_BIN "$BFV" "$BFNAME" "~40 MB" \
        "the package's refusal of a CMake below the floor"
fi

# A machine Kitware built no such release for is status 2 -- distinct from a
# failed install (1) -- relays the installer's reason, and fetches nothing.
# The REAL ci-cmake.sh decides, under a uname stub; curl is the failing stub,
# so a fetch would show as a download error instead.
for m in aarch64 riscv64; do
    mkdir -p "$WORK/m-$m"
    printf '#!/bin/sh\n[ "$1" = -m ] && { echo %s; exit 0; }\nexec /usr/bin/uname "$@"\n' "$m" > "$WORK/m-$m/uname"
    chmod +x "$WORK/m-$m/uname"
done
rm -rf "$WORK/cache"
out="$(run_floor axl_below_floor_cmake_bin AXL_BELOW_FLOOR_CMAKE_BIN "$WORK/real/scripts" "$WORK/m-aarch64:$WORK/curlbin:$WORK/sys")"
check "no below-floor build for aarch64: rc=2, no bin, said once, nothing installed" \
    "$(head -2 <<< "$out")|$(cat "$WORK/err.txt")|$(ls -A "$WORK/cache" 2>/dev/null)" "rc=2
bin=|ci-cmake.sh: no below-floor CMake $BFV for machine 'aarch64' (Kitware built 3.16 for x86_64 only)|"
out="$(run_floor axl_floor_cmake_bin AXL_FLOOR_CMAKE_BIN "$WORK/real/scripts" "$WORK/m-riscv64:$WORK/curlbin:$WORK/sys")"
check "no floor build for riscv64: rc=2, no bin, said once, nothing installed" \
    "$(head -2 <<< "$out")|$(cat "$WORK/err.txt")|$(ls -A "$WORK/cache" 2>/dev/null)" "rc=2
bin=|ci-cmake.sh: no floor CMake $FV for machine 'riscv64'|"
# An installer that does not understand `name` (a stale copy) exits 2 with
# its usage line: that is a FAILURE (1), never the skip status 2.
mkdir -p "$WORK/stale/scripts"
cp "$PROJECT_DIR/scripts/axl-cmake-pin.sh" "$WORK/stale/scripts/"
cat > "$WORK/stale/scripts/ci-cmake.sh" <<EOF
#!/bin/bash
[[ "\$1" == --floor-version ]] && { echo "$FV"; exit 0; }
echo "usage: ci-cmake.sh --version | install DIR" >&2
exit 2
EOF
chmod +x "$WORK/stale/scripts/ci-cmake.sh"
out="$(run_floor axl_floor_cmake_bin AXL_FLOOR_CMAKE_BIN "$WORK/stale/scripts" "$WORK/sys")"
check "a usage error from the installer fails (rc=1), and is shown" \
    "$(head -2 <<< "$out")|$(cat "$WORK/err.txt")" "rc=1
bin=|usage: ci-cmake.sh --version | install DIR"

# ---------------------------------------------------------------------------
# A TREE ANOTHER CMAKE CONFIGURED. Ninja's regeneration edge runs the cmake
# CMakeCache.txt names, so PATH alone never moves an existing tree: the
# entry points that configure reconfigure it --fresh.
# ---------------------------------------------------------------------------
echo "=== axl-build.sh reconfigures a tree another cmake configured ==="
REPO="$WORK/repo"
mkdir -p "$REPO/scripts" "$REPO/build/x64-debug" "$REPO/cmake"
for f in axl-build.sh axl-cmake-pin.sh ci-cmake.sh; do cp "$PROJECT_DIR/scripts/$f" "$REPO/scripts/"; done
: > "$REPO/build/x64-debug/build.ninja"
cache_names() { printf 'CMAKE_COMMAND:INTERNAL=%s\n' "$1" > "$REPO/build/x64-debug/CMakeCache.txt"; }
# Through a variable: check-test-meta reads a literal `axl-build.sh ARGS`
# line as a build this test would have to declare, and nothing here builds.
BUILDER="$REPO/scripts/axl-build.sh"
builder() {   # run the stub repo's axl-build.sh with the CI stub first on PATH
    : > "$CALL_LOG"
    env -u ARCH -u BUILD -u RTK_USB_UNDI -u AXL_PREBUILT_MANIFEST \
        XDG_CACHE_HOME="$WORK/cache" PATH="$WORK/ci:/usr/bin:/bin" \
        "$BUILDER" ARCH=x64 hello 2> "$WORK/err.txt"
}
cache_names /usr/bin/cmake
builder; rc=$?
check "a tree whose CMakeCache names another cmake is configured --fresh, then built" \
    "$rc|$(cat "$CALL_LOG")" "0|$WORK/ci/cmake --fresh --preset x64-debug
$WORK/ci/cmake --build build/x64-debug --target hello"
check "...saying why" "$(cat "$WORK/err.txt")" \
    "axl-build.sh: build/x64-debug was configured by /usr/bin/cmake, not the pinned $WORK/ci/cmake -- reconfiguring it --fresh"
cache_names "$WORK/ci/cmake"
builder; rc=$?
check "control: a tree the pinned cmake configured is only built" \
    "$rc|$(cat "$CALL_LOG")|$(cat "$WORK/err.txt")" "0|$WORK/ci/cmake --build build/x64-debug --target hello|"
# A symlink to the pinned binary IS the pinned binary.
ln -s "$WORK/ci" "$WORK/ci-link"
cache_names "$WORK/ci-link/cmake"
builder
check "control: the pinned cmake spelled through a symlink is not foreign" \
    "$(cat "$CALL_LOG")" "$WORK/ci/cmake --build build/x64-debug --target hello"
# Another binary that reports the pinned version is not foreign either: the
# cmake on PATH can be a wrapper (pip's, a snap's) while the cache records
# the real binary, and that must not reconfigure every tree on every build.
stub_tools "$WORK/ci-other" "$V"
cache_names "$WORK/ci-other/cmake"
builder
check "control: another binary reporting the pinned version is not foreign" \
    "$(cat "$CALL_LOG")" "$WORK/ci/cmake --build build/x64-debug --target hello"
# ...but one that is GONE is (CI's $RUNNER_TEMP after the job): ninja's
# regeneration edge would run a binary that is not there.
cache_names "$WORK/gone/cmake"
builder
check "a tree whose recorded cmake no longer exists is reconfigured --fresh" \
    "$(head -1 "$CALL_LOG")" "$WORK/ci/cmake --fresh --preset x64-debug"
# A --fresh that FAILS still writes a cache naming the pinned cmake; the old
# build.ninja (whose regeneration rule runs the OLD cmake) must not survive
# it, or the next build would read the tree as configured and current.
cache_names /usr/bin/cmake
: > "$REPO/build/x64-debug/build.ninja"; : > "$REPO/build/x64-debug/CTestTestfile.cmake"
STUB_FAIL_FRESH=1 builder; rc=$?
check "a failed --fresh fails the build and leaves no build.ninja or registration" \
    "$rc|$(cd "$REPO/build/x64-debug" && ls build.ninja CTestTestfile.cmake 2>/dev/null)" "1|"
: > "$REPO/build/x64-debug/build.ninja"

echo "=== the ctest-only front ends (verify.sh, run-integration.sh) do too ==="
# cts_tree_current: a tree whose registration is CURRENT but whose cmake is
# not the pinned one is reconfigured --fresh before any ctest reads it.
mkdir -p "$REPO/test/integration/lib"
cp "$PROJECT_DIR/test/integration/lib/ctest-session.sh" "$REPO/test/integration/lib/"
: > "$REPO/cmake/AxlIntegrationTests.cmake.in"
touch -d '2000-01-01' "$REPO/cmake/AxlIntegrationTests.cmake.in"
: > "$REPO/build/x64-debug/CTestTestfile.cmake"
: > "$REPO/build/x64-debug/axl-integration-ctest.cmake"
current() {
    : > "$CALL_LOG"
    env XDG_CACHE_HOME="$WORK/cache" PATH="$WORK/ci:/usr/bin:/bin" bash -c '
        source "$1/test/integration/lib/ctest-session.sh"
        cts_tree_current "$1/build/x64-debug" x64-debug "$1" "$2"
    ' _ "$REPO" "$WORK/cfg.log" > "$WORK/out.txt" 2>&1
}
cache_names /usr/bin/cmake
current; rc=$?
check "a current registration from another cmake is reconfigured --fresh" \
    "$rc|$(cat "$CALL_LOG")" "0|$WORK/ci/cmake --fresh --preset x64-debug"
check "...saying why" "$(cat "$WORK/out.txt")" \
    "ctest-session: $REPO/build/x64-debug was configured by /usr/bin/cmake, not the pinned $WORK/ci/cmake -- reconfiguring it --fresh"
cache_names "$WORK/ci/cmake"
current; rc=$?
check "control: a current registration from the pinned cmake is left alone" \
    "$rc|$(cat "$CALL_LOG")|$(cat "$WORK/out.txt")" "0||"
# A CLEAN CHECKOUT has no build/ at all -- CI's release suite reaches this
# first, in its unit step, before anything has configured a tree. The tree's
# lock file lives beside the tree, in build/, so opening it must not depend
# on build/ already existing (CI run 42694772843: "cannot lock ... to
# configure", step 1/5).
mv "$REPO/build" "$REPO/build.kept"
current; rc=$?
check "a checkout with no build/ yet is configured, not refused for its lock" \
    "$rc|$(cat "$CALL_LOG")" "0|$WORK/ci/cmake --preset x64-debug"
rm -rf "$REPO/build"; mv "$REPO/build.kept" "$REPO/build"
cache_names /usr/bin/cmake
STUB_FAIL_FRESH=1 current; rc=$?
check "a failed --fresh leaves no registration a later ctest could read as current" \
    "$rc|$(cd "$REPO/build/x64-debug" && ls build.ninja CTestTestfile.cmake axl-integration-ctest.cmake 2>/dev/null)" "1|"
: > "$REPO/build/x64-debug/build.ninja"
: > "$REPO/build/x64-debug/CTestTestfile.cmake"
: > "$REPO/build/x64-debug/axl-integration-ctest.cmake"
# The reconfigure runs under the tree's build lock (axl-build.sh's): held
# elsewhere, cts_tree_current waits for it.
exec {held}> "$REPO/build/x64-debug.axl-build.lock"
flock "$held"
# (The waiter must not inherit the held fd: a copy of it keeps the lock.)
( exec {held}>&-; current ) & cpid=$!
waited=yes
for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$cpid" 2>/dev/null || { waited=no; break; }
    sleep 0.1
done
calls_while_held="$(cat "$CALL_LOG")"
# The lock's holder (a build) reconfigured it meanwhile: once it is released,
# the question is asked again and there is nothing left to do.
cache_names "$WORK/ci/cmake"
exec {held}>&-
wait "$cpid"; rc=$?
check "a foreign tree's reconfigure waits for the tree's build lock, then asks again" \
    "$waited|$calls_while_held|$rc|$(cat "$CALL_LOG")" "yes||0|"

echo "=== every entry point that runs cmake or ctest asks for the pinned one ==="
# Inverted on purpose, as test-ci-cmake.sh's workflow scan is: every tracked
# shell script that runs cmake or ctest AS A COMMAND must call the helper,
# unless it is named below with why. A list of the scripts that should call
# it would miss the next one.
NEVER_REAL=(
    # Sourced libraries: their callers (verify.sh, run-integration.sh) pin.
    test/integration/lib/ctest-session.sh
    # Drive only stubs of cmake/ctest that the test itself puts on PATH.
    test/integration/test-release-suite.sh
    test/integration/test-ci-cmake.sh
    test/integration/test-pinned-cmake.sh
    # Its `ctest` lines are ci.yml fixture text; the gate it drives lists
    # with the tree's own ctest (CMAKE_CTEST_COMMAND), whatever PATH says.
    test/integration/test-check-test-est.sh
    # Matches only inside expected-output strings ("(ctest never ran it").
    test/integration/test-ctest-report.sh
)
runs_cmake="$(cd "$PROJECT_DIR" && git ls-files '*.sh' | while read -r f; do
    # Command position: a line start or ; & | ( { ! $( or a backtick (an
    # array's opening paren included), then any of if/then/do/else/env/
    # exec/timeout N, then any VAR=value prefixes. Not `grep -q`: under
    # pipefail its early exit SIGPIPEs sed and reads as no match.
    sed -e 's/[[:space:]]#.*$//' -e '/^[[:space:]]*#/d' "$f" \
        | grep -E '(^|[;&|({`!]|\$\()[[:space:]]*((if|then|do|else|env|exec|timeout[[:space:]]+[0-9]+)[[:space:]]+)*([A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+)*(cmake|ctest)([[:space:]]|$)' \
        > /dev/null && echo "$f"
done)"
check "control: the scan finds the entry points it must (axl-build.sh, lint.sh, verify.sh's ctest)" \
    "$(grep -cxE 'scripts/(axl-build|lint|verify)\.sh' <<< "$runs_cmake")" "3"
missing="$(while read -r f; do
    [[ -n "$f" ]] || continue
    [[ " ${NEVER_REAL[*]} " == *" $f "* ]] && continue
    # A CALL, on a line of code: a comment naming the helper calls nothing.
    grep -qE '^[^#]*axl_use_pinned_cmake([[:space:]]|$)' "$PROJECT_DIR/$f" || echo "$f"
done <<< "$runs_cmake")"
check "every script that runs cmake or ctest calls axl_use_pinned_cmake" "$missing" ""

echo ""
echo "pinned-cmake: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
