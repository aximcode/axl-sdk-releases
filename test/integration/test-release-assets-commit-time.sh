#!/bin/bash
# test-meta: arch=x64 needs= est=5 local-only=0
# test-release-assets-commit-time.sh -- make-release-assets.sh tells "git
# refused" apart from "not a git checkout".
#
# WHY. The archives' mtimes are the commit's (SOURCE_DATE_EPOCH, from `git log
# -1 --format=%ct`). On 2026-11-10 CI's build job -- a container running as
# root over a checkout owned by the runner's uid -- died with "not a git
# checkout: set SOURCE_DATE_EPOCH", which was false: it WAS a checkout, and git
# had refused it (`fatal: detected dubious ownership`, exit 128). The script
# read every failure, and every empty answer, as "not a checkout", because it
# threw git's stderr away. An empty string is not an answer unless you know
# the question was asked.
#
# A stub `git` on PATH plays each answer; nothing is built (the commit time is
# read before any build, and the positive control stops at a stub build
# wrapper that reports the SOURCE_DATE_EPOCH it was handed).
#
# Usage: ./test/integration/test-release-assets-commit-time.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAKER="$PROJECT_DIR/scripts/make-release-assets.sh"

PASS=0; FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

WORK="$(mktemp -d -t axl-commit-time.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# The build wrapper stand-in: it is first asked for a tree (--print-prefix),
# and says what SOURCE_DATE_EPOCH the driver resolved before refusing.
WRAP="$WORK/wrapper"
printf '%s\n' '#!/bin/bash' 'echo "wrapper saw SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH-unset}" >&2' 'exit 1' > "$WRAP"
chmod +x "$WRAP"

# stub_git RC STDOUT STDERR -- a `git` that answers every call so.
stub_git() {
    mkdir -p "$WORK/bin"
    printf '#!/bin/bash\nprintf %%s %q\nprintf %%s %q >&2\nexit %d\n' "$2" "$3" "$1" > "$WORK/bin/git"
    chmod +x "$WORK/bin/git"
}
# run_maker [PATH-PREFIX] -- the driver as CI's build job runs it, minus the
# build; echoes "RC<newline>OUTPUT".
run_maker() {
    local out rc
    out="$(env -u SOURCE_DATE_EPOCH PATH="${1:+$1:}$PATH" AXL_BUILD_WRAPPER="$WRAP" \
           "$MAKER" uefi-tools --arch x64 --out "$WORK/out" 2>&1)"; rc=$?
    printf '%s\n%s' "$rc" "$out"
}

echo "=== git refused the checkout (dubious ownership, exit 128) ==="
stub_git 128 "" "fatal: detected dubious ownership in repository at '$PROJECT_DIR'
To add an exception for this directory, call:

	git config --global --add safe.directory $PROJECT_DIR
"
check "a refusal is named as git's, with git's words and the way out" "$(run_maker "$WORK/bin")" \
"1
make-release-assets: git could not read the commit time of $PROJECT_DIR (exit 128):
    fatal: detected dubious ownership in repository at '$PROJECT_DIR'
    To add an exception for this directory, call:
    	git config --global --add safe.directory $PROJECT_DIR
make-release-assets: the checkout belongs to another user (a container running as root?): trust it first with git config --global --add safe.directory \"$PROJECT_DIR\", or set SOURCE_DATE_EPOCH"

echo "=== not a git checkout at all ==="
stub_git 128 "" "fatal: not a git repository (or any of the parent directories): .git
"
check "no checkout is named as such" "$(run_maker "$WORK/bin")" \
"1
make-release-assets: not a git checkout ($PROJECT_DIR): set SOURCE_DATE_EPOCH to the time the archive should carry"

echo "=== any other git failure ==="
stub_git 1 "" "fatal: bad config line 1 in file /root/.gitconfig
"
check "another failure passes git's stderr through" "$(run_maker "$WORK/bin")" \
"1
make-release-assets: git could not read the commit time of $PROJECT_DIR (exit 1):
    fatal: bad config line 1 in file /root/.gitconfig
make-release-assets: fix git, or set SOURCE_DATE_EPOCH to the time the archive should carry"

echo "=== git succeeded and said nothing a time can be ==="
stub_git 0 "" ""
check "an empty answer is not a commit time" "$(run_maker "$WORK/bin")" \
"1
make-release-assets: git log answered '', not a commit time: set SOURCE_DATE_EPOCH to the time the archive should carry"

echo "=== a SOURCE_DATE_EPOCH the caller set is held to the same rule ==="
out="$(SOURCE_DATE_EPOCH=yesterday AXL_BUILD_WRAPPER="$WRAP" \
       "$MAKER" uefi-tools --arch x64 --out "$WORK/out" 2>&1)"; rc=$?
check "a caller's non-numeric SOURCE_DATE_EPOCH is refused" "$rc
$out" "1
make-release-assets: SOURCE_DATE_EPOCH is 'yesterday', not a time in seconds"
out="$(SOURCE_DATE_EPOCH=1700000000 AXL_BUILD_WRAPPER="$WRAP" \
       "$MAKER" uefi-tools --arch x64 --out "$WORK/out" 2>&1)"; rc=$?
check "a caller's numeric SOURCE_DATE_EPOCH reaches the build" "$rc
$out" "1
wrapper saw SOURCE_DATE_EPOCH=1700000000
make-release-assets: axl-build.sh --print-prefix ARCH=x64 failed"

echo "=== CONTROL: the real git, in this checkout ==="
want_t="$(git -C "$PROJECT_DIR" log -1 --format=%ct)"
check "the commit time reaches the build (the wrapper saw it)" "$(run_maker "")" \
"1
wrapper saw SOURCE_DATE_EPOCH=$want_t
make-release-assets: axl-build.sh --print-prefix ARCH=x64 failed"

echo ""
echo "release-assets-commit-time: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
