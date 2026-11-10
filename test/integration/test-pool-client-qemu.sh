#!/bin/bash
# test-meta: arch=x64 needs=qemu.qmp est=60 local-only=0 builds=agent,shell-launcher,timer-res-selftest guest=none
# guest=none: the pool's own test: it starts a pool service and its guests
#   itself (test/pool/client-cases.py).
# test-pool-client-qemu.sh -- the guest pool's client side (test/pool/
# axlpool/client.py): PoolClient, and `axlpool run` wrapping a test -- its
# environment, its status passed through, 77 for every pool failure, files
# kept on failure, signals forwarded -- against a real `axlpool serve`:
# test/pool/client-cases.py. Arch-independent, so X64 only.
#
# Usage: ./test/integration/test-pool-client-qemu.sh [--arch X64|AARCH64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"

if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" agent shell-launcher timer-res-selftest > /dev/null 2>&1; then
    echo "  FAIL: the agent and shell-launcher did not build (ARCH=$NATIVE)"
    exit 1
fi
if ! python3 -c 'import qemu.qmp' 2>/dev/null; then
    echo "  FAIL: the python3 module qemu.qmp is not installed (pip install --user qemu.qmp)"
    exit 1
fi
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")"
WORK="$(mktemp -d -t axl-poolc.XXXXXXXX)"
# Anything the cases left running (they assert there is none) dies before
# its directory goes: found by PATH, not by pids the cases recorded.
cleanup() {
    local pid
    for pid in $(ps -eo pid=,args= | awk -v w="$WORK" '($2 ~ /qemu-system|virtiofsd/) && index($0, w) {print $1}'); do
        kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0
FAIL=0
run_cases() {
    local out
    out=$(timeout 600 "$@" 2>&1)
    echo "$out" | grep -vE '^client-cases:'
    PASS=$((PASS + $(grep -c '^  PASS:' <<< "$out")))
    FAIL=$((FAIL + $(grep -c '^  FAIL:' <<< "$out")))
    # A crash before the summary is a failure, never silence.
    grep -qE '^client-cases: ' <<< "$out" || { echo "  FAIL: $2 printed no summary"; FAIL=$((FAIL + 1)); }
    # An exception the service did not handle is a defect even when every
    # case passed (asyncio only logs it).
    if grep -qE '^Traceback|Unhandled exception|socket\.send\(\) raised exception' <<< "$out"; then
        echo "  FAIL: $2 printed an unhandled exception"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== guest pool: client ($TEST_ARCH) ==="
run_cases python3 "$PROJECT_DIR/test/pool/client-cases.py" "$TEST_ARCH" "$PREFIX/tools/agent.efi" "$WORK"
echo "pool-client ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
