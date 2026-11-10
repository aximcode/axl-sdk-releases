#!/bin/bash
# test-meta: arch=none needs=flock est=10 local-only=0
# test-axl-build-lock.sh -- scripts/axl-build.sh runs ONE build at a time in
# a tree: concurrent calls on the same tree run one after another, calls on
# different trees run at once.
#
# WHY. Ninja does not support two invocations in one build tree. Each
# decides what is stale on its own, and both then run the SAME step:
# two `objcopy ... X.efi.tmp && rename X.efi.tmp X.efi`, where one renames
# the other's temporary away ("Error renaming ... No such file or
# directory") or reads a .so the other is still linking ("input file ... is
# empty"). The integration suite runs a dozen tests at once, each building
# its fixtures through this script, so the moment anything is stale -- a
# reconfigure (test-cmake-reconfigure-noop.sh), an edit mid-run, a fixture
# the prep did not build -- they collide. That failed CI on 2026-11-09
# (test-stderr-row-qemu.sh, "ninja: build stopped: subcommand failed").
# Per-suite flocks (lib/sshd-lib.sh, lib/console-device-lib.sh) guarded a
# few callers each and nothing guarded the rest; the lock is now in the one
# place every build goes through.
#
# HOW. A stub repository holding a copy of the real script and a stub
# `cmake` on PATH that records when it starts and ends and sleeps
# in between. The real build is never run, and no real tree is touched.
#
# CONTROL: two calls on DIFFERENT trees (x64 and aa64) must overlap -- which
# proves the stub's log can show an overlap at all, and that the lock is per
# tree rather than one lock for everything.
#
# Host-only. Usage: ./test/integration/test-axl-build-lock.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# shellcheck source=lib/cmake-stub.sh
source "$SCRIPT_DIR/lib/cmake-stub.sh"
WORK="$(mktemp -d -t axl-buildlock.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
REPO="$WORK/repo"
mkdir -p "$REPO/scripts" "$REPO/build/x64-debug" "$REPO/build/aa64-debug" "$WORK/bin"
# Both copies named through variables: check-test-meta reads a line with
# `scripts/axl-build.sh ARGS` on it as a build of ARGS, and nothing here
# builds anything real.
REAL_SH="$PROJECT_DIR/scripts/axl-build.sh"
BUILD_SH="$REPO/scripts/axl-build.sh"
cp "$REAL_SH" "$BUILD_SH"
# Configured trees, so the script builds without configuring first.
: > "$REPO/build/x64-debug/build.ninja"
: > "$REPO/build/aa64-debug/build.ninja"
LOG="$WORK/calls.log"
for tool in cmake; do
    # start/end, each with this process's PID and the tree it was given.
    cat > "$WORK/bin/$tool" <<EOF
#!/bin/bash
$(cmake_stub_version_line)
echo "start \$\$ $tool \$*" >> "$LOG"
sleep 1.5
echo "end \$\$ $tool \$*" >> "$LOG"
EOF
    chmod +x "$WORK/bin/$tool"
done
# The copy pins CMake (scripts/axl-cmake-pin.sh): the stub must pass as the
# pinned one, or the real one would run instead of it.
cmake_stub_repo "$REPO" "$WORK/bin"
export PATH="$WORK/bin:$PATH"
unset AXL_PREBUILT_MANIFEST AXL_BUILD_BACKEND ARCH BUILD PREFIX RTK_USB_UNDI SSHD_RTK_OUT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
# max_running -- the most stub builds the log shows running at one moment.
max_running() {
    awk '$1 == "start" { n++; if (n > m) m = n } $1 == "end" { n-- } END { print m + 0 }' "$LOG"
}
# run_together ARGS-PER-CALL... -- one axl-build.sh call per argument (a
# quoted, space-separated list), all started at once.
run_together() {
    local a pids=() rc=0 p
    : > "$LOG"
    for a in "$@"; do
        # shellcheck disable=SC2086
        "$BUILD_SH" $a > /dev/null 2>&1 &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || rc=1; done
    return $rc
}

echo "=== one build at a time in a tree ==="
run_together "ARCH=x64 tools" "ARCH=x64 sshd" "ARCH=x64 tools" "ARCH=x64 hello"
check "four calls on build/x64-debug all ran" "$(grep -c '^end ' "$LOG")" "4"
check "they never overlapped" "$(max_running)" "1"

run_together "ARCH=x64 tools" "ARCH=aa64 tools"
check "a call on x64 and one on aa64 overlap (control: per tree)" "$(max_running)" "2"

echo "=== a call the runner pre-built never waits for the lock ==="
# The suite's dozen concurrent calls are almost all skipped as pre-built;
# taking the lock before that skip would queue every one of them. Hold the
# tree's lock from outside and make a call the manifest names: it must
# return at once, without having built.
export AXL_PREBUILT_MANIFEST="$WORK/manifest"
printf 'x64||tools|%s\n' "$("$BUILD_SH" --env-fingerprint ARCH=x64 tools)" \
    > "$AXL_PREBUILT_MANIFEST"
: > "$LOG"
exec {held}>"$REPO/build/x64-debug.axl-build.lock"
flock "$held"
got=$(timeout 5 "$BUILD_SH" ARCH=x64 tools 2>&1; echo "rc=$?")
exec {held}>&-
check "a pre-built call returns at once while the tree is locked" \
      "$got" "axl-build.sh: pre-built by the runner: ARCH=x64 tools
rc=0"
check "...and built nothing" "$(grep -c '^start ' "$LOG")" "0"
# Control: the same call NOT in the manifest does wait for the held lock.
: > "$AXL_PREBUILT_MANIFEST"
exec {held}>"$REPO/build/x64-debug.axl-build.lock"
flock "$held"
: > "$LOG"
timeout 2 "$BUILD_SH" ARCH=x64 tools > /dev/null 2>&1
check "a call to build waits while the tree is locked (control)" "$?" "124"
# 124 alone could be a slow host running the stub past the timeout; a call
# that WAITED never reached the build at all.
check "...and never reached the build (control)" "$(grep -c '^start ' "$LOG")" "0"
exec {held}>&-
unset AXL_PREBUILT_MANIFEST

echo ""
echo "axl-build-lock: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
