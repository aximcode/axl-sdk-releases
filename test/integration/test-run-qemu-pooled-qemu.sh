#!/bin/bash
# test-meta: arch=both needs= est=40 local-only=0 builds=sha256sum guest=plain
# test-run-qemu-pooled-qemu.sh -- scripts/run-qemu.sh's pooled mode on a REAL
# leased guest (design §6.9; test-run-qemu-pooled.sh pins the rest host-only).
#
# Leased (the runner gives it a `plain` guest), every run-qemu.sh call below
# runs on the lease; booted (no pool), every one boots. The checks are the
# same either way -- the pass count balances -- and each pooled answer is held
# to the BOOTED answer of the same call (AXL_RUN_QEMU_POOL=0), exactly:
#   - an app with an --extra file and arguments (sha256sum of the file): the
#     same output a boot prints, and the file's real digest in it;
#   - a --nsh script with the boot's boilerplate (`fs0:`, `cd \`, `reset -s`)
#     that sets a volatile Shell variable: the same output;
#   - the NEXT run reads that variable: unset, as on a fresh boot -- the
#     mid-lease restore gave it a fresh machine;
#   - a --nsh naming the tool bare (`sha256sum data.txt`): found through the
#     Shell's path, as a boot finds it on its disk's root;
#   - a --nsh whose script leaves %lasterror% at 0xE (a `type` of a missing
#     file, then an `echo`, which sets no status): it RAN -- the agent reads a
#     script whose last status is 0xE as "not run", which made such a call
#     boot instead -- with the output a boot prints;
#   - each run says how it ran: `mode=pooled` leased, `mode=fg` booted.
#
# Usage: ./test/integration/test-run-qemu-pooled-qemu.sh [--arch X64|AARCH64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        *) echo "ERROR: unknown arg '$1'"; exit 2 ;;
    esac
done
case "$ARCH" in
    X64)     NATIVE=x64 ;;
    AARCH64) NATIVE=aa64 ;;
    *) echo "ERROR: --arch X64|AARCH64"; exit 2 ;;
esac

PASS=0 FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
same() {   # same NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | sed 's/^/      /'
    fi
}

WORK="$(mktemp -d -t axl-rqpq.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" sha256sum > "$WORK/build.log" 2>&1; then
    echo "  FAIL: sha256sum did not build (ARCH=$NATIVE)"
    tail -20 "$WORK/build.log"
    exit 1
fi
TOOL="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")/tools/sha256sum.efi"
printf 'pooled run-qemu.sh\n' > "$WORK/data.txt"
DIGEST=$(sha256sum "$WORK/data.txt" | cut -d' ' -f1)

if [[ -n "${AXL_POOL_AGENT:-}" ]]; then MODE="pooled"; else MODE="fg"; fi
if [[ "$MODE" == pooled ]]; then
    echo "=== run-qemu.sh on a leased ${AXL_POOL_CLASS:-?} guest ($ARCH) ==="
else
    echo "=== run-qemu.sh on booted guests ($ARCH) ==="
fi

# rq NAME ARGS...: one run as a caller makes it (stdout+stderr captured), the
# DEBUG leak verdict's clock stamp normalised away; its TIMING lines kept in
# $WORK/NAME.timing (and appended to this test's own log).
rq() {
    local name="$1"
    shift
    AXL_TEST_TIMING_LOG="$WORK/$name.timing" timeout 120 "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$ARCH" --timeout 40 "$@" 2>&1 \
        | grep -v 'Extracted Shell.efi' | sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+ //' \
        > "$WORK/$name.out"
    if [[ -n "${AXL_TEST_TIMING_LOG:-}" && -f "$WORK/$name.timing" ]]; then
        cat "$WORK/$name.timing" >> "$AXL_TEST_TIMING_LOG"
    fi
}
# how NAME: how the run went, from its TIMING line.
how() { grep -o '^TIMING: run-qemu mode=[a-z]*' "$WORK/$1.timing" 2>/dev/null | sed 's/.*mode=//'; }

printf '@echo -off\r\nfs0:\r\ncd \\\r\nset -v RQPOOL set\r\necho rq=[%%RQPOOL%%]\r\nreset -s\r\n' > "$WORK/set.nsh"
printf '@echo -off\r\necho rq=[%%RQPOOL%%]\r\n' > "$WORK/read.nsh"
# A bare tool name, as most scripts write it: a boot's Shell finds the .efi
# on its disk's root through `path` (and the file beside it once `fs0:`
# gives the script a current directory).
printf '@echo -off\r\nfs0:\r\nsha256sum data.txt\r\n' > "$WORK/bare.nsh"
# ...and without `fs0:`, a relative name finds nothing on a boot: the same
# on the lease (the run does not enter its directory).
printf '@echo -off\r\nsha256sum data.txt\r\n' > "$WORK/nocwd.nsh"
# A script whose last status is 0xE (not found): it still ran.
printf '@echo -off\r\ntype nosuch.txt\r\necho after-not-found\r\n' > "$WORK/notfound.nsh"

rq app --extra "$WORK/data.txt" "$TOOL" data.txt
rq set --nsh "$WORK/set.nsh" "$TOOL"
rq read --nsh "$WORK/read.nsh" "$TOOL"
rq bare --nsh "$WORK/bare.nsh" --extra "$WORK/data.txt" "$TOOL"
rq nocwd --nsh "$WORK/nocwd.nsh" --extra "$WORK/data.txt" "$TOOL"
rq notfound --nsh "$WORK/notfound.nsh" "$TOOL"
# The same calls booted: the answer each pooled one is held to.
AXL_RUN_QEMU_POOL=0 rq app-b --extra "$WORK/data.txt" "$TOOL" data.txt
AXL_RUN_QEMU_POOL=0 rq set-b --nsh "$WORK/set.nsh" "$TOOL"
AXL_RUN_QEMU_POOL=0 rq read-b --nsh "$WORK/read.nsh" "$TOOL"
AXL_RUN_QEMU_POOL=0 rq bare-b --nsh "$WORK/bare.nsh" --extra "$WORK/data.txt" "$TOOL"
AXL_RUN_QEMU_POOL=0 rq nocwd-b --nsh "$WORK/nocwd.nsh" --extra "$WORK/data.txt" "$TOOL"
AXL_RUN_QEMU_POOL=0 rq notfound-b --nsh "$WORK/notfound.nsh" "$TOOL"

same "the six runs ran $MODE" \
    "$(how app) $(how set) $(how read) $(how bare) $(how nocwd) $(how notfound)" \
    "$MODE $MODE $MODE $MODE $MODE $MODE"
same "the booted controls booted" \
    "$(how app-b) $(how set-b) $(how read-b) $(how bare-b) $(how nocwd-b) $(how notfound-b)" \
    "fg fg fg fg fg fg"
same "an app with an --extra file: the output a boot prints" "$(cat "$WORK/app.out")" \
    "$(cat "$WORK/app-b.out")"
if grep -qx "$DIGEST  data.txt" "$WORK/app.out"; then
    pass "...the staged file's real digest in it"
else
    fail "...the staged file's real digest in it ($DIGEST)"
    sed 's/^/      /' "$WORK/app.out"
fi
same "a --nsh with the boot's boilerplate: the output a boot prints" "$(cat "$WORK/set.out")" \
    "$(cat "$WORK/set-b.out")"
same "...which set the variable" "$(grep '^rq=' "$WORK/set.out")" "rq=[set]"
same "the next run: a fresh machine, the variable unset as on a boot" "$(cat "$WORK/read.out")" \
    "$(cat "$WORK/read-b.out")"
same "...not the earlier run's value" "$(grep -c '^rq=\[set\]' "$WORK/read.out")" "0"
same "a bare tool name in a --nsh: found as on a boot, the same output" "$(cat "$WORK/bare.out")" \
    "$(cat "$WORK/bare-b.out")"
same "...the digest, not 'is not recognized'" "$(grep -c "^$DIGEST  data.txt\$" "$WORK/bare.out")" "1"
same "a --nsh without fs0: the output a boot prints (no current directory there)" \
    "$(cat "$WORK/nocwd.out")" "$(cat "$WORK/nocwd-b.out")"

same "a --nsh whose last status is 0xE: the output a boot prints" \
    "$(cat "$WORK/notfound.out")" "$(cat "$WORK/notfound-b.out")"
same "...which ran to its last line" "$(grep -cx 'after-not-found' "$WORK/notfound.out")" "1"

printf 'run-qemu-pooled-qemu (%s): %d passed, %d failed\n' "$ARCH" "$PASS" "$FAIL"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
