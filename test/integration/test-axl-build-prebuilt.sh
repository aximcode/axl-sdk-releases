#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-axl-build-prebuilt.sh -- scripts/axl-build.sh skips a build the
# runner already made (AXL_PREBUILT_MANIFEST), and ONLY that one.
#
# run-integration.sh builds every test's declared builds= up front, and
# check-test-meta holds each test's own build line to that declaration, so
# the test's own call is a ~1 s no-op build (a third of a pooled test's
# time). axl-build.sh skips it when a manifest line has the SAME arch, the
# SAME command-line variables, EVERY target asked for, and the same build
# fingerprint: the environment variables the build definition reads (derived
# from cmake/ and the root CMakeLists.txt, not listed by hand), the tree, and
# the state of the call's own build tree (its build.ninja and rules.ninja).
# Anything else builds as before.
#
# Run against a COPY of axl-build.sh in a fake tree (its own cmake/ and
# build tree) with a fake `cmake` on PATH that exits 3: "it tried to build"
# is exit 3, and nothing real is built or touched. Boots nothing.
#
# Usage: ./test/integration/test-axl-build-prebuilt.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

WORK="$(mktemp -d -t axl-prebuilt.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
TREE="$WORK/tree"
mkdir -p "$TREE/scripts" "$TREE/cmake" "$WORK/bin"
SOURCE_SCRIPT="$PROJECT_DIR/scripts/axl-build.sh"   # (a variable: not a build line to check-test-meta)
cp "$SOURCE_SCRIPT" "$TREE/scripts/"
# The script under test, through a variable: nothing this test calls builds,
# so check-test-meta must not read the calls as builds to declare.
BUILDER="$TREE/scripts/axl-build.sh"
# The build definition's environment reads, each spelling the derivation
# knows: $ENV{NAME} and ${NAME}.
printf 'set(OUT "$ENV{SSHD_RTK_OUT}")\nset(CC "$ENV{AXL_X64_GCC}")\nset(P ${AXL_PACK_STATE_DIR})\n' \
    > "$TREE/cmake/AxlOut.cmake"
# It passes as the pinned CMake (lib/cmake-stub.sh), or the copy's
# axl_use_pinned_cmake would put the real one first and it would never run.
# shellcheck source=lib/cmake-stub.sh
source "$SCRIPT_DIR/lib/cmake-stub.sh"
{ echo '#!/bin/bash'; cmake_stub_version_line; printf 'echo "fake cmake $*"\nexit 3\n'; } > "$WORK/bin/cmake"
chmod +x "$WORK/bin/cmake"
cmake_stub_repo "$TREE" "$WORK/bin"
mkdir -p "$TREE/build/x64-debug" "$TREE/build/aa64-debug"
printf 'rule cc\n' > "$TREE/build/x64-debug/build.ninja"
printf 'rule cc\n' > "$TREE/build/aa64-debug/build.ninja"

# A clean environment for every call: none of the build's inputs set, so the
# manifest's fingerprint (computed the same way) is what a call matches.
clean() {
    env -u ARCH -u BUILD -u PREFIX -u AXL_PACK_STATE_DIR -u AXL_X64_GCC -u SSHD_RTK_OUT \
        -u AXL_BUILD_BACKEND -u AXL_PREBUILT_MANIFEST -u RTK_USB_UNDI \
        PATH="$WORK/bin:$PATH" "$@"
}
fp() { clean "$BUILDER" --env-fingerprint "$@"; }   # fp ARGS -> the fingerprint for those args
M="$WORK/manifest"
{
    printf 'x64||agent shell-launcher driver sshd-rtk|%s\n' "$(fp ARCH=x64 agent)"
    printf 'aa64||agent|%s\n' "$(fp ARCH=aa64 agent)"
    printf 'x64|BUILD=RELEASE|probe|%s\n' "$(fp ARCH=x64 BUILD=RELEASE probe)"
} > "$M"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
build() {   # [CALL_ENV=VAR=VALUE] build ARGS... -> "<exit> <first line>"; exit 3 = it tried to build
    local out rc
    # shellcheck disable=SC2086
    out=$(clean AXL_PREBUILT_MANIFEST="$M" ${CALL_ENV:-} "$BUILDER" "$@" 2>&1); rc=$?
    printf '%s %s\n' "$rc" "$(head -1 <<< "$out")"
}
SKIP="axl-build.sh: pre-built by the runner:"

echo "=== axl-build.sh: skipping what the runner pre-built ==="
check "every target pre-built for this arch: skipped (exit 0, said)" \
    "$(build ARCH=x64 agent driver)" "0 $SKIP ARCH=x64 agent driver"
check "the arch defaults to x64" \
    "$(build -s agent)" "0 $SKIP ARCH=x64 agent"
check "-j N (N a separate word) is how, not what: skipped" \
    "$(build -j 4 ARCH=x64 agent)" "0 $SKIP ARCH=x64 agent"
check "a target NOT pre-built: it builds" "$(build ARCH=x64 agent hello | cut -c1-2)" "3 "
check "pre-built for another arch only: it builds" "$(build ARCH=aa64 driver | cut -c1-2)" "3 "
check "a command-line variable the pre-build did not pass: it builds" \
    "$(build ARCH=x64 BUILD=RELEASE agent | cut -c1-2)" "3 "
check "a pre-build made WITH a variable is matched only with it" \
    "$(build ARCH=x64 BUILD=RELEASE probe)" "0 $SKIP ARCH=x64 BUILD=RELEASE probe"
check "... and not without it" "$(build ARCH=x64 probe | cut -c1-2)" "3 "
check "any other option (-B) is not skipped: the build refuses it (2)" \
    "$(build -B ARCH=x64 agent | cut -c1-2)" "2 "

# The environment: what the build definition READS, derived -- not a list.
check "an environment variable cmake/ reads as \${VAR} (AXL_PACK_STATE_DIR) set: it builds" \
    "$(CALL_ENV=AXL_PACK_STATE_DIR=/s build ARCH=x64 agent | cut -c1-2)" "3 "
check "one it reads as \$ENV{VAR} (AXL_X64_GCC) set: it builds" \
    "$(CALL_ENV=AXL_X64_GCC=/g build ARCH=x64 agent | cut -c1-2)" "3 "
check "another it reads as \$ENV{VAR} (SSHD_RTK_OUT) set: it builds" \
    "$(CALL_ENV=SSHD_RTK_OUT=/o build ARCH=x64 sshd-rtk | cut -c1-2)" "3 "
check "one NOTHING reads (UNRELATED_VAR) set: still skipped -- derived, not blanket" \
    "$(CALL_ENV=UNRELATED_VAR=1 build ARCH=x64 agent)" "0 $SKIP ARCH=x64 agent"
check "a variable given on the command line is not read from the environment" \
    "$(clean ARCH=aa64 "$BUILDER" --env-fingerprint ARCH=x64 agent)" "$(fp ARCH=x64 agent)"

# The tree: its build.ninja rewritten (a reconfigure, a wipe) since the
# pre-build means its images may be gone.
printf 'rule cc-reconfigured\n' > "$TREE/build/x64-debug/build.ninja"
check "the call's tree reconfigured since the pre-build: it builds" \
    "$(build ARCH=x64 agent | cut -c1-2)" "3 "
printf 'rule cc\n' > "$TREE/build/x64-debug/build.ninja"
check "... and back as it was: skipped again" "$(build ARCH=x64 agent)" "0 $SKIP ARCH=x64 agent"
# Only the call's OWN tree counts: install.sh, a fixture group or another
# test first-building ANOTHER tree later in the run must not kill the skip.
mkdir -p "$TREE/build/x64-release"
printf 'release-rules\n' > "$TREE/build/x64-release/build.ninja"
check "another tree appearing later: still skipped (only the call's own counts)" \
    "$(build ARCH=x64 agent)" "0 $SKIP ARCH=x64 agent"

# Never empty, even on a tree with none of its inputs (no cmake/, no build
# tree): an empty fingerprint would match another empty one.
bare="$WORK/bare/scripts"; mkdir -p "$bare"; cp "$BUILDER" "$bare/"
check "on a bare tree the fingerprint is still whole (backend, tree, state)" \
    "$(clean "$bare/axl-build.sh" --env-fingerprint ARCH=x64 agent | cut -d' ' -f1,2 )" \
    "backend=cmake tree=$WORK/bare"
out=$(clean "$BUILDER" ARCH=x64 agent 2>&1); rc=$?
check "no manifest (not under the runner): it builds" "$rc" "3"
check "--print-prefix is never skipped: it names the tree" \
    "$(build --print-prefix ARCH=x64)" "0 build/x64-debug"

echo "axl-build-prebuilt: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
